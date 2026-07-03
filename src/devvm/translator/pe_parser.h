// PE (Portable Executable) Parser - Windows binary format
// Purpose: Extract code/data from Windows x86-64 binaries
#ifndef PE_PARSER_H
#define PE_PARSER_H

#include <stdint.h>
#include <stddef.h>

// PE Binary structure (extracted from Windows executable)
typedef struct {
    uint8_t *code;          // .text section (executable code)
    size_t code_size;
    uint64_t code_addr;     // Virtual address

    uint8_t *data;          // .data section (initialized data)
    size_t data_size;
    uint64_t data_addr;

    uint8_t *reloc;         // .reloc section (relocations)
    size_t reloc_size;

    uint64_t entry_point;   // EntryPoint RVA
    int is_x86_64;          // 1 = x86-64, 0 = i386
} PEBinary;

// Parse PE file and extract sections
// Returns: PEBinary structure with extracted code/data sections
// On error: prints to stderr and returns NULL
PEBinary* pe_parse(const char *filename);

// Get symbol/export name by address
const char* pe_get_export_name(PEBinary *pe, uint64_t addr);

// Free PE structure
void pe_free(PEBinary *pe);

#endif // PE_PARSER_H
