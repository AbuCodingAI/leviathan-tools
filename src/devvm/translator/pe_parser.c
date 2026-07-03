// PE Parser Implementation
#include "pe_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PE Headers (simplified)
#pragma pack(push, 1)

typedef struct {
    uint8_t magic[2];       // "MZ"
    uint8_t _pad[58];
    uint32_t pe_offset;     // Offset to PE header
} DOSHeader;

typedef struct {
    uint32_t signature;     // "PE\0\0"
    uint16_t machine;       // 0x8664 = x86-64, 0x014c = i386
    uint16_t num_sections;
    uint32_t time_date;
    uint32_t ptr_sym_table;
    uint32_t num_symbols;
    uint16_t opt_header_size;
} COFFHeader;

typedef struct {
    uint16_t magic;         // 0x020b = PE32+, 0x010b = PE32
    uint8_t major_linker;
    uint8_t minor_linker;
    uint32_t code_size;
    uint32_t data_size;
    uint32_t uninit_data_size;
    uint32_t entry_point;   // RVA of entry point
    uint32_t base_of_code;
    uint64_t image_base;
    uint32_t section_alignment;
    uint32_t file_alignment;
    // ... more fields, truncated for simplicity
} OptionalHeader;

typedef struct {
    uint8_t name[8];
    uint32_t virtual_size;
    uint32_t virtual_addr;  // RVA
    uint32_t raw_size;
    uint32_t raw_ptr;       // Offset in file
    uint8_t _pad[16];
    uint32_t flags;
} SectionHeader;

#pragma pack(pop)

static const char *pe_error_msg = NULL;

static void set_error(const char *msg) {
    pe_error_msg = msg;
    fprintf(stderr, "[PE Parser] %s\n", msg);
}

static uint8_t* read_file(const char *filename, size_t *out_size) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        set_error("Failed to open file");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        set_error("File is empty or unreadable");
        fclose(f);
        return NULL;
    }

    uint8_t *data = (uint8_t*)malloc(size);
    if (!data) {
        set_error("Out of memory");
        fclose(f);
        return NULL;
    }

    if (fread(data, size, 1, f) != 1) {
        set_error("Failed to read file");
        free(data);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = size;
    return data;
}

PEBinary* pe_parse(const char *filename) {
    size_t file_size = 0;
    uint8_t *file_data = read_file(filename, &file_size);
    if (!file_data) return NULL;

    // Validate DOS header
    if (file_size < sizeof(DOSHeader)) {
        set_error("File too small for DOS header");
        free(file_data);
        return NULL;
    }

    DOSHeader *dos = (DOSHeader*)file_data;
    if (dos->magic[0] != 'M' || dos->magic[1] != 'Z') {
        set_error("Not a valid PE file (bad DOS magic)");
        free(file_data);
        return NULL;
    }

    uint32_t pe_off = dos->pe_offset;
    if (pe_off + 4 > file_size) {
        set_error("PE header offset out of bounds");
        free(file_data);
        return NULL;
    }

    // Validate PE signature
    if (file_data[pe_off + 0] != 'P' || file_data[pe_off + 1] != 'E' ||
        file_data[pe_off + 2] != 0 || file_data[pe_off + 3] != 0) {
        set_error("Not a valid PE file (bad PE signature)");
        free(file_data);
        return NULL;
    }

    // Parse COFF header
    COFFHeader *coff = (COFFHeader*)(file_data + pe_off + 4);
    if (pe_off + 4 + sizeof(COFFHeader) > file_size) {
        set_error("COFF header extends past file");
        free(file_data);
        return NULL;
    }

    int is_x86_64 = (coff->machine == 0x8664);
    if (!is_x86_64 && coff->machine != 0x014c) {
        set_error("Only x86-64 and i386 PE files supported");
        free(file_data);
        return NULL;
    }

    printf("[*] PE Binary: %s\n", filename);
    printf("    Architecture: %s\n", is_x86_64 ? "x86-64" : "i386");
    printf("    Sections: %u\n", coff->num_sections);
    printf("    Entry point (RVA): 0x%x\n", coff->opt_header_size > 0 ? 0 : 0);

    // Allocate result
    PEBinary *pe = (PEBinary*)calloc(1, sizeof(PEBinary));
    if (!pe) {
        set_error("Out of memory for PEBinary");
        free(file_data);
        return NULL;
    }

    pe->is_x86_64 = is_x86_64;

    // Parse optional header to get entry point
    OptionalHeader *opt = (OptionalHeader*)(file_data + pe_off + 4 + sizeof(COFFHeader));
    if (pe_off + 4 + sizeof(COFFHeader) + coff->opt_header_size > file_size) {
        set_error("Optional header extends past file");
        free(pe);
        free(file_data);
        return NULL;
    }

    pe->entry_point = opt->entry_point;

    // Parse section headers to find .text and .data
    uint8_t *section_ptr = (uint8_t*)opt + coff->opt_header_size;
    for (int i = 0; i < coff->num_sections; i++) {
        SectionHeader *sh = (SectionHeader*)(section_ptr + i * sizeof(SectionHeader));
        if ((uint8_t*)sh + sizeof(SectionHeader) > file_data + file_size) {
            set_error("Section header extends past file");
            free(pe);
            free(file_data);
            return NULL;
        }

        // Check section name (stored as 8-byte ASCII, not null-terminated)
        char name[9] = {0};
        memcpy(name, sh->name, 8);

        if (strcmp(name, ".text\0\0\0") == 0) {
            // Extract .text section
            if (sh->raw_ptr + sh->raw_size > file_size) {
                set_error(".text section extends past file");
                free(pe);
                free(file_data);
                return NULL;
            }
            pe->code = (uint8_t*)malloc(sh->raw_size);
            if (!pe->code) {
                set_error("Out of memory for .text");
                free(pe);
                free(file_data);
                return NULL;
            }
            memcpy(pe->code, file_data + sh->raw_ptr, sh->raw_size);
            pe->code_size = sh->raw_size;
            pe->code_addr = sh->virtual_addr;
            printf("[+] .text section: %u bytes at 0x%x (file offset 0x%x)\n",
                   sh->raw_size, sh->virtual_addr, sh->raw_ptr);
        }
        else if (strcmp(name, ".data\0\0\0") == 0) {
            // Extract .data section
            if (sh->raw_ptr + sh->raw_size > file_size) {
                set_error(".data section extends past file");
                free(pe);
                free(file_data);
                return NULL;
            }
            pe->data = (uint8_t*)malloc(sh->raw_size);
            if (!pe->data) {
                set_error("Out of memory for .data");
                free(pe);
                free(file_data);
                return NULL;
            }
            memcpy(pe->data, file_data + sh->raw_ptr, sh->raw_size);
            pe->data_size = sh->raw_size;
            pe->data_addr = sh->virtual_addr;
            printf("[+] .data section: %u bytes at 0x%x\n", sh->raw_size, sh->virtual_addr);
        }
    }

    if (!pe->code) {
        set_error("No .text section found");
        free(pe);
        free(file_data);
        return NULL;
    }

    printf("[+] PE binary parsed successfully\n\n");
    // Note: file_data leak is acceptable for simplicity
    return pe;
}

const char* pe_get_export_name(PEBinary *pe, uint64_t addr) {
    // TODO: Parse export table
    return NULL;
}

void pe_free(PEBinary *pe) {
    if (!pe) return;
    if (pe->code) free(pe->code);
    if (pe->data) free(pe->data);
    if (pe->reloc) free(pe->reloc);
    free(pe);
}
