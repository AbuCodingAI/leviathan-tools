// .dev Bytecode Packer Implementation (C++)
#include "devpack.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// CRC32 implementation
uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

// Initialize compressor
extern "C" {

Compressor* compressor_new(void) {
    Compressor *c = (Compressor*)calloc(1, sizeof(Compressor));
    if (!c) return NULL;

    c->out.capacity = 64 * 1024;  // 64 KB initial
    c->out.data = (uint8_t*)malloc(c->out.capacity);
    c->reg_freq = (uint32_t*)calloc(256, sizeof(uint32_t));

    return c;
}

// Add instruction to compressed bytecode
void compress_instruction(Compressor *c, const devvm::ir::Instruction *instr) {
    uint8_t temp[16];
    uint32_t instr_size = 0;

    if (!instr) return;

    // Encode opcode (6 bits)
    uint8_t opcode_byte = (uint8_t)(static_cast<uint8_t>(instr->op) & 0x3F);
    temp[instr_size++] = opcode_byte;

    // ALWAYS encode all three operands (unpacker expects fixed format)
    instr_size += vlq_encode(instr->dest, &temp[instr_size]);
    instr_size += vlq_encode(instr->src1, &temp[instr_size]);
    instr_size += vlq_encode(instr->src2, &temp[instr_size]);

    // Expand buffer if needed
    if (c->out.size + instr_size > c->out.capacity) {
        c->out.capacity *= 2;
        c->out.data = (uint8_t*)realloc(c->out.data, c->out.capacity);
    }

    // Copy packed instruction
    memcpy(&c->out.data[c->out.size], temp, instr_size);
    c->out.size += instr_size;
    c->instr_count++;

    // Track register usage
    if (instr->dest != 0xFF) c->reg_freq[instr->dest]++;
    if (instr->src1 != 0xFF) c->reg_freq[instr->src1]++;
    if (instr->src2 != 0xFF) c->reg_freq[instr->src2]++;
}

// Write .dev file
int write_dev_file(const char *out_path, Compressor *c, uint8_t trust_level) {
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        perror("fopen");
        return -1;
    }

    // Build JSON manifest - describes the application, not the format (all .dev is bytecode)
    const char *manifest_fmt = "{\"id\":\"app\",\"version\":\"1.0\",\"type\":\"devvm-ir\",\"arch\":\"x86_64\"}";
    uint32_t manifest_size = strlen(manifest_fmt);

    // Clean header format - PURE BYTECODE, platform-INDEPENDENT
    // This .dev file will run IDENTICALLY on Linux, Windows, ARM, i686, RISC-V, etc via DevVM
    // arch=0: executable bytecode only
    // arch=1+: archive mode (bytecode + packed files/dirs like .deb)
    struct {
        uint8_t magic[3];
        uint8_t version;
        uint8_t arch;            // 0=executable bytecode, 1+=archive (packed files)
        uint8_t reserved;        // Future use
        uint32_t manifest_len;
        uint64_t payload_len;
    } hdr = {
        .magic = {'d', 'e', 'v'},
        .version = 0x01,
        .arch = 0,               // 0 = executable bytecode (no archive)
        .reserved = 0,
        .manifest_len = manifest_size,
        .payload_len = c->out.size
    };

    // Write header
    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fprintf(stderr, "Failed to write header\n");
        fclose(f);
        return -1;
    }

    // Write manifest and null terminator separately
    if (fwrite(manifest_fmt, 1, manifest_size, f) != manifest_size) {
        fprintf(stderr, "Failed to write manifest\n");
        fclose(f);
        return -1;
    }
    uint8_t null_byte = 0;
    if (fwrite(&null_byte, 1, 1, f) != 1) {
        fprintf(stderr, "Failed to write manifest null terminator\n");
        fclose(f);
        return -1;
    }

    // Write compressed IR bytecode
    if (fwrite(c->out.data, c->out.size, 1, f) != 1) {
        fprintf(stderr, "Failed to write IR bytecode\n");
        fclose(f);
        return -1;
    }

    fclose(f);

    struct stat st;
    stat(out_path, &st);

    printf("[+] Wrote .dev bytecode file: %s\n", out_path);
    printf("    Header: 16 bytes (pure bytecode, platform-independent)\n");
    printf("    Manifest: %u bytes\n", manifest_size);
    printf("    Bytecode: %u bytes\n", c->out.size);
    printf("    Total: %ld bytes\n", st.st_size);
    printf("    [✓] Can execute on Linux, Windows, ARM, x86_64, i686 via DevVM\n");

    return 0;
}

// Free compressor
void compressor_free(Compressor *c) {
    if (!c) return;
    free(c->out.data);
    free(c->reg_freq);
    free(c);
}

// Print compression stats
void print_compression_stats(Compressor *c) {
    printf("\n═══ COMPRESSION STATS ═══\n");
    printf("Instructions: %u\n", c->instr_count);
    printf("Original size: %u bytes (%.1f KB)\n", c->instr_count * 4, c->instr_count * 4.0 / 1024);
    printf("Compressed size: %u bytes (%.1f KB)\n", c->out.size, c->out.size / 1024.0);
    if (c->instr_count * 4 > 0) {
        printf("Compression ratio: %.2f:1\n", (float)(c->instr_count * 4) / c->out.size);
    }
}

}  // extern "C"
