#include "x86_64.h"

#include <stdexcept>

namespace devvm {
namespace jit {

// ============================================================================
// X86Emitter: Low-level machine code generation
// ============================================================================

X86Emitter::X86Emitter() = default;

void X86Emitter::emit_byte(uint8_t byte) {
  code_.push_back(byte);
}

void X86Emitter::emit_word(uint16_t word) {
  code_.push_back(static_cast<uint8_t>(word & 0xFF));
  code_.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
}

void X86Emitter::emit_dword(uint32_t dword) {
  code_.push_back(static_cast<uint8_t>(dword & 0xFF));
  code_.push_back(static_cast<uint8_t>((dword >> 8) & 0xFF));
  code_.push_back(static_cast<uint8_t>((dword >> 16) & 0xFF));
  code_.push_back(static_cast<uint8_t>((dword >> 24) & 0xFF));
}

void X86Emitter::emit_qword(uint64_t qword) {
  emit_dword(static_cast<uint32_t>(qword & 0xFFFFFFFF));
  emit_dword(static_cast<uint32_t>((qword >> 32) & 0xFFFFFFFF));
}

void X86Emitter::emit_rex(bool w, bool r, bool x, bool b) {
  uint8_t rex = 0x40;
  if (w) rex |= 0x08;
  if (r) rex |= 0x04;
  if (x) rex |= 0x02;
  if (b) rex |= 0x01;
  emit_byte(rex);
}

void X86Emitter::emit_modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
  emit_byte((mod << 6) | (reg << 3) | rm);
}

void X86Emitter::emit_sib(uint8_t scale, uint8_t index, uint8_t base) {
  emit_byte((scale << 6) | (index << 3) | base);
}

// mov dst, src (64-bit register to register)
void X86Emitter::emit_mov_rr(X86Register dst, X86Register src) {
  bool src_high = static_cast<uint8_t>(src) >= 8;
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, dst_high, false, src_high);
  emit_byte(0x89);  // MOV r64, r64
  emit_modrm(3, static_cast<uint8_t>(src) & 7, static_cast<uint8_t>(dst) & 7);
}

// mov dst, imm64
void X86Emitter::emit_mov_ri(X86Register dst, int64_t imm) {
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, false, false, dst_high);
  emit_byte(0xB8 + (static_cast<uint8_t>(dst) & 7));
  emit_qword(static_cast<uint64_t>(imm));
}

// mov dst, [base + offset]
void X86Emitter::emit_mov_rm(X86Register dst, X86Register base, int32_t offset) {
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  bool base_high = static_cast<uint8_t>(base) >= 8;
  emit_rex(true, dst_high, false, base_high);
  emit_byte(0x8B);  // MOV r64, r/m64

  if (offset == 0 && base != X86Register::RBP && base != X86Register::R13) {
    emit_modrm(0, static_cast<uint8_t>(dst) & 7, static_cast<uint8_t>(base) & 7);
  } else if (offset >= -128 && offset <= 127) {
    emit_modrm(1, static_cast<uint8_t>(dst) & 7, static_cast<uint8_t>(base) & 7);
    emit_byte(static_cast<uint8_t>(offset));
  } else {
    emit_modrm(2, static_cast<uint8_t>(dst) & 7, static_cast<uint8_t>(base) & 7);
    emit_dword(static_cast<uint32_t>(offset));
  }
}

// mov [base + offset], src
void X86Emitter::emit_mov_mr(X86Register base, int32_t offset, X86Register src) {
  bool base_high = static_cast<uint8_t>(base) >= 8;
  bool src_high = static_cast<uint8_t>(src) >= 8;
  emit_rex(true, src_high, false, base_high);
  emit_byte(0x89);  // MOV r/m64, r64

  if (offset == 0 && base != X86Register::RBP && base != X86Register::R13) {
    emit_modrm(0, static_cast<uint8_t>(src) & 7, static_cast<uint8_t>(base) & 7);
  } else if (offset >= -128 && offset <= 127) {
    emit_modrm(1, static_cast<uint8_t>(src) & 7, static_cast<uint8_t>(base) & 7);
    emit_byte(static_cast<uint8_t>(offset));
  } else {
    emit_modrm(2, static_cast<uint8_t>(src) & 7, static_cast<uint8_t>(base) & 7);
    emit_dword(static_cast<uint32_t>(offset));
  }
}

