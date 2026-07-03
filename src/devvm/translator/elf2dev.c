// elf2dev - Convert ELF binaries to .dev bytecode
// Usage: elf2dev <input.elf> <output.dev> [--trust-level UNTRUSTED|SIGNED|VERIFIED]

#include "elf_parser.h"
#include "x86_disasm.h"
#include "../packer/devpack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_usage(const char *prog) {
    printf("Usage: %s <input.elf> <output.dev> [--trust-level LEVEL]\n", prog);
    printf("Trust levels: UNTRUSTED (default), SIGNED, VERIFIED\n");
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char *elf_path = argv[1];
    const char *dev_path = argv[2];
    uint8_t trust_level = 0;  // UNTRUSTED default

    // Parse trust level if provided
    if (argc >= 5 && strcmp(argv[3], "--trust-level") == 0) {
        if (strcmp(argv[4], "SIGNED") == 0) trust_level = 1;
        else if (strcmp(argv[4], "VERIFIED") == 0) trust_level = 2;
    }

    printf("═══════════════════════════════════════════\n");
    printf("elf2dev - ELF to .dev Bytecode Compiler\n");
    printf("═══════════════════════════════════════════\n\n");

    // Step 1: Parse ELF
    printf("[1/4] Parsing ELF binary…\n");
    ELFBinary *elf = elf_parse(elf_path);
    if (!elf) {
        fprintf(stderr, "Failed to parse ELF: %s\n", elf_error_message);
        return 1;
    }

    // Step 2: Disassemble and translate to IR
    printf("\n[2/4] Disassembling and translating to IR…\n");
    printf("    [LOG] Initializing disassembler\n");
    printf("    [LOG]   Code section: %zu bytes (0x%lx - 0x%lx)\n",
           elf->code_size, elf->code_addr, elf->code_addr + elf->code_size);

    Disassembler *dis = disasm_init(elf->code, elf->code_size, elf->code_addr);
    if (!dis) {
        fprintf(stderr, "Failed to initialize disassembler\n");
        elf_free(elf);
        return 1;
    }

    printf("    [LOG] Creating compressor\n");
    Compressor *comp = compressor_new();
    if (!comp) {
        fprintf(stderr, "Failed to initialize compressor\n");
        disasm_free(dis);
        elf_free(elf);
        return 1;
    }

    // Translate each instruction
    devvm::ir::Instruction instr;
    int syscall_count = 0;
    int instr_count = 0;
    printf("    [LOG] Beginning disassembly loop\n");

    while (disasm_next(dis, &instr)) {
        instr_count++;

        // Count syscalls (will be abstracted in sandbox)
        if (instr.op == devvm::ir::OpCode::SYSCALL) {
            syscall_count++;
            printf("    [*] Found SYSCALL instruction (will be abstracted)\n");
        }

        // Add to compressed bytecode
        compress_instruction(comp, &instr);

        // Progress indicator every 100 instructions
        if (instr_count % 100 == 0) {
            printf("    [LOG] Disassembled %d instructions (offset: 0x%lx/%zu)\n",
                   instr_count, disasm_offset(dis), elf->code_size);
        }
    }

    if (dis->error) {
        fprintf(stderr, "Disassembly error: %s\n", disasm_error(dis));
        compressor_free(comp);
        disasm_free(dis);
        elf_free(elf);
        return 1;
    }

    printf("    [LOG] Disassembly complete\n");
    printf("    Translated: %u instructions\n", comp->instr_count);
    printf("    Syscalls found: %d (will be sandboxed)\n", syscall_count);

    // Step 3: Write .dev file
    printf("\n[3/4] Packing into .dev bytecode…\n");
    if (write_dev_file(dev_path, comp, trust_level) < 0) {
        fprintf(stderr, "Failed to write .dev file\n");
        compressor_free(comp);
        disasm_free(dis);
        elf_free(elf);
        return 1;
    }

    // Step 4: Print statistics
    printf("\n[4/4] Compilation complete!\n");
    printf("═══════════════════════════════════════════\n");
    print_compression_stats(comp);
    printf("\n[✓] Successfully compiled ELF → .dev\n");
    printf("    Input:  %s (%zu bytes)\n", elf_path, elf->code_size);
    printf("    Output: %s\n", dev_path);
    printf("    Trust level: %s\n\n",
           trust_level == 0 ? "UNTRUSTED (sandboxed)" :
           trust_level == 1 ? "SIGNED (verified)" : "VERIFIED");

    // Cleanup
    compressor_free(comp);
    disasm_free(dis);
    elf_free(elf);

    return 0;
}
