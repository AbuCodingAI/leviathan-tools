#include "citron.h"
#include <sys/ptrace.h>
#include <sys/user.h>
#include <stdio.h>

int stack_unwind(debugger_t *dbg) {
    struct user_regs_struct regs;

    if (ptrace(PTRACE_GETREGS, dbg->proc.pid, 0, &regs) < 0) {
        perror("ptrace(GETREGS)");
        return -1;
    }

    uint64_t fp = regs.rbp;
    uint64_t ip = regs.rip;

    printf("Frame  Address          RIP              Function\n");
    printf("-----  -------          ---              --------\n");

    int frame = 0;
    const int max_frames = 16;

    while (frame < max_frames && fp != 0) {
        symbol_t *sym = elf_find_by_addr(&dbg->elf, ip);
        char func[160];
        if (sym) {
            uint64_t off = ip - sym->addr;
            if (off) {
                snprintf(func, sizeof(func), "%s+0x%lx", sym->name, off);
            } else {
                snprintf(func, sizeof(func), "%s", sym->name);
            }
        } else {
            snprintf(func, sizeof(func), "???");
        }

        printf("%d      0x%016lx  0x%016lx  %s\n", frame, fp, ip, func);

        /* Read next frame pointer and return address from stack */
        uint64_t prev_fp, ret_addr;
        if (ptrace_read_mem(dbg->proc.pid, fp, &prev_fp, 8) < 0) {
            break;
        }
        if (ptrace_read_mem(dbg->proc.pid, fp + 8, &ret_addr, 8) < 0) {
            break;
        }

        if (prev_fp <= fp) {
            break; /* Stop if frame pointer is not increasing */
        }

        fp = prev_fp;
        ip = ret_addr;
        frame++;
    }

    return 0;
}
