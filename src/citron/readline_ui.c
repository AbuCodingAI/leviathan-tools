#include "citron.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/ptrace.h>
#include <sys/user.h>

/* Simple command parsing without readline (for minimal size) */

static char *read_line(void) {
    char *line = malloc(256);
    if (!line) return NULL;

    if (!fgets(line, 256, stdin)) {
        free(line);
        return NULL;
    }

    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
        line[len - 1] = '\0';
    }

    return line;
}

static int parse_command(char *line, char *cmd, size_t cmd_size,
                         char **args) {
    while (*line && isspace(*line)) line++;

    if (!*line) return 0;

    size_t i = 0;
    while (i < cmd_size - 1 && *line && !isspace(*line)) {
        cmd[i++] = *line++;
    }
    cmd[i] = '\0';

    while (*line && isspace(*line)) line++;
    *args = line;

    return i > 0;
}

static void cmd_help(debugger_t *dbg, const char *args) {
    (void)dbg;
    (void)args;

    printf("Citron Commands:\n");
    printf("  run [args...]              Start program\n");
    printf("  break <addr|func> [if C]   Breakpoint; optional cond e.g. 'if rax==0'\n");
    printf("  watch <addr|sym> [len]     Hardware watchpoint: stop when memory changes\n");
    printf("  watch delete <id>          Remove a watchpoint (also: watch list)\n");
    printf("  delete <id>                Delete breakpoint\n");
    printf("  continue / c               Resume execution\n");
    printf("  step / s                   Step into\n");
    printf("  next / n                   Step over\n");
    printf("  finish / fin               Run until current function returns\n");
    printf("  list / l [loc]             Show source around current line/location\n");
    printf("  disas [start] [stop]       Disassemble range (defaults around RIP)\n");
    printf("  backtrace / bt             Show call stack\n");
    printf("  registers / r              Show CPU registers\n");
    printf("  print[/x|/d|/s|/c] <op>    Print $reg, symbol, or memory at address\n");
    printf("  x[/Nf] <addr>              Examine N units (f: x b d c s)\n");
    printf("  set $reg = val             Write a register\n");
    printf("  set *<addr> = val          Write memory\n");
    printf("  info <what>                breakpoints|watchpoints|registers|line|frame|locals\n");
    printf("  dev <file.dev>             Inspect a DevVM bytecode file\n");
    printf("  attach <pid> / detach      Attach to / detach from a process\n");
    printf("  quit / q                   Exit debugger\n");
    printf("  help                       Show this help\n");
}

/* Copy the first whitespace-delimited token of `args` into `out`. */
static void first_token(const char *args, char *out, size_t out_size) {
    size_t i = 0;
    while (*args && isspace((unsigned char)*args)) args++;
    while (i + 1 < out_size && *args && !isspace((unsigned char)*args)) {
        out[i++] = *args++;
    }
    out[i] = '\0';
}

/* Resolve a location token (symbol name or hex address) to an address. */
static int resolve_location(debugger_t *dbg, const char *tok, uint64_t *out) {
    symbol_t *sym = elf_find_symbol(&dbg->elf, tok);
    if (sym) {
        *out = sym->addr;
        return 0;
    }
    char *end;
    uint64_t addr = strtoull(tok, &end, 16);
    if (end == tok) return -1;
    *out = addr;
    return 0;
}

/* Operand kinds for print/set/examine. */
enum { OPND_VALUE, OPND_ADDR };

/* Evaluate a print/set operand: "$reg", a symbol, or a numeric literal.
   Registers evaluate to their value (OPND_VALUE); symbols and numbers
   evaluate to an address (OPND_ADDR). Returns 0 on success. */
static int eval_operand(debugger_t *dbg, const char *s, uint64_t *out, int *kind) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '$') {
        int r = reg_index(s);
        if (r < 0) { warn("Unknown register: %s", s); return -1; }
        *out = ptrace_get_reg(dbg->proc.pid, r);
        if (kind) *kind = OPND_VALUE;
        return 0;
    }
    symbol_t *sym = elf_find_symbol(&dbg->elf, s);
    if (sym) {
        *out = sym->addr;
        if (kind) *kind = OPND_ADDR;
        return 0;
    }
    char *end;
    uint64_t v = strtoull(s, &end, 0);
    if (end == s) { warn("Cannot evaluate: %s", s); return -1; }
    *out = v;
    if (kind) *kind = OPND_ADDR;
    return 0;
}

