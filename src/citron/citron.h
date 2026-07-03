#ifndef CITRON_H
#define CITRON_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* Process state */
typedef enum {
    PROC_STOPPED = 0,
    PROC_RUNNING = 1,
    PROC_EXITED = 2,
    PROC_CRASHED = 3,
} proc_state_t;

/* Debugged process handle */
typedef struct {
    pid_t pid;
    proc_state_t state;
    int exit_code;
    int signal;
    uint64_t ip;  /* instruction pointer */
} process_t;

/* Condition comparison operators for conditional breakpoints */
typedef enum {
    COND_NONE = 0,
    COND_EQ,   /* == */
    COND_NE,   /* != */
    COND_LT,   /* <  */
    COND_GT,   /* >  */
    COND_LE,   /* <= */
    COND_GE,   /* >= */
} cond_op_t;

/* Breakpoint */
typedef struct {
    uint32_t id;
    uint64_t addr;
    uint8_t original_byte;
    int enabled;
    /* Conditional breakpoint: only stop when <reg> <op> <val> holds. */
    cond_op_t cond_op;   /* COND_NONE if unconditional */
    int cond_reg;        /* register index (see ptrace_get_reg) */
    uint64_t cond_val;
    char cond_str[64];   /* human-readable condition, for `info breakpoints` */
    uint64_t hit_count;
} breakpoint_t;

#define MAX_BREAKPOINTS 256
typedef struct {
    breakpoint_t bps[MAX_BREAKPOINTS];
    size_t count;
    uint32_t next_id;
} breakpoint_list_t;

/* Hardware watchpoint (x86 debug registers DR0-DR3) */
typedef struct {
    uint32_t id;
    uint64_t addr;
    int len;            /* 1, 2, 4, or 8 bytes */
    int slot;           /* DR0-DR3 slot index, -1 if inactive */
    uint64_t last_val;  /* last observed value (len bytes, zero-extended) */
    int enabled;
} watchpoint_t;

#define MAX_WATCHPOINTS 4

/* Symbol information */
typedef struct {
    char *name;
    uint64_t addr;
    uint64_t size;
    int is_function;
} symbol_t;

/* Symbol table */
typedef struct {
    symbol_t *syms;
    size_t count;
    size_t capacity;
} symtab_t;

/* ELF file handle */
typedef struct {
    int fd;
    uint8_t *data;
    size_t size;
    symtab_t symtab;
    uint64_t base_addr;
    char *path;
} elf_t;

/* DevVM bytecode info */
typedef struct {
    uint8_t *code;
    size_t code_size;      /* payload length (bytes / opcode count) */
    uint8_t *const_pool;
    size_t const_size;     /* manifest length */
    char *manifest;        /* manifest JSON (NUL-terminated) */
    uint8_t version;
    uint8_t arch;
    int is_devvm;
} devvm_t;

/* Main debugger context */
typedef struct {
    process_t proc;
    breakpoint_list_t breakpoints;
    watchpoint_t watchpoints[MAX_WATCHPOINTS];
    size_t watch_count;
    uint32_t next_watch_id;
    elf_t elf;
    devvm_t devvm;
    int quit;
} debugger_t;

/* ptrace_engine.c */
int ptrace_fork_and_trace(const char *path, char *const argv[],
                          debugger_t *dbg);
int ptrace_attach(pid_t pid, debugger_t *dbg);
int ptrace_continue(debugger_t *dbg);
int ptrace_step(debugger_t *dbg);
int ptrace_next(debugger_t *dbg);
int ptrace_finish(debugger_t *dbg);
int ptrace_wait(debugger_t *dbg);
int ptrace_detach(debugger_t *dbg);
/* Hardware watchpoints (DR0-DR3) */
int watch_add(debugger_t *dbg, uint64_t addr, int len);
int watch_delete(debugger_t *dbg, uint32_t id);
void watch_list(debugger_t *dbg);
uint64_t ptrace_get_reg(pid_t pid, int reg);
int ptrace_set_reg(pid_t pid, int reg, uint64_t val);
int ptrace_read_mem(pid_t pid, uint64_t addr, void *buf, size_t len);
int ptrace_write_mem(pid_t pid, uint64_t addr, const void *buf, size_t len);

/* breakpoint.c */
int breakpoint_add(debugger_t *dbg, uint64_t addr);
int breakpoint_delete(debugger_t *dbg, uint32_t id);
int breakpoint_enable(debugger_t *dbg, uint32_t id);
int breakpoint_disable(debugger_t *dbg, uint32_t id);
breakpoint_t *breakpoint_find_by_addr(debugger_t *dbg, uint64_t addr);
breakpoint_t *breakpoint_find_by_id(debugger_t *dbg, uint32_t id);
void breakpoint_list(debugger_t *dbg);
int breakpoint_cond_met(debugger_t *dbg, breakpoint_t *bp);

/* elf_parser.c */
int elf_load(const char *path, elf_t *elf);
void elf_unload(elf_t *elf);
symbol_t *elf_find_symbol(elf_t *elf, const char *name);
symbol_t *elf_find_by_addr(elf_t *elf, uint64_t addr);

/* devvm_parser.c */
int devvm_detect(const uint8_t *data, size_t size);
int devvm_load(const uint8_t *data, size_t size, devvm_t *vm);
int devvm_inspect(const char *path);
void devvm_unload(devvm_t *vm);

/* regs.c */
struct user_regs_struct;
uint64_t regs_get_ip(struct user_regs_struct *regs);
uint64_t regs_get_sp(struct user_regs_struct *regs);
uint64_t regs_get_bp(struct user_regs_struct *regs);
void regs_print(struct user_regs_struct *regs);
/* Map a register name ("rax", "$rip", case-insensitive) to a ptrace_get_reg
   index, or -1 if unknown. */
int reg_index(const char *name);
const char *reg_name(int idx);

/* stack.c */
int stack_unwind(debugger_t *dbg);

/* disasm.c */
int disasm_range(const char *path, uint64_t start, uint64_t stop);

/* source.c — DWARF line info via addr2line */
int source_lookup(debugger_t *dbg, uint64_t addr,
                  char *file, size_t filesz, int *line);
void source_annotate(debugger_t *dbg, uint64_t addr);
int source_list(debugger_t *dbg, uint64_t addr, int before, int after);

/* readline_ui.c */
int cli_repl(debugger_t *dbg);

/* util.c */
void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);
void die(const char *fmt, ...);
void warn(const char *fmt, ...);
void info(const char *fmt, ...);

#endif /* CITRON_H */
