#include "citron.h"
#include <stdio.h>
#include <stdlib.h>

/* Citron does not ship its own x86-64 decoder. Instead we shell out to a
   real disassembler (objdump, falling back to gdb) over the requested
   address range of the target binary. */

static int tool_exists(const char *tool) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", tool);
    return system(cmd) == 0;
}

int disasm_range(const char *path, uint64_t start, uint64_t stop) {
    if (!path) {
        warn("No target binary available for disassembly");
        return -1;
    }
    if (stop <= start) {
        stop = start + 64;
    }

    char cmd[1024];
    if (tool_exists("objdump")) {
        snprintf(cmd, sizeof(cmd),
                 "objdump -d --start-address=0x%lx --stop-address=0x%lx '%s'",
                 (unsigned long)start, (unsigned long)stop, path);
    } else if (tool_exists("gdb")) {
        snprintf(cmd, sizeof(cmd),
                 "gdb -batch -ex 'disassemble 0x%lx,0x%lx' '%s' 2>/dev/null",
                 (unsigned long)start, (unsigned long)stop, path);
    } else {
        warn("Neither objdump nor gdb found; cannot disassemble");
        return -1;
    }

    int rc = system(cmd);
    if (rc != 0) {
        warn("Disassembler command failed (rc=%d)", rc);
        return -1;
    }
    return 0;
}
