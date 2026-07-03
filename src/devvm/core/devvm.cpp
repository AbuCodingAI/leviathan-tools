#include "devvm.h"
#include "../ir/instructions.h"

#include <fstream>
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace devvm {

// Memory implementation (Stack-Heap Separation)
Memory::Memory(size_t heap_size, size_t stack_size)
    : heap_(heap_size), stack_(stack_size), heap_used_(0) {}

Value* Memory::allocate(size_t size) {
  if (size == 0) return nullptr;
  if (heap_used_ + size > heap_.size()) {
    throw std::runtime_error("Heap overflow: not enough memory");
  }
  uint8_t* ptr = heap_.data() + heap_used_;
  allocations_[ptr] = size;
  heap_used_ += size;
  return reinterpret_cast<Value*>(ptr);
}

void Memory::deallocate(void* ptr) {
  allocations_.erase(ptr);
  // In a real allocator, we'd track free chunks for reuse
  // For now, deallocation just removes tracking
}

void Memory::push_stack(Value val, uint64_t sp) {
  if (sp + sizeof(Value) > stack_.size()) {
    throw std::runtime_error("Stack overflow");
  }
  *reinterpret_cast<Value*>(stack_.data() + sp) = val;
}

Value Memory::pop_stack(uint64_t sp) {
  if (sp >= stack_.size()) {
    throw std::runtime_error("Stack underflow");
  }
  return *reinterpret_cast<Value*>(stack_.data() + sp);
}

Value Memory::read(uint64_t addr) {
  if (addr >= heap_.size()) throw std::out_of_range("Memory access out of bounds");
  return *reinterpret_cast<Value*>(heap_.data() + addr);
}

void Memory::write(uint64_t addr, Value val) {
  if (addr >= heap_.size()) throw std::out_of_range("Memory access out of bounds");
  *reinterpret_cast<Value*>(heap_.data() + addr) = val;
}

// VM implementation
VM::VM(TrustLevel trust) : trust_(trust), sandbox_(trust) {
  regs_.regs.resize(256);  // R0-R255
  regs_.pc = 0;
  regs_.sp = 0;
}

VM::~VM() = default;

bool VM::load_dev_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Cannot open .dev file: " + path);

  // Read magic bytes
  uint8_t magic[3];
  file.read(reinterpret_cast<char*>(magic), 3);
  if (std::memcmp(magic, MAGIC, 3) != 0) {
    throw std::runtime_error("Invalid .dev file: bad magic bytes");
  }

  // Read trust level
  uint8_t trust_byte;
  file.read(reinterpret_cast<char*>(&trust_byte), 1);
  trust_ = static_cast<TrustLevel>(trust_byte);

  // Read IR size
  uint32_t ir_size;
  file.read(reinterpret_cast<char*>(&ir_size), 4);

  // Read checksum
  uint32_t checksum;
  file.read(reinterpret_cast<char*>(&checksum), 4);

  // Read IR instructions
  ir_.resize(ir_size);
  file.read(reinterpret_cast<char*>(ir_.data()), ir_size * 4);

  // Verify terminator
  uint8_t vmtl[4];
  file.read(reinterpret_cast<char*>(vmtl), 4);
  if (std::memcmp(vmtl, VMTL, 4) != 0) {
    throw std::runtime_error("Invalid .dev file: missing vmtl terminator");
  }

  return true;
}

int VM::execute(const std::string& dev_path) {
  try {
    load_dev_file(dev_path);
    run();
    return regs_.rax.i64;
  } catch (const std::exception& e) {
    if (trust_ == TrustLevel::UNTRUSTED) {
      // Sandboxed: don't expose internal errors
      return 1;
    }
    throw;
  }
}

void VM::step() {
  if (halted_ || regs_.pc >= ir_.size()) {
    halted_ = true;
    return;
  }

  uint32_t instr_raw = ir_[regs_.pc];
  ir::Instruction instr = ir::Instruction::decode(instr_raw);

  if (verbose_) {
    log_instruction(instr);
  }

  if (profile_) {
    opcode_counts_[instr.op]++;
  }

  instruction_count_++;
  execute_instruction(instr_raw);
  regs_.pc++;
}

