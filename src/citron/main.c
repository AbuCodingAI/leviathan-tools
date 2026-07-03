#include "citron.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog) {
    fprintf(stderr,
            "Citron — Lightweight Rescue Debugger\n"
            "Usage: %s [options] <program> [args...]\n"
            "       %s -a <pid>            (attach to running process)\n"
            "\n"
            "Options:\n"
            "  -h                 Show this help\n"
            "  -a <pid>           Attach to process\n"
            "\n"
            "Commands at prompt:\n"
            "  run [args...]      Start program\n"
            "  break <addr>       Set breakpoint\n"
            "  continue           Resume execution\n"
            "  step               Step into\n"
            "  next               Step over\n"
            "  backtrace          Show stack\n"
            "  registers          Show CPU registers\n"
            "  print <expr>       Print variable\n"
            "  quit               Exit debugger\n"
            "  help               Show help\n",
            prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    debugger_t dbg = {0};
    int attach_mode = 0;
    pid_t attach_pid = 0;

    /* Parse options */
    int opt;
    while ((opt = getopt(argc, argv, "ha:")) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return 0;
        case 'a':
            attach_mode = 1;
            attach_pid = atoi(optarg);
            if (attach_pid <= 0) {
                die("Invalid PID: %s", optarg);
            }
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (attach_mode) {
        if (ptrace_attach(attach_pid, &dbg) < 0) {
            die("Failed to attach to PID %d", attach_pid);
        }
        info("Attached to process %d", attach_pid);
    } else {
        if (optind >= argc) {
            usage(argv[0]);
            return 1;
        }

        const char *prog = argv[optind];
        char **prog_argv = &argv[optind];

        /* If the target is a DevVM (.dev) bytecode file, just identify and
           summarize it — it is not a native executable to ptrace. */
        FILE *tf = fopen(prog, "rb");
        if (tf) {
            uint8_t magic[3] = {0};
            size_t n = fread(magic, 1, sizeof(magic), tf);
            fclose(tf);
            if (n == sizeof(magic) && devvm_detect(magic, n)) {
                devvm_inspect(prog);
                return 0;
            }
        }

        /* Load ELF info before running */
        if (elf_load(prog, &dbg.elf) < 0) {
            warn("Could not load symbol table from %s", prog);
        }

        /* Fork and trace */
        if (ptrace_fork_and_trace(prog, prog_argv, &dbg) < 0) {
            die("Failed to start process: %s", prog);
        }

        info("Tracing process %d: %s", dbg.proc.pid, prog);
    }

    /* Run interactive debugger */
    if (cli_repl(&dbg) < 0) {
        warn("REPL error");
    }

    /* Cleanup — only detach from a process that is still alive. */
    if (dbg.proc.pid > 0 && dbg.proc.state != PROC_EXITED &&
        dbg.proc.state != PROC_CRASHED) {
        ptrace_detach(&dbg);
    }
    elf_unload(&dbg.elf);
    devvm_unload(&dbg.devvm);

    return 0;
}
