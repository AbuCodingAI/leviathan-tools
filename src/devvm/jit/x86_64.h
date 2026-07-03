#pragma once

#include "../ir/instructions.h"
#include "../core/devvm.h"

#include <cstdint>
#include <vector>
#include <functional>
#include <memory>

namespace devvm {
namespace jit {

// x86-64 register enumeration for code generation
enum class X86Register : uint8_t {
  RAX = 0,
  RCX = 1,
  RDX = 2,
  RBX = 3,
  RSP = 4,
  RBP = 5,
  RSI = 6,
  RDI = 7,
  R8 = 8,
  R9 = 9,
  R10 = 10,
  R11 = 11,
  R12 = 12,
  R13 = 13,
  R14 = 14,
  R15 = 15,
};

// x86-64 condition codes for jumps
enum class ConditionCode : uint8_t {
  JZ = 0x74,   // Jump if zero
  JNZ = 0x75,  // Jump if not zero
  JL = 0x7C,   // Jump if less
  JLE = 0x7E,  // Jump if less or equal
  JG = 0x7F,   // Jump if greater
  JGE = 0x7D,  // Jump if greater or equal
  JE = 0x74,   // Jump if equal
  JNE = 0x75,  // Jump if not equal
};

// x86-64 machine code generator
class X86Emitter {
 public:
  X86Emitter();

  // Helper functions for encoding (public for JIT compiler)
  void emit_byte(uint8_t byte);
  void emit_word(uint16_t word);
  void emit_dword(uint32_t dword);
  void emit_qword(uint64_t qword);
  void emit_rex(bool w, bool r, bool x, bool b);
  void emit_modrm(uint8_t mod, uint8_t reg, uint8_t rm);
  void emit_sib(uint8_t scale, uint8_t index, uint8_t base);

  // Emit various x86-64 instructions
  void emit_mov_rr(X86Register dst, X86Register src);  // mov dst, src
  void emit_mov_ri(X86Register dst, int64_t imm);      // mov dst, imm
  void emit_mov_rm(X86Register dst, X86Register base, int32_t offset = 0);  // mov dst, [base+offset]
  void emit_mov_mr(X86Register base, int32_t offset, X86Register src);      // mov [base+offset], src

  void emit_add_rr(X86Register dst, X86Register src);  // add dst, src
  void emit_sub_rr(X86Register dst, X86Register src);  // sub dst, src
  void emit_imul_rr(X86Register dst, X86Register src); // imul dst, src
  void emit_idiv_r(X86Register divisor);               // idiv divisor (rax/divisor)

  void emit_and_rr(X86Register dst, X86Register src);  // and dst, src
  void emit_or_rr(X86Register dst, X86Register src);   // or dst, src
  void emit_xor_rr(X86Register dst, X86Register src);  // xor dst, src

  void emit_shl_ri(X86Register dst, uint8_t amount);   // shl dst, amount
  void emit_shr_ri(X86Register dst, uint8_t amount);   // shr dst, amount
  void emit_sar_ri(X86Register dst, uint8_t amount);   // sar dst, amount (arithmetic)

  void emit_neg_r(X86Register dst);                    // neg dst
  void emit_not_r(X86Register dst);                    // not dst

  void emit_cmp_rr(X86Register left, X86Register right);  // cmp left, right
  void emit_jcc(ConditionCode cc, uint32_t target_offset); // conditional jump
  void emit_jmp(uint32_t target_offset);               // jmp target

  void emit_call_r(X86Register target);                // call reg
  void emit_ret();                                     // ret

  void emit_push_r(X86Register src);                   // push src
  void emit_pop_r(X86Register dst);                    // pop dst

  // SSE floating-point (xmm0-xmm15 for f64)
  void emit_movsd_rr(uint8_t dst_xmm, uint8_t src_xmm);  // movsd xmmD, xmmS
  void emit_addsd_rr(uint8_t dst_xmm, uint8_t src_xmm);  // addsd xmmD, xmmS
  void emit_subsd_rr(uint8_t dst_xmm, uint8_t src_xmm);  // subsd xmmD, xmmS
  void emit_mulsd_rr(uint8_t dst_xmm, uint8_t src_xmm);  // mulsd xmmD, xmmS
  void emit_divsd_rr(uint8_t dst_xmm, uint8_t src_xmm);  // divsd xmmD, xmmS

  void emit_prologue();                                // Function prologue (push rbp, mov rbp rsp)
  void emit_epilogue();                                // Function epilogue (leave, ret)

  // Get the emitted code
  const std::vector<uint8_t>& code() const { return code_; }
  std::vector<uint8_t> take_code() { return std::move(code_); }
  size_t offset() const { return code_.size(); }

 private:
  std::vector<uint8_t> code_;
};

// JIT Compiler: converts IR sequences to x86-64 native code
class JITCompiler {
 public:
  JITCompiler();

  // Compile a sequence of IR instructions to x86-64
  std::vector<uint8_t> compile(const std::vector<ir::Instruction>& instructions);

  // Compile a single instruction
  void compile_instruction(X86Emitter& emitter, const ir::Instruction& instr);

 private:
  // Register allocation: map VM registers to x86-64 registers
  X86Register allocate_register(uint8_t vm_reg);
  void spill_register(X86Emitter& emitter, uint8_t vm_reg);

  // State tracking
  std::map<uint8_t, X86Register> register_map_;  // VM reg -> x86 reg mapping
};

}  // namespace jit
}  // namespace devvm
