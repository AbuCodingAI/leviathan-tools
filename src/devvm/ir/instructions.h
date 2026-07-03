#pragma once

#include <cstdint>
#include <string>

namespace devvm {
namespace ir {

// ═══════════════════════════════════════════════════════════════════════════
// LEVIATHAN BYTECODE IR - COMPLETE INSTRUCTION SET
// ═══════════════════════════════════════════════════════════════════════════
//
// Format Variants:
// 1. Regular byte opcode:  [opcode:8 | dest:8 | src1:8 | src2:8]
// 2. Text opcode (CALL):   "CALL" + null-terminated function name
// 3. Multi-byte opcode:    "MB" [ext_opcode:8 | dest:8 | src1:8 | src2:8] "MBE"
//
// Single-byte opcodes:     0x00-0xFF (256 total)
//   0x00-0x6F:   Regular opcodes (112)
//   0x70-0x7F:   Import/dependency system (16)
//   0x80-0x8F:   File I/O operations (16) - from CFS
//   0x90-0x9F:   96-bit 3D vector ops (13, 3 reserved)
//   0xA0-0xFF:   OI (Overspill Includes) - reserved for future (96)
//
// Multi-byte opcodes:      "MB" [id:8] ... "MBE" (unlimited, for future)
// Text opcodes:            "CALL" function_name, etc.
// ═══════════════════════════════════════════════════════════════════════════

enum class OpCode : uint8_t {
  // ═══════════════════════════════════════════════════════════════════════
  // CONTROL FLOW (0x00-0x0F)
  // ═══════════════════════════════════════════════════════════════════════
  NOP = 0x00,
  HALT = 0x01,
  JMP = 0x02,
  JZ = 0x03,        // Jump if zero
  JNZ = 0x04,       // Jump if not zero
  RET = 0x06,       // Return from function (CALL is text-based)

  // ═══════════════════════════════════════════════════════════════════════
  // ARITHMETIC (3-address: dest = src1 op src2) (0x10-0x1F)
  // ═══════════════════════════════════════════════════════════════════════
  ADD = 0x10,
  SUB = 0x11,
  MUL = 0x12,
  DIV = 0x13,
  MOD = 0x14,
  AND = 0x15,
  OR = 0x16,
  XOR = 0x17,
  SHL = 0x18,       // Shift left
  SHR = 0x19,       // Shift right

  // ═══════════════════════════════════════════════════════════════════════
  // UNARY OPERATIONS (0x20-0x2F)
  // ═══════════════════════════════════════════════════════════════════════
  NEG = 0x20,       // Negate (two-address)
  NOT = 0x21,       // Logical NOT
  CAST = 0x22,      // Type cast

  // ═══════════════════════════════════════════════════════════════════════
  // MEMORY OPERATIONS (0x30-0x3F)
  // ═══════════════════════════════════════════════════════════════════════
  LOAD = 0x30,      // dest = [addr]
  STORE = 0x31,     // [addr] = value
  MOVE = 0x32,      // dest = src (register move)
  ALLOC = 0x33,     // dest = allocate(size)

  // ═══════════════════════════════════════════════════════════════════════
  // SYSCALLS & I/O (0x40-0x4F)
  // ═══════════════════════════════════════════════════════════════════════
  SYSCALL = 0x40,   // Sandboxed syscall (abstraction layer)

  // ═══════════════════════════════════════════════════════════════════════
  // STACK & FUNCTION (0x50-0x5F)
  // ═══════════════════════════════════════════════════════════════════════
  PUSH = 0x50,
  POP = 0x51,
  FRAME = 0x52,     // Setup stack frame

  // ═══════════════════════════════════════════════════════════════════════
  // COMPARISON & FLAGS (0x60-0x6F)
  // ═══════════════════════════════════════════════════════════════════════
  CMP = 0x60,       // Compare, set flags
  EQ = 0x61,        // Equal
  NE = 0x62,        // Not equal
  LT = 0x63,        // Less than
  LE = 0x64,        // Less or equal
  GT = 0x65,        // Greater than
  GE = 0x66,        // Greater or equal

  // ═══════════════════════════════════════════════════════════════════════
  // IMPORT SYSTEM (0x70-0x7F) - DEPENDENCY/LIBRARY MANAGEMENT
  // ═══════════════════════════════════════════════════════════════════════
  // Import external .so libraries (numpy, tkinter, pygame, etc.)
  // Mapping is configured during .dev compilation via manifest
  //
  // Example mapping (in .dev manifest):
  //   0x71 → numpy
  //   0x72 → tkinter
  //   0x73 → turtle
  //   0x74 → pygame
  //   etc.
  // ═══════════════════════════════════════════════════════════════════════
  IMPORT = 0x70,    // Import library (dest = library_handle)
  // 0x71-0x7F: Library call slots (reserved for dynamic dispatch)

