#include "citron.h"
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int ptrace_fork_and_trace(const char *path, char *const argv[],
                          debugger_t *dbg) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* Child process */
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0) {
            perror("ptrace(TRACEME)");
            exit(1);
        }

        /* Raise SIGSTOP to let parent attach before we run */
        raise(SIGSTOP);

        /* Execute the target program */
        execvp(path, argv);
        perror("execvp");
        exit(1);
    }

    /* Parent - wait for the child's initial SIGSTOP (this is BEFORE execvp,
       so the address space is still the forked copy of citron). */
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "Child did not stop as expected\n");
        return -1;
    }

    /* Kill the tracee if we die, and get a distinct stop at exec. */
    ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_EXITKILL | PTRACE_O_TRACEEXEC);

    /* Run the child through execvp so it stops again AT THE EXEC EVENT — now
       the TARGET program is mapped. Setting breakpoints before this point
       would write into the pre-exec image and be discarded by execvp (the bug
       that made breakpoints silently never fire). */
    if (ptrace(PTRACE_CONT, pid, 0, 0) < 0) {
        perror("ptrace(CONT)");
        return -1;
    }
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (WIFEXITED(status)) {
        fprintf(stderr, "Target exited before exec (bad path: %s?)\n", path);
        return -1;
    }

    dbg->proc.pid = pid;
    dbg->proc.state = PROC_STOPPED;

    /* Record the entry instruction pointer so the REPL knows where we are. */
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == 0) {
        dbg->proc.ip = regs_get_ip(&regs);
    }

    return 0;
}

int ptrace_attach(pid_t pid, debugger_t *dbg) {
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) {
        perror("ptrace(ATTACH)");
        return -1;
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }

    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "Process did not stop as expected\n");
        return -1;
    }

    dbg->proc.pid = pid;
    dbg->proc.state = PROC_STOPPED;

    /* Try to load ELF from /proc/pid/exe */
    char exe_path[256];
    snprintf(exe_path, sizeof(exe_path), "/proc/%d/exe", pid);

    if (elf_load(exe_path, &dbg->elf) < 0) {
        warn("Could not load symbols from %s", exe_path);
    }

    return 0;
}

/* When set, ptrace_wait suppresses the informational "Stopped at" /
   "Breakpoint hit" messages. Used for internal micro-steps performed while
   stepping over a breakpoint so the user only sees the meaningful stop. */
static int quiet_wait = 0;

/* When set, the current stop came from PTRACE_SINGLESTEP rather than an INT3.
   Single-step traps report the exact RIP, so ptrace_wait must NOT apply the
   "rewind RIP past the 0xCC" correction — doing so would wrongly snap RIP
   back onto a breakpoint we are trying to step past. */
static int single_stepping = 0;

/* Set by ptrace_wait when the stop should be silently resumed: a conditional
   breakpoint whose condition was false, or a hardware write-watchpoint that
   wrote the same value (no actual change). ptrace_continue consumes it and
   loops so the user only ever sees meaningful stops. */
static int g_autocontinue = 0;

/* ------------------------------------------------------------------ */
/* x86-64 hardware debug registers (DR0-DR3 addr, DR6 status, DR7 ctl) */
/* ------------------------------------------------------------------ */

static long dr_offset(int n) {
    /* offset of u_debugreg[n] within struct user, for PTRACE_PEEK/POKEUSER */
    return (long)offsetof(struct user, u_debugreg[0]) + (long)n * (long)sizeof(long);
}

static int dr_set(pid_t pid, int n, unsigned long val) {
    return ptrace(PTRACE_POKEUSER, pid, dr_offset(n), (void *)val);
}

static unsigned long dr_get(pid_t pid, int n) {
    errno = 0;
    return (unsigned long)ptrace(PTRACE_PEEKUSER, pid, dr_offset(n), 0);
}

/* Encode DR7 length field for a watch size. */
static unsigned long dr7_len_bits(int len) {
    switch (len) {
    case 1: return 0x0; /* 00 = 1 byte  */
    case 2: return 0x1; /* 01 = 2 bytes */
    case 8: return 0x2; /* 10 = 8 bytes */
    default: return 0x3; /* 11 = 4 bytes */
    }
}

