// Test: Full JIT→C Pipeline
// Reads a .dev file, unpacks bytecode, generates C, compiles, and executes

#include "jit_to_c.h"
#include <iostream>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "════════════════════════════════════════════════════════════\n";
        std::cout << "  JIT→C Pipeline - Full Test\n";
        std::cout << "════════════════════════════════════════════════════════════\n\n";
        std::cout << "Usage: " << argv[0] << " <file.dev> [args...]\n\n";
        std::cout << "Example:\n";
        std::cout << "  " << argv[0] << " /tmp/test.dev\n\n";
        std::cout << "This will:\n";
        std::cout << "  1. Read .dev bytecode file\n";
        std::cout << "  2. Unpack IR instructions\n";
        std::cout << "  3. Generate C code\n";
        std::cout << "  4. Compile with gcc/clang/tcc\n";
        std::cout << "  5. Cache binary\n";
        std::cout << "  6. Execute\n\n";
        std::cout << "════════════════════════════════════════════════════════════\n";
        return 1;
    }

    std::string dev_path = argv[1];

    // Check if file exists
    if (!std::filesystem::exists(dev_path)) {
        std::cerr << "Error: File not found: " << dev_path << std::endl;
        return 1;
    }

    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << "  JIT→C Pipeline Test\n";
    std::cout << "════════════════════════════════════════════════════════════\n\n";

    // Collect remaining arguments
    std::vector<std::string> args;
    for (int i = 2; i < argc; i++) {
        args.push_back(argv[i]);
    }

    // Create JIT compiler and run
    devvm::jit::JITToC jit;
    int exit_code = jit.compile_and_run(dev_path, args);

    std::cout << "\n════════════════════════════════════════════════════════════\n";
    std::cout << "  Test Complete\n";
    std::cout << "════════════════════════════════════════════════════════════\n";
    std::cout << "  Exit code: " << exit_code << "\n";
    std::cout << "════════════════════════════════════════════════════════════\n\n";

    return exit_code;
}
