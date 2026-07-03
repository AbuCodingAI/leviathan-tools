#include "i686.h"
#include <cstring>
#include <stdexcept>

namespace devvm {
namespace jit {

I686Emitter::I686Emitter() = default;

void I686Emitter::emit_byte(uint8_t byte) {
  code_.push_back(byte);
}

void I686Emitter::emit_word(uint16_t word) {
  code_.push_back(word & 0xFF);
  code_.push_back((word >> 8) & 0xFF);
}

void I686Emitter::emit_dword(uint32_t dword) {
  code_.push_back(dword & 0xFF);
  code_.push_back((dword >> 8) & 0xFF);
  code_.push_back((dword >> 16) & 0xFF);
  code_.push_back((dword >> 24) & 0xFF);
}

void I686Emitter::emit_modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
  code_.push_back((mod << 6) | (reg << 3) | rm);
}

void I686Emitter::emit_sib(uint8_t scale, uint8_t index, uint8_t base) {
  code_.push_back((scale << 6) | (index << 3) | base);
}

// mov dst, src (register to register, 32-bit)
void I686Emitter::emit_mov_rr(I686Register dst, I686Register src) {
  emit_byte(0x89);  // MOV r/m32, r32
  emit_modrm(0b11, static_cast<uint8_t>(src), static_cast<uint8_t>(dst));
}

// mov dst, imm (32-bit immediate)
void I686Emitter::emit_mov_ri(I686Register dst, int32_t imm) {
  emit_byte(0xB8 + static_cast<uint8_t>(dst));  // MOV r32, imm32
  emit_dword(imm);
}

// mov dst, [base+offset] (load from memory)
void I686Emitter::emit_mov_rm(I686Register dst, I686Register base, int32_t offset) {
  emit_byte(0x8B);  // MOV r32, r/m32
  if (offset == 0 && base != I686Register::EBP) {
    emit_modrm(0b00, static_cast<uint8_t>(dst), static_cast<uint8_t>(base));
  } else if (offset >= -128 && offset <= 127) {
    emit_modrm(0b01, static_cast<uint8_t>(dst), static_cast<uint8_t>(base));
    emit_byte(offset & 0xFF);
  } else {
    emit_modrm(0b10, static_cast<uint8_t>(dst), static_cast<uint8_t>(base));
    emit_dword(offset);
  }
}

// mov [base+offset], src (store to memory)
void I686Emitter::emit_mov_mr(I686Register base, int32_t offset, I686Register src) {
  emit_byte(0x89);  // MOV r/m32, r32
  if (offset == 0 && base != I686Register::EBP) {
    emit_modrm(0b00, static_cast<uint8_t>(src), static_cast<uint8_t>(base));
  } else if (offset >= -128 && offset <= 127) {
    emit_modrm(0b01, static_cast<uint8_t>(src), static_cast<uint8_t>(base));
    emit_byte(offset & 0xFF);
  } else {
    emit_modrm(0b10, static_cast<uint8_t>(src), static_cast<uint8_t>(base));
    emit_dword(offset);
  }
}

// add dst, src
void I686Emitter::emit_add_rr(I686Register dst, I686Register src) {
  emit_byte(0x01);  // ADD r/m32, r32
  emit_modrm(0b11, static_cast<uint8_t>(src), static_cast<uint8_t>(dst));
}

// sub dst, src
void I686Emitter::emit_sub_rr(I686Register dst, I686Register src) {
  emit_byte(0x29);  // SUB r/m32, r32
  emit_modrm(0b11, static_cast<uint8_t>(src), static_cast<uint8_t>(dst));
}

// imul dst, src (32-bit signed multiply)
void I686Emitter::emit_imul_rr(I686Register dst, I686Register src) {
  emit_byte(0x0F);
  emit_byte(0xAF);  // IMUL r32, r/m32
  emit_modrm(0b11, static_cast<uint8_t>(dst), static_cast<uint8_t>(src));
}

// idiv divisor (eax / divisor, result in eax, remainder in edx)
void I686Emitter::emit_idiv_r(I686Register divisor) {
  emit_byte(0xF7);  // IDIV r/m32
  emit_modrm(0b11, 0b111, static_cast<uint8_t>(divisor));
}

// and dst, src
void I686Emitter::emit_and_rr(I686Register dst, I686Register src) {
  emit_byte(0x21);  // AND r/m32, r32
  emit_modrm(0b11, static_cast<uint8_t>(src), static_cast<uint8_t>(dst));
}

// or dst, src
void I686Emitter::emit_or_rr(I686Register dst, I686Register src) {
  emit_byte(0x09);  // OR r/m32, r32
  emit_modrm(0b11, static_cast<uint8_t>(src), static_cast<uint8_t>(dst));
}

// xor dst, src
void I686Emitter::emit_xor_rr(I686Register dst, I686Register src) {
  emit_byte(0x31);  // XOR r/m32, r32
  emit_modrm(0b11, static_cast<uint8_t>(src), static_cast<uint8_t>(dst));
}

// shl dst, amount (shift left)
void I686Emitter::emit_shl_ri(I686Register dst, uint8_t amount) {
  emit_byte(0xC1);  // SHL r/m32, imm8
  emit_modrm(0b11, 0b100, static_cast<uint8_t>(dst));
  emit_byte(amount);
}

// shr dst, amount (logical shift right)
void I686Emitter::emit_shr_ri(I686Register dst, uint8_t amount) {
  emit_byte(0xC1);  // SHR r/m32, imm8
  emit_modrm(0b11, 0b101, static_cast<uint8_t>(dst));
  emit_byte(amount);
}

// sar dst, amount (arithmetic shift right)
void I686Emitter::emit_sar_ri(I686Register dst, uint8_t amount) {
  emit_byte(0xC1);  // SAR r/m32, imm8
  emit_modrm(0b11, 0b111, static_cast<uint8_t>(dst));
  emit_byte(amount);
}

// neg dst (two's complement negate)
void I686Emitter::emit_neg_r(I686Register dst) {
  emit_byte(0xF7);  // NEG r/m32
  emit_modrm(0b11, 0b011, static_cast<uint8_t>(dst));
}

// not dst (bitwise NOT)
void I686Emitter::emit_not_r(I686Register dst) {
  emit_byte(0xF7);  // NOT r/m32
  emit_modrm(0b11, 0b010, static_cast<uint8_t>(dst));
}

// cmp left, right (compare, sets flags)
void I686Emitter::emit_cmp_rr(I686Register left, I686Register right) {
  emit_byte(0x39);  // CMP r/m32, r32
  emit_modrm(0b11, static_cast<uint8_t>(right), static_cast<uint8_t>(left));
}

// conditional jump (short range, -128 to +127)
void I686Emitter::emit_jcc(ConditionCode cc, uint32_t target_offset) {
  int32_t offset = static_cast<int32_t>(target_offset) - static_cast<int32_t>(code_.size()) - 2;
  emit_byte(0x70 | static_cast<uint8_t>(cc));  // Jcc rel8
  emit_byte(offset & 0xFF);
}

// unconditional jump (short range)
void I686Emitter::emit_jmp(uint32_t target_offset) {
  int32_t offset = static_cast<int32_t>(target_offset) - static_cast<int32_t>(code_.size()) - 2;
  emit_byte(0xEB);  // JMP rel8
  emit_byte(offset & 0xFF);
}

// call reg (call a register)
void I686Emitter::emit_call_r(I686Register target) {
  emit_byte(0xFF);  // CALL r/m32
  emit_modrm(0b11, 0b010, static_cast<uint8_t>(target));
}

// call imm (call immediate address, relative)
void I686Emitter::emit_call_imm(uint32_t target) {
  int32_t offset = static_cast<int32_t>(target) - static_cast<int32_t>(code_.size()) - 5;
  emit_byte(0xE8);  // CALL rel32
  emit_dword(offset);
}

// ret (return from subroutine)
void I686Emitter::emit_ret() {
  emit_byte(0xC3);  // RET
}

// push src (push to stack)
void I686Emitter::emit_push_r(I686Register src) {
  emit_byte(0x50 + static_cast<uint8_t>(src));  // PUSH r32
}

// pop dst (pop from stack)
void I686Emitter::emit_pop_r(I686Register dst) {
  emit_byte(0x58 + static_cast<uint8_t>(dst));  // POP r32
}

// movsd xmmD, xmmS (SSE2: move 64-bit float)
void I686Emitter::emit_movsd_rr(uint8_t dst_xmm, uint8_t src_xmm) {
  emit_byte(0xF2);  // SSE2 prefix
  emit_byte(0x0F);
  emit_byte(0x10);  // MOVSD xmm, xmm
  emit_modrm(0b11, dst_xmm, src_xmm);
}

// addsd xmmD, xmmS
void I686Emitter::emit_addsd_rr(uint8_t dst_xmm, uint8_t src_xmm) {
  emit_byte(0xF2);
  emit_byte(0x0F);
  emit_byte(0x58);  // ADDSD xmm, xmm
  emit_modrm(0b11, dst_xmm, src_xmm);
}

// subsd xmmD, xmmS
void I686Emitter::emit_subsd_rr(uint8_t dst_xmm, uint8_t src_xmm) {
  emit_byte(0xF2);
  emit_byte(0x0F);
  emit_byte(0x5C);  // SUBSD xmm, xmm
  emit_modrm(0b11, dst_xmm, src_xmm);
}

// mulsd xmmD, xmmS
void I686Emitter::emit_mulsd_rr(uint8_t dst_xmm, uint8_t src_xmm) {
  emit_byte(0xF2);
  emit_byte(0x0F);
  emit_byte(0x59);  // MULSD xmm, xmm
  emit_modrm(0b11, dst_xmm, src_xmm);
}

// divsd xmmD, xmmS
void I686Emitter::emit_divsd_rr(uint8_t dst_xmm, uint8_t src_xmm) {
  emit_byte(0xF2);
  emit_byte(0x0F);
  emit_byte(0x5E);  // DIVSD xmm, xmm
  emit_modrm(0b11, dst_xmm, src_xmm);
}

// prologue: push ebp, mov ebp esp
void I686Emitter::emit_prologue() {
  emit_push_r(I686Register::EBP);
  emit_mov_rr(I686Register::EBP, I686Register::ESP);
}

// epilogue: leave (mov esp ebp, pop ebp), ret
void I686Emitter::emit_epilogue() {
  emit_byte(0xC9);  // LEAVE
  emit_ret();
}

// ─────────────────────────────────────────────────────────────

I686JITCompiler::I686JITCompiler() = default;

std::vector<uint8_t> I686JITCompiler::compile(const std::vector<ir::Instruction>& instructions) {
  I686Emitter emitter;
  emitter.emit_prologue();

  for (const auto& instr : instructions) {
    compile_instruction(emitter, instr);
  }

  emitter.emit_epilogue();
  return emitter.take_code();
}

void I686JITCompiler::compile_instruction(I686Emitter& emitter, const ir::Instruction& instr) {
  // Map IR OpCodes to i686 machine code
  switch (instr.op) {
    case ir::OpCode::MOVE: {
      auto src_reg = allocate_register(instr.src1);
      auto dst_reg = allocate_register(instr.dest);
      emitter.emit_mov_rr(dst_reg, src_reg);
      break;
    }
    case ir::OpCode::ADD: {
      auto src1_reg = allocate_register(instr.src1);
      auto src2_reg = allocate_register(instr.src2);
      auto dst_reg = allocate_register(instr.dest);
      emitter.emit_mov_rr(dst_reg, src1_reg);
      emitter.emit_add_rr(dst_reg, src2_reg);
      break;
    }
    case ir::OpCode::SUB: {
      auto src1_reg = allocate_register(instr.src1);
      auto src2_reg = allocate_register(instr.src2);
      auto dst_reg = allocate_register(instr.dest);
      emitter.emit_mov_rr(dst_reg, src1_reg);
      emitter.emit_sub_rr(dst_reg, src2_reg);
      break;
    }
    case ir::OpCode::MUL: {
      auto src1_reg = allocate_register(instr.src1);
      auto src2_reg = allocate_register(instr.src2);
      auto dst_reg = allocate_register(instr.dest);
      emitter.emit_mov_rr(dst_reg, src1_reg);
      emitter.emit_imul_rr(dst_reg, src2_reg);
      break;
    }
    case ir::OpCode::HALT:
      // HALT: just return
      break;
    default:
      // Silently skip unsupported opcodes for now
      break;
  }
}

I686Register I686JITCompiler::allocate_register(uint8_t vm_reg) {
  if (register_map_.count(vm_reg)) {
    return register_map_[vm_reg];
  }

  // Simple allocation: use available registers in order
  static const I686Register available[] = {
    I686Register::EAX, I686Register::ECX, I686Register::EDX,
    I686Register::EBX, I686Register::ESI, I686Register::EDI
  };

  for (auto reg : available) {
    bool in_use = false;
    for (const auto& [vm, allocated] : register_map_) {
      if (allocated == reg) {
        in_use = true;
        break;
      }
    }
    if (!in_use) {
      register_map_[vm_reg] = reg;
      return reg;
    }
  }

  throw std::runtime_error("No available i686 registers for allocation");
}

void I686JITCompiler::spill_register(I686Emitter& emitter, uint8_t vm_reg) {
  // Spill to stack: mov [esp-offset], reg
  if (register_map_.count(vm_reg)) {
    auto reg = register_map_[vm_reg];
    emitter.emit_mov_mr(I686Register::ESP, -4, reg);
    register_map_.erase(vm_reg);
  }
}

}  // namespace jit
}  // namespace devvm
