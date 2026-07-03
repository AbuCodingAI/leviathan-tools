#include "citron.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* DevVM (.dev) bytecode support.

   On-disk 24-byte header:
     [0..2]   magic  = 'd','e','v'
     [3]      version (u8)
     [4]      arch    (u8)
     [5..7]   reserved
     [8..11]  manifest_len (u32, little-endian)
     [12..15] reserved
     [16..23] payload_len  (u64, little-endian)
   Followed by the manifest JSON (manifest_len bytes) then the bytecode
   payload (payload_len bytes). Citron only identifies and summarizes .dev
   files; it does not execute them. */

int devvm_detect(const uint8_t *data, size_t size) {
    if (!data || size < 3) return 0;
    return data[0] == 'd' && data[1] == 'e' && data[2] == 'v';
}

int devvm_load(const uint8_t *data, size_t size, devvm_t *vm) {
    if (!devvm_detect(data, size)) {
        return -1;
    }
    if (size < 24) {
        warn("DevVM file truncated (need 24-byte header, have %zu)", size);
        return -1;
    }

    vm->version = data[3];
    vm->arch = data[4];

    uint32_t manifest_len = 0;
    uint64_t payload_len = 0;
    memcpy(&manifest_len, data + 8, sizeof(manifest_len));
    memcpy(&payload_len, data + 16, sizeof(payload_len));

    /* Clamp the manifest to what is actually present in the file */
    size_t manifest_off = 24;
    size_t avail = (manifest_off < size) ? size - manifest_off : 0;
    if (manifest_len > avail) {
        warn("DevVM manifest_len (%u) exceeds file; clamping to %zu",
             manifest_len, avail);
        manifest_len = (uint32_t)avail;
    }

    vm->const_size = manifest_len;  /* manifest length */
    vm->code_size = payload_len;    /* payload / opcode count */
    vm->is_devvm = 1;

    if (manifest_len > 0) {
        vm->manifest = malloc(manifest_len + 1);
        if (vm->manifest) {
            memcpy(vm->manifest, data + manifest_off, manifest_len);
            vm->manifest[manifest_len] = '\0';
        }
    }

    return 0;
}

int devvm_inspect(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        warn("Cannot open %s", path);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        warn("%s is empty", path);
        fclose(f);
        return -1;
    }

    uint8_t *data = malloc((size_t)sz);
    if (!data) {
        fclose(f);
        return -1;
    }
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        warn("Short read on %s", path);
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (!devvm_detect(data, (size_t)sz)) {
        warn("%s is not a DevVM (.dev) file", path);
        free(data);
        return -1;
    }

    devvm_t vm = {0};
    if (devvm_load(data, (size_t)sz, &vm) < 0) {
        free(data);
        return -1;
    }

    printf("DevVM file: %s\n", path);
    printf("  Type:         .dev bytecode\n");
    printf("  File size:    %ld bytes\n", sz);
    printf("  Version:      %u\n", vm.version);
    printf("  Arch:         %u\n", vm.arch);
    printf("  Manifest len: %zu bytes\n", vm.const_size);
    printf("  Payload len:  %zu bytes\n", vm.code_size);
    printf("  Opcode count: ~%zu (single-byte opcodes)\n", vm.code_size);
    if (vm.manifest && *vm.manifest) {
        printf("  Manifest:     %s\n", vm.manifest);
    }

    devvm_unload(&vm);
    free(data);
    return 0;
}

void devvm_unload(devvm_t *vm) {
    if (vm->code) {
        free(vm->code);
        vm->code = NULL;
    }
    if (vm->const_pool) {
        free(vm->const_pool);
        vm->const_pool = NULL;
    }
    if (vm->manifest) {
        free(vm->manifest);
        vm->manifest = NULL;
    }
    vm->is_devvm = 0;
}