/* Parse a condition expression like "rax==0" or "rdi != 0x10" into a
   breakpoint's cond fields. Returns 0 on success. */
static int parse_condition(const char *expr, breakpoint_t *bp) {
    while (*expr && isspace((unsigned char)*expr)) expr++;

    /* register name */
    char reg[32];
    size_t i = 0;
    while (*expr && (isalnum((unsigned char)*expr) || *expr == '$') &&
           i < sizeof(reg) - 1) {
        reg[i++] = *expr++;
    }
    reg[i] = '\0';
    int ridx = reg_index(reg);
    if (ridx < 0) {
        warn("Condition: unknown register '%s'", reg);
        return -1;
    }

    while (*expr && isspace((unsigned char)*expr)) expr++;

    cond_op_t op;
    if      (strncmp(expr, "==", 2) == 0) { op = COND_EQ; expr += 2; }
    else if (strncmp(expr, "!=", 2) == 0) { op = COND_NE; expr += 2; }
    else if (strncmp(expr, "<=", 2) == 0) { op = COND_LE; expr += 2; }
    else if (strncmp(expr, ">=", 2) == 0) { op = COND_GE; expr += 2; }
    else if (*expr == '<')                { op = COND_LT; expr += 1; }
    else if (*expr == '>')                { op = COND_GT; expr += 1; }
    else {
        warn("Condition: expected operator (==, !=, <, >, <=, >=)");
        return -1;
    }

    while (*expr && isspace((unsigned char)*expr)) expr++;
    char *end;
    uint64_t val = strtoull(expr, &end, 0);
    if (end == expr) {
        warn("Condition: expected a numeric value");
        return -1;
    }

    bp->cond_reg = ridx;
    bp->cond_op = op;
    bp->cond_val = val;
    return 0;
}

static void cmd_break(debugger_t *dbg, const char *args) {
    if (!*args) {
        warn("Usage: break <address|function> [if <reg><op><value>]");
        return;
    }

    char tok[256];
    first_token(args, tok, sizeof(tok));

    uint64_t addr;
    if (resolve_location(dbg, tok, &addr) < 0) {
        warn("Unknown symbol or invalid address: %s", tok);
        return;
    }
    if (elf_find_symbol(&dbg->elf, tok)) {
        info("Resolved '%s' to 0x%lx", tok, addr);
    }

    /* Optional "if <condition>" clause. */
    const char *cond = strstr(args, " if ");
    breakpoint_t parsed = {0};
    parsed.cond_op = COND_NONE;
    if (cond) {
        cond += 4;
        if (parse_condition(cond, &parsed) < 0) return;
    }

    int id = breakpoint_add(dbg, addr);
    if (id > 0 && parsed.cond_op != COND_NONE) {
        breakpoint_t *bp = breakpoint_find_by_id(dbg, (uint32_t)id);
        if (bp) {
            bp->cond_op = parsed.cond_op;
            bp->cond_reg = parsed.cond_reg;
            bp->cond_val = parsed.cond_val;
            snprintf(bp->cond_str, sizeof(bp->cond_str), "%s %s 0x%lx",
                     reg_name(parsed.cond_reg),
                     parsed.cond_op == COND_EQ ? "==" :
                     parsed.cond_op == COND_NE ? "!=" :
                     parsed.cond_op == COND_LT ? "<"  :
                     parsed.cond_op == COND_GT ? ">"  :
                     parsed.cond_op == COND_LE ? "<=" : ">=",
                     parsed.cond_val);
            info("Breakpoint %d is conditional: %s", id, bp->cond_str);
        }
    }
}

static void cmd_delete(debugger_t *dbg, const char *args) {
    if (!*args) {
        warn("Usage: delete <id>");
        return;
    }

    uint32_t id = atoi(args);
    breakpoint_delete(dbg, id);
}

static void cmd_continue(debugger_t *dbg, const char *args) {
    (void)args;
    if (ptrace_continue(dbg) < 0) {
        warn("Failed to continue");
    }
}

static void cmd_step(debugger_t *dbg, const char *args) {
    (void)args;
    if (ptrace_step(dbg) < 0) {
        warn("Failed to step");
    }
}

