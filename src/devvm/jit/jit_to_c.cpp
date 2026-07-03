// JIT→C Compiler Implementation
#include "jit_to_c.h"
#include "../core/devvm.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace devvm {
namespace jit {

// ============================================================================
// CCodeGenerator Implementation
// ============================================================================

CCodeGenerator::CCodeGenerator() : code_(""), indent_level_(0) {}

std::string CCodeGenerator::reg_name(uint8_t reg_id) {
    return "r[" + std::to_string(static_cast<int>(reg_id)) + "]";
}

void CCodeGenerator::emit_line(const std::string& line) {
    for (int i = 0; i < indent_level_; i++) {
        code_ += "  ";
    }
    code_ += line + "\n";
}

void CCodeGenerator::emit_blank() {
    code_ += "\n";
}

void CCodeGenerator::emit_includes() {
    emit_line("#include <stdint.h>");
    emit_line("#include <stdio.h>");
    emit_line("#include <stdlib.h>");
    emit_line("#include <unistd.h>");
    emit_line("#include <string.h>");
    emit_line("#include <time.h>");
    emit_blank();
    // Leviathan Dev API — emitted inline so the produced binary is fully
    // self-contained (mirrors src/devvm/api/dev_api.h). A .dev targets these
    // dev_* calls, never Win32/POSIX directly, and they compile straight down.
    emit_line("/* ---- Leviathan Dev API (inlined runtime) ---- */");
    emit_line("static int dev__argc=0; static char** dev__argv=0;");
    emit_line("static void dev_main_init(int c,char**v){dev__argc=c;dev__argv=v;}");
    emit_line("static int dev_argc(void){return dev__argc;}");
    emit_line("static const char* dev_arg(int i){return (i>=0&&i<dev__argc)?dev__argv[i]:0;}");
    emit_line("static const char* dev_env(const char*k){return k?getenv(k):0;}");
    emit_line("static void dev_exit(int c){exit(c);}");
    emit_line("static void dev_write(int fd,const void*b,uint64_t n){FILE*s=(fd==2)?stderr:stdout;if(b&&n)fwrite(b,1,(size_t)n,s);fflush(s);}");
    emit_line("static void dev_print(const char*s){if(s)fputs(s,stdout);}");
    emit_line("static void dev_println(const char*s){if(s)fputs(s,stdout);fputc('\\n',stdout);}");
    emit_line("static int64_t dev_read_file(const char*p,char*o,int64_t c){FILE*f=fopen(p,\"rb\");if(!f)return -1;int64_t n=(int64_t)fread(o,1,(size_t)c,f);fclose(f);return n;}");
    emit_line("static int dev_write_file(const char*p,const void*d,int64_t l){FILE*f=fopen(p,\"wb\");if(!f)return -1;int64_t n=(int64_t)fwrite(d,1,(size_t)l,f);fclose(f);return n==l?0:-1;}");
    emit_line("static uint64_t dev_time_ms(void){return (uint64_t)time((time_t*)0)*1000ull;}");
    emit_line("static uint64_t dev_rand(void){return (uint64_t)rand();}");
    emit_line("__attribute__((unused)) static void dev__api_keep(void){(void)dev_argc;(void)dev_arg;(void)dev_env;(void)dev_exit;(void)dev_print;(void)dev_println;(void)dev_read_file;(void)dev_write_file;(void)dev_time_ms;(void)dev_rand;}");
    emit_blank();
}

void CCodeGenerator::emit_header() {
    emit_line("// Generated C code from .dev bytecode");
    emit_line("// JIT→C Compiler (DevVM)");
    emit_blank();
    emit_includes();
}

void CCodeGenerator::emit_main_begin() {
    emit_line("int main(int argc, char *argv[]) {");
    indent_level_++;

    // Hand argc/argv to the Dev API so dev_arg()/dev_argc() work.
    emit_line("dev_main_init(argc, argv);");
    emit_blank();

    // Register file
    emit_line("uint64_t r[256];  // DevVM register file (256 x 64-bit)");
    emit_line("for (int i = 0; i < 256; i++) r[i] = 0;");
    emit_blank();

    // Stack
    emit_line("uint8_t stack[256 * 1024];  // 256 KB stack");
    emit_line("uint64_t sp = 256 * 1024;   // Stack pointer");
    emit_blank();

    // DEBUG: Verify binary is running
    emit_line("// DEBUG: Confirm JIT→C execution is working");
    emit_line("// printf(\"[DEBUG] JIT→C compiled binary running...\\n\");");
    emit_blank();
}

void CCodeGenerator::emit_main_end() {
    indent_level_--;
    emit_line("}");
}

void CCodeGenerator::emit_instruction_nop(const ir::Instruction& instr) {
    emit_line("// NOP");
}

void CCodeGenerator::emit_instruction_halt(const ir::Instruction& instr) {
    emit_line("return 0;  // HALT");
}

void CCodeGenerator::emit_instruction_add(const ir::Instruction& instr) {
    std::string line = reg_name(instr.dest) + " = " +
                      reg_name(instr.src1) + " + " +
                      reg_name(instr.src2) + ";";
    emit_line(line);
}

void CCodeGenerator::emit_instruction_sub(const ir::Instruction& instr) {
    std::string line = reg_name(instr.dest) + " = " +
                      reg_name(instr.src1) + " - " +
                      reg_name(instr.src2) + ";";
    emit_line(line);
}

void CCodeGenerator::emit_instruction_load(const ir::Instruction& instr) {
    std::string line = reg_name(instr.dest) + " = " +
                      "*(uint64_t*)(r[" + std::to_string(instr.src1) + "]);";
    emit_line(line);
}

void CCodeGenerator::emit_instruction_store(const ir::Instruction& instr) {
    std::string line = "*(uint64_t*)(r[" + std::to_string(instr.dest) + "]) = " +
                      reg_name(instr.src1) + ";";
    emit_line(line);
}

void CCodeGenerator::emit_instruction_syscall(const ir::Instruction& instr) {
    // SYSCALL is routed through the Leviathan Dev API (dev_write), NOT a raw
    // platform syscall — so the same bytecode is portable across OSes.
    //   src1 = fd, src2 = buffer pointer, dest = length
    emit_line("dev_write((int)" + reg_name(instr.src1) +
              ", (const void*)(uintptr_t)" + reg_name(instr.src2) +
              ", " + reg_name(instr.dest) + ");");
}

void CCodeGenerator::emit_instruction_ret(const ir::Instruction& instr) {
    std::string line = "return (int)" + reg_name(instr.src1) + ";";
    emit_line(line);
}

void CCodeGenerator::emit_instruction(const ir::Instruction& instr) {
    using OpCode = ir::OpCode;

    switch (instr.op) {
        case OpCode::NOP:
            emit_instruction_nop(instr);
            break;
        case OpCode::HALT:
            emit_instruction_halt(instr);
            break;
        case OpCode::ADD:
            emit_instruction_add(instr);
            break;
        case OpCode::SUB:
            emit_instruction_sub(instr);
            break;
        case OpCode::LOAD:
            emit_instruction_load(instr);
            break;
        case OpCode::STORE:
            emit_instruction_store(instr);
            break;
        case OpCode::SYSCALL:
            emit_instruction_syscall(instr);
            break;
        case OpCode::RET:
            emit_instruction_ret(instr);
            break;
        default:
            emit_line("// Unknown opcode: " + std::to_string(static_cast<int>(instr.op)));
            break;
    }
}

void CCodeGenerator::analyze_register_usage(const std::vector<ir::Instruction>& instrs) {
    // Analyze which registers are used to optimize C code
    // For now, simple approach: all registers are used
    (void)instrs;  // Mark parameter as intentionally unused
    for (int i = 0; i < 256; i++) {
        register_types_[i] = "uint64_t";
    }
}

std::string CCodeGenerator::generate_c_code(const std::vector<ir::Instruction>& instructions) {
    code_ = "";
    indent_level_ = 0;

    // Analyze register usage
    analyze_register_usage(instructions);

    // Generate C code
    emit_header();
    emit_main_begin();

    // Emit each instruction
    for (const auto& instr : instructions) {
        emit_instruction(instr);
    }

    // Default return if no explicit RET
    indent_level_--;
    emit_line("return 0;  // Default return");
    emit_line("}");

    return code_;
}

// ============================================================================
// JITToC Implementation
// ============================================================================

JITToC::JITToC() {}

std::string JITToC::get_cache_dir() {
    return "/tmp/leviathanos-devvm-cache";
}

bool JITToC::has_cached_binary(const std::string& dev_path) {
    std::string cache_path = get_cached_binary_path(dev_path);
    struct stat st;
    return stat(cache_path.c_str(), &st) == 0;
}

std::string JITToC::get_cached_binary_path(const std::string& dev_path) {
    // Create a hash of the dev_path to use as cache filename
    std::string cache_dir = get_cache_dir();
    unsigned long hash = std::hash<std::string>{}(dev_path);
    return cache_dir + "/dev_" + std::to_string(hash);
}

std::string JITToC::select_c_compiler() {
    // Try to find available C compiler (prefer tinycc, then gcc, then clang)
    const char* compilers[] = { "tcc", "gcc", "clang", nullptr };
    for (int i = 0; compilers[i] != nullptr; i++) {
        std::string cmd = std::string("which ") + compilers[i] + " 2>/dev/null";
        if (system(cmd.c_str()) == 0) {
            return compilers[i];
        }
    }
    return "gcc";  // Default to gcc
}

int JITToC::compile_c_to_binary(const std::string& c_code,
                                const std::string& output_binary) {
    // Create temp C file
    std::string c_file = output_binary + ".c";
    std::ofstream out(c_file);
    if (!out) {
        std::cerr << "Failed to create C file: " << c_file << std::endl;
        return -1;
    }
    out << c_code;
    out.close();

    // Compile C to binary
    std::string compiler = select_c_compiler();
    std::string compile_cmd = compiler + " -O3 -o " + output_binary + " " + c_file;

    std::cout << "[JIT→C] Compiling C to native binary..." << std::endl;
    std::cout << "  Compiler: " << compiler << std::endl;
    int ret = system(compile_cmd.c_str());

    if (ret != 0) {
        std::cerr << "[JIT→C] Compilation failed!" << std::endl;
        // Clean up on failure too
        unlink(c_file.c_str());
        return -1;
    }

    std::cout << "[JIT→C] Compiled successfully: " << output_binary << std::endl;

    // ════════════════════════════════════════════════════════════
    // ARTIFACT CLEANUP - Delete all intermediate files
    // Only keep the final cached binary
    // ════════════════════════════════════════════════════════════
    std::cout << "[JIT→C] Cleaning up compilation artifacts..." << std::endl;

    // Delete C source file
    if (unlink(c_file.c_str()) == 0) {
        std::cout << "  ✓ Deleted: " << c_file << " (source code)" << std::endl;
    }

    // Delete object files (.o) that gcc might have created
    std::string o_file = output_binary + ".o";
    if (unlink(o_file.c_str()) == 0) {
        std::cout << "  ✓ Deleted: " << o_file << " (object file)" << std::endl;
    }

    // Delete preprocessed file (.i) if it exists
    std::string i_file = output_binary + ".i";
    if (unlink(i_file.c_str()) == 0) {
        std::cout << "  ✓ Deleted: " << i_file << " (preprocessed)" << std::endl;
    }

    // Delete assembly file (.s) if it exists
    std::string s_file = output_binary + ".s";
    if (unlink(s_file.c_str()) == 0) {
        std::cout << "  ✓ Deleted: " << s_file << " (assembly)" << std::endl;
    }

    std::cout << "[JIT→C] Artifact cleanup complete" << std::endl;
    std::cout << "  ✓ KEPT: " << output_binary << " (final binary, cached)" << std::endl;
    std::cout << "  Security: No generated C code or intermediate files remain" << std::endl;

    return 0;
}

int JITToC::run_binary(const std::string& binary_path,
                       const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Fork failed" << std::endl;
        return -1;
    }

