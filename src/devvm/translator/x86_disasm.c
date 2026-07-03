// x86-64 Disassembler - Minimal Working Version
#include "x86_disasm.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

Disassembler* disasm_init(const uint8_t *code, size_t size, uint64_t base_addr) {
    Disassembler *dis = (Disassembler*)malloc(sizeof(Disassembler));
    if (!dis) return NULL;
    dis->code = code;
    dis->code_size = size;
    dis->offset = 0;
    dis->base_addr = base_addr;
    dis->error = 0;
    dis->error_msg = NULL;
    return dis;
}

static void set_error(Disassembler *dis, const char *msg) {
    dis->error = 1;
    dis->error_msg = msg;
}

int disasm_next(Disassembler *dis, devvm::ir::Instruction *out) {
    if (dis->offset >= dis->code_size) {
        return 0;  // End of code
    }

    memset(out, 0, sizeof(*out));

    uint8_t opcode = dis->code[dis->offset++];

    // Simple instruction mapping
    switch (opcode) {
        case 0x90:  // NOP
            out->op = devvm::ir::OpCode::NOP;
            return 1;

        case 0xC3:  // RET
            out->op = devvm::ir::OpCode::RET;
            return 1;

        case 0x0F:  // Two-byte opcodes
            if (dis->offset < dis->code_size && dis->code[dis->offset] == 0x05) {
                dis->offset++;  // SYSCALL
                out->op = devvm::ir::OpCode::SYSCALL;
                return 1;
            }
            // Unknown two-byte opcode - skip
            dis->offset++;
            out->op = devvm::ir::OpCode::NOP;
            return 1;

        // PUSH (0x50-0x57)
        case 0x50 ... 0x57:
            out->op = devvm::ir::OpCode::PUSH;
            out->src1 = opcode & 7;
            return 1;

        // POP (0x58-0x5F)
        case 0x58 ... 0x5F:
            out->op = devvm::ir::OpCode::POP;
            out->dest = opcode & 7;
            return 1;

        // MOV r32/r64, imm (0xB8-0xBF)
        case 0xB8 ... 0xBF:
            if (dis->offset + 8 <= dis->code_size) {
                out->op = devvm::ir::OpCode::MOVE;
                out->dest = opcode & 7;
                out->src1 = 255;  // Immediate marker
                dis->offset += 8;
                return 1;
            }
            break;

        // 0x89: MOV r/m64, r64
        case 0x89:
            if (dis->offset < dis->code_size) {
                uint8_t modrm = dis->code[dis->offset++];
                uint8_t mod = (modrm >> 6) & 3;
                uint8_t reg = (modrm >> 3) & 7;
                uint8_t rm = modrm & 7;

                out->op = devvm::ir::OpCode::STORE;
                out->dest = rm;
                out->src1 = reg;

                // Skip displacement
                if (mod == 1) dis->offset += 1;
                else if (mod == 2) dis->offset += 4;
                return 1;
            }
            break;

        // 0x8B: MOV r64, r/m64
        case 0x8B:
            if (dis->offset < dis->code_size) {
                uint8_t modrm = dis->code[dis->offset++];
                uint8_t mod = (modrm >> 6) & 3;
                uint8_t reg = (modrm >> 3) & 7;
                uint8_t rm = modrm & 7;

                out->op = devvm::ir::OpCode::LOAD;
                out->dest = reg;
                out->src1 = rm;

                // Skip displacement
                if (mod == 1) dis->offset += 1;
                else if (mod == 2) dis->offset += 4;
                return 1;
            }
            break;

        // Arithmetic: 0x01 (ADD), 0x29 (SUB), 0x21 (AND), 0x09 (OR), 0x31 (XOR)
        case 0x01:
        case 0x29:
        case 0x21:
        case 0x09:
        case 0x31:
            if (dis->offset < dis->code_size) {
                uint8_t modrm = dis->code[dis->offset++];
                uint8_t mod = (modrm >> 6) & 3;
                if (mod == 3) {  // Register-register
                    uint8_t reg = (modrm >> 3) & 7;
                    uint8_t rm = modrm & 7;

                    if (opcode == 0x01) out->op = devvm::ir::OpCode::ADD;
                    else if (opcode == 0x29) out->op = devvm::ir::OpCode::SUB;
                    else if (opcode == 0x21) out->op = devvm::ir::OpCode::AND;
                    else if (opcode == 0x09) out->op = devvm::ir::OpCode::OR;
                    else if (opcode == 0x31) out->op = devvm::ir::OpCode::XOR;

                    out->dest = reg;
                    out->src1 = reg;
                    out->src2 = rm;
                    return 1;
                }
                // Memory operands not yet supported
            }
            break;

        default:
            // Unknown instruction - emit NOP as fallback
            out->op = devvm::ir::OpCode::NOP;
            return 1;
    }

    out->op = devvm::ir::OpCode::NOP;
    return 1;
}

size_t disasm_offset(Disassembler *dis) {
    return dis->offset;
}

const char* disasm_error(Disassembler *dis) {
    return dis->error_msg;
}

void disasm_free(Disassembler *dis) {
    free(dis);
}