// add dst, src
void X86Emitter::emit_add_rr(X86Register dst, X86Register src) {
  bool src_high = static_cast<uint8_t>(src) >= 8;
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, dst_high, false, src_high);
  emit_byte(0x01);  // ADD r/m64, r64
  emit_modrm(3, static_cast<uint8_t>(src) & 7, static_cast<uint8_t>(dst) & 7);
}

// sub dst, src
void X86Emitter::emit_sub_rr(X86Register dst, X86Register src) {
  bool src_high = static_cast<uint8_t>(src) >= 8;
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, dst_high, false, src_high);
  emit_byte(0x29);  // SUB r/m64, r64
  emit_modrm(3, static_cast<uint8_t>(src) & 7, static_cast<uint8_t>(dst) & 7);
}

// imul dst, src (sign-extend multiply)
void X86Emitter::emit_imul_rr(X86Register dst, X86Register src) {
  bool src_high = static_cast<uint8_t>(src) >= 8;
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, dst_high, false, src_high);
  emit_word(0xAF0F);  // IMUL r64, r/m64
  emit_modrm(3, static_cast<uint8_t>(dst) & 7, static_cast<uint8_t>(src) & 7);
}

// idiv divisor (divides rdx:rax by divisor, quotient in rax, remainder in rdx)
void X86Emitter::emit_idiv_r(X86Register divisor) {
  bool div_high = static_cast<uint8_t>(divisor) >= 8;
  emit_rex(true, false, false, div_high);
  emit_byte(0xF7);  // IDIV r/m64
  emit_modrm(3, 7, static_cast<uint8_t>(divisor) & 7);
}

// and dst, src
void X86Emitter::emit_and_rr(X86Register dst, X86Register src) {
  bool src_high = static_cast<uint8_t>(src) >= 8;
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, dst_high, false, src_high);
  emit_byte(0x21);  // AND r/m64, r64
  emit_modrm(3, static_cast<uint8_t>(src) & 7, static_cast<uint8_t>(dst) & 7);
}

// or dst, src
void X86Emitter::emit_or_rr(X86Register dst, X86Register src) {
  bool src_high = static_cast<uint8_t>(src) >= 8;
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, dst_high, false, src_high);
  emit_byte(0x09);  // OR r/m64, r64
  emit_modrm(3, static_cast<uint8_t>(src) & 7, static_cast<uint8_t>(dst) & 7);
}

// xor dst, src
void X86Emitter::emit_xor_rr(X86Register dst, X86Register src) {
  bool src_high = static_cast<uint8_t>(src) >= 8;
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, dst_high, false, src_high);
  emit_byte(0x31);  // XOR r/m64, r64
  emit_modrm(3, static_cast<uint8_t>(src) & 7, static_cast<uint8_t>(dst) & 7);
}

// shl dst, amount
void X86Emitter::emit_shl_ri(X86Register dst, uint8_t amount) {
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, false, false, dst_high);
  emit_byte(0xC1);  // SHL r/m64, imm8
  emit_modrm(3, 4, static_cast<uint8_t>(dst) & 7);
  emit_byte(amount);
}

// shr dst, amount
void X86Emitter::emit_shr_ri(X86Register dst, uint8_t amount) {
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, false, false, dst_high);
  emit_byte(0xC1);  // SHR r/m64, imm8
  emit_modrm(3, 5, static_cast<uint8_t>(dst) & 7);
  emit_byte(amount);
}

// sar dst, amount (arithmetic shift right)
void X86Emitter::emit_sar_ri(X86Register dst, uint8_t amount) {
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, false, false, dst_high);
  emit_byte(0xC1);  // SAR r/m64, imm8
  emit_modrm(3, 7, static_cast<uint8_t>(dst) & 7);
  emit_byte(amount);
}

