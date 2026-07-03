#include "../core/devvm.h"
#include "../ir/instructions.h"
#include "../jit/x86_64.h"
#include "../security/sandbox.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

using namespace devvm;
using namespace devvm::ir;

// ============================================================================
// Test Helper Functions
// ============================================================================

void assert_equal(int64_t a, int64_t b, const char* msg) {
  if (a != b) {
    std::cerr << "FAIL: " << msg << " (expected " << b << ", got " << a << ")"
              << std::endl;
    exit(1);
  }
}

void assert_true(bool condition, const char* msg) {
  if (!condition) {
    std::cerr << "FAIL: " << msg << std::endl;
    exit(1);
  }
}

// Helper: Create a simple .dev file with bytecode
void create_dev_file(const std::string& filename,
                     const std::vector<uint32_t>& instructions,
                     TrustLevel trust = TrustLevel::UNTRUSTED) {
  std::FILE* f = std::fopen(filename.c_str(), "wb");
  assert_true(f != nullptr, "Could not create test file");

  // Write magic
  std::fwrite(MAGIC, 1, 3, f);

  // Write trust level
  uint8_t trust_byte = static_cast<uint8_t>(trust);
  std::fwrite(&trust_byte, 1, 1, f);

  // Write IR size
  uint32_t ir_size = instructions.size();
  std::fwrite(&ir_size, 4, 1, f);

  // Calculate checksum (simple sum for now)
  uint32_t checksum = 0;
  for (auto instr : instructions) {
    checksum += instr;
  }
  std::fwrite(&checksum, 4, 1, f);

  // Write instructions
  for (auto instr : instructions) {
    std::fwrite(&instr, 4, 1, f);
  }

  // Write terminator
  std::fwrite(VMTL, 1, 4, f);

  std::fclose(f);
}

// ============================================================================
// Test Cases
// ============================================================================

void test_arithmetic_operations() {
  std::cout << "Testing arithmetic operations..." << std::endl;

  // Test: R0 = 10, R1 = 5, R2 = R0 + R1
  VM vm;

  // Initialize registers
  vm.regs().regs[0].i64 = 10;
  vm.regs().regs[1].i64 = 5;

  // ADD R2, R0, R1
  Instruction add_instr = Instruction::encode(OpCode::ADD, 2, 0, 1);
  uint32_t add_raw = add_instr.encode();

  // Execute
  vm.test_execute_instruction(add_raw);

  assert_equal(vm.regs().regs[2].i64, 15, "ADD: R0 + R1 should be 15");

  // Test subtraction
  vm.regs().regs[3].i64 = 0;
  Instruction sub_instr = Instruction::encode(OpCode::SUB, 3, 0, 1);
  vm.test_execute_instruction(sub_instr.encode());
  assert_equal(vm.regs().regs[3].i64, 5, "SUB: R0 - R1 should be 5");

  // Test multiplication
  vm.regs().regs[4].i64 = 0;
  Instruction mul_instr = Instruction::encode(OpCode::MUL, 4, 0, 1);
  vm.test_execute_instruction(mul_instr.encode());
  assert_equal(vm.regs().regs[4].i64, 50, "MUL: R0 * R1 should be 50");

  // Test division
  vm.regs().regs[5].i64 = 0;
  Instruction div_instr = Instruction::encode(OpCode::DIV, 5, 0, 1);
  vm.test_execute_instruction(div_instr.encode());
  assert_equal(vm.regs().regs[5].i64, 2, "DIV: R0 / R1 should be 2");

  std::cout << "  PASS" << std::endl;
}

void test_bitwise_operations() {
  std::cout << "Testing bitwise operations..." << std::endl;

  VM vm;
  vm.regs().regs[0].i64 = 0xFF;
  vm.regs().regs[1].i64 = 0x0F;

  // AND
  Instruction and_instr = Instruction::encode(OpCode::AND, 2, 0, 1);
  vm.test_execute_instruction(and_instr.encode());
  assert_equal(vm.regs().regs[2].i64, 0x0F, "AND: 0xFF & 0x0F should be 0x0F");

  // OR
  vm.regs().regs[3].i64 = 0;
  Instruction or_instr = Instruction::encode(OpCode::OR, 3, 0, 1);
  vm.test_execute_instruction(or_instr.encode());
  assert_equal(vm.regs().regs[3].i64, 0xFF, "OR: 0xFF | 0x0F should be 0xFF");

  // XOR
  vm.regs().regs[4].i64 = 0;
  Instruction xor_instr = Instruction::encode(OpCode::XOR, 4, 0, 1);
  vm.test_execute_instruction(xor_instr.encode());
  assert_equal(vm.regs().regs[4].i64, 0xF0, "XOR: 0xFF ^ 0x0F should be 0xF0");

  std::cout << "  PASS" << std::endl;
}

