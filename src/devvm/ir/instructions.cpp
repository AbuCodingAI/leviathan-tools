#include "instructions.h"

namespace devvm {
namespace ir {

Instruction Instruction::encode(OpCode op, uint8_t dest, uint8_t src1, uint8_t src2) {
  return {op, dest, src1, src2};
}

Instruction Instruction::decode(uint32_t raw) {
  return {
      static_cast<OpCode>((raw >> 24) & 0xFF),
      static_cast<uint8_t>((raw >> 16) & 0xFF),
      static_cast<uint8_t>((raw >> 8) & 0xFF),
      static_cast<uint8_t>(raw & 0xFF),
  };
}

uint32_t Instruction::encode() const {
  return (static_cast<uint32_t>(op) << 24) |
         (static_cast<uint32_t>(dest) << 16) |
         (static_cast<uint32_t>(src1) << 8) |
         static_cast<uint32_t>(src2);
}

}  // namespace ir
}  // namespace devvm
