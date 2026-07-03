#include "citron.h"
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Source-level debugging.
 *
 * Citron does not embed a from-scratch DWARF `.debug_line` parser; instead it
 * shells out to the system `addr2line` (part of binutils, already a hard
 * dependency of the disassembler path). This maps a runtime address to
 * file:line and lets us print the current source line and a `list` window.
 *
 * For position-independent executables (ET_DYN) the runtime address must be
 * translated back to a link-time address by subtracting the load base, which
 * we read from /proc/<pid>/maps. For classic -no-pie ET_EXEC binaries the two
 * coincide and no adjustment is needed. */

/* Read the load base of the main executable from /proc/<pid>/maps: the lowest
   mapping start whose pathname matches the ELF path. Returns 0 on failure. */
static uint64_t load_base(pid_t pid, const char *elfpath) {
    if (pid <= 0 || !elfpath) return 0;
    char maps[64];
    snprintf(maps, sizeof(maps), "/proc/%d/maps", pid);
    FILE *f = fopen(maps, "r");
    if (!f) return 0;

    uint64_t base = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        const char *path = strchr(line, '/');
        if (!path) continue;
        /* Trim trailing newline for comparison */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strcmp(path, elfpath) == 0) {
            uint64_t start = strtoull(line, NULL, 16);
            if (base == 0 || start < base) base = start;
        }
    }
    fclose(f);
    return base;
}

/* Translate a live runtime address to a link-time address for addr2line. */
static uint64_t to_link_addr(debugger_t *dbg, uint64_t addr) {
    if (!dbg->elf.data || dbg->elf.size < sizeof(Elf64_Ehdr)) return addr;
    Elf64_Ehdr *e = (Elf64_Ehdr *)dbg->elf.data;
    if (e->e_type != ET_DYN) return addr;          /* ET_EXEC: 1:1 mapping */
    uint64_t base = load_base(dbg->proc.pid, dbg->elf.path);
    if (base && addr >= base) return addr - base;
    return addr;
}

/* Resolve addr -> file:line. Returns 1 on success (file/line filled), else 0. */
int source_lookup(debugger_t *dbg, uint64_t addr,
                  char *file, size_t filesz, int *line) {
    if (!dbg->elf.path) return 0;
    uint64_t la = to_link_addr(dbg, addr);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "addr2line -e '%s' 0x%lx 2>/dev/null",
             dbg->elf.path, (unsigned long)la);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;

    char out[1024];
    int ok = 0;
    if (fgets(out, sizeof(out), p)) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = '\0';
        /* Format: /path/to/file.c:NN  (or ??:0 / ??:?  when unknown) */
        char *colon = strrchr(out, ':');
        if (colon && out[0] != '?') {
            *colon = '\0';
            int ln = atoi(colon + 1);
            if (ln > 0) {
                snprintf(file, filesz, "%s", out);
                *line = ln;
                ok = 1;
            }
        }
    }
    pclose(p);
    return ok;
}

/* Print one source line (its text) for the current stop, if resolvable. */
static int print_source_line(const char *file, int line, const char *prefix) {
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    char buf[1024];
    int cur = 0;
    int printed = 0;
    while (fgets(buf, sizeof(buf), f)) {
        cur++;
        if (cur == line) {
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            printf("%s%d\t%s\n", prefix, cur, buf);
            printed = 1;
            break;
        }
    }
    fclose(f);
    return printed;
}

/* One-line "at file:line" annotation plus the source text, used after stops. */
void source_annotate(debugger_t *dbg, uint64_t addr) {
    char file[512];
    int line = 0;
    if (!source_lookup(dbg, addr, file, sizeof(file), &line)) return;

    /* Show just the basename in the header to keep it compact. */
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    printf("[citron] at %s:%d\n", base, line);
    print_source_line(file, line, "    ");
}

/* `list` command: print a window of source lines around addr. */
int source_list(debugger_t *dbg, uint64_t addr, int before, int after) {
    char file[512];
    int line = 0;
    if (!source_lookup(dbg, addr, file, sizeof(file), &line)) {
        warn("No source line information for 0x%lx", addr);
        return -1;
    }

    FILE *f = fopen(file, "r");
    if (!f) {
        warn("Cannot open source file: %s", file);
        return -1;
    }

    int lo = line - before;
    if (lo < 1) lo = 1;
    int hi = line + after;

    printf("--- %s (around line %d) ---\n", file, line);
    char buf[1024];
    int cur = 0;
    while (fgets(buf, sizeof(buf), f)) {
        cur++;
        if (cur < lo) continue;
        if (cur > hi) break;
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        printf("%s%5d\t%s\n", cur == line ? "=> " : "   ", cur, buf);
    }
    fclose(f);
    return 0;
}