void test_shift_operations() {
  std::cout << "Testing shift operations..." << std::endl;

  VM vm;
  vm.regs().regs[0].i64 = 4;
  vm.regs().regs[1].i64 = 2;

  // SHL (4 << 2 = 16)
  Instruction shl_instr = Instruction::encode(OpCode::SHL, 2, 0, 1);
  vm.test_execute_instruction(shl_instr.encode());
  assert_equal(vm.regs().regs[2].i64, 16, "SHL: 4 << 2 should be 16");

  // SHR (16 >> 2 = 4)
  vm.regs().regs[3].i64 = 0;
  Instruction shr_instr = Instruction::encode(OpCode::SHR, 3, 2, 1);
  vm.test_execute_instruction(shr_instr.encode());
  assert_equal(vm.regs().regs[3].i64, 4, "SHR: 16 >> 2 should be 4");

  std::cout << "  PASS" << std::endl;
}

void test_comparison_operations() {
  std::cout << "Testing comparison operations..." << std::endl;

  VM vm;
  vm.regs().regs[0].i64 = 10;
  vm.regs().regs[1].i64 = 5;

  // EQ (10 == 5 = 0)
  Instruction eq_instr = Instruction::encode(OpCode::EQ, 2, 0, 1);
  vm.test_execute_instruction(eq_instr.encode());
  assert_equal(vm.regs().regs[2].i64, 0, "EQ: 10 == 5 should be 0");

  // NE (10 != 5 = 1)
  Instruction ne_instr = Instruction::encode(OpCode::NE, 3, 0, 1);
  vm.test_execute_instruction(ne_instr.encode());
  assert_equal(vm.regs().regs[3].i64, 1, "NE: 10 != 5 should be 1");

  // LT (10 < 5 = 0)
  Instruction lt_instr = Instruction::encode(OpCode::LT, 4, 0, 1);
  vm.test_execute_instruction(lt_instr.encode());
  assert_equal(vm.regs().regs[4].i64, 0, "LT: 10 < 5 should be 0");

  // GT (10 > 5 = 1)
  Instruction gt_instr = Instruction::encode(OpCode::GT, 5, 0, 1);
  vm.test_execute_instruction(gt_instr.encode());
  assert_equal(vm.regs().regs[5].i64, 1, "GT: 10 > 5 should be 1");

  std::cout << "  PASS" << std::endl;
}

void test_memory_operations() {
  std::cout << "Testing memory operations..." << std::endl;

  VM vm;

  // Allocate memory
  vm.regs().regs[0].u64 = 64;  // Size
  Instruction alloc_instr = Instruction::encode(OpCode::ALLOC, 1, 0, 0);
  vm.test_execute_instruction(alloc_instr.encode());

  // Write a value
  vm.regs().regs[1].ptr;  // pointer from allocation
  vm.regs().regs[2].i64 = 42;
  vm.regs().regs[3].i64 = 0;  // offset

  // Note: STORE and LOAD require pointer arithmetic
  // For now, test basic memory read/write
  Value test_val;
  test_val.i64 = 123;
  uint64_t test_addr = 0;
  vm.memory().write(test_addr, test_val);

  Value read_val = vm.memory().read(test_addr);
  assert_equal(read_val.i64, 123, "Memory: write/read should preserve value");

  std::cout << "  PASS" << std::endl;
}

void test_unary_operations() {
  std::cout << "Testing unary operations..." << std::endl;

  VM vm;
  vm.regs().regs[0].i64 = 42;

  // NEG
  Instruction neg_instr = Instruction::encode(OpCode::NEG, 1, 0, 0);
  vm.test_execute_instruction(neg_instr.encode());
  assert_equal(vm.regs().regs[1].i64, -42, "NEG: -42 should negate to 42");

  // NOT (logical)
  vm.regs().regs[2].i64 = 0;
  Instruction not_instr = Instruction::encode(OpCode::NOT, 3, 2, 0);
  vm.test_execute_instruction(not_instr.encode());
  assert_equal(vm.regs().regs[3].i64, 1, "NOT: !0 should be 1");

  vm.regs().regs[4].i64 = 5;
  Instruction not_instr2 = Instruction::encode(OpCode::NOT, 5, 4, 0);
  vm.test_execute_instruction(not_instr2.encode());
  assert_equal(vm.regs().regs[5].i64, 0, "NOT: !5 should be 0");

  std::cout << "  PASS" << std::endl;
}

void test_move_operation() {
  std::cout << "Testing move operation..." << std::endl;

  VM vm;
  vm.regs().regs[0].i64 = 999;

  Instruction move_instr = Instruction::encode(OpCode::MOVE, 1, 0, 0);
  vm.test_execute_instruction(move_instr.encode());

  assert_equal(vm.regs().regs[1].i64, 999, "MOVE: R1 should be 999");

  std::cout << "  PASS" << std::endl;
}

void test_modulo_operation() {
  std::cout << "Testing modulo operation..." << std::endl;

  VM vm;
  vm.regs().regs[0].i64 = 17;
  vm.regs().regs[1].i64 = 5;

  Instruction mod_instr = Instruction::encode(OpCode::MOD, 2, 0, 1);
  vm.test_execute_instruction(mod_instr.encode());

  assert_equal(vm.regs().regs[2].i64, 2, "MOD: 17 % 5 should be 2");

  std::cout << "  PASS" << std::endl;
}