static void cmd_next(debugger_t *dbg, const char *args) {
    (void)args;
    if (ptrace_next(dbg) < 0) {
        warn("Failed to step over");
    }
}

static void cmd_backtrace(debugger_t *dbg, const char *args) {
    (void)args;
    if (stack_unwind(dbg) < 0) {
        warn("Failed to unwind stack");
    }
}

static void cmd_disas(debugger_t *dbg, const char *args) {
    uint64_t start = 0, stop = 0;

    if (*args) {
        char *end;
        start = strtoull(args, &end, 16);
        while (*end && isspace((unsigned char)*end)) end++;
        if (*end) {
            stop = strtoull(end, NULL, 16);
        } else {
            stop = start + 64;
        }
    } else if (dbg->proc.pid > 0) {
        /* Default: a window around the current instruction pointer */
        start = dbg->proc.ip ? dbg->proc.ip
                             : ptrace_get_reg(dbg->proc.pid, 8);
        stop = start + 64;
    } else {
        warn("Usage: disas <start-hex> [stop-hex]");
        return;
    }

    disasm_range(dbg->elf.path, start, stop);
}

static void cmd_dev(debugger_t *dbg, const char *args) {
    (void)dbg;
    if (!*args) {
        warn("Usage: dev <file.dev>");
        return;
    }
    char path[512];
    first_token(args, path, sizeof(path));
    devvm_inspect(path);
}

static void cmd_registers(debugger_t *dbg, const char *args) {
    (void)args;

    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, dbg->proc.pid, 0, &regs) < 0) {
        warn("Failed to get registers");
        return;
    }

    regs_print(&regs);
}

