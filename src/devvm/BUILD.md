# DevVM Build and Integration Guide

## Quick Start

```bash
cd /media/abu/CoderDrive/LeviathanOS
make test
```

This builds and runs all 13 tests. Expected output: "All tests PASSED!"

## Build Targets

```bash
make              # Build all targets (devtool, dada, dashboard, test)
make test         # Build and run DevVM test suite
make clean        # Remove all binaries
```

## Compiler Requirements

- GCC 11+ or Clang 12+ with C++20 support
- Standard POSIX libc (for syscall wrapping)
- Linux kernel 3.2.0+ (for target runtime)

## Architecture

**x86-64 ONLY** - All code targets x86-64 architecture exclusively.

Conservative baseline flags:
- `-march=x86-64` (no AVX, no native)
- `-mtune=generic`
- `-mno-avx -mno-avx2`

This ensures compatibility with older CPUs while maintaining performance.

## Components

### 1. Core VM (`src/devvm/core/`)
- `devvm.h` - VM class, memory management, register file
- `devvm.cpp` - Full 50+ instruction interpreter

Features:
- 256 general-purpose 64-bit registers
- 256MB heap with allocation tracking
- Stack for function calls and local variables
- Program counter and stack pointer

### 2. Instruction Set (`src/devvm/ir/`)
- `instructions.h` - OpCode enum and instruction format
- `instructions.cpp` - Encoding/decoding logic

Format: 32-bit 3-address code [opcode:8 | dest:8 | src1:8 | src2:8]

### 3. JIT Compiler (`src/devvm/jit/`)
- `x86_64.h` - X86Emitter and JITCompiler classes
- `x86_64.cpp` - Machine code generation

Features:
- Full x86-64 instruction encoding
- Register allocation
- MOD-RM byte construction
- REX prefix handling

### 4. Sandboxing (`src/devvm/security/`)
- `sandbox.h` - Policy and syscall definitions
- `sandbox.cpp` - Syscall filtering and safe wrappers

Three trust levels:
- UNTRUSTED: Minimal syscalls (read, write, mmap, exit)
- SIGNED: Extended access (file I/O, memory management)
- VERIFIED: Most syscalls (except fork/execve/clone)

### 5. Tests (`src/devvm/tests/`)
- `test_devvm.cpp` - 13 comprehensive test cases

Coverage:
- Arithmetic, bitwise, shift operations
- Memory and stack operations
- Control flow and comparisons
- JIT compilation
- Sandbox enforcement
- Error handling

## Compilation Details

### Object Files Generated
```
src/devvm/core/devvm.cpp       -> compiled
src/devvm/ir/instructions.cpp   -> compiled
src/devvm/jit/x86_64.cpp        -> compiled
src/devvm/security/sandbox.cpp  -> compiled
src/devvm/tests/test_devvm.cpp  -> compiled
```

### Linking
```
bin/test_devvm <- all object files linked
```

### Compiler Warnings
- `-Wall -Wextra -Wpedantic` enabled
- Zero warnings in production code
- Any warnings = build failure

## Instruction Execution Flow

```
.dev file
    ↓
load_dev_file()
  ├─ Read magic bytes
  ├─ Verify signature
  ├─ Load IR instructions
  └─ Verify terminator
    ↓
VM::run()
  ├─ Loop until halted
  ├─ PC → instruction at ir_[PC]
  └─ execute_instruction(instr)
    ↓
execute_instruction()
  ├─ Decode [opcode, dest, src1, src2]
  ├─ Bounds-check registers
  ├─ Execute operation
  └─ Update PC
```

## Stack Layout During Execution

```
High Address
    ↑
    │ [Return Address] ← SP (after CALL)
    │ [Local Variables]
    │ [Caller's Frame]
    │ [...]
    ↓
Low Address
```

FRAME N creates N stack entries, PUSH/POP manage individual values.

## Memory Management

### Allocation
```c++
regs[0].u64 = 1024;  // Size
ALLOC R1, R0, 0      // R1 = malloc(1024)
```

