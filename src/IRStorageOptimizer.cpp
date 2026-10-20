#include "IRStorageOptimizer.hpp"

#include "GSUAddressProof.hpp"
#include "IRControlFlow.hpp"
#include <algorithm>
#include <set>

namespace {
constexpr std::size_t MaxSlots = 256, MaxBlocks = 256, MaxValues = 8192;
constexpr std::size_t MaxInstructions = 16000, MaxWork = 2000000;

bool ramPointer(const Type& type) {
    return type.pointer_level > 0 && type.space == AddressSpace::RAM;
}
bool integer(const Type& type) {
    return type.pointer_level == 0 &&
        (type.base == BaseType::BYTE || type.base == BaseType::WORD || type.base == BaseType::BOOL);
}
int extent(const Type& type) {
    if (type.sizeInBytes <= 0 || type.sizeInBytes > 65528 || type.array_size < 0 ||
        type.array_size > 65528 / type.sizeInBytes) return 0;
    return type.sizeInBytes * std::max(1, type.array_size);
}
int reservation(const Type& type) {
    const auto bytes = extent(type);
    const auto alignment = std::max(std::max(2, type.alignment), storageAlignment(type));
    if (!bytes || alignment > 128 || (alignment & (alignment - 1))) return 0;
    // Match emitAddress's upward rounding from an even FP-relative base.
    return ((bytes + 1) / 2) * 2 + alignment - 2;
}
struct Provenance {
    SymbolId symbol;
    std::int64_t low = 0, high = 0;
};
struct Access {
    SymbolId symbol;
    bool read = false, write = false, full_write = false;
};
using Location = std::pair<std::size_t, std::size_t>;
struct Analysis {
    std::vector<Provenance> pointers;
    std::vector<std::vector<Access>> accesses;
    std::map<SymbolId, Location> initializers;
    std::set<SymbolId> mutated, referenced;
};

bool analyze(const IRFunction& function, const Analyzer::LocalSymbolTable& locals,
             const GSUAddressProof& ranges, Analysis& result) {
    result.pointers.resize(static_cast<std::size_t>(function.value_count) + 1);
    result.accesses.resize(function.blocks.size());
    std::vector<AddressSpace> spaces(result.pointers.size(), AddressSpace::NONE);
    // In a flat address model these may observe any stack byte. Do not infer
    // noalias from an absent source-level &local expression.
    for (const auto& parameter : function.parameters)
        if (ramPointer(parameter.type)) return false;
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
        if (instruction.result.isValid()) spaces.at(instruction.result.value) = instruction.type.space;
        if (instruction.opcode == IROpcode::Call || instruction.opcode == IROpcode::Load ||
            instruction.opcode == IROpcode::Store ||
            (instruction.opcode == IROpcode::Address && instruction.operation == "temporary")) return false;
        if (instruction.opcode == IROpcode::Address && instruction.operation.empty() && ramPointer(instruction.type)) {
            const auto found = locals.find(instruction.symbol_id);
            if (found == locals.end() || isFarPointer(instruction.type) || !extent(found->second.type)) return false;
            result.pointers.at(instruction.result.value) = {instruction.symbol_id, 0, 0};
            result.referenced.insert(instruction.symbol_id);
        }
    }
    // Only owned root/member/typed-offset paths retain provenance. There is
    // no cast/PHI/pointer-load guess, and an unproved path freezes the plan.
    for (unsigned round = 0; round < 64; ++round) {
        bool changed = false;
        for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
            if (!instruction.result.isValid() || !ramPointer(instruction.type) ||
                result.pointers.at(instruction.result.value).symbol.isValid() || instruction.operands.empty()) continue;
            auto base = result.pointers.at(instruction.operands[0].value);
            if (!base.symbol.isValid() || isFarPointer(instruction.type)) continue;
            if (instruction.opcode == IROpcode::Address && instruction.operation == "member") {
                base.low += instruction.immediate; base.high += instruction.immediate;
            } else if (instruction.opcode == IROpcode::PointerOffset && instruction.operands.size() == 2 &&
                       (instruction.operation == "+" || instruction.operation == "-")) {
                std::int64_t low = 0, high = 0;
                if (!ranges.scalarBounds(instruction.operands[1], block.id, low, high)) continue;
                // Scalar facts are at most one word. Keep arithmetic bounded
                // before multiplying even if this routine receives hostile IR.
                if (low < -65535 || high > 65535 || instruction.immediate < 1 || instruction.immediate > 65528) return false;
                low *= instruction.immediate; high *= instruction.immediate;
                if (instruction.operation == "-") { const auto old = low; low = -high; high = -old; }
                base.low += low; base.high += high;
            } else continue;
            const auto bytes = extent(locals.at(base.symbol).type);
            // One-past arithmetic is harmless only if never dereferenced.
            // Keeping it here still requires every terminal access below to
            // fit completely, not just be aligned/non-null/in the RAM bank.
            if (base.low < 0 || base.high > bytes || base.low > base.high) return false;
            result.pointers.at(instruction.result.value) = base; changed = true;
        }
        if (!changed) break;
    }
    for (const auto& block : function.blocks) {
        auto& accesses = result.accesses.at(block.id.value);
        accesses.resize(block.instructions.size());
        for (std::size_t position = 0; position < block.instructions.size(); ++position) {
            const auto& instruction = block.instructions[position];
            for (std::size_t operand = 0; operand < instruction.operands.size(); ++operand) {
                const auto pointer = result.pointers.at(instruction.operands[operand].value);
                if (!pointer.symbol.isValid()) continue;
                const bool derived = operand == 0 && instruction.result.isValid() &&
                    result.pointers.at(instruction.result.value).symbol.isValid() &&
                    (instruction.opcode == IROpcode::Address || instruction.opcode == IROpcode::PointerOffset);
                const bool memory = operand == 0 && (instruction.opcode == IROpcode::LoadIndirect ||
                    instruction.opcode == IROpcode::StoreIndirect || instruction.opcode == IROpcode::MemoryInitialize);
                if (!derived && !memory) return false; // identity, escape, or an unknown alias
            }
            if (instruction.opcode != IROpcode::LoadIndirect && instruction.opcode != IROpcode::StoreIndirect &&
                instruction.opcode != IROpcode::MemoryInitialize) continue;
            const auto pointer_id = instruction.operands.at(0);
            const auto pointer = result.pointers.at(pointer_id.value);
            if (!pointer.symbol.isValid()) {
                // ROM reads cannot alias local RAM; every untracked RAM read
                // or write can, including numeric and incoming pointers.
                const bool rom_read = instruction.opcode == IROpcode::LoadIndirect && spaces.at(pointer_id.value) == AddressSpace::ROM;
                if (!rom_read) return false;
                continue;
            }
            const auto& symbol = locals.at(pointer.symbol);
            const auto bytes = extent(symbol.type);
            std::int64_t width = instruction.type.sizeInBytes;
            if (instruction.opcode == IROpcode::MemoryInitialize)
                width *= static_cast<std::int64_t>(instruction.initialization_values.size());
            if (width <= 0 || width > bytes || pointer.low < 0 || pointer.high > bytes - width ||
                instruction.memory_volatile || symbol.type.is_volatile) return false;
            auto& access = accesses[position]; access.symbol = pointer.symbol;
            access.read = instruction.opcode == IROpcode::LoadIndirect;
            access.write = !access.read;
            access.full_write = access.write && pointer.low == 0 && pointer.high == 0 && width == bytes;
            if (instruction.opcode == IROpcode::MemoryInitialize) {
                if (!access.full_write || !result.initializers.emplace(pointer.symbol, Location{block.id.value, position}).second)
                    result.mutated.insert(pointer.symbol);
            } else if (access.write) result.mutated.insert(pointer.symbol);
        }
    }
    return true;
}