/* Rebuild DR7 from the current watchpoint table and push it to the tracee. */
static void wp_rebuild_dr7(debugger_t *dbg) {
    unsigned long dr7 = 0;
    for (size_t i = 0; i < dbg->watch_count; i++) {
        watchpoint_t *w = &dbg->watchpoints[i];
        if (!w->enabled || w->slot < 0) continue;
        int n = w->slot;
        dr7 |= (1UL << (2 * n));                       /* Ln: local enable   */
        dr7 |= (0x1UL << (16 + 4 * n));                /* R/Wn = 01: on write */
        dr7 |= (dr7_len_bits(w->len) << (18 + 4 * n)); /* LENn                */
    }
    if (dr7) dr7 |= (1UL << 8) | (1UL << 9);           /* LE|GE exact match   */
    dr_set(dbg->proc.pid, 7, dr7);
}

/* Read the current value at a watch location, zero-extended to 64 bits. */
static uint64_t wp_read(debugger_t *dbg, watchpoint_t *w) {
    uint64_t v = 0;
    if (ptrace_read_mem(dbg->proc.pid, w->addr, &v, w->len) < 0) return w->last_val;
    if (w->len < 8) v &= (w->len == 4) ? 0xffffffffULL
                       : (w->len == 2) ? 0xffffULL
                       : (w->len == 1) ? 0xffULL : ~0ULL;
    return v;
}

/* Inspect DR6 after a SIGTRAP: returns 0 if a watchpoint accounted for the
   stop (reporting any change, or requesting auto-continue on a same-value
   write), or -1 if no data watchpoint fired. */
static int wp_check(debugger_t *dbg) {
    if (dbg->watch_count == 0) return -1;
    unsigned long dr6 = dr_get(dbg->proc.pid, 6);
    if ((dr6 & 0xf) == 0) return -1;   /* no DR0-DR3 data breakpoint bit set */

    int any_fire = 0, any_change = 0;
    for (size_t i = 0; i < dbg->watch_count; i++) {
        watchpoint_t *w = &dbg->watchpoints[i];
        if (w->slot < 0 || !(dr6 & (1UL << w->slot))) continue;
        any_fire = 1;
        uint64_t nv = wp_read(dbg, w);
        if (nv != w->last_val) {
            any_change = 1;
            if (!quiet_wait) {
                info("Watchpoint %u: *0x%lx changed 0x%lx -> 0x%lx",
                     w->id, w->addr, w->last_val, nv);
                source_annotate(dbg, dbg->proc.ip);
            }
        }
        w->last_val = nv;
    }
    dr_set(dbg->proc.pid, 6, 0);   /* clear status so it re-arms cleanly */

    if (!any_fire) return -1;
    if (!any_change) g_autocontinue = 1;   /* same-value write: keep going */
    return 0;
}

int watch_add(debugger_t *dbg, uint64_t addr, int len) {
    if (dbg->proc.pid <= 0) {
        warn("No running process to watch");
        return -1;
    }
    /* Normalize length to 1/2/4/8 and respect alignment (hardware requires
       the address to be aligned to the watch size). */
    if (len != 1 && len != 2 && len != 4 && len != 8) len = 4;
    while (len > 1 && (addr % (uint64_t)len) != 0) len /= 2;

    /* Find a free DR slot (0-3). */
    int used[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < dbg->watch_count; i++) {
        if (dbg->watchpoints[i].slot >= 0 && dbg->watchpoints[i].slot < 4)
            used[dbg->watchpoints[i].slot] = 1;
    }
    int slot = -1;
    for (int s = 0; s < 4; s++) {
        if (!used[s]) { slot = s; break; }
    }
    if (slot < 0 || dbg->watch_count >= MAX_WATCHPOINTS) {
        warn("All %d hardware watchpoints in use", MAX_WATCHPOINTS);
        return -1;
    }

    watchpoint_t *w = &dbg->watchpoints[dbg->watch_count];
    w->id = ++dbg->next_watch_id;
    w->addr = addr;
    w->len = len;
    w->slot = slot;
    w->enabled = 1;
    dbg->watch_count++;
    w->last_val = wp_read(dbg, w);

    if (dr_set(dbg->proc.pid, slot, addr) < 0) {
        warn("Could not program debug register DR%d", slot);
        dbg->watch_count--;
        return -1;
    }
    wp_rebuild_dr7(dbg);

    info("Watchpoint %u set on *0x%lx (%d bytes, DR%d), current value 0x%lx",
         w->id, addr, len, slot, w->last_val);
    return (int)w->id;
}

