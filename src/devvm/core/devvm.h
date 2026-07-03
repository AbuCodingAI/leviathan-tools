#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <map>
#include <memory>
#include <string>
#include "../security/sandbox.h"
#include "../ir/instructions.h"

namespace devvm {

// Magic bytes: 'd' 'e' 'v'
constexpr uint8_t MAGIC[3] = {100, 101, 118};
// Terminator: 'v' 'm' 't' 'l'
constexpr uint8_t VMTL[4] = {118, 109, 116, 108};
// Unpack flag: 'u' 'n' 'p' 'a' 'c' 'k'
constexpr uint8_t UNPACK[6] = {117, 110, 112, 97, 99, 107};

enum class TrustLevel : uint8_t {
  UNTRUSTED = 0,  // Sandboxed execution
  SIGNED = 1,     // Cryptographically signed
  VERIFIED = 2,   // Verified by OS
};

struct DevFileHeader {
  uint8_t magic[3];        // Magic: "dev"
  TrustLevel trust_level;  // Trust/signature indicator
  uint32_t ir_size;        // Size of IR section
  uint32_t checksum;       // CRC32 of IR
};

// VM Value: 64-bit tagged union
union Value {
  int64_t i64;
  uint64_t u64;
  double f64;
  void* ptr;
};

// VM Register set
struct Registers {
  std::vector<Value> regs;  // General registers (R0-R255)
  Value rax;                // Return value
  uint64_t pc;              // Program counter
  uint64_t sp;              // Stack pointer
};

// VM Memory Management (Stack-Heap Separation)
class Memory {
 public:
  Memory(size_t heap_size = 128 * 1024 * 1024, size_t stack_size = 128 * 1024 * 1024);

  // Heap allocation (malloc-style)
  Value* allocate(size_t size);
  void deallocate(void* ptr);

  // Stack operations
  void push_stack(Value val, uint64_t sp);
  Value pop_stack(uint64_t sp);

  // General memory access
  Value read(uint64_t addr);
  void write(uint64_t addr, Value val);

  size_t heap_size() const { return heap_.size(); }
  size_t stack_size() const { return stack_.size(); }
  size_t heap_used() const { return heap_used_; }

 private:
  std::vector<uint8_t> heap_;      // Heap: grow upward
  std::vector<uint8_t> stack_;     // Stack: separate region, grow downward
  size_t heap_used_ = 0;           // Current heap pointer
  std::map<void*, size_t> allocations_;  // Tracking allocations

  friend class VM;
};

// Main VM Engine
class VM {
 public:
  explicit VM(TrustLevel trust = TrustLevel::UNTRUSTED);
  ~VM();

  // Load and execute a .dev file
  int execute(const std::string& dev_path);

  // Low-level execution
  void step();                    // Execute one instruction
  void run();                     // Run until termination
  bool is_halted() const;

  // Debug/profiling
  void set_verbose(bool v) { verbose_ = v; }
  void set_profile(bool p) { profile_ = p; }
  uint64_t instruction_count() const { return instruction_count_; }
  void print_stats() const;

  // Memory/Register access
  Memory& memory() { return memory_; }
  Registers& regs() { return regs_; }
  TrustLevel trust() const { return trust_; }

  // Public instruction execution for testing
  void test_execute_instruction(uint32_t instr) { execute_instruction(instr); }

 private:
  Registers regs_;
  Memory memory_;
  TrustLevel trust_;
  std::vector<uint32_t> ir_;     // Intermediate representation
  bool halted_ = false;
  security::Sandbox sandbox_;    // Syscall sandbox

  // Debug/profiling
  bool verbose_ = false;
  bool profile_ = false;
  uint64_t instruction_count_ = 0;
  std::map<ir::OpCode, uint64_t> opcode_counts_;

  // Execution loop
  void execute_instruction(uint32_t instr);
  void log_instruction(const ir::Instruction& instr);

  // File loading
  bool load_dev_file(const std::string& path);

  friend class TestHarness;  // For testing
};

}  // namespace devvm
