// PE to .dev Bytecode Compiler - Converts Windows PE binaries to DevVM bytecode
// Usage: pe2dev <input.exe> <output.dev> [--trust-level LEVEL]
// This creates cross-platform bytecode that runs on Linux, Windows, ARM, etc via DevVM

#include "pe_parser.h"
#include "x86_disasm.h"
#include "../packer/devpack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    printf("═══════════════════════════════════════════\n");
    printf("pe2dev - PE to .dev Bytecode Compiler\n");
    printf("═══════════════════════════════════════════\n\n");

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.exe> <output.dev> [--trust-level LEVEL]\n", argv[0]);
        fprintf(stderr, "Trust levels: UNTRUSTED (default), SIGNED, VERIFIED\n");
        return 1;
    }

    const char *in_path = argv[1];
    const char *out_path = argv[2];
    uint8_t trust_level = 0;  // UNTRUSTED

    // Parse optional trust level
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--trust-level") == 0 && i + 1 < argc) {
            const char *level = argv[++i];
            if (strcmp(level, "SIGNED") == 0) trust_level = 1;
            else if (strcmp(level, "VERIFIED") == 0) trust_level = 2;
        }
    }

    // [1/4] Parse PE binary
    printf("[1/4] Parsing PE binary…\n");
    PEBinary *pe = pe_parse(in_path);
    if (!pe) return 1;

    // [2/4] Disassemble and translate to IR
    printf("[2/4] Disassembling and translating to IR…\n");
    Disassembler *dis = disasm_init(pe->code, pe->code_size, pe->code_addr);
    if (!dis) {
        fprintf(stderr, "Failed to initialize disassembler\n");
        pe_free(pe);
        return 1;
    }

    Compressor *compressor = compressor_new();
    if (!compressor) {
        fprintf(stderr, "Failed to create compressor\n");
        disasm_free(dis);
        pe_free(pe);
        return 1;
    }

    uint32_t instr_count = 0;
    uint32_t syscall_count = 0;
    devvm::ir::Instruction ir_instr;

    while (disasm_next(dis, &ir_instr)) {
        instr_count++;

        // Track syscalls (Windows API calls will be converted to IR SYSCALL)
        if (ir_instr.op == devvm::ir::OpCode::SYSCALL) {
            syscall_count++;
            printf("    [*] Found Windows API call (will be abstracted)\n");
        }

        compress_instruction(compressor, &ir_instr);
    }

    printf("    Translated: %u instructions\n", instr_count);
    printf("    API calls found: %u (will be sandboxed)\n", syscall_count);

    // [3/4] Pack into .dev bytecode
    printf("[3/4] Packing into .dev bytecode…\n");
    int ret = write_dev_file(out_path, compressor, trust_level);
    if (ret != 0) {
        fprintf(stderr, "Failed to write .dev file\n");
        compressor_free(compressor);
        disasm_free(dis);
        pe_free(pe);
        return 1;
    }

    // [4/4] Summary
    printf("[4/4] Compilation complete!\n");
    printf("═══════════════════════════════════════════\n\n");
    print_compression_stats(compressor);

    printf("[✓] Successfully compiled PE → .dev\n");
    printf("    Input:  %s (Windows x86-64 binary)\n", in_path);
    printf("    Output: %s (platform-independent bytecode)\n", out_path);
    printf("    This .dev file will run on Linux, Windows, ARM, i386, etc!\n\n");

    compressor_free(compressor);
    disasm_free(dis);
    pe_free(pe);

    return 0;
}
