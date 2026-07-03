#include "citron.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void *xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr && size > 0) {
        die("malloc(%zu) failed", size);
    }
    return ptr;
}

void *xrealloc(void *ptr, size_t size) {
    void *new = realloc(ptr, size);
    if (!new && size > 0) {
        die("realloc(%zu) failed", size);
    }
    return new;
}

char *xstrdup(const char *s) {
    char *dup = strdup(s);
    if (!dup) {
        die("strdup failed");
    }
    return dup;
}

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "citron: error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

void warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "citron: warning: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[citron] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
