// x86-64 Disassembler - Decode machine code to IR
// Purpose: Convert x86-64 instructions to DevVM IR instructions

#ifndef X86_DISASM_H
#define X86_DISASM_H

#include <stdint.h>
#include <stddef.h>
#include "../ir/instructions.h"  // DevVM IR

#ifdef __cplusplus
extern "C" {
#endif

// Disassembler state
typedef struct {
    const uint8_t *code;     // Pointer to x86-64 code
    size_t code_size;
    size_t offset;           // Current offset in code
    uint64_t base_addr;      // Base address for jumps/calls
    int error;               // Error flag
    const char *error_msg;   // Error message
} Disassembler;

// Initialize disassembler
Disassembler* disasm_init(const uint8_t *code, size_t size, uint64_t base_addr);

// Decode next instruction
// Returns: 1 if instruction decoded, 0 if error or end of code
// Fills out_instr with the decoded instruction
int disasm_next(Disassembler *dis, devvm::ir::Instruction *out_instr);

// Get current offset
size_t disasm_offset(Disassembler *dis);

// Get error message
const char* disasm_error(Disassembler *dis);

// Free disassembler
void disasm_free(Disassembler *dis);

#ifdef __cplusplus
}
#endif

#endif // X86_DISASM_H