int watch_delete(debugger_t *dbg, uint32_t id) {
    for (size_t i = 0; i < dbg->watch_count; i++) {
        if (dbg->watchpoints[i].id != id) continue;
        int slot = dbg->watchpoints[i].slot;
        /* Remove from table, then rebuild control register. */
        memmove(&dbg->watchpoints[i], &dbg->watchpoints[i + 1],
                (dbg->watch_count - i - 1) * sizeof(watchpoint_t));
        dbg->watch_count--;
        if (dbg->proc.pid > 0) {
            if (slot >= 0) dr_set(dbg->proc.pid, slot, 0);
            wp_rebuild_dr7(dbg);
        }
        info("Watchpoint %u deleted", id);
        return 0;
    }
    warn("Watchpoint %u not found", id);
    return -1;
}

void watch_list(debugger_t *dbg) {
    if (dbg->watch_count == 0) {
        info("No watchpoints set");
        return;
    }
    printf("ID   Address          Len  DR   Last value\n");
    printf("--   -------          ---  --   ----------\n");
    for (size_t i = 0; i < dbg->watch_count; i++) {
        watchpoint_t *w = &dbg->watchpoints[i];
        printf("%-4u 0x%014lx   %-3d  DR%d  0x%lx\n",
               w->id, w->addr, w->len, w->slot, w->last_val);
    }
}

/* Single-step the instruction sitting under a breakpoint.
   Precondition: RIP == bp->addr and the 0xCC is currently installed.
   Restores the original byte, single-steps one instruction, then re-arms
   the 0xCC (unless the process exited or the breakpoint was disabled). */
static int step_over_breakpoint(debugger_t *dbg, breakpoint_t *bp) {
    /* Rewind: restore the original opcode byte */
    if (ptrace_write_mem(dbg->proc.pid, bp->addr, &bp->original_byte, 1) < 0) {
        return -1;
    }

    /* Execute exactly one (now-original) instruction */
    quiet_wait = 1;
    single_stepping = 1;   /* tell ptrace_wait not to apply the INT3 rewind */
    if (ptrace(PTRACE_SINGLESTEP, dbg->proc.pid, 0, 0) < 0) {
        perror("ptrace(SINGLESTEP)");
        quiet_wait = 0;
        single_stepping = 0;
        return -1;
    }
    dbg->proc.state = PROC_RUNNING;
    int rc = ptrace_wait(dbg);
    quiet_wait = 0;
    single_stepping = 0;
    if (rc < 0) return -1;

    /* If the target exited/crashed while stepping, nothing to re-arm */
    if (dbg->proc.state != PROC_STOPPED) {
        return 0;
    }

    /* Re-insert the breakpoint so it fires again next time */
    if (bp->enabled) {
        uint8_t int3 = 0xcc;
        if (ptrace_write_mem(dbg->proc.pid, bp->addr, &int3, 1) < 0) {
            return -1;
        }
    }
    return 0;
}

int ptrace_continue(debugger_t *dbg) {
    if (dbg->proc.pid <= 0) return -1;

    /* Loop so that conditional breakpoints whose condition is false, and
       hardware watchpoints that observed no real change, resume transparently
       instead of returning control to the user. */
    for (;;) {
        /* If we are parked on an active breakpoint, step over it first so the
           original instruction executes, then re-arm before running free. */
        breakpoint_t *bp = breakpoint_find_by_addr(dbg, dbg->proc.ip);
        if (bp && bp->enabled) {
            if (step_over_breakpoint(dbg, bp) < 0) return -1;
            if (dbg->proc.state != PROC_STOPPED) return 0;
        }

        g_autocontinue = 0;
        if (ptrace(PTRACE_CONT, dbg->proc.pid, 0, 0) < 0) {
            perror("ptrace(CONT)");
            return -1;
        }

        dbg->proc.state = PROC_RUNNING;
        if (ptrace_wait(dbg) < 0) return -1;
        if (dbg->proc.state != PROC_STOPPED) return 0;

        if (g_autocontinue) {
            g_autocontinue = 0;
            continue;   /* condition unmet / no change: keep running */
        }
        return 0;
    }
}

