#pragma once

// SPC-700 backend: lowers verified IR to SPC-700 assembly text and assembles
// it in-process with the SPC700 encoder, so the object written directly and
// the one rebuilt from `--emit-asm` come from the same text.
//
// Strategy (stage 3 baseline): every IR value lives in its own frame slot and
// A/X/Y and the direct-page scratch bytes are used only inside one IR
// instruction. Frames follow the ABI in SPC700Target.hpp; run-time checks
// follow the language specification and report through the SPC700 fault
// channel. See docs/spc700-target.md for the supported-lowering matrix.

#include "Analyzer.hpp"
#include "DataSegment.hpp"
#include "IR.hpp"
#include "ObjectFile.hpp"
#include "TargetConfig.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

class SPC700CodeGenerator {
public:
    // Borrows the analyzer tables and data segment for the duration of
    // generate()/generateAssembly(); they must outlive this object.
    SPC700CodeGenerator(const std::map<std::string, Analyzer::LocalSymbolTable>& local_symbols,
                        const std::map<std::string, FunctionSymbol>& function_symbols,
                        const DataSegmentManager& data, const CompilerConfig& config);

    std::string generateAssembly(const IRModule& module);
    ObjectFile generate(const IRModule& module);

private:
    // Frame layout of the function being emitted, relative to FP.
    struct Frame {
        int locals = 0;       // L: source locals and IR temporaries, even
        int values = 0;       // V: IR value slots, even
        int arguments = 0;    // first argument offset = 2 + L + V
        std::map<std::uint32_t, int> value_offsets; // value id -> FP offset
        int bytes() const { return 2 + locals + values; } // allocated by the callee
    };

    const std::map<std::string, Analyzer::LocalSymbolTable>& m_local_symbols;
    const std::map<std::string, FunctionSymbol>& m_function_symbols;
    const DataSegmentManager& m_data;
    CompilerConfig m_config;

    std::ostringstream m_out;
    const IRFunction* m_function = nullptr;
    std::size_t m_function_index = 0;
    Frame m_frame;
    std::vector<const IRInstruction*> m_producers;
    std::size_t m_label_serial = 0;
    bool m_uses_unsigned_division = false;
    bool m_uses_signed_division = false;

    // --- Emission helpers (SPC700CodeGenerator.cpp)
    void emit(const std::string& line);
    void bind(const std::string& label);
    std::string newLabel();
    std::string blockLabel(IRBlockId block) const;
    std::string epilogueLabel() const;
    static std::string hex8(unsigned value);
    static std::string hex16(unsigned value);
    static std::string dp(unsigned address);
    [[noreturn]] void unsupported(const std::string& what, const Token& source) const;
    [[noreturn]] void internal(const std::string& what, const Token& source) const;

    // --- Layout and functions
    void emitFunction(const IRFunction& function);
    void layoutFrame(const IRFunction& function);
    int localOffset(int analyzer_offset) const;
    int parameterOffset(int analyzer_offset) const;
    const Analyzer::LocalSymbolTable& locals() const;
    std::string functionLabel(const std::string& name, bool& exported) const;
    void emitPrologue();
    void emitEpilogue();
    void emitData();
    void emitDivisionHelpers();

    // --- Frame and value access
    const IRInstruction& producer(IRValueId value, const Token& source) const;
    const Type& typeOf(IRValueId value, const Token& source) const;
    int slotBytes(const Type& type, const Token& source) const;
    void frameRead(int offset, unsigned destination, int bytes);
    void frameWrite(int offset, unsigned source, int bytes);
    // Loads a value into a direct-page word, extended to 16 bits by its own
    // type's signedness (bool and pointers are unsigned).
    void loadWord(IRValueId value, unsigned destination, const Token& source);
    void storeValue(const IRInstruction& instruction, unsigned source);
    void setWord(unsigned destination, std::uint16_t value);
    void emitFault(std::uint8_t code);
    void negateWord(unsigned address);

    // --- Instruction lowering
    void emitBlock(const IRBasicBlock& block);
    void emitInstruction(const IRInstruction& instruction);
    void emitAddress(const IRInstruction& instruction);
    void emitPointerOffset(const IRInstruction& instruction);
    void emitPointerCompare(const IRInstruction& instruction);
    void emitLoadIndirect(const IRInstruction& instruction);
    void emitStoreIndirect(const IRInstruction& instruction);
    void emitCall(const IRInstruction& instruction);
    void emitReturn(const IRInstruction& instruction);
    void emitCondBranch(const IRInstruction& instruction);
    void emitSwitch(const IRInstruction& instruction);
    // Address in the pointer scratch word: checks alignment, null and range
    // for an access of `width` bytes, as the language specification requires.
    void emitAddressCheck(const Type& pointer, int width, unsigned address);
    bool directSymbolAddress(IRValueId value, const Token& source) const;

    // --- Integer lowering (SPC700IntegerLowering.cpp)
    void emitBinary(const IRInstruction& instruction);
    void emitUnary(const IRInstruction& instruction);
    void emitCast(const IRInstruction& instruction);
    void emitComparison(const IRInstruction& instruction);
    void emitShift(const IRInstruction& instruction);
    void emitMultiply();
    void emitDivision(const IRInstruction& instruction);
    void normalizeBool(unsigned address);
};
