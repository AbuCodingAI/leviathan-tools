#pragma once

#include "../ir/instructions.h"
#include "../core/devvm.h"

#include <cstdint>
#include <vector>
#include <functional>
#include <memory>
#include <map>

namespace devvm {
namespace jit {

// i686 (32-bit x86) register enumeration for code generation
enum class I686Register : uint8_t {
  EAX = 0,
  ECX = 1,
  EDX = 2,
  EBX = 3,
  ESP = 4,  // Stack pointer (special)
  EBP = 5,  // Base pointer (special)
  ESI = 6,
  EDI = 7,
};

// i686 condition codes for jumps (same as x86-64)
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

// i686 machine code generator (32-bit x86)
class I686Emitter {
 public:
  I686Emitter();

  // Helper functions for encoding
  void emit_byte(uint8_t byte);
  void emit_word(uint16_t word);
  void emit_dword(uint32_t dword);
  void emit_modrm(uint8_t mod, uint8_t reg, uint8_t rm);
  void emit_sib(uint8_t scale, uint8_t index, uint8_t base);

  // Emit various i686 instructions (32-bit operands)
  void emit_mov_rr(I686Register dst, I686Register src);  // mov dst, src
  void emit_mov_ri(I686Register dst, int32_t imm);       // mov dst, imm
  void emit_mov_rm(I686Register dst, I686Register base, int32_t offset = 0);  // mov dst, [base+offset]
  void emit_mov_mr(I686Register base, int32_t offset, I686Register src);      // mov [base+offset], src

  void emit_add_rr(I686Register dst, I686Register src);  // add dst, src
  void emit_sub_rr(I686Register dst, I686Register src);  // sub dst, src
  void emit_imul_rr(I686Register dst, I686Register src); // imul dst, src
  void emit_idiv_r(I686Register divisor);                // idiv divisor (eax/divisor)

  void emit_and_rr(I686Register dst, I686Register src);  // and dst, src
  void emit_or_rr(I686Register dst, I686Register src);   // or dst, src
  void emit_xor_rr(I686Register dst, I686Register src);  // xor dst, src

  void emit_shl_ri(I686Register dst, uint8_t amount);    // shl dst, amount
  void emit_shr_ri(I686Register dst, uint8_t amount);    // shr dst, amount
  void emit_sar_ri(I686Register dst, uint8_t amount);    // sar dst, amount (arithmetic)

  void emit_neg_r(I686Register dst);                     // neg dst
  void emit_not_r(I686Register dst);                     // not dst

  void emit_cmp_rr(I686Register left, I686Register right);  // cmp left, right
  void emit_jcc(ConditionCode cc, uint32_t target_offset);  // conditional jump
  void emit_jmp(uint32_t target_offset);                 // jmp target

  void emit_call_r(I686Register target);                 // call reg
  void emit_call_imm(uint32_t target);                   // call imm (for relative jumps)
  void emit_ret();                                       // ret

  void emit_push_r(I686Register src);                    // push src
  void emit_pop_r(I686Register dst);                     // pop dst

  // SSE floating-point (xmm0-xmm7 for i686, f64 in xmm)
  void emit_movsd_rr(uint8_t dst_xmm, uint8_t src_xmm);  // movsd xmmD, xmmS
  void emit_addsd_rr(uint8_t dst_xmm, uint8_t src_xmm);  // addsd xmmD, xmmS
  void emit_subsd_rr(uint8_t dst_xmm, uint8_t src_xmm);  // subsd xmmD, xmmS
  void emit_mulsd_rr(uint8_t dst_xmm, uint8_t src_xmm);  // mulsd xmmD, xmmS
  void emit_divsd_rr(uint8_t dst_xmm, uint8_t src_xmm);  // divsd xmmD, xmmS

  void emit_prologue();                                   // Function prologue (push ebp, mov ebp esp)
  void emit_epilogue();                                   // Function epilogue (leave, ret)

  // Get the emitted code
  const std::vector<uint8_t>& code() const { return code_; }
  std::vector<uint8_t> take_code() { return std::move(code_); }
  size_t offset() const { return code_.size(); }

 private:
  std::vector<uint8_t> code_;
};

// JIT Compiler: converts IR sequences to i686 native code
class I686JITCompiler {
 public:
  I686JITCompiler();

  // Compile a sequence of IR instructions to i686
  std::vector<uint8_t> compile(const std::vector<ir::Instruction>& instructions);

  // Compile a single instruction
  void compile_instruction(I686Emitter& emitter, const ir::Instruction& instr);

 private:
  // Register allocation: map VM registers to i686 registers
  I686Register allocate_register(uint8_t vm_reg);
  void spill_register(I686Emitter& emitter, uint8_t vm_reg);

  // State tracking
  std::map<uint8_t, I686Register> register_map_;  // VM reg -> i686 reg mapping
};

}  // namespace jit
}  // namespace devvm