int ptrace_step(debugger_t *dbg) {
    if (dbg->proc.pid <= 0) return -1;

    /* Stepping while parked on a breakpoint requires the restore/step/re-arm
       cycle, otherwise we would single-step the 0xCC and re-trap forever. */
    breakpoint_t *bp = breakpoint_find_by_addr(dbg, dbg->proc.ip);
    if (bp && bp->enabled) {
        int rc = step_over_breakpoint(dbg, bp);
        if (rc == 0 && dbg->proc.state == PROC_STOPPED) {
            info("Stepped to 0x%lx", dbg->proc.ip);
        }
        return rc;
    }

    single_stepping = 1;
    if (ptrace(PTRACE_SINGLESTEP, dbg->proc.pid, 0, 0) < 0) {
        perror("ptrace(SINGLESTEP)");
        single_stepping = 0;
        return -1;
    }

    dbg->proc.state = PROC_RUNNING;
    int rc = ptrace_wait(dbg);
    single_stepping = 0;
    return rc;
}

/* Step over: like step, but if the current instruction is a CALL, run the
   whole callee by planting a temporary breakpoint at the return address. */
int ptrace_next(debugger_t *dbg) {
    if (dbg->proc.pid <= 0) return -1;

    uint64_t rip = dbg->proc.ip;
    uint8_t buf[16];
    if (ptrace_read_mem(dbg->proc.pid, rip, buf, sizeof(buf)) < 0) {
        return ptrace_step(dbg);
    }

    /* If a breakpoint is installed here, byte[0] is 0xCC; use the original. */
    breakpoint_t *here = breakpoint_find_by_addr(dbg, rip);
    if (here && here->enabled) {
        buf[0] = here->original_byte;
    }

    /* Skip legacy/REX prefixes to reach the primary opcode */
    int off = 0;
    while (off < 14 &&
           (buf[off] == 0x66 || buf[off] == 0x67 || buf[off] == 0xf0 ||
            buf[off] == 0xf2 || buf[off] == 0xf3 || buf[off] == 0x2e ||
            buf[off] == 0x36 || buf[off] == 0x3e || buf[off] == 0x26 ||
            buf[off] == 0x64 || buf[off] == 0x65 ||
            (buf[off] >= 0x40 && buf[off] <= 0x4f))) {
        off++;
    }

    uint8_t op = buf[off];
    int is_call = 0;
    if (op == 0xe8 || op == 0x9a) {
        is_call = 1;                      /* call rel32 / far call */
    } else if (op == 0xff) {
        uint8_t reg = (buf[off + 1] >> 3) & 7;
        if (reg == 2 || reg == 3) is_call = 1; /* call r/m (near/far) */
    }

    if (!is_call) {
        return ptrace_step(dbg);
    }

    /* Single-step *into* the call so the return address is pushed, then read
       it off the top of the stack. ptrace_step handles the breakpoint dance
       if one is parked on the call itself. */
    if (ptrace_step(dbg) < 0) return -1;
    if (dbg->proc.state != PROC_STOPPED) return 0;

    uint64_t rsp = ptrace_get_reg(dbg->proc.pid, 7);
    uint64_t ret_addr = 0;
    if (ptrace_read_mem(dbg->proc.pid, rsp, &ret_addr, 8) < 0) {
        return 0; /* fall back: leave the debugger inside the callee */
    }

    /* Plant a temporary breakpoint at the return address (unless a user
       breakpoint already lives there). */
    breakpoint_t *existing = breakpoint_find_by_addr(dbg, ret_addr);
    uint8_t orig = 0;
    int temp = 0;
    if (!existing) {
        if (ptrace_read_mem(dbg->proc.pid, ret_addr, &orig, 1) == 1) {
            uint8_t int3 = 0xcc;
            if (ptrace_write_mem(dbg->proc.pid, ret_addr, &int3, 1) == 1) {
                temp = 1;
            }
        }
    }

    /* Run until we come back (or hit a user breakpoint / signal / exit). */
    if (ptrace(PTRACE_CONT, dbg->proc.pid, 0, 0) < 0) {
        perror("ptrace(CONT)");
        if (temp) ptrace_write_mem(dbg->proc.pid, ret_addr, &orig, 1);
        return -1;
    }
    quiet_wait = 1;
    ptrace_wait(dbg);
    quiet_wait = 0;

    if (dbg->proc.state != PROC_STOPPED) {
        return 0; /* exited/crashed inside the callee */
    }

    if (temp && dbg->proc.ip == ret_addr + 1) {
        /* Our temporary breakpoint fired: rewind, restore, report. */
        ptrace_write_mem(dbg->proc.pid, ret_addr, &orig, 1);
        ptrace_set_reg(dbg->proc.pid, 8, ret_addr);
        dbg->proc.ip = ret_addr;
        info("Stepped over call, stopped at 0x%lx", ret_addr);
    } else {
        /* Something else stopped us first (user breakpoint / signal). Clean
           up the temporary breakpoint; ptrace_wait already reported it. */
        if (temp) ptrace_write_mem(dbg->proc.pid, ret_addr, &orig, 1);
        info("Stopped at 0x%lx", dbg->proc.ip);
    }
    return 0;
}