void VM::log_instruction(const ir::Instruction& instr) {
  fprintf(stderr, "[PC=%04lx] ", regs_.pc);

  switch (instr.op) {
    case ir::OpCode::ADD:
      fprintf(stderr, "ADD R%d = R%d + R%d\n", instr.dest, instr.src1, instr.src2);
      break;
    case ir::OpCode::SUB:
      fprintf(stderr, "SUB R%d = R%d - R%d\n", instr.dest, instr.src1, instr.src2);
      break;
    case ir::OpCode::MUL:
      fprintf(stderr, "MUL R%d = R%d * R%d\n", instr.dest, instr.src1, instr.src2);
      break;
    case ir::OpCode::DIV:
      fprintf(stderr, "DIV R%d = R%d / R%d\n", instr.dest, instr.src1, instr.src2);
      break;
    case ir::OpCode::LOAD:
      fprintf(stderr, "LOAD R%d = [R%d + R%d]\n", instr.dest, instr.src1, instr.src2);
      break;
    case ir::OpCode::STORE:
      fprintf(stderr, "STORE [R%d + R%d] = R%d\n", instr.dest, instr.src2, instr.src1);
      break;
    case ir::OpCode::MOVE:
      fprintf(stderr, "MOVE R%d = R%d\n", instr.dest, instr.src1);
      break;
    case ir::OpCode::HALT:
      fprintf(stderr, "HALT\n");
      break;
    default:
      fprintf(stderr, "OpCode(%d) R%d, R%d, R%d\n", static_cast<int>(instr.op),
              instr.dest, instr.src1, instr.src2);
  }
}

void VM::run() {
  while (!halted_) {
    step();
  }
}