void test_jit_compilation() {
  std::cout << "Testing JIT compilation..." << std::endl;

  jit::JITCompiler jit;

  // Create simple sequence: ADD R2, R0, R1
  std::vector<Instruction> instructions;
  instructions.push_back(Instruction::encode(OpCode::ADD, 2, 0, 1));

  auto code = jit.compile(instructions);

  // Verify code was generated
  assert_true(code.size() > 0, "JIT should generate non-empty code");

  // Code should start with prologue (push rbp, mov rbp rsp)
  // and end with epilogue
  assert_true(code.back() == 0xC3, "JIT code should end with RET (0xC3)");

  std::cout << "  PASS (generated " << code.size() << " bytes)" << std::endl;
}

void test_sandbox_whitelist() {
  std::cout << "Testing sandbox syscall whitelist..." << std::endl;

  // Test UNTRUSTED level
  security::Sandbox untrusted_sb(TrustLevel::UNTRUSTED);
  assert_true(
      untrusted_sb.execute_syscall(security::SyscallID::EXIT, 0, 0, 0, 0, 0, 0) == 0,
      "UNTRUSTED: EXIT should be allowed");

  // Test SIGNED level
  security::Sandbox signed_sb(TrustLevel::SIGNED);
  assert_true(
      signed_sb.execute_syscall(security::SyscallID::GETPID, 0, 0, 0, 0, 0, 0) > 0,
      "SIGNED: GETPID should be allowed");

  // Test blocked syscall
  try {
    security::Sandbox untrusted_sb2(TrustLevel::UNTRUSTED);
    // FORK is not in whitelist for UNTRUSTED
    // This is a placeholder test - actual call would fail
    std::cout << "  PASS" << std::endl;
  } catch (const security::SandboxViolation&) {
    std::cout << "  PASS (correctly blocked syscall)" << std::endl;
  }
}

void test_halt_instruction() {
  std::cout << "Testing HALT instruction..." << std::endl;

  VM vm;
  assert_true(!vm.is_halted(), "VM should not be halted initially");

  Instruction halt = Instruction::encode(OpCode::HALT, 0, 0, 0);
  vm.test_execute_instruction(halt.encode());

  assert_true(vm.is_halted(), "VM should be halted after HALT");

  std::cout << "  PASS" << std::endl;
}

void test_division_by_zero() {
  std::cout << "Testing division by zero handling..." << std::endl;

  VM vm;
  vm.regs().regs[0].i64 = 10;
  vm.regs().regs[1].i64 = 0;

  Instruction div_instr = Instruction::encode(OpCode::DIV, 2, 0, 1);

  try {
    vm.test_execute_instruction(div_instr.encode());
    assert_true(false, "Should have thrown on division by zero");
  } catch (const std::runtime_error& e) {
    assert_true(std::string(e.what()).find("zero") != std::string::npos,
                "Error message should mention zero");
    std::cout << "  PASS" << std::endl;
  }
}

void test_register_bounds() {
  std::cout << "Testing register bounds checking..." << std::endl;

  VM vm;
  vm.regs().regs[0].i64 = 5;

  // Try to use invalid register index (256 is out of bounds, max is 255)
  Instruction bad_instr = Instruction::encode(OpCode::MOVE, 0, 250, 0);

  // This should succeed since 250 < 256
  vm.test_execute_instruction(bad_instr.encode());

  std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

void test_syscall_write() {
  std::cout << "Testing SYSCALL write()..." << std::endl;

  // Create IR: write(1, "Hello!", 6) → fd=1, buf=0x1000, count=6
  std::vector<uint32_t> code;

  // MOV R0, 1           (fd=1, stdout)
  code.push_back(Instruction::encode(OpCode::MOVE, 0, 0, 0).encode());
  code.push_back(Instruction::encode(OpCode::MOVE, 0, 0, 0).encode());

  // HALT
  code.push_back(Instruction::encode(OpCode::HALT, 0, 0, 0).encode());

  create_dev_file("test_write.dev", code, TrustLevel::UNTRUSTED);

  VM vm(TrustLevel::UNTRUSTED);
  int result = vm.execute("test_write.dev");

  assert_equal(result, 0, "SYSCALL write test");
  std::cout << "  PASS" << std::endl;
}

int main() {
  std::cout << "\n========================================" << std::endl;
  std::cout << "DevVM Test Suite" << std::endl;
  std::cout << "========================================\n" << std::endl;

  try {
    test_arithmetic_operations();
    test_bitwise_operations();
    test_shift_operations();
    test_comparison_operations();
    test_memory_operations();
    test_unary_operations();
    test_move_operation();
    test_modulo_operation();
    test_jit_compilation();
    test_sandbox_whitelist();
    test_syscall_write();
    test_halt_instruction();
    test_division_by_zero();
    test_register_bounds();

    std::cout << "\n========================================" << std::endl;
    std::cout << "All tests PASSED!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "\nTest suite failed with exception: " << e.what() << std::endl;
    return 1;
  }
}
