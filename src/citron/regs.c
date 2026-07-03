#include "citron.h"
#include <sys/user.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Register index table. Indices must match ptrace_get_reg/ptrace_set_reg. */
static const char *reg_names[] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "eflags",
};
#define NREGS ((int)(sizeof(reg_names) / sizeof(reg_names[0])))

int reg_index(const char *name) {
    if (!name) return -1;
    if (*name == '$') name++;   /* accept GDB-style $rax */
    char low[16];
    size_t i = 0;
    while (name[i] && i < sizeof(low) - 1) {
        low[i] = (char)tolower((unsigned char)name[i]);
        i++;
    }
    low[i] = '\0';
    /* Common aliases */
    if (strcmp(low, "pc") == 0) return 8;   /* rip */
    if (strcmp(low, "sp") == 0) return 7;   /* rsp */
    if (strcmp(low, "fp") == 0) return 6;   /* rbp */
    for (int r = 0; r < NREGS; r++) {
        if (strcmp(low, reg_names[r]) == 0) return r;
    }
    return -1;
}

const char *reg_name(int idx) {
    if (idx < 0 || idx >= NREGS) return "?";
    return reg_names[idx];
}

uint64_t regs_get_ip(struct user_regs_struct *regs) {
    return regs->rip;
}

uint64_t regs_get_sp(struct user_regs_struct *regs) {
    return regs->rsp;
}

uint64_t regs_get_bp(struct user_regs_struct *regs) {
    return regs->rbp;
}

void regs_print(struct user_regs_struct *regs) {
    printf("RAX: 0x%016llx  RBX: 0x%016llx\n", regs->rax, regs->rbx);
    printf("RCX: 0x%016llx  RDX: 0x%016llx\n", regs->rcx, regs->rdx);
    printf("RSI: 0x%016llx  RDI: 0x%016llx\n", regs->rsi, regs->rdi);
    printf("RBP: 0x%016llx  RSP: 0x%016llx\n", regs->rbp, regs->rsp);
    printf("RIP: 0x%016llx\n", regs->rip);
    printf("EFLAGS: 0x%016llx\n", regs->eflags);
}
