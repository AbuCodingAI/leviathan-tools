// JIT→C Compiler: Analyzes bytecode, generates optimized C code
// Purpose: Convert .dev IR bytecode to C source, then compile to native binary

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include "../ir/instructions.h"

namespace devvm {
namespace jit {

// C code generation context
class CCodeGenerator {
 public:
    CCodeGenerator();

    // Compile IR instructions to C code
    std::string generate_c_code(const std::vector<ir::Instruction>& instructions);

    // Get the generated C code
    const std::string& get_code() const { return code_; }

 private:
    std::string code_;
    std::map<uint8_t, std::string> register_types_;  // Register→type mapping
    int indent_level_ = 0;

    // Code emission helpers
    void emit_header();
    void emit_includes();
    void emit_main_begin();
    void emit_main_end();
    void emit_line(const std::string& line);
    void emit_blank();

    // Instruction-to-C translation
    void emit_instruction(const ir::Instruction& instr);
    void emit_instruction_nop(const ir::Instruction& instr);
    void emit_instruction_halt(const ir::Instruction& instr);
    void emit_instruction_add(const ir::Instruction& instr);
    void emit_instruction_sub(const ir::Instruction& instr);
    void emit_instruction_load(const ir::Instruction& instr);
    void emit_instruction_store(const ir::Instruction& instr);
    void emit_instruction_syscall(const ir::Instruction& instr);
    void emit_instruction_ret(const ir::Instruction& instr);

    // Optimization helpers
    void optimize_loop_unrolling(std::vector<ir::Instruction>& instrs);
    void optimize_dead_code_elimination(std::vector<ir::Instruction>& instrs);
    void analyze_register_usage(const std::vector<ir::Instruction>& instrs);

    // Register helpers
    std::string reg_name(uint8_t reg_id);
};

// JIT→C compiler interface
class JITToC {
 public:
    JITToC();

    // Compile .dev bytecode to C, then to native binary
    // Returns: exit code of compiled program (or -1 on error)
    int compile_and_run(const std::string& dev_path,
                        const std::vector<std::string>& args = {});

    // Just generate C code (for inspection)
    std::string generate_c(const std::string& dev_path);

    // Get compilation cache directory
    static std::string get_cache_dir();

    // Check if cached binary exists
    static bool has_cached_binary(const std::string& dev_path);

    // Get cached binary path
    static std::string get_cached_binary_path(const std::string& dev_path);

 private:
    CCodeGenerator codegen_;

    // Compilation helpers
    int compile_c_to_binary(const std::string& c_code,
                           const std::string& output_binary);
    int run_binary(const std::string& binary_path,
                   const std::vector<std::string>& args);

    // C compiler selection (TinyCC, GCC, Clang)
    std::string select_c_compiler();
};

}  // namespace jit
}  // namespace devvm