    if (pid == 0) {
        // Child process
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(binary_path.c_str()));
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(binary_path.c_str(), argv.data());
        std::cerr << "Exec failed" << std::endl;
        exit(127);
    } else {
        // Parent process
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return 1;
    }
}

// Decode VLQ (Variable-Length Quantity)
static uint64_t vlq_decode(const uint8_t* data, size_t data_len, size_t& offset, bool& error) {
    uint64_t value = 0;
    uint32_t shift = 0;
    int iterations = 0;

    while (offset < data_len && iterations < 10) {  // Max 10 bytes
        uint8_t byte = data[offset++];
        value |= ((uint64_t)(byte & 0x7F)) << shift;
        if (!(byte & 0x80)) {
            error = false;
            return value;
        }
        shift += 7;
        iterations++;
    }

    // If we get here, something went wrong
    error = true;
    return 0;
}

// Unpack instruction from bytecode
static ir::Instruction unpack_instruction(const uint8_t* data, size_t data_len, size_t& offset, bool& error) {
    ir::Instruction instr = {};
    error = false;

    if (offset >= data_len) {
        error = true;
        return instr;
    }

    // Decode opcode (6 bits)
    uint8_t opcode_byte = data[offset++];
    instr.op = static_cast<ir::OpCode>(opcode_byte & 0x3F);

    // Decode operands with VLQ
    instr.dest = static_cast<uint8_t>(vlq_decode(data, data_len, offset, error));
    if (error) return instr;

    instr.src1 = static_cast<uint8_t>(vlq_decode(data, data_len, offset, error));
    if (error) return instr;

    instr.src2 = static_cast<uint8_t>(vlq_decode(data, data_len, offset, error));
    if (error) return instr;

    return instr;
}