void VM::execute_instruction(uint32_t instr_raw) {
  using namespace ir;
  auto instr = Instruction::decode(instr_raw);

  // Bounds checking for register access
  auto check_reg = [this](uint8_t reg) {
    if (reg >= regs_.regs.size()) {
      throw std::out_of_range("Register index out of bounds: " + std::to_string(reg));
    }
  };

  switch (instr.op) {
    case OpCode::NOP:
      // No operation
      break;

    case OpCode::HALT:
      halted_ = true;
      break;

    // Arithmetic operations (3-address: dest = src1 op src2)
    case OpCode::ADD:
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          regs_.regs[instr.src1].i64 + regs_.regs[instr.src2].i64;
      break;

    case OpCode::SUB:
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          regs_.regs[instr.src1].i64 - regs_.regs[instr.src2].i64;
      break;

    case OpCode::MUL:
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          regs_.regs[instr.src1].i64 * regs_.regs[instr.src2].i64;
      break;

    case OpCode::DIV:
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      if (regs_.regs[instr.src2].i64 == 0) {
        throw std::runtime_error("Division by zero");
      }
      regs_.regs[instr.dest].i64 =
          regs_.regs[instr.src1].i64 / regs_.regs[instr.src2].i64;
      break;

    case OpCode::MOD:
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      if (regs_.regs[instr.src2].i64 == 0) {
        throw std::runtime_error("Modulo by zero");
      }
      regs_.regs[instr.dest].i64 =
          regs_.regs[instr.src1].i64 % regs_.regs[instr.src2].i64;
      break;

    case OpCode::AND:
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          regs_.regs[instr.src1].i64 & regs_.regs[instr.src2].i64;
      break;

    case OpCode::OR:
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          regs_.regs[instr.src1].i64 | regs_.regs[instr.src2].i64;
      break;

    case OpCode::XOR:
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          regs_.regs[instr.src1].i64 ^ regs_.regs[instr.src2].i64;
      break;

    case OpCode::SHL:
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      if (regs_.regs[instr.src2].i64 < 0 || regs_.regs[instr.src2].i64 >= 64) {
        throw std::runtime_error("Shift amount out of range");
      }
      regs_.regs[instr.dest].i64 =
          regs_.regs[instr.src1].i64 << regs_.regs[instr.src2].i64;
      break;

    case OpCode::SHR:
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      if (regs_.regs[instr.src2].i64 < 0 || regs_.regs[instr.src2].i64 >= 64) {
        throw std::runtime_error("Shift amount out of range");
      }
      regs_.regs[instr.dest].i64 =
          regs_.regs[instr.src1].i64 >> regs_.regs[instr.src2].i64;
      break;

    // Unary operations
    case OpCode::NEG:
      check_reg(instr.dest);
      check_reg(instr.src1);
      regs_.regs[instr.dest].i64 = -regs_.regs[instr.src1].i64;
      break;

    case OpCode::NOT:
      check_reg(instr.dest);
      check_reg(instr.src1);
      regs_.regs[instr.dest].i64 = regs_.regs[instr.src1].i64 ? 0 : 1;
      break;

    case OpCode::CAST:
      check_reg(instr.dest);
      check_reg(instr.src1);
      // src2 indicates cast type (0=i64, 1=u64, 2=f64, 3=ptr)
      // For now, just move the bits
      regs_.regs[instr.dest] = regs_.regs[instr.src1];
      break;

    // Memory operations
    case OpCode::LOAD:
      // dest = [src1 + src2 offset]
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      {
        uint64_t addr = regs_.regs[instr.src1].u64 + regs_.regs[instr.src2].u64;
        regs_.regs[instr.dest] = memory_.read(addr);
      }
      break;

    case OpCode::STORE:
      // [dest + src2 offset] = src1
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      {
        uint64_t addr = regs_.regs[instr.dest].u64 + regs_.regs[instr.src2].u64;
        memory_.write(addr, regs_.regs[instr.src1]);
      }
      break;

    case OpCode::MOVE:
      // dest = src1
      check_reg(instr.dest);
      check_reg(instr.src1);
      regs_.regs[instr.dest] = regs_.regs[instr.src1];
      break;

    case OpCode::ALLOC:
      // dest = allocate(src1 bytes)
      check_reg(instr.dest);
      check_reg(instr.src1);
      {
        size_t size = static_cast<size_t>(regs_.regs[instr.src1].u64);
        Value* ptr = memory_.allocate(size);
        regs_.regs[instr.dest].ptr = ptr;
      }
      break;

    // Comparison operations (set dest to 1 or 0)
    case OpCode::CMP:
      // Compare src1 and src2, set flags (stored in dest register)
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      // Store comparison result: -1 if src1 < src2, 0 if equal, 1 if src1 > src2
      if (regs_.regs[instr.src1].i64 < regs_.regs[instr.src2].i64) {
        regs_.regs[instr.dest].i64 = -1;
      } else if (regs_.regs[instr.src1].i64 > regs_.regs[instr.src2].i64) {
        regs_.regs[instr.dest].i64 = 1;
      } else {
        regs_.regs[instr.dest].i64 = 0;
      }
      break;

    case OpCode::EQ:
      // dest = (src1 == src2)
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          (regs_.regs[instr.src1].i64 == regs_.regs[instr.src2].i64) ? 1 : 0;
      break;

    case OpCode::NE:
      // dest = (src1 != src2)
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          (regs_.regs[instr.src1].i64 != regs_.regs[instr.src2].i64) ? 1 : 0;
      break;

    case OpCode::LT:
      // dest = (src1 < src2)
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          (regs_.regs[instr.src1].i64 < regs_.regs[instr.src2].i64) ? 1 : 0;
      break;

    case OpCode::LE:
      // dest = (src1 <= src2)
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          (regs_.regs[instr.src1].i64 <= regs_.regs[instr.src2].i64) ? 1 : 0;
      break;

    case OpCode::GT:
      // dest = (src1 > src2)
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          (regs_.regs[instr.src1].i64 > regs_.regs[instr.src2].i64) ? 1 : 0;
      break;

    case OpCode::GE:
      // dest = (src1 >= src2)
      check_reg(instr.dest);
      check_reg(instr.src1);
      check_reg(instr.src2);
      regs_.regs[instr.dest].i64 =
          (regs_.regs[instr.src1].i64 >= regs_.regs[instr.src2].i64) ? 1 : 0;
      break;

    // Control flow
    case OpCode::JMP:
      // Jump to address in src1
      check_reg(instr.src1);
      regs_.pc = regs_.regs[instr.src1].u64;
      if (regs_.pc >= ir_.size()) {
        throw std::runtime_error("Jump target out of bounds");
      }
      regs_.pc--;  // Will be incremented at end of step()
      break;

    case OpCode::JZ:
      // Jump if src1 == 0
      check_reg(instr.src1);
      check_reg(instr.src2);
      if (regs_.regs[instr.src1].i64 == 0) {
        regs_.pc = regs_.regs[instr.src2].u64;
        if (regs_.pc >= ir_.size()) {
          throw std::runtime_error("Jump target out of bounds");
        }
        regs_.pc--;  // Will be incremented at end of step()
      }
      break;

    case OpCode::JNZ:
      // Jump if src1 != 0
      check_reg(instr.src1);
      check_reg(instr.src2);
      if (regs_.regs[instr.src1].i64 != 0) {
        regs_.pc = regs_.regs[instr.src2].u64;
        if (regs_.pc >= ir_.size()) {
          throw std::runtime_error("Jump target out of bounds");
        }
        regs_.pc--;  // Will be incremented at end of step()
      }
      break;

    // Stack operations
    case OpCode::PUSH:
      // Push src1 onto stack
      check_reg(instr.src1);
      {
        size_t new_sp = regs_.sp + sizeof(Value);
        if (new_sp >= memory_.heap_size()) {
          throw std::runtime_error("Stack overflow");
        }
        memory_.write(regs_.sp, regs_.regs[instr.src1]);
        regs_.sp = new_sp;
      }
      break;

    case OpCode::POP:
      // Pop stack into dest
      check_reg(instr.dest);
      if (regs_.sp < sizeof(Value)) {
        throw std::runtime_error("Stack underflow");
      }
      regs_.sp -= sizeof(Value);
      regs_.regs[instr.dest] = memory_.read(regs_.sp);
      break;

    case OpCode::FRAME:
      // Setup stack frame with src1 locals
      check_reg(instr.src1);
      {
        size_t frame_size = regs_.regs[instr.src1].u64 * sizeof(Value);
        size_t new_sp = regs_.sp + frame_size;
        if (new_sp >= memory_.heap_size()) {
          throw std::runtime_error("Stack overflow");
        }
        // Zero-initialize frame
        for (size_t i = 0; i < frame_size; i += sizeof(Value)) {
          Value zero = {};
          memory_.write(regs_.sp + i, zero);
        }
        regs_.sp = new_sp;
      }
      break;

    // Function call/return
    case OpCode::CALL:
      // Call function at src1, store return address in src2
      check_reg(instr.src1);
      check_reg(instr.src2);
      {
        // Store return address
        Value ret_addr;
        ret_addr.u64 = regs_.pc + 1;
        size_t new_sp = regs_.sp + sizeof(Value);
        if (new_sp >= memory_.heap_size()) {
          throw std::runtime_error("Stack overflow");
        }
        memory_.write(regs_.sp, ret_addr);
        regs_.sp = new_sp;

        // Jump to function
        regs_.pc = regs_.regs[instr.src1].u64;
        if (regs_.pc >= ir_.size()) {
          throw std::runtime_error("Call target out of bounds");
        }
        regs_.pc--;  // Will be incremented at end of step()
      }
      break;

    case OpCode::RET:
      // Return from function
      if (regs_.sp < sizeof(Value)) {
        throw std::runtime_error("Stack underflow on return");
      }
      regs_.sp -= sizeof(Value);
      {
        Value ret_addr = memory_.read(regs_.sp);
        regs_.pc = ret_addr.u64;
        if (regs_.pc >= ir_.size()) {
          halted_ = true;  // Return outside IR boundary means exit
        } else {
          regs_.pc--;  // Will be incremented at end of step()
        }
      }
      break;

    case OpCode::SYSCALL: {
      // Sandboxed syscall - filtered by security module
      check_reg(instr.dest);
      check_reg(instr.src1);
      // syscall_id in src1, arguments in R0-R5
      int32_t syscall_id = static_cast<int32_t>(regs_.regs[instr.src1].i64);
      int64_t arg0 = regs_.regs[0].i64;
      int64_t arg1 = regs_.regs[1].i64;
      int64_t arg2 = regs_.regs[2].i64;
      int64_t arg3 = regs_.regs[3].i64;
      int64_t arg4 = regs_.regs[4].i64;
      int64_t arg5 = regs_.regs[5].i64;

      try {
        int64_t result = sandbox_.execute_syscall(
            static_cast<security::SyscallID>(syscall_id),
            arg0, arg1, arg2, arg3, arg4, arg5);
        regs_.regs[instr.dest].i64 = result;
      } catch (const security::SandboxViolation& e) {
        throw std::runtime_error(std::string("Sandbox violation: ") + e.what());
      }
      break;
    }

    default:
      throw std::runtime_error("Unknown opcode: " + std::to_string(static_cast<int>(instr.op)));
  }
}

bool VM::is_halted() const {
  return halted_;
}

void VM::print_stats() const {
  fprintf(stderr, "\n========================================\n");
  fprintf(stderr, "DevVM Execution Statistics\n");
  fprintf(stderr, "========================================\n");
  fprintf(stderr, "Total instructions executed: %lu\n", instruction_count_);
  fprintf(stderr, "Final PC: 0x%04lx\n", regs_.pc);
  fprintf(stderr, "Heap used: %lu bytes\n", memory_.heap_used());

  if (profile_ && !opcode_counts_.empty()) {
    fprintf(stderr, "\nOpcode distribution:\n");
    for (const auto& [op, count] : opcode_counts_) {
      fprintf(stderr, "  OpCode(%2d): %6lu\n", static_cast<int>(op), count);
    }
  }

  fprintf(stderr, "\nFinal register state (RAX = return value):\n");
  fprintf(stderr, "  RAX = %ld (0x%016lx)\n", regs_.rax.i64, regs_.rax.u64);
  fprintf(stderr, "  RSP = 0x%016lx (stack pointer)\n", regs_.sp);
  fprintf(stderr, "========================================\n\n");
}

}  // namespace devvm
