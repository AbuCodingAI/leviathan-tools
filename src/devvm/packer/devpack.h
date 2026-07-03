// .dev Bytecode Packer - Ultra-Compact IR Encoding
// Purpose: Compress 3-address IR to minimal bytes while abstracting syscalls

#ifndef DEVPACK_H
#define DEVPACK_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
#include "../ir/instructions.h"
using namespace devvm::ir;
#endif

// .dev file format - PURE BYTECODE (platform-INDEPENDENT)
// Same .dev runs identically on Linux, Windows, ARM, x86, RISC-V, i686, etc via DevVM
// arch field specifies package mode:
//   0 = executable bytecode only (no archive)
//   1+ = archive mode (compressed files/folders packed like .deb)
typedef struct {
    uint8_t magic[3];      // "dev"
    uint8_t version;       // 0x01
    uint8_t arch;          // 0=executable, 1+=archive (packed files/dirs)
    uint8_t reserved;      // Future use
    uint32_t manifest_len; // JSON manifest size
    uint64_t payload_len;  // Compressed IR bytecode + optional archive
} DevFileHeader;           // 16 bytes - executable bytecode OR universal package format

// Instruction compression: 3-address code → variable-length encoding
// Original: 32 bits [opcode:8 | dest:8 | src1:8 | src2:8]
// Packed: 1-4 bytes depending on operand sizes

// 4-bit register encoding (compress 2 registers per byte!)
// Register IDs: 0-15 (4 bits each)
// Packed: [reg1:4 | reg2:4]
static inline uint32_t encode_4bit_regs(uint8_t reg1, uint8_t reg2, uint8_t *out) {
    out[0] = ((reg1 & 0xF) << 4) | (reg2 & 0xF);
    return 1;
}

static inline void decode_4bit_regs(uint8_t byte, uint8_t *out_reg1, uint8_t *out_reg2) {
    *out_reg1 = (byte >> 4) & 0xF;
    *out_reg2 = byte & 0xF;
}

// VLQ (Variable-Length Quantity) encoding for values > 15
// Single byte: 0xxxxxxx (0-127)
// Multi-byte: 1xxxxxxx [continuation...]
typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
} BytecodeBuffer;

// Instruction compressor
typedef struct {
    BytecodeBuffer out;
    uint32_t *reg_freq;    // Register usage frequency (for optimization)
    uint32_t instr_count;
} Compressor;

// Create VLQ-encoded value
static inline uint32_t vlq_encode(uint64_t value, uint8_t *out) {
    uint32_t len = 0;
    while (value >= 128) {
        out[len++] = (value & 0x7F) | 0x80;
        value >>= 7;
    }
    out[len++] = value & 0x7F;
    return len;
}

// Decode VLQ
static inline uint64_t vlq_decode(const uint8_t *in, uint32_t *out_len) {
    uint64_t value = 0;
    uint32_t shift = 0;
    uint32_t i = 0;
    while (1) {
        uint8_t byte = in[i++];
        value |= ((uint64_t)(byte & 0x7F)) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    *out_len = i;
    return value;
}

// Compact instruction format:
// OpCode (4-6 bits) + Operands (VLQ-encoded)
//
// Format 1 (1 byte): OpCode only (for NOP, RET, etc)
//   [OpCode:6 | Unused:2]
//
// Format 2 (2-5 bytes): OpCode + 1 operand
//   [OpCode:6 | Unused:2] [VLQ operand...]
//
// Format 3 (3+ bytes): OpCode + 3 operands (most common)
//   [OpCode:6 | Unused:2] [VLQ dest] [VLQ src1] [VLQ src2]

#ifdef __cplusplus
static inline uint32_t pack_instruction(const Instruction *instr,
                                        uint8_t *out, uint32_t out_cap) {
    uint32_t pos = 0;

    if (pos >= out_cap) return 0;

    // Encode opcode (6 bits)
    uint8_t opcode_byte = (uint8_t)(static_cast<uint8_t>(instr->op) & 0x3F);
    out[pos++] = opcode_byte;

    // Encode operands with VLQ
    if (instr->dest != 0xFF) {  // Has destination
        pos += vlq_encode(instr->dest, &out[pos]);
    }
    if (instr->src1 != 0xFF) {  // Has src1
        pos += vlq_encode(instr->src1, &out[pos]);
    }
    if (instr->src2 != 0xFF) {  // Has src2
        pos += vlq_encode(instr->src2, &out[pos]);
    }

    return pos;
}

static inline Instruction unpack_instruction(const uint8_t *data,
                                             uint32_t *out_len) {
    Instruction instr = {};
    uint32_t pos = 0;
    uint32_t vlq_len = 0;

    instr.op = static_cast<OpCode>(data[pos++] & 0x3F);

    // Decode operands
    if (pos < 256) {  // Sanity check
        instr.dest = (uint8_t)vlq_decode(&data[pos], &vlq_len);
        pos += vlq_len;
    }
    if (pos < 256) {
        instr.src1 = (uint8_t)vlq_decode(&data[pos], &vlq_len);
        pos += vlq_len;
    }
    if (pos < 256) {
        instr.src2 = (uint8_t)vlq_decode(&data[pos], &vlq_len);
        pos += vlq_len;
    }

    *out_len = pos;
    return instr;
}
#endif

// CRC32 checksum
uint32_t crc32(const uint8_t *data, uint32_t len);

// Compiler interface
#ifdef __cplusplus
extern "C" {

Compressor* compressor_new(void);
void compress_instruction(Compressor *c, const Instruction *instr);
int write_dev_file(const char *out_path, Compressor *c, uint8_t trust_level);
void compressor_free(Compressor *c);
void print_compression_stats(Compressor *c);

}
#else

Compressor* compressor_new(void);
void compress_instruction(Compressor *c, const void *instr);
int write_dev_file(const char *out_path, Compressor *c, uint8_t trust_level);
void compressor_free(Compressor *c);
void print_compression_stats(Compressor *c);

#endif

#endif // DEVPACK_H