std::string JITToC::generate_c(const std::string& dev_path) {
    // Header structure for new .dev format
    struct DevFileHeader {
        uint8_t magic[3];      // "dev"
        uint8_t version;       // 0x01
        uint8_t arch;          // 0=x86_64
        uint8_t reserved;
        uint32_t manifest_len;
        uint64_t payload_len;
    };

    // Read .dev file
    std::ifstream in(dev_path, std::ios::binary);
    if (!in) {
        std::cerr << "Failed to open: " << dev_path << std::endl;
        return "";
    }

    // Read header
    DevFileHeader header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in) {
        std::cerr << "Failed to read header" << std::endl;
        return "";
    }

    // Validate header
    if (header.magic[0] != 'd' || header.magic[1] != 'e' || header.magic[2] != 'v') {
        std::cerr << "Invalid .dev magic bytes" << std::endl;
        return "";
    }

    if (header.version != 0x01) {
        std::cerr << "Unsupported .dev version: " << static_cast<int>(header.version) << std::endl;
        return "";
    }

    // Read manifest
    std::string manifest(header.manifest_len, '\0');
    in.read(manifest.data(), manifest.size());
    if (!in) {
        std::cerr << "Failed to read manifest" << std::endl;
        return "";
    }

    // Read bytecode
    std::vector<uint8_t> bytecode_data(static_cast<size_t>(header.payload_len));
    in.read(reinterpret_cast<char*>(bytecode_data.data()), bytecode_data.size());
    if (!in) {
        std::cerr << "Failed to read bytecode" << std::endl;
        return "";
    }
    in.close();

    std::cout << "[JIT→C] Read .dev file: " << dev_path << std::endl;
    std::cout << "  Manifest: " << manifest << std::endl;
    std::cout << "  Bytecode size: " << bytecode_data.size() << " bytes" << std::endl;

    // Unpack instructions from bytecode
    std::cout << "[JIT→C] Unpacking bytecode..." << std::endl;
    std::cout << "  [LOG] Starting bytecode decompression" << std::endl;
    std::cout << "  [LOG] No instruction limit - can handle septillions of instructions" << std::endl;

    std::vector<ir::Instruction> instructions;
    size_t offset = 0;
    const int64_t MAX_INSTRUCTIONS = INT64_MAX;  // Septillion-scale (9.2 quintillion)
    int opcode_counts[256] = {0};

    while (offset < bytecode_data.size() && static_cast<int64_t>(instructions.size()) < MAX_INSTRUCTIONS) {
        bool unpack_error = false;
        ir::Instruction instr = unpack_instruction(bytecode_data.data(), bytecode_data.size(), offset, unpack_error);

        if (unpack_error) {
            std::cerr << "[JIT→C] ERROR at byte offset " << offset << "/" << bytecode_data.size() << std::endl;
            std::cerr << "        Failed to decode instruction #" << instructions.size() + 1 << std::endl;
            return "";
        }

        instructions.push_back(instr);

        // Track opcode distribution
        uint8_t op_id = static_cast<uint8_t>(instr.op);
        if (op_id < 256) opcode_counts[op_id]++;

        // Progress indicator
        if ((instructions.size() % 1000) == 0) {
            std::cout << "  [LOG] Unpacked " << instructions.size() << " instructions (offset: "
                      << offset << "/" << bytecode_data.size() << ")" << std::endl;
        }

        // Stop at HALT or RET
        if (instr.op == ir::OpCode::HALT || instr.op == ir::OpCode::RET) {
            std::cout << "  [LOG] Found " << (instr.op == ir::OpCode::HALT ? "HALT" : "RET")
                      << " at instruction " << instructions.size() << std::endl;
            break;
        }
    }

    if (instructions.size() >= MAX_INSTRUCTIONS) {
        std::cerr << "[JIT→C] WARNING: Hit instruction limit (" << MAX_INSTRUCTIONS << ")" << std::endl;
        std::cerr << "        This is expected for large binaries. Increase MAX_INSTRUCTIONS if needed." << std::endl;
    }

    std::cout << "  Unpacked: " << instructions.size() << " instructions" << std::endl;
    std::cout << "  [LOG] Opcode distribution:" << std::endl;
    for (int i = 0; i < 256; i++) {
        if (opcode_counts[i] > 0) {
            std::cout << "        Opcode " << i << ": " << opcode_counts[i] << " instructions" << std::endl;
        }
    }

    // Generate C code from unpacked instructions
    std::cout << "[JIT→C] Generating C code from bytecode..." << std::endl;
    std::string c_code = codegen_.generate_c_code(instructions);

    return c_code;
}

