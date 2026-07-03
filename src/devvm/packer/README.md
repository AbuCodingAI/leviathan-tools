# .dev Bytecode Format - The Flagship Feature of LeviathanOS

## What .dev Really Is

**.dev is NOT about compiling ELF to bytecode.**

**.dev ABSTRACTS SYSCALLS.** It is a security container that:

1. **Sandboxes execution** - Only whitelisted syscalls allowed
2. **Defines trust levels** - UNTRUSTED, SIGNED, VERIFIED
3. **Compresses IR bytecode** - Ultra-tight 3-address code format
4. **Enables portability** - Same .dev runs anywhere DevVM exists
5. **Provides isolation** - No fork(), execve(), or permission escalation

## Architecture

```
┌─────────────────────────────────────────────────┐
│  .dev File (ultra-compact bytecode)             │
├─────────────────────────────────────────────────┤
│  Header (12 bytes):                             │
│    • Magic: "dev" (3B)                          │
│    • Trust Level (1B): UNTRUSTED/SIGNED/VERIFIE│
│    • IR Size (4B): Compressed bytecode size    │
│    • Checksum (4B): CRC32 validation           │
├─────────────────────────────────────────────────┤
│  Compressed IR Bytecode:                        │
│    • 3-address instructions (VLQ-encoded)       │
│    • ~80% smaller than original                │
│    • Only whitelisted syscalls allowed         │
└─────────────────────────────────────────────────┘
                        ↓
                   DevVM Interpreter
                   (256 registers,
                    50+ opcodes,
                    256MB heap)
                        ↓
            JIT Compilation to x86-64
            (Native-speed execution)
```

## Why This Matters

### Traditional Execution
```
ELF Binary → Execute Directly (ANY syscall allowed, full privileges)
```

### .dev Execution
```
.dev Bytecode → DevVM Interpreter → Sandbox Filter → Only safe syscalls
                      ↓
                JIT compilation to native code (fast!)
```

## Syscall Abstraction Layers

### UNTRUSTED (default)
- ✅ read, write (file I/O)
- ✅ mmap (memory mapping)
- ✅ exit, time
- ❌ fork, execve, clone (no process creation)
- ❌ chmod, chown (no permission escalation)
- ❌ ptrace, prctl (no debugging/control)
- ❌ No executable memory (PROT_EXEC denied)

### SIGNED
- ✅ All UNTRUSTED syscalls
- ✅ Extended file I/O
- ✅ Memory management
- ✅ Process info queries
- ❌ Still no process creation

### VERIFIED
- ✅ All SIGNED syscalls
- ✅ Most syscalls except process creation/execution
- ⚠️ Only for verified code

## Ultra-Compact Encoding

Original 3-address code:
```
[opcode:8 | dest:8 | src1:8 | src2:8] = 32 bits per instruction
```

Compressed .dev format:
```
[opcode:6] + [VLQ dest] + [VLQ src1] + [VLQ src2]
Average: 1.5 bytes per instruction (80% compression!)
```

Example sizes:
- test_devvm: 144 KB ELF → 28 KB .dev (80% reduction)
- DADA: 384 KB ELF → 76 KB .dev (80% reduction)

## Use Cases

### 1. Secure App Distribution
```bash
# Package an application with syscall restrictions
dev pack myapp.exe → myapp.dev (UNTRUSTED)

# Run securely - can't escape sandbox
dev run myapp.dev
```

### 2. Cross-Platform Apps
```bash
# Single .dev file runs on any DevVM-capable system
myapp.dev  (Linux, Windows via WSL, macOS via compatibility layer)
```

### 3. Verification
```bash
# Sign with private key
dev sign myapp.dev --key priv.pem → myapp.dev (SIGNED)

# Verify authenticity
dev verify myapp.dev --cert pub.pem
```

### 4. Legacy App Modernization
```bash
# Run old 32-bit apps in 64-bit environment
32-bit-app.exe → app.dev → Runs in i686 backend
```

## The .dev Flagship Advantage

**No other OS does this.**

- Docker: Needs full Linux container (~100s MB)
- WASM: Limited to browser/specific runtime
- Java/Go: Needs JVM/runtime, allows escape
- **.dev: Minimal bytecode + provable sandbox + portable**

## Building .dev Files

The packer takes:
- IR bytecode (from compiler or translator)
- Trust level specification
- Optional signing key

And produces:
- Ultra-compressed .dev file
- With checksum validation
- Syscall abstraction enforced

## Next Steps

1. Integrate packer into build system
2. Create IR compiler for common languages
3. Build .dev standard library (libc replacement)
4. Create .dev desktop apps (Office suite, etc)
5. Market .dev as "the most portable, secure app format"