/* Read a NUL-terminated C string from the tracee at addr into buf. */
static void read_cstring(debugger_t *dbg, uint64_t addr, char *buf, size_t max) {
    size_t i = 0;
    while (i + 1 < max) {
        uint8_t c = 0;
        if (ptrace_read_mem(dbg->proc.pid, addr + i, &c, 1) < 0) break;
        if (c == 0) break;
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
}

/* print[/fmt] <operand>
   operand: $reg | symbol | hex-address
   fmt: x hex, d signed-dec, u unsigned-dec, c char, s C-string, a addr+symbol */
static void cmd_print(debugger_t *dbg, char fmt, const char *args) {
    if (!*args) {
        warn("Usage: print[/x|/d|/s|/c] <$reg | symbol | address>");
        return;
    }
    char tok[256];
    first_token(args, tok, sizeof(tok));

    uint64_t val;
    int kind;
    if (eval_operand(dbg, tok, &val, &kind) < 0) return;

    if (fmt == 's') {
        char s[256];
        read_cstring(dbg, val, s, sizeof(s));
        printf("0x%lx: \"%s\"\n", val, s);
        return;
    }

    if (kind == OPND_VALUE) {
        /* A register: show its value directly. */
        switch (fmt) {
        case 'd': printf("%s = %ld\n", tok, (long)val); break;
        case 'u': printf("%s = %lu\n", tok, (unsigned long)val); break;
        case 'c': printf("%s = '%c' (0x%lx)\n", tok,
                         isprint((int)(val & 0xff)) ? (int)(val & 0xff) : '.', val); break;
        default:  printf("%s = 0x%lx (%ld)\n", tok, val, (long)val); break;
        }
        return;
    }

    /* An address (symbol or literal): read 8 bytes there. */
    uint64_t mem = 0;
    if (ptrace_read_mem(dbg->proc.pid, val, &mem, 8) < 0) {
        /* If it was a symbol we can still report its address. */
        printf("0x%lx\n", val);
        return;
    }
    symbol_t *sym = elf_find_symbol(&dbg->elf, tok);
    const char *label = sym ? tok : NULL;
    switch (fmt) {
    case 'd': printf("0x%lx: %ld\n", val, (long)mem); break;
    case 'u': printf("0x%lx: %lu\n", val, (unsigned long)mem); break;
    case 'c': printf("0x%lx: '%c' (0x%lx)\n", val,
                     isprint((int)(mem & 0xff)) ? (int)(mem & 0xff) : '.', mem); break;
    default:
        if (label) printf("%s @ 0x%lx: 0x%016lx\n", label, val, mem);
        else       printf("0x%lx: 0x%016lx\n", val, mem);
        break;
    }
}

/* x[/NF] <addr> — examine N units of memory.
   F: x hex-words(8B), b hex-bytes, d dec-words, c chars, s C-string. */
static void cmd_examine(debugger_t *dbg, const char *spec, const char *args) {
    if (!*args) {
        warn("Usage: x[/Nf] <addr>   (f = x hex, b bytes, d dec, c char, s string)");
        return;
    }
    int count = 1;
    char fmt = 'x';
    if (spec && *spec) {
        char *end;
        long n = strtol(spec, &end, 10);
        if (end != spec && n > 0) count = (int)n;
        if (*end) fmt = *end;
    }

    char tok[256];
    first_token(args, tok, sizeof(tok));
    uint64_t addr, kindval;
    int kind;
    if (eval_operand(dbg, tok, &kindval, &kind) < 0) return;
    addr = kindval;

    if (fmt == 's') {
        for (int i = 0; i < count; i++) {
            char s[256];
            read_cstring(dbg, addr, s, sizeof(s));
            printf("0x%lx: \"%s\"\n", addr, s);
            addr += strlen(s) + 1;
        }
        return;
    }

    if (fmt == 'b' || fmt == 'c') {
        for (int i = 0; i < count; i++) {
            if (i % 16 == 0) printf("0x%lx:", addr + i);
            uint8_t b = 0;
            if (ptrace_read_mem(dbg->proc.pid, addr + i, &b, 1) < 0) { printf(" ??"); }
            else if (fmt == 'c') printf(" %c", isprint(b) ? b : '.');
            else printf(" %02x", b);
            if (i % 16 == 15 || i == count - 1) printf("\n");
        }
        return;
    }

    /* word formats: 8 bytes each */
    for (int i = 0; i < count; i++) {
        uint64_t w = 0;
        if (ptrace_read_mem(dbg->proc.pid, addr + (uint64_t)i * 8, &w, 8) < 0) {
            printf("0x%lx: <unreadable>\n", addr + (uint64_t)i * 8);
            continue;
        }
        if (fmt == 'd') printf("0x%lx: %ld\n", addr + (uint64_t)i * 8, (long)w);
        else            printf("0x%lx: 0x%016lx\n", addr + (uint64_t)i * 8, w);
    }
}

/* set $reg = value | set *addr = value */
static void cmd_set(debugger_t *dbg, const char *args) {
    const char *eq = strchr(args, '=');
    if (!eq) {
        warn("Usage: set $reg = value   |   set *<addr> = value");
        return;
    }

    /* Left operand (trim trailing spaces). */
    char lhs[128];
    size_t n = (size_t)(eq - args);
    if (n >= sizeof(lhs)) n = sizeof(lhs) - 1;
    memcpy(lhs, args, n);
    lhs[n] = '\0';
    while (n > 0 && isspace((unsigned char)lhs[n - 1])) lhs[--n] = '\0';
    char *lp = lhs;
    while (*lp && isspace((unsigned char)*lp)) lp++;

    /* Right operand: a value ($reg, symbol, or number). */
    uint64_t value;
    int vkind;
    if (eval_operand(dbg, eq + 1, &value, &vkind) < 0) return;

    if (*lp == '$') {
        int r = reg_index(lp);
        if (r < 0) { warn("Unknown register: %s", lp); return; }
        if (ptrace_set_reg(dbg->proc.pid, r, value) < 0) {
            warn("Failed to set %s", lp);
            return;
        }
        if (r == 8) dbg->proc.ip = value;   /* keep cached RIP coherent */
        info("%s = 0x%lx", reg_name(r), value);
    } else if (*lp == '*') {
        uint64_t addr, akind;
        int k;
        if (eval_operand(dbg, lp + 1, &addr, &k) < 0) return;
        akind = addr;
        if (ptrace_write_mem(dbg->proc.pid, akind, &value, 8) < 0) {
            warn("Failed to write memory at 0x%lx", akind);
            return;
        }
        info("*0x%lx = 0x%lx", akind, value);
    } else {
        warn("Usage: set $reg = value   |   set *<addr> = value");
    }
}

static void cmd_watch(debugger_t *dbg, const char *args) {
    if (!*args) {
        warn("Usage: watch <addr|symbol> [len]  |  watch delete <id>  |  watch list");
        return;
    }
    char tok[256];
    first_token(args, tok, sizeof(tok));

    if (strcmp(tok, "delete") == 0 || strcmp(tok, "del") == 0) {
        const char *p = args + strlen(tok);
        watch_delete(dbg, (uint32_t)atoi(p));
        return;
    }
    if (strcmp(tok, "list") == 0) {
        watch_list(dbg);
        return;
    }

    uint64_t addr;
    if (resolve_location(dbg, tok, &addr) < 0) {
        warn("Unknown symbol or invalid address: %s", tok);
        return;
    }
    int len = 4;
    const char *rest = args + strlen(tok);
    while (*rest && isspace((unsigned char)*rest)) rest++;
    if (*rest) len = atoi(rest);
    watch_add(dbg, addr, len);
}

static void cmd_finish(debugger_t *dbg, const char *args) {
    (void)args;
    if (ptrace_finish(dbg) < 0) {
        warn("Failed to finish");
    }
}

static void cmd_list(debugger_t *dbg, const char *args) {
    uint64_t addr = 0;
    if (*args) {
        char tok[256];
        first_token(args, tok, sizeof(tok));
        if (resolve_location(dbg, tok, &addr) < 0) {
            warn("Unknown location: %s", tok);
            return;
        }
    } else if (dbg->proc.pid > 0) {
        addr = dbg->proc.ip ? dbg->proc.ip : ptrace_get_reg(dbg->proc.pid, 8);
    } else {
        warn("No location and no running process");
        return;
    }
    source_list(dbg, addr, 5, 5);
}

static void cmd_info(debugger_t *dbg, const char *args) {
    char tok[64];
    first_token(args, tok, sizeof(tok));

    if (strcmp(tok, "breakpoints") == 0 || strcmp(tok, "break") == 0 ||
        strcmp(tok, "b") == 0) {
        breakpoint_list(dbg);
    } else if (strcmp(tok, "watchpoints") == 0 || strcmp(tok, "watch") == 0 ||
               strcmp(tok, "w") == 0) {
        watch_list(dbg);
    } else if (strcmp(tok, "registers") == 0 || strcmp(tok, "reg") == 0 ||
               strcmp(tok, "r") == 0) {
        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, dbg->proc.pid, 0, &regs) < 0)
            warn("Failed to get registers");
        else regs_print(&regs);
    } else if (strcmp(tok, "line") == 0) {
        uint64_t ip = dbg->proc.ip;
        char file[512];
        int line = 0;
        if (source_lookup(dbg, ip, file, sizeof(file), &line))
            printf("0x%lx is at %s:%d\n", ip, file, line);
        else
            warn("No line info for 0x%lx", ip);
    } else if (strcmp(tok, "frame") == 0) {
        uint64_t ip = dbg->proc.ip;
        uint64_t bp = ptrace_get_reg(dbg->proc.pid, 6);
        uint64_t sp = ptrace_get_reg(dbg->proc.pid, 7);
        symbol_t *sym = elf_find_by_addr(&dbg->elf, ip);
        printf("Frame at rbp=0x%lx rsp=0x%lx\n", bp, sp);
        printf("  rip = 0x%lx in %s\n", ip, sym ? sym->name : "???");
        source_annotate(dbg, ip);
    } else if (strcmp(tok, "locals") == 0) {
        /* Full local-variable enumeration needs a .debug_info DIE walker,
           which Citron does not implement. Show the frame + source context as
           a best-effort substitute. */
        warn("info locals: variable-level DWARF (.debug_info) not parsed.");
        info("Showing current frame and source context instead:");
        cmd_info(dbg, "frame");
        source_list(dbg, dbg->proc.ip, 3, 3);
    } else {
        warn("Usage: info <breakpoints|watchpoints|registers|line|frame|locals>");
    }
}