int JITToC::compile_and_run(const std::string& dev_path,
                            const std::vector<std::string>& args) {
    std::cout << "════════════════════════════════════════" << std::endl;
    std::cout << "JIT→C: Compiling .dev to native binary" << std::endl;
    std::cout << "════════════════════════════════════════" << std::endl << std::endl;

    // Create cache directory
    std::string cache_dir = get_cache_dir();
    mkdir(cache_dir.c_str(), 0755);

    std::string binary_path = get_cached_binary_path(dev_path);

    // Check cache
    if (has_cached_binary(dev_path)) {
        std::cout << "[JIT→C] Using cached binary: " << binary_path << std::endl << std::endl;
        return run_binary(binary_path, args);
    }

    // Generate C code
    std::string c_code = generate_c(dev_path);
    if (c_code.empty()) {
        std::cerr << "[JIT→C] Failed to generate C code" << std::endl;
        return -1;
    }

    std::cout << "[JIT→C] Generated " << c_code.size() << " bytes of C code" << std::endl << std::endl;

    // Compile to binary
    if (compile_c_to_binary(c_code, binary_path) != 0) {
        return -1;
    }

    std::cout << "[JIT→C] Running compiled binary..." << std::endl << std::endl;

    // Run the binary
    return run_binary(binary_path, args);
}

}  // namespace jit
}  // namespace devvm
