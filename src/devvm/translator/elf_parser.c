// ELF Parser Implementation
#include "elf_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

const char *elf_error_message = NULL;

// Internal error handler
static void set_error(const char *msg) {
    elf_error_message = msg;
    fprintf(stderr, "[ELF Parser] %s\n", msg);
}

// Read ELF file into memory
static uint8_t* read_file(const char *filename, size_t *out_size) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        set_error("Failed to open file");
        return NULL;
    }

    // Get file size
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

// Parse ELF header and extract sections
ELFBinary* elf_parse(const char *filename) {
    size_t file_size = 0;
    uint8_t *file_data = read_file(filename, &file_size);
    if (!file_data) {
        return NULL;
    }

    // Verify ELF magic
    if (file_size < sizeof(Elf64_Ehdr)) {
        set_error("File too small for ELF header");
        free(file_data);
        return NULL;
    }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_data;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        set_error("Not a valid ELF file (bad magic)");
        free(file_data);
        return NULL;
    }

    // Check architecture
    int is_x86_64 = (ehdr->e_machine == EM_X86_64);
    if (!is_x86_64 && ehdr->e_machine != EM_386) {
        set_error("Only x86-64 and i686 binaries supported");
        free(file_data);
        return NULL;
    }

    printf("[*] ELF Binary: %s\n", filename);
    printf("    Architecture: %s\n", is_x86_64 ? "x86-64" : "i686");
    printf("    Entry point: 0x%lx\n", ehdr->e_entry);
    printf("    Program headers: %u\n", ehdr->e_phnum);
    printf("    Section headers: %u\n", ehdr->e_shnum);

    // Allocate result
    ELFBinary *elf = (ELFBinary*)calloc(1, sizeof(ELFBinary));
    if (!elf) {
        set_error("Out of memory for ELFBinary");
        free(file_data);
        return NULL;
    }

    elf->entry_point = ehdr->e_entry;
    elf->is_x86_64 = is_x86_64;

    // Handle case with no section headers (use program headers instead)
    if (ehdr->e_shnum == 0 && ehdr->e_phnum > 0) {
        printf("[*] No section headers found, using program headers\n");
        Elf64_Phdr *phdrs = (Elf64_Phdr *)(file_data + ehdr->e_phoff);

        for (int i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD) {
                // First LOAD is usually .text (executable)
                if (phdrs[i].p_flags & PF_X) {
                    if (elf->code == NULL) {
                        elf->code = (uint8_t*)malloc(phdrs[i].p_filesz);
                        memcpy(elf->code, file_data + phdrs[i].p_offset, phdrs[i].p_filesz);
                        elf->code_size = phdrs[i].p_filesz;
                        elf->code_addr = phdrs[i].p_vaddr;
                        printf("[+] .text from LOAD[%d]: %zu bytes at 0x%lx\n", i, elf->code_size, elf->code_addr);
                    }
                } else {
                    // Non-executable LOAD is usually .data
                    if (elf->data == NULL && phdrs[i].p_filesz > 0) {
                        elf->data = (uint8_t*)malloc(phdrs[i].p_filesz);
                        memcpy(elf->data, file_data + phdrs[i].p_offset, phdrs[i].p_filesz);
                        elf->data_size = phdrs[i].p_filesz;
                        elf->data_addr = phdrs[i].p_vaddr;
                        printf("[+] .data from LOAD[%d]: %zu bytes at 0x%lx\n", i, elf->data_size, elf->data_addr);
                    }
                }
            }
        }

        if (!elf->code) {
            set_error("No executable LOAD segment found");
            free(elf);
            free(file_data);
            return NULL;
        }

        printf("[+] Binary parsed successfully (no-section-header format)\n\n");
        return elf;
    }

    // Read section headers (normal case)
    if (ehdr->e_shnum == 0) {
        set_error("No section or program headers found");
        free(elf);
        free(file_data);
        return NULL;
    }

    Elf64_Shdr *shdrs = (Elf64_Shdr *)(file_data + ehdr->e_shoff);
    if (ehdr->e_shoff + (ehdr->e_shnum * sizeof(Elf64_Shdr)) > file_size) {
        set_error("Section headers extend past file");
        free(elf);
        free(file_data);
        return NULL;
    }

    // Find .text, .data, .symtab, .strtab
    Elf64_Shdr *text_shdr = NULL;
    Elf64_Shdr *data_shdr = NULL;
    Elf64_Shdr *symtab_shdr = NULL;
    Elf64_Shdr *strtab_shdr = NULL;
    Elf64_Shdr *shstrtab_shdr = &shdrs[ehdr->e_shstrndx];

    // Get string table for section names
    const char *shstrtab = (const char *)(file_data + shstrtab_shdr->sh_offset);

    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char *name = &shstrtab[shdrs[i].sh_name];

        if (strcmp(name, ".text") == 0) {
            text_shdr = &shdrs[i];
        } else if (strcmp(name, ".data") == 0) {
            data_shdr = &shdrs[i];
        } else if (strcmp(name, ".symtab") == 0) {
            symtab_shdr = &shdrs[i];
        } else if (strcmp(name, ".strtab") == 0) {
            strtab_shdr = &shdrs[i];
        }
    }

    // Extract .text section (required)
    if (!text_shdr) {
        set_error("No .text section found");
        free(elf);
        free(file_data);
        return NULL;
    }

    elf->code = (uint8_t*)malloc(text_shdr->sh_size);
    if (!elf->code) {
        set_error("Out of memory for .text section");
        free(elf);
        free(file_data);
        return NULL;
    }

    memcpy(elf->code, file_data + text_shdr->sh_offset, text_shdr->sh_size);
    elf->code_size = text_shdr->sh_size;
    elf->code_addr = text_shdr->sh_addr;

    printf("[+] .text section: %zu bytes at 0x%lx\n", elf->code_size, elf->code_addr);

    // Extract .data section (optional)
    if (data_shdr && data_shdr->sh_size > 0) {
        elf->data = (uint8_t*)malloc(data_shdr->sh_size);
        if (elf->data) {
            memcpy(elf->data, file_data + data_shdr->sh_offset, data_shdr->sh_size);
            elf->data_size = data_shdr->sh_size;
            elf->data_addr = data_shdr->sh_addr;
            printf("[+] .data section: %zu bytes at 0x%lx\n", elf->data_size, elf->data_addr);
        }
    }

    // Extract symbol table (optional, for function names)
    if (symtab_shdr && symtab_shdr->sh_size > 0) {
        elf->symtab = (uint8_t*)malloc(symtab_shdr->sh_size);
        if (elf->symtab) {
            memcpy(elf->symtab, file_data + symtab_shdr->sh_offset, symtab_shdr->sh_size);
            elf->symtab_size = symtab_shdr->sh_size;
        }
    }

    // Extract string table (optional, for symbol names)
    if (strtab_shdr && strtab_shdr->sh_size > 0) {
        elf->strtab = (const char *)(file_data + strtab_shdr->sh_offset);
        elf->strtab_size = strtab_shdr->sh_size;
    }

    printf("[+] Binary parsed successfully\n\n");

    // Don't free file_data yet - strtab points into it
    // We'll keep it around (leak is acceptable for this use case)

    return elf;
}

// Look up symbol by address
const char* elf_get_symbol_name(ELFBinary *elf, uint64_t addr) {
    if (!elf->symtab || !elf->strtab) {
        return NULL;
    }

    Elf64_Sym *syms = (Elf64_Sym *)elf->symtab;
    size_t num_syms = elf->symtab_size / sizeof(Elf64_Sym);

    for (size_t i = 0; i < num_syms; i++) {
        if (syms[i].st_value == addr && syms[i].st_name < elf->strtab_size) {
            return &elf->strtab[syms[i].st_name];
        }
    }

    return NULL;
}

// Free ELF structure
void elf_free(ELFBinary *elf) {
    if (!elf) return;
    if (elf->code) free(elf->code);
    if (elf->data) free(elf->data);
    if (elf->symtab) free(elf->symtab);
    // Don't free strtab - it points into file_data
    free(elf);
}