static void cmd_attach(debugger_t *dbg, const char *args) {
    if (!*args) {
        warn("Usage: attach <pid>");
        return;
    }

    pid_t pid = atoi(args);
    if (ptrace_attach(pid, dbg) < 0) {
        warn("Failed to attach to PID %d", pid);
    }
}

static void cmd_detach(debugger_t *dbg, const char *args) {
    (void)args;
    if (ptrace_detach(dbg) < 0) {
        warn("Failed to detach");
    }
}

int cli_repl(debugger_t *dbg) {
    info("Citron Debugger. Type 'help' for commands.");
    info("Free software (GNU GPLv3), ABSOLUTELY NO WARRANTY — type 'show w' / 'show c'.");

    while (!dbg->quit) {
        printf("(citron) ");
        fflush(stdout);

        char *line = read_line();
        if (!line) {
            break;
        }

        char cmd[64];
        char *args;
        if (!parse_command(line, cmd, sizeof(cmd), &args)) {
            free(line);
            continue;
        }

        if (strcmp(cmd, "help") == 0) {
            cmd_help(dbg, args);
        } else if (strcmp(cmd, "break") == 0) {
            cmd_break(dbg, args);
        } else if (strcmp(cmd, "delete") == 0) {
            cmd_delete(dbg, args);
        } else if (strcmp(cmd, "continue") == 0 || strcmp(cmd, "c") == 0) {
            cmd_continue(dbg, args);
        } else if (strcmp(cmd, "step") == 0 || strcmp(cmd, "s") == 0) {
            cmd_step(dbg, args);
        } else if (strcmp(cmd, "next") == 0 || strcmp(cmd, "n") == 0) {
            cmd_next(dbg, args);
        } else if (strcmp(cmd, "finish") == 0 || strcmp(cmd, "fin") == 0) {
            cmd_finish(dbg, args);
        } else if (strcmp(cmd, "disas") == 0 || strcmp(cmd, "disassemble") == 0) {
            cmd_disas(dbg, args);
        } else if (strcmp(cmd, "dev") == 0) {
            cmd_dev(dbg, args);
        } else if (strcmp(cmd, "backtrace") == 0 || strcmp(cmd, "bt") == 0) {
            cmd_backtrace(dbg, args);
        } else if (strcmp(cmd, "registers") == 0 || strcmp(cmd, "r") == 0) {
            cmd_registers(dbg, args);
        } else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "l") == 0) {
            cmd_list(dbg, args);
        } else if (strcmp(cmd, "watch") == 0 || strcmp(cmd, "w") == 0) {
            cmd_watch(dbg, args);
        } else if (strcmp(cmd, "set") == 0) {
            cmd_set(dbg, args);
        } else if (strcmp(cmd, "info") == 0 || strcmp(cmd, "i") == 0) {
            cmd_info(dbg, args);
        } else if (strcmp(cmd, "print") == 0 || strcmp(cmd, "p") == 0 ||
                   strncmp(cmd, "print/", 6) == 0 || strncmp(cmd, "p/", 2) == 0) {
            const char *slash = strchr(cmd, '/');
            cmd_print(dbg, slash ? slash[1] : 0, args);
        } else if (strcmp(cmd, "x") == 0 || strncmp(cmd, "x/", 2) == 0) {
            const char *slash = strchr(cmd, '/');
            cmd_examine(dbg, slash ? slash + 1 : "", args);
        } else if (strcmp(cmd, "attach") == 0) {
            cmd_attach(dbg, args);
        } else if (strcmp(cmd, "detach") == 0) {
            cmd_detach(dbg, args);
        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
            dbg->quit = 1;
        } else if (strcmp(cmd, "show") == 0 && args && strcmp(args, "w") == 0) {
            printf("This program comes with ABSOLUTELY NO WARRANTY, to the extent\n"
                   "permitted by law. See the GNU GPL v3, sections 15-17.\n");
        } else if (strcmp(cmd, "show") == 0 && args && strcmp(args, "c") == 0) {
            printf("Citron is free software under the GNU GPL v3; you may redistribute\n"
                   "and/or modify it under those terms. Full text: /usr/share/doc/\n"
                   "leviathanos/LICENSE or https://www.gnu.org/licenses/gpl-3.0.html\n");
        } else if (strcmp(cmd, "license") == 0) {
            printf("Citron — part of LeviathanOS. GNU GPL v3, no warranty.\n"
                   "See LICENSE or https://www.gnu.org/licenses/gpl-3.0.html\n");
        } else {
            warn("Unknown command: %s", cmd);
        }

        free(line);
    }

    return 0;
}
