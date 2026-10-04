#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "DataSegment.hpp"
#include "IR.hpp"
#include "LinearScanAllocator.hpp"
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
        bool long_form = false;
    };

    struct LocalBranchFixup {
        std::size_t patch_offset = 0;
        std::size_t target_offset = 0;
        Token source = {TokenType::UNKNOWN, "", 0, 0};
    };

    void emitByte(std::uint8_t byte);
    void emitWord(std::uint16_t word);
    void emitLiteral(std::int64_t value);
    void emitMove(std::uint8_t destination, std::uint8_t source);
    void emitPush(std::uint8_t reg);
    void emitStackGuard(std::size_t required, const Token& source);
    void emitPop(std::uint8_t reg);
    void emitStore(std::uint8_t address_reg, std::uint8_t value_reg, bool byte);
    void emitImmediateArithmetic(std::uint8_t op_base, std::int64_t value,
                                 const Token& source);
    void emitAdjustStack(std::size_t bytes, bool add, const Token& source);
    void emitFunctionEpilogue();

    void emitBlock(const IRBasicBlock& block);
    void emitInstruction(const IRInstruction& instruction);
    void emitBranch(IRBlockId target, const Token& source);
    void emitBlockBranch(std::uint8_t opcode, IRBlockId target, const Token& source);
    void emitConditionalBranch(const IRInstruction& instruction);
    void emitSwitch(const IRInstruction& instruction);
    void patchBranches();
    void patchLocalBranches(const std::vector<LocalBranchFixup>& fixups);

    std::uint8_t scratchRegister() const;
    const IRInstruction& producer(IRValueId value, const Token& source) const;
    bool constantValue(IRValueId value, std::int64_t& result) const;
    void materialize(IRValueId value);
    void emitAddress(const IRInstruction& instruction);
    void emitBinary(const IRInstruction& instruction);
    void emitIntegerOperation(const IRInstruction& instruction);
    void emitCall(const IRInstruction& instruction);
    void emitCast(const IRInstruction& instruction);
    void emitBoolNormalization();
    void emitLoadIndirect(const IRInstruction& instruction);
    void emitStoreIndirect(const IRInstruction& instruction);
    void emitHardwareLoop(const IRInstruction& instruction);
    void emitRegisterLiteral(std::uint8_t reg, std::uint16_t value);
    void emitNearBank(std::uint8_t reg, AddressSpace space);
    void emitSelectBank(std::uint8_t reg, AddressSpace space);
    void emitAddressCheck(const Type& pointer, int width);
    void emitGuard(std::uint8_t success_branch, std::uint16_t fault);
    void emitAddressFault(std::uint16_t fault);
    void emitCompare(std::uint8_t left, std::uint16_t right);
    std::string localLabel();
    void bindLabel(const std::string& name);
    void emitLocalJump(const std::string& name, std::uint8_t condition = 5);
    void emitPointerValueCheck(const Type& type, int width);
    void emitPointerCompare(const IRInstruction& instruction);
    void emitPointerOffset(const IRInstruction& instruction);
    void emitSpill(IRValueId value, bool load);
    bool needsPointerSpill(const IRInstruction& instruction) const;
    void addRelocation(const std::string& symbol, std::size_t patch_offset,
                       RelocationType type);
    void fail(const std::string& message, const Token& source) const;
    void buildRegisterAllocation(const IRFunction& function);
    bool usesRegisterAllocation(IRValueId value) const;
    void saveLiveRegistersForCall(const IRInstruction& instruction,
                                  std::vector<std::uint8_t>& saved_registers);
    void restoreRegistersAfterCall(const std::vector<std::uint8_t>& saved_registers);
    ObjectFile generateInternal(const IRModule& module);

    const std::map<std::string, Analyzer::LocalSymbolTable>& m_all_local_symbols;
    const std::map<std::string, FunctionSymbol>& m_global_function_symbols;
    const DataSegmentManager& m_data_manager;
    const CompilerConfig& m_config;

    ObjectFile m_object_file;
    const IRFunction* m_current_function = nullptr;
    std::map<std::uint32_t, const IRInstruction*> m_values;
    std::map<std::uint32_t, std::size_t> m_use_counts;
    std::set<std::uint32_t> m_active_values;
    std::set<std::uint32_t> m_materialized_values;
    LinearScanAllocator m_register_allocator;
    std::map<std::uint32_t, std::size_t> m_block_addresses;
    std::vector<BranchFixup> m_branch_fixups;
    std::size_t m_current_block_index = 0;
    std::size_t m_current_instruction_position = 0;
    std::size_t m_emission_position = 0;
    bool m_isInPlottingContext = false;
    int m_known_plot_options = -1;
    bool m_force_long_branches = false;
    bool m_checked_pointer_mode = false;
    IRValueId m_emitting_value;
    std::map<std::uint32_t, int> m_spill_offsets;
    std::size_t m_local_label_serial = 0;
};