/* finish: run until the current function returns. Plants a temporary
   breakpoint at the return address ([rbp+8], standard frame) and reports the
   return value in RAX. Note: for a directly recursive function this stops at
   the first return to that address rather than unwinding the exact frame. */
int ptrace_finish(debugger_t *dbg) {
    if (dbg->proc.pid <= 0) return -1;

    /* Determine this frame's return address. If we are parked on the very
       first instruction of a function (typical: `break foo; finish`) the
       prologue has not run yet, so RBP still belongs to the caller and the
       return address sits at the top of the stack ([rsp]). Otherwise the
       standard frame is set up and the return address is at [rbp+8]. */
    uint64_t ret_addr = 0;
    symbol_t *fn = elf_find_by_addr(&dbg->elf, dbg->proc.ip);
    if (fn && dbg->proc.ip == fn->addr) {
        uint64_t rsp = ptrace_get_reg(dbg->proc.pid, 7);
        if (ptrace_read_mem(dbg->proc.pid, rsp, &ret_addr, 8) < 0) ret_addr = 0;
    } else {
        uint64_t rbp = ptrace_get_reg(dbg->proc.pid, 6);
        if (rbp && ptrace_read_mem(dbg->proc.pid, rbp + 8, &ret_addr, 8) < 0)
            ret_addr = 0;
    }
    if (ret_addr == 0) {
        warn("finish: could not determine return address for this frame");
        return -1;
    }

    /* Plant a temporary breakpoint at the return address (unless a user
       breakpoint already lives there). */
    breakpoint_t *existing = breakpoint_find_by_addr(dbg, ret_addr);
    uint8_t orig = 0;
    int temp = 0;
    if (!existing) {
        if (ptrace_read_mem(dbg->proc.pid, ret_addr, &orig, 1) == 1) {
            uint8_t int3 = 0xcc;
            if (ptrace_write_mem(dbg->proc.pid, ret_addr, &int3, 1) == 1) temp = 1;
        }
    }

    /* Run until we return, transparently resuming past conditional
       breakpoints and no-change watchpoints (same loop discipline as
       ptrace_continue). */
    for (;;) {
        breakpoint_t *cur = breakpoint_find_by_addr(dbg, dbg->proc.ip);
        if (cur && cur->enabled) {
            if (step_over_breakpoint(dbg, cur) < 0) {
                if (temp) ptrace_write_mem(dbg->proc.pid, ret_addr, &orig, 1);
                return -1;
            }
            if (dbg->proc.state != PROC_STOPPED) return 0;
        }

        g_autocontinue = 0;
        if (ptrace(PTRACE_CONT, dbg->proc.pid, 0, 0) < 0) {
            perror("ptrace(CONT)");
            if (temp) ptrace_write_mem(dbg->proc.pid, ret_addr, &orig, 1);
            return -1;
        }
        quiet_wait = 1;
        ptrace_wait(dbg);
        quiet_wait = 0;

        if (dbg->proc.state != PROC_STOPPED) return 0;
        if (g_autocontinue) { g_autocontinue = 0; continue; }
        break;
    }

    if (temp && dbg->proc.ip == ret_addr + 1) {
        ptrace_write_mem(dbg->proc.pid, ret_addr, &orig, 1);
        ptrace_set_reg(dbg->proc.pid, 8, ret_addr);
        dbg->proc.ip = ret_addr;
        uint64_t rv = ptrace_get_reg(dbg->proc.pid, 0);
        info("Ran until return -> 0x%lx (rax = 0x%lx / %ld)",
             ret_addr, rv, (long)rv);
        source_annotate(dbg, ret_addr);
    } else {
        if (temp) ptrace_write_mem(dbg->proc.pid, ret_addr, &orig, 1);
        info("Stopped at 0x%lx", dbg->proc.ip);
        source_annotate(dbg, dbg->proc.ip);
    }
    return 0;
}

