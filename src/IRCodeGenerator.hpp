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
#include "GSUStaticStackProof.hpp"
#include "IR.hpp"
#include "IRGlobalOptimizer.hpp"
#include "LinearScanAllocator.hpp"
#include "GSUSpillCache.hpp"
#include "ObjectFile.hpp"
#include "Opcodes.hpp"
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
    friend struct IRCodeGeneratorTestAccess;
    struct NeedsLongBranch {};
    // A stack-free leaf touched the stack after all; the pass is re-emitted
    // with that function excluded (m_stackful_leaves only grows).
    struct NeedsStackFrame {};
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

    enum class SpillCategory { IRValue, DivModAuxiliary };
    struct SpillIssue {
        std::string function;
        Token source = {TokenType::UNKNOWN, "", 0, 0};
        IRValueId value;
        SpillCategory category = SpillCategory::IRValue;
        std::size_t mapped_values = 0, word_slots = 0;
    };
    // Allocation evidence belongs to the exact emitted candidate, not the
    // mutable emitter left behind by a later Os/CACHE/relaxation attempt.
    // These owned diagnostics are invocation-only, never object metadata.
    struct FunctionMetric {
        std::uint64_t speed_score = 0;
        bool no_spills = false;
    };
    struct GeneratedCandidate {
        ObjectFile object;
        std::vector<SpillIssue> issues;
        std::uint64_t speed_score = 0;
        std::uint64_t inclusive_score = 0; // Os exchange evidence: callee-inclusive estimate.
        std::map<std::string, FunctionMetric> function_metrics;
        // Function name -> estimated fetch saving of its selected automatic
        // CACHE windows, in candidateSpeed's loop-weighted byte units.
        std::map<std::string, std::uint64_t> cache_credit;
    };
    struct CandidateReference {
        std::uint64_t speed_score;
        bool no_spills;
        std::map<std::string, FunctionMetric> function_metrics;
        CandidateReference(const GeneratedCandidate& candidate)
            : speed_score(candidate.speed_score), no_spills(candidate.issues.empty()),
              function_metrics(candidate.function_metrics) {}
    };

    void emitByte(std::uint8_t byte);
    void emitWord(std::uint16_t word);
    void emitLiteral(std::int64_t value);
    void emitMove(Register destination, std::uint8_t source);
    void emitPush(std::uint8_t reg);
    void emitStackGuard(std::size_t required, const Token& source);
    void emitPop(std::uint8_t reg);
    void emitStore(Register address, std::uint8_t value_reg, bool byte);
    bool matchesLastRamWordAddress(IRValueId address) const;
    void emitImmediateArithmetic(OpCode operation, std::int64_t value,
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
    bool patchBranches();
    void patchLocalBranches(const std::vector<LocalBranchFixup>& fixups);

    std::uint8_t scratchRegister() const;
    const IRInstruction& producer(IRValueId value, const Token& source) const;
    bool constantValue(IRValueId value, std::int64_t& result) const;
    void materialize(IRValueId value);
    void emitAddress(const IRInstruction& instruction);
    void emitBinary(const IRInstruction& instruction);
    void emitMultiply(const IRInstruction& instruction);
    bool emitAllocatedMultiply(const IRInstruction& instruction, std::uint8_t destination);
    bool emitAllocatedOperation(const IRInstruction& instruction, std::uint8_t destination);
    bool emitConstantArithmetic(const IRInstruction& instruction);
    void emitConstantShift(bool left, unsigned count, bool unsigned_right);
    void emitWordMask(std::uint16_t mask);
    void emitBitExtract(const IRInstruction& instruction);
    void emitBinaryOperands(const IRInstruction& instruction);
    void emitScalarComparison(const IRInstruction& instruction, bool reverse);
    void emitIntegerOperation(const IRInstruction& instruction);
    void emitDivMod(const IRInstruction& instruction);
    void emitDivModResult(const IRInstruction& instruction);
    void emitCall(const IRInstruction& instruction);
    void emitCast(const IRInstruction& instruction);
    void emitBoolNormalization();
    void emitLoadIndirect(const IRInstruction& instruction);
    void emitStoreIndirect(const IRInstruction& instruction);
    void emitHardwareLoop(const IRInstruction& instruction);
    void planHardwareLoopPreservation();
    void planRegionalLoopValues();
    void emitRegionalLoopValues(std::uint32_t loop_id);
    // Loads an innermost software loop's regional values when `entry_block`
    // branches into that loop (after the edge's PHI copies).
    void emitSoftwareRegionalValues(std::uint32_t entry_block, IRBlockId target);
    int regionalRegister(IRValueId value) const;
    bool phiCopiesEmpty(std::uint32_t from, IRBlockId target) const;
    bool phiCopiesUseR6(std::uint32_t from, IRBlockId target) const;
    IRBlockId threadedLoopEntry(IRBlockId target) const;
    void planCallClobbers(const IRModule& module);
    void planRegisterArguments(const IRModule& module);
    void planStaticFrames(const IRModule& module);
    // R3 (or R0) = address of the current frame's word at `offset` below/above
    // FP. A static frame's negative offsets resolve to link-time RAM instead.
    void emitFrameAddress(Register reg, int offset);
    void requireFramePointer(const Token& source) const;
    void planShortFrame();
    bool shortFrameSlot(int offset) const { return m_short_frame_offsets.count(offset) != 0; }
    void emitShortFrameAccess(bool load, std::uint8_t reg, int offset);
    bool framePointerAllocatable(const IRFunction& function) const;
    void layoutStaticFrames(const IRModule& module);
    // Entry-prefix loads of a register-argument function's parameters, keyed
    // by load value; empty when the function keeps the stack argument area.
    std::map<std::uint32_t, std::uint8_t> parameterInputs(const IRFunction& function) const;
    void emitParameterInputs();
    void emitRegisterArguments(const IRInstruction& call);
    // Parallel register copies (destination, source) with R0 breaking cycles.
    void emitParallelCopies(std::vector<std::pair<std::uint8_t, std::uint8_t>> copies);
    std::uint16_t callClobbers(const IRInstruction& instruction) const;
    void emitLoopBoundary(const IRInstruction& terminator, bool reload);
    void emitBoundaryTransfer(std::uint32_t value, bool load);
    void emitHardwareLoopCapacityGuard(const Token& source);
    void emitHardwareLoopRestore(std::uint32_t loop_id);
    void emitMemoryInitialize(const IRInstruction& instruction);
    void planLocalValues();
    int planFrameStorage();
    void planLoopCaches();
    void selectAutomaticCaches();
    std::string loopEntrySymbol(IRBlockId target, bool backedge) const;
    void emitRegisterLiteral(std::uint8_t reg, std::uint16_t value);
    void emitWordLiteral(Register destination, std::uint16_t value);
    bool optimized() const { return m_config.optimization != OptimizationLevel::Baseline; }
    bool globallyOptimized() const { return isGlobalOptimization(m_config.optimization); }
    bool sizeOptimized() const { return m_config.optimization == OptimizationLevel::Size; }
    void runAddressProof(const IRFunction& function, std::size_t frame_bytes);
    void collectStaticObjects();
    bool provenAddress(IRValueId value, const Type& pointer, int width) const {
        return globallyOptimized() && m_address_proof.provesAccess(value, pointer, width);
    }
    void emitPhiCopies(IRBlockId target, const Token& source);
    void emitNearBank(std::uint8_t reg, AddressSpace space);
    void emitSelectBank(std::uint8_t reg, AddressSpace space);
    enum class AddressComponents { Unknown, NonNull, Aligned, NonNullAligned };
    void emitAddressCheck(const Type& pointer, int width,
                          AddressComponents known = AddressComponents::Unknown);
    void ensureAddressChecked(IRValueId value, int width);
    void emitGuard(OpCode success_condition, std::uint16_t fault);
    void emitAddressFault(std::uint16_t fault);
    void emitCompare(Register left_register, std::uint16_t right);
    // Z/S test of a register against zero; only for BEQ/BNE/BMI/BPL users.
    void emitZeroTest(Register value_register);
    std::string localLabel();
    void bindLabel(const std::string& name);
    void emitLocalJump(const std::string& name, std::uint8_t condition = 5);
    void emitPointerValueCheck(const Type& type, int width);
    void emitPointerCompare(const IRInstruction& instruction);
    void emitPointerOffset(const IRInstruction& instruction);
    bool emitConstantPointerOffset(const IRInstruction& instruction);
    void emitSpill(IRValueId value, bool load);
    void emitWordSpillStore(IRValueId value, std::uint8_t source);
    void retainSpillCopy(IRValueId value, bool after_load);
    bool needsPointerSpill(const IRInstruction& instruction) const;
    void addRelocation(const std::string& symbol, std::size_t patch_offset,
                       RelocationType type);
    [[noreturn]] void fail(const std::string& message, const Token& source) const;
    void buildRegisterAllocation(const IRFunction& function);
    // Caller-saved extras (R2, R4, R14) join R5/R7/R8 only when this function
    // does not use them as plot, multiply, division, far-bank, or ROM hardware.
    std::vector<std::uint8_t> allocatableRegisters(const IRFunction& function) const;
    bool usesRegisterAllocation(IRValueId value) const;
    void saveLiveRegistersForCall(const IRInstruction& instruction,
                                  std::vector<std::uint8_t>& saved_registers);
    void restoreRegistersAfterCall(const std::vector<std::uint8_t>& saved_registers);
    void recordFinalSpillPlan();
    // `smallest` > 0 enables Os's bounded byte-for-speed exchange against the
    // smallest candidate seen in this search; 0 keeps strict bytes-first.
    bool preferCandidate(const GeneratedCandidate& candidate, const GeneratedCandidate& current,
                         std::size_t smallest = 0) const;
    ObjectFile finishCandidate(GeneratedCandidate candidate) const;
    GeneratedCandidate generateInternal(const IRModule& module);
    GeneratedCandidate generateInternalPass(const IRModule& module);
    bool stacklessLeafCandidate(const IRFunction& function, bool frameless) const;
    void emitPublicRegisterEntry(const IRFunction& function);
    GeneratedCandidate generateOptimized(const IRModule& module, OptimizationLevel policy,
        IRCompactionPolicy compaction = IRCompactionPolicy::Enabled, bool pool_enabled = true,
        const CandidateReference* legacy = nullptr);
    struct FunctionCandidate {
        IRFunction function;
        OptimizationLevel policy = OptimizationLevel::O2;
        bool shared_division = false;
        bool pool_enabled = true;
        bool no_spills = false;
        std::size_t bytes = 0;
        std::uint64_t speed_score = 0;
        std::uint64_t inclusive_score = 0;
    };
    void considerFunctionCandidates(const IRModule& module, const GeneratedCandidate& candidate,
        const CandidateReference* legacy = nullptr);
    void recordCandidateMetrics(const IRModule& module, GeneratedCandidate& candidate) const;
    bool admissibleMetric(const FunctionMetric& proposed, const FunctionMetric& legacy) const;
    bool admissibleExtension(const GeneratedCandidate& proposed, const CandidateReference& legacy) const;
    void resetCandidateEmissionState() noexcept;
    std::uint64_t candidateSpeed(const IRModule& module, const ObjectFile& object,
                                 const IRFunction* only = nullptr) const;
    // `credit` (optional) discounts each function's own estimate by its
    // selected CACHE windows; candidateSpeed itself stays cache-unaware.
    std::uint64_t inclusiveSpeed(const IRModule& module, const ObjectFile& object,
                                 const IRFunction* only = nullptr,
                                 const std::map<std::string, std::uint64_t>* credit = nullptr) const;
    // Os: the uncached re-emission of a CACHE-probed candidate wins unless the
    // cached one is within the size exchange budget and clearly faster.
    bool preferUncachedCandidate(const GeneratedCandidate& uncached, const GeneratedCandidate& cached) const;
    struct SizeChoice { std::size_t bytes; std::uint64_t inclusive; std::uint64_t speed; };
    static bool sizeFirstPreferred(const SizeChoice& proposed, const SizeChoice& existing, std::size_t smallest);
    GeneratedCandidate generateMixedCandidate(const IRModule& module);
    const Analyzer::LocalSymbolTable& localSymbols(const IRFunction& function) const;
    void emitSharedDivision(const IRInstruction& instruction);
    void emitDivisionHelpers();

    const std::map<std::string, Analyzer::LocalSymbolTable>& m_all_local_symbols;
    const std::map<std::string, FunctionSymbol>& m_global_function_symbols;
    const DataSegmentManager& m_data_manager;
    const CompilerConfig& m_config;

    ObjectFile m_object_file;
    std::vector<SpillIssue> m_spill_issues;
    // Own the synthetic descriptor: the current-function observer may borrow
    // it while emitting a kernel, including branch-relaxation retries.
    IRFunction m_division_helper_context;
    const IRFunction* m_current_function = nullptr;
    std::map<std::uint32_t, const IRInstruction*> m_values;
    std::map<std::uint32_t, std::size_t> m_use_counts;
    std::set<std::uint32_t> m_active_values;
    std::set<std::uint32_t> m_materialized_values;
    std::set<std::uint32_t> m_branch_comparisons;
    LinearScanAllocator m_register_allocator;
    GSUAddressProof m_address_proof;
    // Defined static RAM objects of this candidate, and whether any emitted
    // function relied on their extent (requires the linker's nonnull marker).
    GSUAddressProof::StaticObjects m_static_objects;
    bool m_static_ram_proof_used = false;
    GSUStackCheckCredit m_stack_check_credit;
    GSUStaticStackProof m_stack_proof;
    std::set<std::uint32_t> m_boundary_stores_emitted;
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
    std::set<std::string> m_relaxation_pending, m_long_branch_functions;
    std::size_t m_branch_serial = 0;
    bool m_checked_pointer_mode = false;
    bool m_has_hardware_loop = false;
    // Only a closed PLOT/CACHE-only scope may borrow volatile R4/R6. General
    // scopes, calls and nested bodies retain the checked stack-save contract.
    std::set<std::uint32_t> m_register_hardware_loops;
    struct RegionalLoopValues {
        std::set<std::uint32_t> blocks;
        std::vector<std::pair<IRValueId, std::uint8_t>> values;
    };
    std::map<std::uint32_t, RegionalLoopValues> m_regional_loop_values;
    std::map<std::uint32_t, RegionalLoopValues> m_regional_software_loops; // keyed by the loop's entry block
    void emitRegionalValues(const RegionalLoopValues& region);
    std::map<std::string, std::uint16_t> m_call_clobbers;
    struct AutomaticCache { std::size_t offset; IRBlockId header; std::uint32_t trips; bool entry_slot = false; };
    std::vector<AutomaticCache> m_auto_caches;
    // Code offsets of CACHE bytes placed by inline division kernels. Each was
    // a selector-reset NOP; an enclosing selected window restores the NOP.
    std::vector<std::size_t> m_division_caches;
    std::map<std::uint32_t, std::uint32_t> m_cache_trips;
    // Headers whose m_cache_trips entry is a proved constant count, not the
    // AssumedDynamicTrips estimate. Only these earn Os cache credit.
    std::set<std::uint32_t> m_constant_cache_trips;
    std::map<std::string, std::uint64_t> m_cache_credit;
    // Header -> loop blocks of every automatic-cache candidate: an entry
    // placeholder is emitted only on edges from outside the loop.
    std::map<std::uint32_t, std::set<std::uint32_t>> m_cache_loop_blocks;
    std::map<std::uint32_t, IRBlockId> m_hardware_headers;
    std::set<std::pair<std::uint32_t, std::uint32_t>> m_cached_backedges;
    std::set<std::uint32_t> m_cached_headers;
    bool m_manual_cache = false;
    bool m_allow_auto_cache = true, m_selected_auto_cache = false, m_had_cache_probe = false;
    std::map<std::uint32_t, std::uint8_t> m_cursor_updates;
    std::set<std::uint32_t> m_cursor_elided_values, m_ephemeral_values;
    std::map<std::string, std::size_t> m_register_arguments; // link symbol -> register parameters
    // Public functions of this module with a register entry (link symbol ->
    // GSUAbi::registerCallSymbol). Their public label loads the stack
    // arguments into the register-argument registers and falls through.
    std::map<std::string, std::string> m_public_register_entries;
    // External callees whose prototype admits a register call: link symbol ->
    // parameter count. Their callers reference the register entry instead.
    std::map<std::string, std::size_t> m_external_register_calls;
    // Functions whose locals/spills live in a compiled (static) frame, the
    // current owner and its frame size, emitted frame sizes and used slots.
    std::set<std::string> m_static_frame_functions;
    std::string m_static_frame_owner;
    int m_static_frame_bytes = 0;
    std::map<std::string, int> m_static_frame_sizes;
    std::set<std::pair<std::string, int>> m_static_frame_slots;
    // Current function never addresses FP: it neither saves nor sets R9.
    bool m_frameless = false;
    // Current function is a leaf that never touches the stack: no R11 save,
    // hence no stack guard either. Stack operations and LINKs emitted while
    // it is set are counted; any count excludes the function and retries.
    bool m_stackless = false;
    std::size_t m_stack_touches = 0;
    std::set<std::string> m_stackful_leaves;
    bool m_stackless_retry = false;
    // Short RAM (CompilerConfig::short_ram_bytes): word slots of admitted
    // static frames live in the LMS/SMS window. The current function's FP
    // offsets there; owner -> frame slot -> window offset; owner -> window
    // bytes; and the admitted total, an upper bound of the window's stack.
    std::set<int> m_short_frame_offsets;
    std::map<std::string, std::map<int, int>> m_short_frame_layout;
    std::map<std::string, int> m_short_frame_sizes;
    std::size_t m_short_ram_admitted = 0;
    std::map<std::uint32_t, std::uint8_t> m_parameter_inputs; // current function: load value -> register
    std::map<std::uint32_t, int> m_ephemeral_address_widths;
    // Successful checks of an immutable SSA near-RAM address, within this
    // emitted block only. Never evidence about a subsequently reloaded pointer.
    std::map<std::uint32_t, int> m_checked_addresses;
    IRValueId m_emitting_value;
    // A strictly adjacent immutable scalar/near-RAM-pointer snapshot in R0,
    // never a lexical variable or far pair. Its frame home stays authoritative;
    // emitted bytes, memory/volatile work, calls and CFG entries fence it.
    IRValueId m_accumulator_value;
    // Set when a scalar load wrote its allocated register directly. R0 still
    // holds the address, so the definition must not be copied or snapshotted.
    bool m_direct_register_load = false;
    // SBK uses the last physical RAM address, not a register. Only remember
    // checked near-word accesses; stack traffic and control-flow joins fence it.
    struct RamWordAddress {
        IRValueId source;
        bool has_frame = false;
        int frame_offset = 0;
        RamWordAddress& operator=(IRValueId value) {
            source = value; has_frame = false; return *this;
        }
        void rememberFrame(int offset) {
            source = IRValueId{}; has_frame = true; frame_offset = offset;
        }
    } m_last_ram_word_address;
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
    std::map<std::string, FunctionCandidate> m_function_candidates;
    std::map<std::string, std::size_t> m_function_smallest; // Fewest bytes seen per function.
    std::map<std::string, OptimizationLevel> m_function_policies;
    std::map<std::string, bool> m_function_division_policies;
    std::map<std::string, bool> m_function_pool_policies;
    bool m_collect_function_candidates = false;
    bool m_pool_enabled = true;
    std::size_t m_local_label_serial = 0;
};