  // ═══════════════════════════════════════════════════════════════════════
  // FILE I/O OPERATIONS (0x80-0x8F) - FROM CFS (COMPILE FILE SYSTEM)
  // ═══════════════════════════════════════════════════════════════════════
  FOPEN = 0x80,     // Open file by hierarchical tag
  FREAD = 0x81,     // Read from file
  FWRITE = 0x82,    // Write to file
  FCLOSE = 0x83,    // Close file handle
  FSEEK = 0x84,     // Seek within file
  FTELL = 0x85,     // Get file position
  FSIZE = 0x86,     // Get file size
  FEXISTS = 0x87,   // Check if file exists

  // ═══════════════════════════════════════════════════════════════════════
  // 96-BIT 3D VECTOR OPERATIONS (0x90-0x9F) - LEVIATHAN UNIQUE FEATURE
  // ═══════════════════════════════════════════════════════════════════════
  // ONLY bytecode with native 96-bit 3D vector support
  // 96-bit = 3 × 32-bit floats (x, y, z)
  //
  // Enables without GPU/libraries:
  // - 3D game engines (vertices, matrices, transforms)
  // - Physics simulations (position, velocity, acceleration)
  // - Graphics (rotations, scaling, projections)
  // - VFX (particles, cloth, fluids)
  // - Motion capture (skeletal animation)
  // ═══════════════════════════════════════════════════════════════════════
  LOAD96 = 0x90,    // Load 96-bit vector from memory
  STORE96 = 0x91,   // Store 96-bit vector to memory
  MOVE96 = 0x92,    // Move vector between registers

  ADD96 = 0x93,     // Vector addition (component-wise)
  SUB96 = 0x94,     // Vector subtraction (component-wise)
  MUL96 = 0x95,     // Vector multiply (component-wise)
  DIV96 = 0x96,     // Vector divide (component-wise)

  DOT96 = 0x97,     // Dot product (returns scalar)
  CROSS96 = 0x98,   // Cross product (returns 96-bit vector)
  NORMALIZE96 = 0x99, // Normalize vector to unit length
  LENGTH96 = 0x9A,  // Vector magnitude

  TRANSFORM96 = 0x9B, // Apply 4×4 matrix transform
  LERP96 = 0x9C,    // Linear interpolation

  // ═══════════════════════════════════════════════════════════════════════
  // OVERSPILL INCLUDES (OI) (0xA0-0xFF) - RESERVED FOR FUTURE
  // ═══════════════════════════════════════════════════════════════════════
  // When 0xA0-0xFF fills, switch to multi-byte encoding: "MB" ... "MBE"
};

// ═══════════════════════════════════════════════════════════════════════════
// SPECIAL TEXT-BASED INSTRUCTIONS
// ═══════════════════════════════════════════════════════════════════════════
// These are NOT encoded as bytes, but as literal ASCII text in bytecode:
//
// CALL:  "CALL" + null-terminated function name
//   Example: 0x43 0x41 0x4C 0x4C "malloc" 0x00
//   Or:      0x43 0x41 0x4C 0x4C "numpy.dot" 0x00
//
// MB:    Multi-byte opcode (for extended opcodes beyond 0xFF)
//   Format: "MB" [ext_opcode:8 | dest:8 | src1:8 | src2:8] "MBE"
//   Example: 0x4D 0x42 [0x01] [0x01] [0x02] [0x03] 0x4D 0x42 0x45
// ═══════════════════════════════════════════════════════════════════════════

// Standard instruction encoding
struct Instruction {
  OpCode op;
  uint8_t dest;     // Destination register
  uint8_t src1;     // Source register 1
  uint8_t src2;     // Source register 2

  static Instruction encode(OpCode op, uint8_t dest = 0, uint8_t src1 = 0, uint8_t src2 = 0);
  static Instruction decode(uint32_t raw);
  uint32_t encode() const;
};

// Text-based instructions
struct CallInstruction {
  std::string function_name;  // "malloc", "numpy.dot", etc.
};

struct MultiByteInstruction {
  uint8_t ext_opcode;         // Extended opcode ID (0x00-0xFF)
  uint8_t dest;
  uint8_t src1;
  uint8_t src2;
};

// ═══════════════════════════════════════════════════════════════════════════
// EXAMPLE IR SEQUENCE
// ═══════════════════════════════════════════════════════════════════════════
//
// int add(int a, int b) { return a + b; }
//
// Bytecode:
//   FRAME R0, 2        // Setup frame with 2 locals
//   MOVE R1, R0        // R1 = param a
//   MOVE R2, R1        // R2 = param b
//   ADD R3, R1, R2     // R3 = R1 + R2
//   MOVE RAX, R3       // RAX = return value
//   RET                // Return
//
// With function call:
//   CALL "malloc"      // Call malloc (text opcode)
//   MOVE R0, RAX       // R0 = returned pointer
//
// With 3D vector:
//   LOAD96 V0, [R0]    // Load vector from memory
//   ADD96 V1, V0, V0   // V1 = V0 + V0 (double it)
//   TRANSFORM96 V2, V1 // Apply transform
//   STORE96 [R1], V2   // Store result
// ═══════════════════════════════════════════════════════════════════════════

}  // namespace ir
}  // namespace devvm
