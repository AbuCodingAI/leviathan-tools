/* ==========================================================================
 *  Leviathan Dev API  —  dev_api.h
 * ==========================================================================
 *  Leviathan's OWN application API surface.
 *
 *  A .dev program targets these dev_* calls — never Win32, never raw Linux
 *  syscalls. Because every dev_* function is portable C, the JIT/AOT compiler
 *  emits them straight into the generated C, and they "compile down" to native
 *  code on ANY platform with a C compiler. The same .dev → a native Windows
 *  .exe, a Linux ELF, an ARM binary — the API is the stable contract, C is the
 *  portable substrate.
 *
 *  This header is (a) the canonical reference/spec, and (b) mirrored inline by
 *  the codegen (src/devvm/jit/jit_to_c.cpp) so produced binaries are fully
 *  self-contained — nothing to install, matching `dev run --static-vm`.
 *
 *  Design rule: if a capability would normally require a platform API, it gets
 *  a dev_* wrapper here so authors never touch the platform directly.
 * ========================================================================== */
#ifndef LEVIATHAN_DEV_API_H
#define LEVIATHAN_DEV_API_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- process / args / environment ------------------------------------- */
static int    dev__argc = 0;
static char** dev__argv = 0;
static inline void        dev_main_init(int argc, char** argv) { dev__argc = argc; dev__argv = argv; }
static inline int         dev_argc(void)         { return dev__argc; }
static inline const char* dev_arg(int i)         { return (i >= 0 && i < dev__argc) ? dev__argv[i] : (const char*)0; }
static inline const char* dev_env(const char* k) { return k ? getenv(k) : (const char*)0; }
static inline void        dev_exit(int code)     { exit(code); }

/* ---- console I/O ------------------------------------------------------- */
static inline void dev_write(int fd, const void* buf, uint64_t len) {
    FILE* s = (fd == 2) ? stderr : stdout;
    if (buf && len) fwrite(buf, 1, (size_t)len, s);
    fflush(s);
}
static inline void dev_print(const char* s)   { if (s) fputs(s, stdout); }
static inline void dev_println(const char* s) { if (s) fputs(s, stdout); fputc('\n', stdout); }
static inline int  dev_read_line(char* out, int cap) {  /* returns length, -1 on EOF */
    if (!out || cap <= 0) return -1;
    if (!fgets(out, cap, stdin)) return -1;
    int n = (int)strlen(out);
    if (n > 0 && out[n - 1] == '\n') out[--n] = '\0';
    return n;
}

/* ---- files (portable, no platform handles) ---------------------------- */
static inline int64_t dev_read_file(const char* path, char* out, int64_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    int64_t n = (int64_t)fread(out, 1, (size_t)cap, f);
    fclose(f);
    return n;
}
static inline int dev_write_file(const char* path, const void* data, int64_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    int64_t n = (int64_t)fwrite(data, 1, (size_t)len, f);
    fclose(f);
    return (n == len) ? 0 : -1;
}

/* ---- time / randomness ------------------------------------------------ */
static inline uint64_t dev_time_ms(void) { return (uint64_t)time((time_t*)0) * 1000ull; }
static inline uint64_t dev_rand(void)    { return (uint64_t)rand(); }
static inline void     dev_srand(uint64_t seed) { srand((unsigned)seed); }

#endif /* LEVIATHAN_DEV_API_H */
