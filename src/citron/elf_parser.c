#include "citron.h"
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void symtab_push(symtab_t *st, const char *name, uint64_t addr,
                        uint64_t size, int is_func) {
    if (st->count >= st->capacity) {
        size_t nc = st->capacity ? st->capacity * 2 : 64;
        st->syms = xrealloc(st->syms, nc * sizeof(symbol_t));
        st->capacity = nc;
    }
    symbol_t *sym = &st->syms[st->count++];
    sym->name = xstrdup(name);
    sym->addr = addr;
    sym->size = size;
    sym->is_function = is_func;
}

/* Collect STT_FUNC and STT_OBJECT symbols (functions for code resolution,
   objects so `print`/`watch` can name globals). Tries SHT_SYMTAB, then
   SHT_DYNSYM. */
static void elf_parse_symbols(elf_t *elf) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf->data;

    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0 ||
        ehdr->e_shentsize < sizeof(Elf64_Shdr)) {
        return;
    }
    if (ehdr->e_shoff + (size_t)ehdr->e_shnum * ehdr->e_shentsize >
        elf->size) {
        warn("ELF section headers out of bounds");
        return;
    }

    Elf64_Shdr *shdrs = (Elf64_Shdr *)(elf->data + ehdr->e_shoff);

    for (int pass = 0; pass < 2 && elf->symtab.count == 0; pass++) {
        uint32_t want = (pass == 0) ? SHT_SYMTAB : SHT_DYNSYM;

        for (int i = 0; i < ehdr->e_shnum; i++) {
            Elf64_Shdr *symsh = &shdrs[i];
            if (symsh->sh_type != want || symsh->sh_entsize == 0) continue;
            if (symsh->sh_link >= ehdr->e_shnum) continue;
            if (symsh->sh_offset + symsh->sh_size > elf->size) continue;

            Elf64_Shdr *strsh = &shdrs[symsh->sh_link];
            if (strsh->sh_offset >= elf->size) continue;

            const char *strtab = (const char *)(elf->data + strsh->sh_offset);
            size_t str_max = elf->size - strsh->sh_offset;
            Elf64_Sym *syms = (Elf64_Sym *)(elf->data + symsh->sh_offset);
            size_t nsyms = symsh->sh_size / symsh->sh_entsize;

            for (size_t s = 0; s < nsyms; s++) {
                int type = ELF64_ST_TYPE(syms[s].st_info);
                if (type != STT_FUNC && type != STT_OBJECT) continue;
                if (syms[s].st_value == 0) continue;
                if (syms[s].st_name == 0 || syms[s].st_name >= str_max) continue;
                const char *name = strtab + syms[s].st_name;
                if (!*name) continue;
                symtab_push(&elf->symtab, name, syms[s].st_value,
                            syms[s].st_size, type == STT_FUNC);
            }
        }
    }
}

int elf_load(const char *path, elf_t *elf) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return -1;
    }

    elf->size = st.st_size;
    elf->data = mmap(NULL, elf->size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (elf->data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }

    elf->fd = fd;
    elf->path = xstrdup(path);

    /* Verify ELF header */
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf->data;
    if (elf->size < sizeof(Elf64_Ehdr) ||
        memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        warn("Not an ELF file: %s", path);
        elf_unload(elf);
        return -1;
    }

    /* Parse function symbols from .symtab (fallback .dynsym).
       DWARF/line info is out of scope. */
    elf_parse_symbols(elf);

    info("Loaded ELF: %s (size: %zu, %zu symbols)", path, elf->size,
         elf->symtab.count);
    return 0;
}

void elf_unload(elf_t *elf) {
    if (elf->data) {
        munmap(elf->data, elf->size);
        elf->data = NULL;
    }
    if (elf->fd >= 0) {
        close(elf->fd);
        elf->fd = -1;
    }
    if (elf->symtab.syms) {
        for (size_t i = 0; i < elf->symtab.count; i++) {
            free(elf->symtab.syms[i].name);
        }
        free(elf->symtab.syms);
        elf->symtab.syms = NULL;
        elf->symtab.count = 0;
        elf->symtab.capacity = 0;
    }
    if (elf->path) {
        free(elf->path);
        elf->path = NULL;
    }
}

symbol_t *elf_find_symbol(elf_t *elf, const char *name) {
    for (size_t i = 0; i < elf->symtab.count; i++) {
        if (strcmp(elf->symtab.syms[i].name, name) == 0) {
            return &elf->symtab.syms[i];
        }
    }
    return NULL;
}

symbol_t *elf_find_by_addr(elf_t *elf, uint64_t addr) {
    /* Best match = the function symbol with the greatest start address that is
       still <= addr. If the symbol carries a size, respect its upper bound;
       many .dynsym entries have size 0, so treat those as open-ended. */
    symbol_t *best = NULL;
    for (size_t i = 0; i < elf->symtab.count; i++) {
        symbol_t *sym = &elf->symtab.syms[i];
        if (!sym->is_function) continue;   /* code-address resolution only */
        if (sym->addr > addr) continue;
        if (sym->size && addr >= sym->addr + sym->size) continue;
        if (!best || sym->addr > best->addr) best = sym;
    }
    return best;
}
