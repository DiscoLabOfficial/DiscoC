#pragma once

// Complete SPC-700 opcode map shared by the discas encoder and the assembly
// exporter, so encoding and decoding cannot drift apart. Mnemonics and operand
// spellings follow the canonical Sony syntax: `!` marks a 16-bit absolute
// address, an unmarked address is a direct-page byte.

#include <cstddef>
#include <cstdint>

namespace SPC700Isa {

enum class OperandPattern : std::uint8_t {
    None,
    A, X, Y, YA, SP, PSW, C,  // Registers / carry, written by name.
    Imm,                      // #imm8
    Dp, DpX, DpY,             // dp, dp+X, dp+Y
    Abs, AbsX, AbsY,          // !abs, !abs+X, !abs+Y
    IndX, IndXInc, IndY,      // (X), (X)+, (Y)
    DpXInd, DpIndY,           // [dp+X], [dp]+Y
    AbsXInd,                  // [!abs+X]
    DpBit,                    // dp.bit (bit encoded in the opcode)
    MemBit, NotMemBit,        // mem.bit, /mem.bit (13-bit address, 3-bit bit index)
    Rel,                      // 8-bit PC-relative branch target
    Upage,                    // PCALL page-$FF offset
    TcallN                    // TCALL vector number (encoded in the opcode)
};

// Order of operand bytes after the opcode. Most instructions follow source
// order; `op dp,#imm` and `op dp,dp` store the second operand first.
enum class OperandLayout : std::uint8_t { Normal, Reversed };

struct InstructionInfo {
    std::uint8_t opcode;
    const char* mnemonic;           // Lowercase canonical spelling.
    OperandPattern operands[2];
    std::uint8_t size;              // Total bytes including the opcode.
    OperandLayout layout;
    std::int8_t fixed;              // Bit/vector selected by the opcode, or -1.
};

// Every opcode 0x00-0xFF has exactly one entry; the reference is valid for the
// lifetime of the program.
const InstructionInfo& instruction(std::uint8_t opcode);

// Bytes an operand of `pattern` occupies in the instruction stream.
std::size_t operandBytes(OperandPattern pattern);

} // namespace SPC700Isa
