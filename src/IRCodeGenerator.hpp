#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "DataSegment.hpp"
#include "IR.hpp"
#include "ObjectFile.hpp"
#include "Parser.hpp"

// Target backend for the verified IR. It intentionally shares the existing
// GSU ABI and byte encodings, while keeping AST ownership out of codegen.
class IRCodeGenerator {
public:
    IRCodeGenerator(
        const std::map<std::string, Analyzer::LocalSymbolTable>& all_local_symbols,
        const std::map<std::string, FunctionSymbol>& global_function_symbols,
        const DataSegmentManager& data_manager,
        const CompilerConfig& config);

    ObjectFile generate(const IRModule& module);

private:
    struct BranchFixup {
        std::size_t patch_offset = 0;
        IRBlockId target;
        Token source = {TokenType::UNKNOWN, "", 0, 0};
    };

    void emitByte(std::uint8_t byte);
    void emitWord(std::uint16_t word);
    void emitLiteral(std::int64_t value);
    void emitMove(std::uint8_t destination, std::uint8_t source);
    void emitPush(std::uint8_t reg);
    void emitPop(std::uint8_t reg);
    void emitStore(std::uint8_t address_reg, std::uint8_t value_reg, bool byte);
    void emitImmediateArithmetic(std::uint8_t op_base, std::int64_t value,
                                 const Token& source);
    void emitAdjustStack(std::size_t bytes, bool add, const Token& source);
    void emitFunctionEpilogue();

    void emitBlock(const IRBasicBlock& block);
    void emitInstruction(const IRInstruction& instruction);
    void emitBranch(IRBlockId target, const Token& source);
    void emitConditionalBranch(const IRInstruction& instruction);
    void emitSwitch(const IRInstruction& instruction);
    void patchBranches();

    std::uint8_t scratchRegister() const;
    const IRInstruction& producer(IRValueId value, const Token& source) const;
    bool constantValue(IRValueId value, std::int64_t& result) const;
    void materialize(IRValueId value);
    void emitAddress(const IRInstruction& instruction);
    void emitBinary(const IRInstruction& instruction);
    void emitCall(const IRInstruction& instruction);
    void emitCast(const IRInstruction& instruction);
    void emitLoadIndirect(const IRInstruction& instruction);
    void emitStoreIndirect(const IRInstruction& instruction);
    void emitHardwareLoop(const IRInstruction& instruction);
    void addRelocation(const std::string& symbol, std::size_t patch_offset,
                       RelocationType type);
    void fail(const std::string& message, const Token& source) const;

    const std::map<std::string, Analyzer::LocalSymbolTable>& m_all_local_symbols;
    const std::map<std::string, FunctionSymbol>& m_global_function_symbols;
    const DataSegmentManager& m_data_manager;
    const CompilerConfig& m_config;

    ObjectFile m_object_file;
    const IRFunction* m_current_function = nullptr;
    std::map<std::uint32_t, const IRInstruction*> m_values;
    std::map<std::uint32_t, std::size_t> m_use_counts;
    std::set<std::uint32_t> m_active_values;
    std::map<std::uint32_t, std::size_t> m_block_addresses;
    std::vector<BranchFixup> m_branch_fixups;
    std::size_t m_current_block_index = 0;
    bool m_isInPlottingContext = false;
};