int ptrace_wait(debugger_t *dbg) {
    if (dbg->proc.pid <= 0) return -1;

    int status;
    pid_t ret = waitpid(dbg->proc.pid, &status, 0);

    if (ret < 0) {
        perror("waitpid");
        return -1;
    }

    dbg->proc.state = PROC_STOPPED;

    if (WIFEXITED(status)) {
        dbg->proc.exit_code = WEXITSTATUS(status);
        dbg->proc.state = PROC_EXITED;
        info("Process exited with code %d", dbg->proc.exit_code);
        return 0;
    }

    if (WIFSIGNALED(status)) {
        dbg->proc.signal = WTERMSIG(status);
        dbg->proc.state = PROC_CRASHED;
        info("Process terminated by signal %d", dbg->proc.signal);
        return 0;
    }

    if (WIFSTOPPED(status)) {
        dbg->proc.signal = WSTOPSIG(status);

        /* Get current instruction pointer */
        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, dbg->proc.pid, 0, &regs) == 0) {
            dbg->proc.ip = regs_get_ip(&regs);
        }

        /* A hardware watchpoint reports via SIGTRAP with a DR6 status bit.
           Handle that first — its RIP is *after* the accessing instruction,
           so the INT3 rewind logic below must not run for it. */
        if (dbg->proc.signal == SIGTRAP && wp_check(dbg) == 0) {
            return 0;
        }

        if (dbg->proc.signal == SIGTRAP && !single_stepping) {
            /* After an INT3 trap (from PTRACE_CONT) the CPU has already
               advanced RIP past the 0xCC. If that lands one byte after a known
               breakpoint, rewind RIP to the breakpoint address so the target
               can be resumed correctly (restore/step/re-arm in continue/step).
               This must NOT run for single-step traps: those already report the
               exact RIP, and rewinding would snap us back onto the very
               breakpoint we are stepping past — an infinite re-trap. */
            breakpoint_t *bp = breakpoint_find_by_addr(dbg, dbg->proc.ip - 1);
            if (bp && bp->enabled) {
                dbg->proc.ip -= 1;
                ptrace_set_reg(dbg->proc.pid, 8, dbg->proc.ip);
                bp->hit_count++;
                if (bp->cond_op != COND_NONE && !breakpoint_cond_met(dbg, bp)) {
                    /* Condition false: request a transparent auto-continue. */
                    g_autocontinue = 1;
                } else if (!quiet_wait) {
                    info("Breakpoint %u hit at 0x%lx", bp->id, dbg->proc.ip);
                    source_annotate(dbg, dbg->proc.ip);
                }
            } else if (!quiet_wait) {
                info("Stopped at 0x%lx", dbg->proc.ip);
                source_annotate(dbg, dbg->proc.ip);
            }
        } else if (dbg->proc.signal == SIGTRAP) {
            /* Single-step trap: RIP is already correct — no rewind. */
            if (!quiet_wait) {
                info("Stopped at 0x%lx", dbg->proc.ip);
                source_annotate(dbg, dbg->proc.ip);
            }
        } else if (!quiet_wait) {
            info("Stopped by signal %d at 0x%lx", dbg->proc.signal,
                 dbg->proc.ip);
        }

        return 0;
    }

    return -1;
}