// neg dst
void X86Emitter::emit_neg_r(X86Register dst) {
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, false, false, dst_high);
  emit_byte(0xF7);  // NEG r/m64
  emit_modrm(3, 3, static_cast<uint8_t>(dst) & 7);
}

// not dst
void X86Emitter::emit_not_r(X86Register dst) {
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  emit_rex(true, false, false, dst_high);
  emit_byte(0xF7);  // NOT r/m64
  emit_modrm(3, 2, static_cast<uint8_t>(dst) & 7);
}

// cmp left, right
void X86Emitter::emit_cmp_rr(X86Register left, X86Register right) {
  bool left_high = static_cast<uint8_t>(left) >= 8;
  bool right_high = static_cast<uint8_t>(right) >= 8;
  emit_rex(true, right_high, false, left_high);
  emit_byte(0x39);  // CMP r/m64, r64
  emit_modrm(3, static_cast<uint8_t>(right) & 7, static_cast<uint8_t>(left) & 7);
}

// Conditional jump (2-byte encoding)
void X86Emitter::emit_jcc(ConditionCode cc, uint32_t target_offset) {
  int32_t delta = static_cast<int32_t>(target_offset) - static_cast<int32_t>(offset()) - 6;
  emit_byte(0x0F);
  emit_byte(static_cast<uint8_t>(cc));
  emit_dword(static_cast<uint32_t>(delta));
}

// Unconditional jump
void X86Emitter::emit_jmp(uint32_t target_offset) {
  int32_t delta = static_cast<int32_t>(target_offset) - static_cast<int32_t>(offset()) - 5;
  emit_byte(0xE9);
  emit_dword(static_cast<uint32_t>(delta));
}

// call reg
void X86Emitter::emit_call_r(X86Register target) {
  bool target_high = static_cast<uint8_t>(target) >= 8;
  emit_rex(false, false, false, target_high);
  emit_byte(0xFF);  // CALL r/m64
  emit_modrm(3, 2, static_cast<uint8_t>(target) & 7);
}

// ret
void X86Emitter::emit_ret() {
  emit_byte(0xC3);
}

// push src
void X86Emitter::emit_push_r(X86Register src) {
  bool src_high = static_cast<uint8_t>(src) >= 8;
  if (src_high) {
    emit_rex(false, false, false, true);
  }
  emit_byte(0x50 + (static_cast<uint8_t>(src) & 7));
}

// pop dst
void X86Emitter::emit_pop_r(X86Register dst) {
  bool dst_high = static_cast<uint8_t>(dst) >= 8;
  if (dst_high) {
    emit_rex(false, false, false, true);
  }
  emit_byte(0x58 + (static_cast<uint8_t>(dst) & 7));
}

// SSE floating-point operations (for f64 / xmmN registers)
void X86Emitter::emit_movsd_rr(uint8_t dst_xmm, uint8_t src_xmm) {
  emit_byte(0xF2);  // Prefix for double-precision
  emit_byte(0x0F);  // Two-byte opcode
  emit_byte(0x10);  // MOVSD
  emit_modrm(3, dst_xmm & 7, src_xmm & 7);
}

void X86Emitter::emit_addsd_rr(uint8_t dst_xmm, uint8_t src_xmm) {
  emit_byte(0xF2);  // Prefix for double-precision
  emit_byte(0x0F);  // Two-byte opcode
  emit_byte(0x58);  // ADDSD
  emit_modrm(3, dst_xmm & 7, src_xmm & 7);
}

void X86Emitter::emit_subsd_rr(uint8_t dst_xmm, uint8_t src_xmm) {
  emit_byte(0xF2);  // Prefix for double-precision
  emit_byte(0x0F);  // Two-byte opcode
  emit_byte(0x5C);  // SUBSD
  emit_modrm(3, dst_xmm & 7, src_xmm & 7);
}

