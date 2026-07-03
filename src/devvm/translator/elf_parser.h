// ELF Binary Parser - Extract code/data sections
// Purpose: Read ELF files and prepare for IR translation

#ifndef ELF_PARSER_H
#define ELF_PARSER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Parsed ELF structure
typedef struct {
    uint8_t *code;           // .text section (machine code)
    size_t code_size;
    uint64_t code_addr;      // Virtual address in ELF

    uint8_t *data;           // .data section (initialized data)
    size_t data_size;
    uint64_t data_addr;

    uint8_t *symtab;         // Symbol table raw data
    size_t symtab_size;

    const char *strtab;      // String table
    size_t strtab_size;

    uint64_t entry_point;    // Entry point address
    int is_x86_64;           // 1 if x86-64, 0 if i686
} ELFBinary;

// Parse ELF file
// Returns: ELFBinary structure (allocated on heap)
// On error: Returns NULL and sets error message
ELFBinary* elf_parse(const char *filename);

// Get function name by address
const char* elf_get_symbol_name(ELFBinary *elf, uint64_t addr);

// Free ELF structure
void elf_free(ELFBinary *elf);

// Error message (set by elf_parse on failure)
extern const char *elf_error_message;

#ifdef __cplusplus
}
#endif

#endif // ELF_PARSER_H