int ptrace_detach(debugger_t *dbg) {
    if (dbg->proc.pid <= 0) return -1;

    if (ptrace(PTRACE_DETACH, dbg->proc.pid, 0, 0) < 0) {
        perror("ptrace(DETACH)");
        return -1;
    }

    dbg->proc.pid = -1;
    dbg->proc.state = PROC_EXITED;

    return 0;
}

uint64_t ptrace_get_reg(pid_t pid, int reg) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) < 0) {
        perror("ptrace(GETREGS)");
        return 0;
    }

    /* Map register indices to actual fields */
    switch (reg) {
    case 0:
        return regs.rax;
    case 1:
        return regs.rbx;
    case 2:
        return regs.rcx;
    case 3:
        return regs.rdx;
    case 4:
        return regs.rsi;
    case 5:
        return regs.rdi;
    case 6:
        return regs.rbp;
    case 7:
        return regs.rsp;
    case 8:
        return regs.rip;
    case 9:
        return regs.r8;
    case 10:
        return regs.r9;
    case 11:
        return regs.r10;
    case 12:
        return regs.r11;
    case 13:
        return regs.r12;
    case 14:
        return regs.r13;
    case 15:
        return regs.r14;
    case 16:
        return regs.r15;
    case 17:
        return regs.eflags;
    default:
        return 0;
    }
}

int ptrace_set_reg(pid_t pid, int reg, uint64_t val) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) < 0) {
        perror("ptrace(GETREGS)");
        return -1;
    }

    switch (reg) {
    case 0:
        regs.rax = val;
        break;
    case 1:
        regs.rbx = val;
        break;
    case 2:
        regs.rcx = val;
        break;
    case 3:
        regs.rdx = val;
        break;
    case 4:
        regs.rsi = val;
        break;
    case 5:
        regs.rdi = val;
        break;
    case 6:
        regs.rbp = val;
        break;
    case 7:
        regs.rsp = val;
        break;
    case 8:
        regs.rip = val;
        break;
    case 9:
        regs.r8 = val;
        break;
    case 10:
        regs.r9 = val;
        break;
    case 11:
        regs.r10 = val;
        break;
    case 12:
        regs.r11 = val;
        break;
    case 13:
        regs.r12 = val;
        break;
    case 14:
        regs.r13 = val;
        break;
    case 15:
        regs.r14 = val;
        break;
    case 16:
        regs.r15 = val;
        break;
    case 17:
        regs.eflags = val;
        break;
    default:
        return -1;
    }

    if (ptrace(PTRACE_SETREGS, pid, 0, &regs) < 0) {
        perror("ptrace(SETREGS)");
        return -1;
    }

    return 0;
}

int ptrace_read_mem(pid_t pid, uint64_t addr, void *buf, size_t len) {
    size_t read = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (read < len) {
        size_t chunk = (len - read > 8) ? 8 : (len - read);
        errno = 0;

        long word = ptrace(PTRACE_PEEKDATA, pid, addr + read, 0);
        if (word < 0 && errno != 0) {
            perror("ptrace(PEEKDATA)");
            return -1;
        }

        memcpy(dst + read, &word, chunk);
        read += chunk;
    }

    return len;
}

int ptrace_write_mem(pid_t pid, uint64_t addr, const void *buf, size_t len) {
    size_t written = 0;
    const uint8_t *src = (const uint8_t *)buf;

    while (written < len) {
        uint64_t waddr = addr + written;
        size_t chunk = (len - written >= 8) ? 8 : (len - written);

        long word;
        if (chunk < 8) {
            /* Partial word: read-modify-write so we don't clobber the
               surrounding bytes. (A naive full-word POKE of a 1-byte
               breakpoint would zero the next 7 bytes of code — corrupting
               instructions and wedging the target.) */
            errno = 0;
            word = ptrace(PTRACE_PEEKDATA, pid, waddr, 0);
            if (word == -1 && errno != 0) {
                perror("ptrace(PEEKDATA)");
                return -1;
            }
        } else {
            word = 0;
        }
        memcpy(&word, src + written, chunk);   /* overlay only the bytes we write */

        if (ptrace(PTRACE_POKEDATA, pid, waddr, word) < 0) {
            perror("ptrace(POKEDATA)");
            return -1;
        }

        written += chunk;
    }

    return len;
}