void X86Emitter::emit_mulsd_rr(uint8_t dst_xmm, uint8_t src_xmm) {
  emit_byte(0xF2);  // Prefix for double-precision
  emit_byte(0x0F);  // Two-byte opcode
  emit_byte(0x59);  // MULSD
  emit_modrm(3, dst_xmm & 7, src_xmm & 7);
}

void X86Emitter::emit_divsd_rr(uint8_t dst_xmm, uint8_t src_xmm) {
  emit_byte(0xF2);  // Prefix for double-precision
  emit_byte(0x0F);  // Two-byte opcode
  emit_byte(0x5E);  // DIVSD
  emit_modrm(3, dst_xmm & 7, src_xmm & 7);
}

// Function prologue: push rbp, mov rbp rsp
void X86Emitter::emit_prologue() {
  emit_push_r(X86Register::RBP);
  emit_mov_rr(X86Register::RBP, X86Register::RSP);
}

// Function epilogue: leave (pop rbp), ret
void X86Emitter::emit_epilogue() {
  emit_byte(0xC9);  // LEAVE
  emit_ret();
}

// ============================================================================
// JITCompiler: IR to x86-64 translation
// ============================================================================

JITCompiler::JITCompiler() = default;

X86Register JITCompiler::allocate_register(uint8_t vm_reg) {
  if (register_map_.count(vm_reg)) {
    return register_map_[vm_reg];
  }

  // Simple allocation: use first available register
  static const X86Register available[] = {
      X86Register::RCX, X86Register::RDX, X86Register::RSI,
      X86Register::RDI, X86Register::R8,  X86Register::R9,
      X86Register::R10, X86Register::R11, X86Register::R12,
      X86Register::R13, X86Register::R14, X86Register::R15,
  };

  for (auto reg : available) {
    bool used = false;
    for (const auto& [_, mapped] : register_map_) {
      if (mapped == reg) {
        used = true;
        break;
      }
    }
    if (!used) {
      register_map_[vm_reg] = reg;
      return reg;
    }
  }

  // Fallback: spill and reuse RAX
  throw std::runtime_error("Register allocation failed: out of registers");
}

void JITCompiler::spill_register(X86Emitter& /* emitter */, uint8_t vm_reg) {
  if (register_map_.count(vm_reg)) {
    register_map_.erase(vm_reg);
  }
}

