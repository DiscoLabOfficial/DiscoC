#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "DataSegment.hpp"
#include "GSUAddressProof.hpp"
#include "GSUStackCheckCredit.hpp"
#include "IR.hpp"
#include "IRGlobalOptimizer.hpp"
#include "LinearScanAllocator.hpp"
#include "GSUSpillCache.hpp"
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
        std::string local_target;
        std::size_t serial = 0;
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
    bool matchesLastRamWordAddress(IRValueId address) const;
    void emitImmediateArithmetic(std::uint8_t op_base, std::int64_t value,
                                 const Token& source);
    void emitAdjustStack(std::size_t bytes, bool add, const Token& source);
    void emitFunctionEpilogue();

    void emitBlock(const IRBasicBlock& block);
    void emitInstruction(const IRInstruction& instruction);
    void emitBranch(IRBlockId target, const Token& source);
    void emitBlockBranch(std::uint8_t opcode, IRBlockId target, const Token& source);
    bool longBranch(std::size_t serial) const;
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
    bool emitAllocatedOperation(const IRInstruction& instruction, std::uint8_t destination);
    bool emitConstantArithmetic(const IRInstruction& instruction);
    void emitConstantShift(bool left, unsigned count, bool unsigned_right);
    void emitWordMask(std::uint16_t mask);
    void emitBitExtract(const IRInstruction& instruction);
    void emitBinaryOperands(IRValueId left, IRValueId right);
    void emitIntegerOperation(const IRInstruction& instruction);
    void emitDivMod(const IRInstruction& instruction);
    void emitDivModResult(const IRInstruction& instruction);
    void emitCall(const IRInstruction& instruction);
    void emitCast(const IRInstruction& instruction);
    void emitBoolNormalization();
    void emitLoadIndirect(const IRInstruction& instruction);
    void emitStoreIndirect(const IRInstruction& instruction);
    void emitHardwareLoop(const IRInstruction& instruction);
    void emitMemoryInitialize(const IRInstruction& instruction);
    void selectAutomaticCaches();
    void emitRegisterLiteral(std::uint8_t reg, std::uint16_t value);
    void emitWordLiteral(std::uint8_t reg, std::uint16_t value);
    bool optimized() const { return m_config.optimization != OptimizationLevel::Baseline; }
    bool globallyOptimized() const { return isGlobalOptimization(m_config.optimization); }
    bool sizeOptimized() const { return m_config.optimization == OptimizationLevel::Size; }
    bool provenAddress(IRValueId value, const Type& pointer, int width) const {
        return globallyOptimized() && m_address_proof.provesAccess(value, pointer, width);
    }
    void emitPhiCopies(IRBlockId target, const Token& source);
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
    void emitWordSpillStore(IRValueId value, std::uint8_t source);
    void retainSpillCopy(IRValueId value, bool after_load);
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
    ObjectFile generateOptimized(const IRModule& module, OptimizationLevel policy,
        IRCompactionPolicy compaction = IRCompactionPolicy::Enabled);
    void emitSharedDivision(const IRInstruction& instruction);
    void emitDivisionHelpers();

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
    std::set<std::uint32_t> m_branch_comparisons;
    LinearScanAllocator m_register_allocator;
    GSUAddressProof m_address_proof;
    GSUStackCheckCredit m_stack_check_credit;
    std::map<std::uint32_t, std::size_t> m_block_addresses;
    std::vector<BranchFixup> m_branch_fixups;
    std::size_t m_current_block_index = 0;
    std::size_t m_current_instruction_position = 0;
    std::size_t m_emission_position = 0;
    bool m_isInPlottingContext = false;
    int m_known_plot_options = -1;
    int m_known_color = -1;
    bool m_force_long_branches = false;
    // Stable emission ordinals, not byte offsets: widening an earlier branch
    // must not change the identity of a later one on the next bounded pass.
    std::map<std::string, std::set<std::size_t>> m_long_branches;
    std::size_t m_branch_serial = 0;
    bool m_checked_pointer_mode = false;
    bool m_has_hardware_loop = false;
    struct AutomaticCache { std::size_t offset; IRBlockId header; std::uint32_t loop_id, trips; };
    std::vector<AutomaticCache> m_auto_caches;
    bool m_manual_cache = false;
    IRValueId m_emitting_value;
    // A strictly adjacent scalar snapshot in R0, never a lexical variable or
    // pointer-pair alias. Any emitted byte or control-flow entry invalidates it.
    IRValueId m_accumulator_value;
    // SBK uses the last physical RAM address, not a register. Only remember
    // checked near-word accesses; stack traffic and control-flow joins fence it.
    IRValueId m_last_ram_word_address;
    std::map<std::uint32_t, int> m_spill_offsets;
    GSUSpillCache m_spill_cache;
    std::map<std::uint32_t, std::size_t> m_block_remaining_uses;
    // A private frame word owns each pair's secondary component across CFG
    // edges/calls. It is not an allocator value defined before its IR point.
    std::map<std::uint32_t, int> m_divmod_offsets;
    std::map<std::uint16_t, std::size_t> m_fault_offsets;
    std::string m_epilogue_label;
    bool m_share_division = false;
    OptimizationLevel m_allocation_policy = OptimizationLevel::O2;
    std::set<bool> m_division_candidates;
    std::set<bool> m_division_helpers;
    std::size_t m_local_label_serial = 0;
};