bool initializedBeforeReads(const IRFunction& function, const Analysis& analysis,
                            const IRControlFlow& cfg, SymbolId symbol, Location initialization) {
    for (const auto& block : function.blocks) {
        const auto& accesses = analysis.accesses.at(block.id.value);
        for (std::size_t position = 0; position < accesses.size(); ++position) {
            const auto& access = accesses[position];
            if (access.symbol != symbol || !access.read) continue;
            if (!cfg.dominates(initialization.first, block.id.value) ||
                (initialization.first == block.id.value && initialization.second >= position)) return false;
        }
    }
    return true;
}

bool optimize(IRFunction& function, const Analyzer::LocalSymbolTable& locals, IRStorageOptions options) {
    if (function.storage_layout_valid) return false; // Already a final owned snapshot.
    if (function.total_local_alloc_size < 0 || function.total_local_alloc_size > 65530 || function.total_local_alloc_size % 2) return false;
    if (locals.size() > MaxSlots || function.blocks.size() > MaxBlocks || function.value_count > MaxValues) return false;
    std::size_t instructions = 0;
    for (const auto& block : function.blocks) {
        if (block.instructions.size() > MaxInstructions - instructions) return false;
        instructions += block.instructions.size();
        for (const auto& instruction : block.instructions)
            if ((instruction.opcode == IROpcode::HardwareLoop || instruction.opcode == IROpcode::HardwareLoopEnd) &&
                instruction.targets.empty()) return false;
    }
    int original_depth = 0;
    for (const auto& local : locals) {
        if (!local.first.isValid() || local.second.id != local.first || !reservation(local.second.type)) return false;
        if (local.second.stackOffset < 0) {
            if (local.second.stackOffset < -65528 || local.second.stackOffset % 2) return false;
            original_depth = std::max(original_depth, -local.second.stackOffset);
        }
    }
    GSUAddressProof ranges;
    ranges.run(function, locals, static_cast<std::size_t>(function.total_local_alloc_size), true);
    Analysis analysis;
    if (!analyze(function, locals, ranges, analysis)) return false;
    const IRControlFlow cfg(function);

    std::map<SymbolId, IRImmutableLocalArray> pools;
    std::set<Location> erased;
    std::size_t pool_bytes = 0;
    if (options.immutable_arrays == IRImmutableArrayPolicy::CopiedNearRamPayload) {
        for (const auto& initializer : analysis.initializers) {
            const auto& symbol = locals.at(initializer.first);
            const auto& type = symbol.type;
            if (symbol.stackOffset >= 0 || !integer(type) || type.array_size < 4 || type.array_size > 256 ||
                (type.sizeInBytes != 1 && type.sizeInBytes != 2) ||
                type.sizeInBytes != (type.base == BaseType::WORD ? 2 : 1) ||
                type.is_volatile || type.alignment > 2 || analysis.mutated.count(initializer.first) ||
                !initializedBeforeReads(function, analysis, cfg, initializer.first, initializer.second)) continue;
            const auto& instruction = function.blocks.at(initializer.second.first).instructions.at(initializer.second.second);
            if (instruction.initialization_values.size() != static_cast<std::size_t>(type.array_size)) continue;
            if (static_cast<std::size_t>(extent(type)) > 65528 - pool_bytes) continue;
            IRImmutableLocalArray pool; pool.type = type;
            for (const auto value : instruction.initialization_values) {
                pool.bytes.push_back(static_cast<std::uint8_t>(value & 255));
                if (type.sizeInBytes == 2) pool.bytes.push_back(static_cast<std::uint8_t>(value >> 8));
            }
            pool_bytes += pool.bytes.size();
            pools.emplace(initializer.first, std::move(pool)); erased.insert(initializer.second);
        }
    }

    std::vector<SymbolId> slots;
    std::map<SymbolId, std::size_t> indices;
    Analyzer::LocalSymbolTable layout;
    for (const auto& local : locals) {
        if (local.second.stackOffset > 0) layout.emplace(local);
        else if (analysis.referenced.count(local.first) && !pools.count(local.first)) {
            indices.emplace(local.first, slots.size()); slots.push_back(local.first); layout.emplace(local);
        }
    }
    // Whole-object content liveness. A partial store does not kill the object;
    // even a dead store interferes with every live object it could overwrite.
    using Live = std::set<std::size_t>;
    std::vector<Live> uses(function.blocks.size()), defs(function.blocks.size());
    std::vector<Live> live_in(function.blocks.size()), live_out(function.blocks.size());
    std::vector<std::set<std::size_t>> conflicts(slots.size());
    for (const auto& block : function.blocks) for (const auto& access : analysis.accesses.at(block.id.value)) {
        const auto slot = indices.find(access.symbol);
        if (slot == indices.end()) continue;
        if (access.read && !defs[block.id.value].count(slot->second)) uses[block.id.value].insert(slot->second);
        if (access.full_write) defs[block.id.value].insert(slot->second);
    }
    std::size_t work = 0; bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t reverse = function.blocks.size(); reverse > 0; --reverse) {
            const auto block = reverse - 1;
            Live out;
            for (const auto successor : cfg.successors[block])
                out.insert(live_in[successor].begin(), live_in[successor].end());
            auto in = uses[block];
            for (const auto slot : out) if (!defs[block].count(slot)) in.insert(slot);
            work += out.size() + in.size() + 1;
            if (work > MaxWork) return false;
            if (in != live_in[block] || out != live_out[block]) {
                live_in[block] = std::move(in); live_out[block] = std::move(out); changed = true;
            }
        }
    }
    const auto interfere = [&](std::size_t a, std::size_t b) {
        if (a != b) { conflicts[a].insert(b); conflicts[b].insert(a); }
    };
    for (const auto& block : function.blocks) {
        auto live = live_out[block.id.value];
        for (const auto a : live) for (const auto b : live) interfere(a, b);
        const auto& accesses = analysis.accesses.at(block.id.value);
        for (std::size_t reverse = accesses.size(); reverse > 0; --reverse) {
            const auto& access = accesses[reverse - 1]; const auto slot = indices.find(access.symbol);
            if (slot == indices.end()) continue;
            if (access.write) for (const auto other : live) interfere(slot->second, other);
            if (access.full_write) live.erase(slot->second);
            if (access.read) {
                for (const auto other : live) interfere(slot->second, other);
                live.insert(slot->second);
            }
        }
    }
    struct Home { int depth, bytes; std::vector<std::size_t> occupants; };
    std::vector<Home> homes; int depth = 0;
    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
        auto& symbol = layout.at(slots[slot]); const auto bytes = reservation(symbol.type);
        Home* selected = nullptr;
        for (auto& home : homes) {
            if (home.bytes < bytes) continue;
            bool available = true;
            for (const auto occupant : home.occupants) available = available && !conflicts[slot].count(occupant);
            if (available) { selected = &home; break; }
        }
        if (!selected) {
            if (bytes > 65528 - depth) return false;
            depth += bytes; homes.push_back({depth, bytes, {}}); selected = &homes.back();
        }
        selected->occupants.push_back(slot); symbol.stackOffset = -selected->depth;
    }
    // Preserve explicit/unexplained frame reservations (used by checked-frame
    // clients), while reclaiming only the analyzer's known local area.
    const auto known_frame = original_depth ? original_depth + 2 : 0;
    const auto extra = std::max(0, function.total_local_alloc_size - known_frame);
    if (depth > 65528 - extra) return false;
    const auto frame = depth + extra > 0 ? depth + extra + 2 : 0;
    if (frame > function.total_local_alloc_size) return false;

    function.storage_layout = std::move(layout);
    function.immutable_local_arrays = std::move(pools);
    function.total_local_alloc_size = frame;
    function.storage_layout_valid = true;
    for (auto& block : function.blocks) {
        auto& code = block.instructions;
        for (std::size_t reverse = code.size(); reverse > 0; --reverse)
            if (erased.count(Location{block.id.value, reverse - 1}))
                code.erase(code.begin() + static_cast<std::vector<IRInstruction>::difference_type>(reverse - 1));
    }
    return true;
}
}

void IRStorageOptimizer::run(IRModule& module,
    const std::map<std::string, Analyzer::LocalSymbolTable>& original_locals, IRStorageOptions options) {
    IRVerifier::verify(module);
    if (module.target != TargetKind::GSU) return;
    for (auto& function : module.functions) {
        const auto locals = original_locals.find(function.name);
        const Analyzer::LocalSymbolTable empty;
        try { optimize(function, locals == original_locals.end() ? empty : locals->second, options); }
        catch (const CompilerError&) {
            // Optional bounded CFG/range analysis failure retains the original
            // function. All planning mutations are local until commit above.
            if (function.storage_layout_valid) throw;
        }
    }
    IRVerifier::verify(module);
}