### Deallocation
Not automatic - memory leaks until program exit.

### Access Violations
- Out-of-bounds read/write throws exception
- UNTRUSTED mode silently halts
- SIGNED/VERIFIED mode throws

## Security Model

### UNTRUSTED Code (Default)
```
Allowed:  read, write, mmap, mprotect, munmap, exit, time
Blocked:  fork, execve, clone, chmod, chown, ptrace
Restrict: No PROT_EXEC memory mappings
```

### SIGNED Code
```
Allowed:  + open, close, stat, fstat, getpid, getuid
Blocked:  fork, execve, clone
```

### VERIFIED Code
```
Allowed:  Most syscalls
Blocked:  fork, execve, clone (only)
```

## JIT Hot Path Example

Input IR:
```
ADD R2, R0, R1       // R2 = R0 + R1
SUB R3, R2, R1       // R3 = R2 - R1
```

Generated x86-64:
```asm
push rbp
mov rbp, rsp
mov rax, [regs + 0*8]    // Load R0
mov rcx, [regs + 1*8]    // Load R1
add rax, rcx             // ADD
mov [regs + 2*8], rax    // Store R2
mov rax, [regs + 2*8]    // Load R2
sub rax, rcx             // SUB
mov [regs + 3*8], rax    // Store R3
leave
ret
```

## Error Handling

All operations validate:
1. Register indices (0-255)
2. Memory bounds (0 to heap_size)
3. Stack bounds (no overflow/underflow)
4. Arithmetic (no div-by-zero)
5. Shifts (amount 0-63)

Validation failures throw `std::runtime_error` with description.

## Performance Characteristics

### Interpreter Mode
- ~1-2 CPU cycles per IR instruction
- Memory access: 256MB heap with fast lookup
- Branch prediction: JMP/JZ/JNZ use native jumps

### JIT Mode
- Machine code runs at native speed
- Compilation overhead amortized over hot loops
- Register allocation on-demand

## Testing

Run all tests:
```bash
make test
```

Individual test functions:
- `test_arithmetic_operations()` - ADD, SUB, MUL, DIV, MOD
- `test_bitwise_operations()` - AND, OR, XOR
- `test_shift_operations()` - SHL, SHR
- `test_comparison_operations()` - EQ, NE, LT, LE, GT, GE
- `test_memory_operations()` - Allocate and access
- `test_unary_operations()` - NEG, NOT
- `test_move_operation()` - Register moves
- `test_modulo_operation()` - MOD
- `test_jit_compilation()` - Code generation
- `test_sandbox_whitelist()` - Policy enforcement
- `test_halt_instruction()` - HALT opcode
- `test_division_by_zero()` - Error handling
- `test_register_bounds()` - Bounds validation

Expected: 13/13 PASS

## Integration Checklist

- [ ] Copy src/devvm/ to target project
- [ ] Update Makefile with DEVVM_TEST target
- [ ] Run `make test` - verify 13/13 pass
- [ ] Integrate VM::execute() with dev tool
- [ ] Implement .dev file compiler
- [ ] Create standard library (.dev SDK)
- [ ] Add debugging support (breakpoints, stack traces)
- [ ] Implement i686 backend for 32-bit

## Common Issues

**Build fails with "unknown architecture"**
- Check compiler version (need GCC 11+ or Clang 12+)
- Verify C++20 support: `g++ --version`

**Tests fail with "Division by zero"**
- Check MOD instruction - validates divisor != 0

**Sandbox blocks legitimate syscall**
- Review UNTRUSTED whitelist, may need SIGNED trust level

**JIT code crashes**
- X86Emitter may need debugging - add disassembly output

## Documentation

- `IMPLEMENTATION.md` - Full architecture and design
- `devvm_notes.md` - Security model and execution flow
- `BUILD.md` (this file) - Build and integration guide

## License and Attribution

DevVM is part of LeviathanOS Pv3 project.

Contact: abu.shariffaiml@gmail.com