void JITCompiler::compile_instruction(X86Emitter& emitter, const ir::Instruction& instr) {
  using namespace ir;

  switch (instr.op) {
    case OpCode::NOP:
      // No operation
      break;

    case OpCode::HALT:
      emitter.emit_ret();
      break;

    case OpCode::ADD: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      if (dst != src1) {
        emitter.emit_mov_rr(dst, src1);
      }
      emitter.emit_add_rr(dst, src2);
      break;
    }

    case OpCode::SUB: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      if (dst != src1) {
        emitter.emit_mov_rr(dst, src1);
      }
      emitter.emit_sub_rr(dst, src2);
      break;
    }

    case OpCode::MUL: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      if (dst != X86Register::RAX) {
        emitter.emit_mov_rr(X86Register::RAX, src1);
      }
      emitter.emit_imul_rr(X86Register::RAX, src2);
      if (dst != X86Register::RAX) {
        emitter.emit_mov_rr(dst, X86Register::RAX);
      }
      break;
    }

    case OpCode::DIV: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      // Move dividend to RAX
      if (src1 != X86Register::RAX) {
        emitter.emit_mov_rr(X86Register::RAX, src1);
      }
      // Sign-extend to RDX:RAX
      emitter.emit_byte(0x48);  // REX.W
      emitter.emit_byte(0x99);  // CQO (sign-extend RAX to RDX:RAX)
      // Divide
      emitter.emit_idiv_r(src2);
      // Result in RAX
      if (dst != X86Register::RAX) {
        emitter.emit_mov_rr(dst, X86Register::RAX);
      }
      break;
    }

    case OpCode::MOD: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      // Move dividend to RAX
      if (src1 != X86Register::RAX) {
        emitter.emit_mov_rr(X86Register::RAX, src1);
      }
      // Sign-extend to RDX:RAX
      emitter.emit_byte(0x48);  // REX.W
      emitter.emit_byte(0x99);  // CQO
      // Divide
      emitter.emit_idiv_r(src2);
      // Result in RDX
      if (dst != X86Register::RDX) {
        emitter.emit_mov_rr(dst, X86Register::RDX);
      }
      break;
    }

    case OpCode::AND: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      if (dst != src1) {
        emitter.emit_mov_rr(dst, src1);
      }
      emitter.emit_and_rr(dst, src2);
      break;
    }

    case OpCode::OR: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      if (dst != src1) {
        emitter.emit_mov_rr(dst, src1);
      }
      emitter.emit_or_rr(dst, src2);
      break;
    }

    case OpCode::XOR: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      if (dst != src1) {
        emitter.emit_mov_rr(dst, src1);
      }
      emitter.emit_xor_rr(dst, src2);
      break;
    }

    case OpCode::SHL: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      if (dst != src1) {
        emitter.emit_mov_rr(dst, src1);
      }
      if (src2 != X86Register::RCX) {
        emitter.emit_mov_rr(X86Register::RCX, src2);
      }
      emitter.emit_byte(0x48);  // REX.W
      emitter.emit_byte(0xD3);  // SHL r/m64, CL
      emitter.emit_modrm(3, 4, static_cast<uint8_t>(dst) & 7);
      break;
    }

    case OpCode::SHR: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      if (dst != src1) {
        emitter.emit_mov_rr(dst, src1);
      }
      if (src2 != X86Register::RCX) {
        emitter.emit_mov_rr(X86Register::RCX, src2);
      }
      emitter.emit_byte(0x48);  // REX.W
      emitter.emit_byte(0xD3);  // SHR r/m64, CL
      emitter.emit_modrm(3, 5, static_cast<uint8_t>(dst) & 7);
      break;
    }

    case OpCode::NEG: {
      auto src1 = allocate_register(instr.src1);
      auto dst = allocate_register(instr.dest);
      if (dst != src1) {
        emitter.emit_mov_rr(dst, src1);
      }
      emitter.emit_neg_r(dst);
      break;
    }

    case OpCode::NOT: {
      auto src1 = allocate_register(instr.src1);
      auto dst = allocate_register(instr.dest);
      if (dst != src1) {
        emitter.emit_mov_rr(dst, src1);
      }
      emitter.emit_not_r(dst);
      break;
    }

    case OpCode::MOVE: {
      auto src1 = allocate_register(instr.src1);
      auto dst = allocate_register(instr.dest);
      if (dst != src1) {
        emitter.emit_mov_rr(dst, src1);
      }
      break;
    }

    case OpCode::LOAD: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      auto addr = allocate_register(255);  // Temporary
      emitter.emit_mov_rr(addr, src1);
      emitter.emit_add_rr(addr, src2);
      emitter.emit_mov_rm(dst, addr);
      spill_register(emitter, 255);
      break;
    }

    case OpCode::STORE: {
      auto src1 = allocate_register(instr.src1);
      auto src2 = allocate_register(instr.src2);
      auto dst = allocate_register(instr.dest);
      auto addr = allocate_register(255);  // Temporary
      emitter.emit_mov_rr(addr, dst);
      emitter.emit_add_rr(addr, src2);
      emitter.emit_mov_mr(addr, 0, src1);
      spill_register(emitter, 255);
      break;
    }

    default:
      // Unsupported instruction in JIT - would need interpreter fallback
      break;
  }
}

std::vector<uint8_t> JITCompiler::compile(const std::vector<ir::Instruction>& instructions) {
  X86Emitter emitter;

  emitter.emit_prologue();

  for (const auto& instr : instructions) {
    compile_instruction(emitter, instr);
  }

  emitter.emit_epilogue();

  return emitter.take_code();
}

}  // namespace jit
}  // namespace devvm
