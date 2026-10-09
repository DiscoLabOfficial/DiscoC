#include "IRGlobalOptimizer.hpp"
#include "IRControlFlow.hpp"
#include "IRLocalOptimizer.hpp"
#include "IRConditionalOptimizer.hpp"
#include "IRDivModFusion.hpp"
#include "IRValueOptimizer.hpp"
#include "IRSizeOptimizer.hpp"
#include "IRCompactOptimizer.hpp"
#include "ConstantEvaluator.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace {
// Compiler-owned logical-expression slots have no declaration SymbolId.
// Give each root its own domain and stable IR identity rather than letting
// all anonymous slots alias the invalid SymbolId or inventing source IDs.
struct StorageId {
    SymbolId symbol;
    std::uint32_t temporary = 0;
    bool operator<(const StorageId& other) const {
        if (symbol != other.symbol) return symbol < other.symbol;
        return temporary < other.temporary;
    }
    bool operator==(const StorageId& other) const { return symbol == other.symbol && temporary == other.temporary; }
    bool operator!=(const StorageId& other) const { return !(*this == other); }
};

bool scalar(const Type& type) {
    return type.pointer_level == 0 && type.array_size == 0 &&
        (type.base == BaseType::BYTE || type.base == BaseType::WORD || type.base == BaseType::BOOL);
}
bool sameScalar(const Type& a, const Type& b) {
    return scalar(a) && scalar(b) && a.base == b.base && a.is_unsigned == b.is_unsigned && a.enum_name == b.enum_name;
}
bool promotableCell(const Type& t) {
    return scalar(t) || (t.pointer_level > 0 && t.array_size == 0 && !isFarPointer(t));
}
bool sameCell(const Type& a, const Type& b) {
    return sameScalar(a, b) || (a.pointer_level > 0 && a.pointer_level == b.pointer_level &&
        !isFarPointer(a) && !isFarPointer(b) && a.base == b.base && a.structName == b.structName &&
        a.is_unsigned == b.is_unsigned && a.enum_name == b.enum_name && samePointerLayers(a, b));
}
IRValueId newValue(IRFunction& function) {
    if (function.value_count >= 100000) throw CompilerError("O2 value growth limit exceeded.", 1, 1);
    return IRValueId{++function.value_count};
}
using Definitions = std::vector<IRInstruction>;
Definitions definitions(const IRFunction& function) {
    Definitions result(static_cast<std::size_t>(function.value_count) + 1);
    for (const auto& b : function.blocks) for (const auto& i : b.instructions)
        if (i.result.isValid()) result.at(i.result.value) = i;
    return result;
}
IRValueId resolve(IRValueId id, std::vector<IRValueId>& aliases) {
    auto root = id;
    while (aliases.at(root.value).isValid()) root = aliases.at(root.value);
    while (aliases.at(id.value).isValid()) {
        auto next = aliases.at(id.value); aliases[id.value] = root; id = next;
    }
    return root;
}
void compact(IRFunction& f, std::vector<IRValueId>& aliases, const std::set<std::uint32_t>& erased) {
    std::vector<std::size_t> uses(static_cast<std::size_t>(f.value_count) + 1, 0);
    for (auto& b : f.blocks) for (auto& i : b.instructions) {
        for (auto& v : i.operands) v = resolve(v, aliases);
        if (!erased.count(i.result.value)) for (const auto v : i.operands) ++uses.at(v.value);
    }
    std::vector<IRValueId> renumber(uses.size());
    std::uint32_t next = 0;
    for (auto& b : f.blocks) {
        b.instructions.erase(std::remove_if(b.instructions.begin(), b.instructions.end(), [&](const IRInstruction& i) {
            return i.result.isValid() && (erased.count(i.result.value) ||
                (i.opcode == IROpcode::Address && i.operation.empty() && !uses.at(i.result.value)));
        }), b.instructions.end());
        for (const auto& i : b.instructions) if (i.result.isValid()) renumber.at(i.result.value) = IRValueId{++next};
    }
    for (auto& b : f.blocks) for (auto& i : b.instructions) {
        for (auto& v : i.operands) v = renumber.at(v.value);
        if (i.result.isValid()) i.result = renumber.at(i.result.value);
    }
    f.value_count = next;
}

void simplifyPhis(IRFunction& f) {
    std::vector<IRValueId> aliases(static_cast<std::size_t>(f.value_count) + 1);
    std::set<std::uint32_t> erased;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& b : f.blocks) for (const auto& phi : b.instructions) {
            if (phi.opcode != IROpcode::Phi || erased.count(phi.result.value)) continue;
            IRValueId common; bool equal = true;
            for (const auto v : phi.operands) {
                const auto root = resolve(v, aliases);
                if (root.value == phi.result.value) continue;
                if (!common.isValid()) common = root;
                else equal = equal && root.value == common.value;
            }
            if (equal && common.isValid()) { aliases[phi.result.value] = common; erased.insert(phi.result.value); changed = true; }
        }
    }
    compact(f, aliases, erased);
}

// Pruned dominance-frontier SSA. Definite assignment is checked first: an
// uninitialized memory read must not become an invented zero/undef value.
void promote(IRFunction& f, const Analyzer::LocalSymbolTable& locals, bool near_pointers) {
    IRControlFlow cfg(f);
    const auto defs = definitions(f);
    struct Candidate { Type type; bool parameter; IRInstruction address; };
    // Distinct fields and distinct anonymous slots need distinct rename stacks.
    using Cell = std::pair<StorageId, std::int64_t>;
    std::map<Cell, Candidate> candidates;
    std::map<std::uint32_t, Cell> addresses;
    std::map<StorageId, IRInstruction> roots;
    std::map<StorageId, std::int64_t> extents;
    std::map<StorageId, std::int64_t> stack_offsets;
    std::set<StorageId> blocked;
    std::map<StorageId, std::vector<Cell>> cells;
    for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
        if (i.opcode != IROpcode::Address ||
            isFarPointer(i.type) || i.type.space != AddressSpace::RAM) continue;
        if (i.operation == "temporary" && !i.symbol_id.isValid()) {
            const auto type = pointeeType(i.type);
            if (type.sizeInBytes <= 0) continue;
            const StorageId root{SymbolId{}, i.result.value};
            roots.emplace(root, i); extents.emplace(root, type.sizeInBytes); stack_offsets.emplace(root, i.immediate);
            addresses.emplace(i.result.value, Cell{root, 0});
            // Non-scalar/volatile slots still participate in alias rejection;
            // promoting an overlapping scalar would hide those accesses.
            if (!scalar(type) || type.sizeInBytes > 2 || type.is_volatile || i.immediate + type.sizeInBytes > 0)
                blocked.insert(root);
            continue;
        }
        if (!i.operation.empty() || !i.symbol_id.isValid()) continue;
        const auto local = locals.find(i.symbol_id);
        if (local == locals.end() || local->second.type.is_volatile || isFarPointer(local->second.type) ||
            (!near_pointers && local->second.type.pointer_level)) continue;
        const auto& type = local->second.type;
        const auto extent = static_cast<std::int64_t>(type.sizeInBytes) * std::max(1, type.array_size);
        if (extent <= 0 || (!promotableCell(type) && (extent > 64 || local->second.stackOffset > 0))) continue;
        const StorageId root{i.symbol_id, 0};
        roots.emplace(root, i); extents.emplace(root, extent); stack_offsets.emplace(root, local->second.stackOffset);
        addresses.emplace(i.result.value, Cell{root, 0});
    }
    // Anonymous slots are independent only while their frame intervals are.
    // Conservatively retain memory for aliased/reused offsets, including a
    // local whose address was not otherwise eligible for promotion.
    struct TemporaryInterval { std::int64_t begin, end; StorageId root; };
    std::vector<TemporaryInterval> temporary_intervals;
    std::size_t temporary_count = 0;
    for (const auto& root : roots) if (root.first.temporary) ++temporary_count;
    if (temporary_count > 1024 || (temporary_count && locals.size() > 25000000 / temporary_count))
        throw CompilerError("O2 SSA analysis resource limit exceeded.", 1, 1);
    for (const auto& root : roots) if (root.first.temporary) {
        const auto offset = stack_offsets.at(root.first);
        temporary_intervals.push_back({offset, offset + extents.at(root.first), root.first});
        for (const auto& local : locals) if (local.second.stackOffset < 0) {
            const auto begin = static_cast<std::int64_t>(local.second.stackOffset);
            const auto end = begin + static_cast<std::int64_t>(local.second.type.sizeInBytes) * std::max(1, local.second.type.array_size);
            if (offset < end && begin < offset + extents.at(root.first)) {
                blocked.insert(root.first); blocked.insert(StorageId{local.first, 0});
            }
        }
    }
    std::sort(temporary_intervals.begin(), temporary_intervals.end(), [](const TemporaryInterval& a, const TemporaryInterval& b) {
        if (a.begin != b.begin) return a.begin < b.begin;
        if (a.end != b.end) return a.end < b.end;
        return a.root < b.root;
    });
    std::int64_t active_end = std::numeric_limits<std::int64_t>::min();
    StorageId active_root;
    for (const auto& interval : temporary_intervals) {
        if (interval.begin < active_end) { blocked.insert(interval.root); blocked.insert(active_root); }
        if (interval.end > active_end) { active_end = interval.end; active_root = interval.root; }
    }
    // Only statically bounded, in-object paths are scalarized. A dynamic index,
    // cast/alias, escaping subobject, volatile access or overlapping cell blocks
    // the entire aggregate. Optional path discovery has a fixed depth budget.
    for (unsigned round = 0; round < 32; ++round) {
        bool changed = false;
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
            if (!i.result.isValid() || addresses.count(i.result.value) || i.operands.empty() ||
                i.type.pointer_level != 1 || isFarPointer(i.type) || i.type.space != AddressSpace::RAM) continue;
            const auto base = addresses.find(i.operands[0].value);
            if (base == addresses.end()) continue;
            auto offset = base->second.second;
            if (i.opcode == IROpcode::Address && i.operation == "member") {
                if (i.immediate > extents.at(base->second.first)) continue;
                offset += i.immediate;
            }
            else if (i.opcode == IROpcode::PointerOffset && i.operands.size() == 2 &&
                     (i.operation == "+" || i.operation == "-") && i.immediate <= 64) {
                const auto& index = defs.at(i.operands[1].value);
                if (index.opcode != IROpcode::Constant) continue;
                const auto amount = ConstantEvaluator::convert(index.immediate, index.type) * i.immediate;
                offset += i.operation == "-" ? -amount : amount;
            } else continue;
            if (offset < 0 || offset > extents.at(base->second.first)) continue;
            addresses.emplace(i.result.value, Cell{base->second.first, offset}); changed = true;
        }
        if (!changed) break;
    }
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        for (std::size_t o = 0; o < i.operands.size(); ++o) {
            const auto address = addresses.find(i.operands[o].value);
            if (address == addresses.end()) continue;
            const auto cell = address->second;
            if (o == 0 && addresses.count(i.result.value) &&
                (i.opcode == IROpcode::Address || i.opcode == IROpcode::PointerOffset)) continue;
            const auto stack_offset = stack_offsets.at(cell.first);
            if (o != 0 || (i.opcode != IROpcode::LoadIndirect && i.opcode != IROpcode::StoreIndirect) ||
                i.memory_volatile || !sameCell(i.type, pointeeType(defs.at(i.operands[0].value).type)) ||
                i.type.sizeInBytes > extents.at(cell.first) - cell.second ||
                (stack_offset + cell.second) % storageAlignment(i.type) != 0) {
                blocked.insert(cell.first); continue;
            }
            const auto previous = candidates.find(cell);
            if (previous != candidates.end() && !sameCell(previous->second.type, i.type)) blocked.insert(cell.first);
            if (previous == candidates.end()) {
                auto& members = cells[cell.first];
                if (members.size() >= 16) { blocked.insert(cell.first); continue; }
                for (const auto& other : members)
                    if (cell.second < other.second + candidates.at(other).type.sizeInBytes &&
                        other.second < cell.second + i.type.sizeInBytes) blocked.insert(cell.first);
                members.push_back(cell);
                candidates.emplace(cell, Candidate{i.type, stack_offset > 0, roots.at(cell.first)});
            }
        }
    // A second invocation must not re-resolve an already promoted variable's
    // loop PHIs using the now-absent stores.
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.opcode == IROpcode::Phi && i.symbol_id.isValid()) blocked.insert(StorageId{i.symbol_id, 0});
    for (auto i = candidates.begin(); i != candidates.end();)
        if (blocked.count(i->first.first)) i = candidates.erase(i); else ++i;
    // Loading a pointer also validates its representation. Unlike an integer
    // parameter load, that check cannot be hoisted ahead of a volatile witness
    // or into a previously untaken path. Anchor an eligible incoming pointer
    // at its original first entry-block load; otherwise retain its storage.
    std::map<Cell, IRValueId> pointer_initial_loads;
    for (auto c = candidates.begin(); c != candidates.end();) {
        if (!c->second.parameter || !c->second.type.pointer_level) { ++c; continue; }
        IRValueId initial;
        for (const auto& i : f.blocks.at(f.entry.value).instructions) {
            if (i.opcode != IROpcode::LoadIndirect && i.opcode != IROpcode::StoreIndirect) continue;
            const auto a = addresses.find(i.operands[0].value);
            if (a == addresses.end() || a->second != c->first) continue;
            if (i.opcode == IROpcode::LoadIndirect) initial = i.result;
            break;
        }
        if (!initial.isValid()) c = candidates.erase(c);
        else { pointer_initial_loads.emplace(c->first, initial); ++c; }
    }
    std::size_t instruction_count = 0;
    for (const auto& b : f.blocks) instruction_count += b.instructions.size();
    // Pruned SSA still performs per-variable dataflow. Bound that work before
    // scanning user-controlled functions rather than allowing quadratic stalls.
    if (candidates.size() > 1024 || (!candidates.empty() && instruction_count > 25000000 / candidates.size()))
        throw CompilerError("O2 SSA analysis resource limit exceeded.", 1, 1);
    const auto n = f.blocks.size();
    std::size_t phi_inputs = 0;
    std::map<std::uint32_t, Cell> phi_cells;
    for (auto it = candidates.begin(); it != candidates.end();) {
        const auto symbol = it->first;
        std::vector<bool> gen(n, false), use(n, false), in(n, true), out(n, true), lin(n, false), lout(n, false);
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
            if (i.opcode != IROpcode::LoadIndirect && i.opcode != IROpcode::StoreIndirect) continue;
            const auto a = addresses.find(i.operands[0].value);
            if (a == addresses.end() || a->second != symbol) continue;
            if (i.opcode == IROpcode::StoreIndirect) gen[b.id.value] = true;
            else if (!gen[b.id.value]) use[b.id.value] = true;
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t b = 0; b < n; ++b) if (cfg.reachable[b]) {
                bool assigned = b == f.entry.value ? it->second.parameter : true;
                if (b != f.entry.value) for (const auto p : cfg.predecessors[b]) if (cfg.reachable[p]) assigned = assigned && out[p];
                const bool next = assigned || gen[b];
                if (in[b] != assigned || out[b] != next) { in[b] = assigned; out[b] = next; changed = true; }
            }
        }
        bool safe = true;
        for (std::size_t b = 0; b < n; ++b) if (cfg.reachable[b] && use[b] && !in[b]) safe = false;
        if (!safe) { it = candidates.erase(it); continue; }
        changed = true;
        while (changed) {
            changed = false;
            for (std::size_t r = n; r > 0; --r) {
                const auto b = r - 1;
                bool needed = false;
                for (const auto s : cfg.successors[b]) needed = needed || lin[s];
                const bool next = use[b] || (needed && !gen[b]);
                if (lin[b] != next || lout[b] != needed) { lin[b] = next; lout[b] = needed; changed = true; }
            }
        }
        std::set<std::uint32_t> visited;
        std::vector<std::uint32_t> work;
        for (std::size_t b = 0; b < n; ++b) if (gen[b] || (b == f.entry.value && it->second.parameter)) work.push_back(static_cast<std::uint32_t>(b));
        while (!work.empty()) {
            const auto b = work.back(); work.pop_back();
            for (const auto join : cfg.frontier[b]) {
                if (!lin[join] || !visited.insert(join).second) continue;
                phi_inputs += cfg.predecessors[join].size();
                if (phi_inputs > 1000000) throw CompilerError("O2 phi-input growth limit exceeded.", 1, 1);
                IRInstruction phi; phi.opcode = IROpcode::Phi; phi.type = it->second.type;
                phi.result = newValue(f); phi.symbol_id = symbol.first.symbol; phi.source = it->second.address.source;
                phi_cells.emplace(phi.result.value, symbol);
                for (const auto p : cfg.predecessors[join]) { phi.targets.push_back(IRBlockId{p}); phi.operands.push_back(IRValueId{}); }
                f.blocks[join].instructions.insert(f.blocks[join].instructions.begin(), std::move(phi));
                if (!gen[join]) work.push_back(join);
            }
        }
        ++it;
    }
    if (candidates.empty()) return;
    std::set<std::uint32_t> initial_loads;
    for (const auto& c : candidates) if (c.second.parameter) {
        const auto pointer_load = pointer_initial_loads.find(c.first);
        if (pointer_load != pointer_initial_loads.end()) { initial_loads.insert(pointer_load->second.value); continue; }
        auto address = c.second.address; address.result = newValue(f);
        IRInstruction load; load.opcode = IROpcode::LoadIndirect; load.type = c.second.type;
        load.result = newValue(f); load.operands = {address.result}; load.source = address.source;
        initial_loads.insert(load.result.value);
        auto& entry = f.blocks.at(f.entry.value).instructions;
        entry.insert(entry.begin(), load); entry.insert(entry.begin(), address);
        addresses.emplace(address.result.value, c.first);
    }
    std::vector<IRValueId> aliases(static_cast<std::size_t>(f.value_count) + 1);
    std::set<std::uint32_t> erased;
    std::map<Cell, std::vector<IRValueId>> stacks;
    struct Visit { std::uint32_t block; bool leave; std::map<Cell, std::size_t> heights; };
    std::vector<Visit> work{{f.entry.value, false, {}}};
    while (!work.empty()) {
        auto visit = std::move(work.back()); work.pop_back();
        if (visit.leave) { for (const auto& h : visit.heights) stacks[h.first].resize(h.second); continue; }
        for (const auto& c : candidates) visit.heights.emplace(c.first, stacks[c.first].size());
        auto& block = f.blocks.at(visit.block);
        for (auto& i : block.instructions) {
            const auto phi_cell = phi_cells.find(i.result.value);
            if (i.opcode == IROpcode::Phi && phi_cell != phi_cells.end()) stacks[phi_cell->second].push_back(i.result);
            if (i.opcode != IROpcode::LoadIndirect && i.opcode != IROpcode::StoreIndirect) continue;
            const auto address = addresses.find(i.operands[0].value);
            if (address == addresses.end() || !candidates.count(address->second)) continue;
            auto& stack = stacks[address->second];
            if (i.opcode == IROpcode::StoreIndirect) {
                stack.push_back(resolve(i.operands[1], aliases));
                // Store has no result; an inert marker is removed below.
                i.opcode = IROpcode::Store; i.operands.clear(); i.operation = "ssa.erased";
            } else if (initial_loads.count(i.result.value)) stack.push_back(i.result);
            else {
                if (stack.empty()) throw CompilerError("O2 SSA: missing definite-assignment value.", i.source);
                aliases.at(i.result.value) = resolve(stack.back(), aliases); erased.insert(i.result.value);
            }
        }
        for (const auto s : cfg.successors[visit.block]) for (auto& phi : f.blocks[s].instructions) {
            if (phi.opcode != IROpcode::Phi) break;
            const auto cell = phi_cells.find(phi.result.value);
            if (cell == phi_cells.end()) continue;
            const auto& stack = stacks[cell->second];
            if (stack.empty()) throw CompilerError("O2 SSA: missing phi incoming value.", phi.source);
            for (std::size_t e = 0; e < phi.targets.size(); ++e)
                if (phi.targets[e].value == visit.block) phi.operands[e] = resolve(stack.back(), aliases);
        }
        visit.leave = true; work.push_back(visit);
        for (auto child = cfg.children[visit.block].rbegin(); child != cfg.children[visit.block].rend(); ++child)
            work.push_back({*child, false, {}});
    }
    for (auto& b : f.blocks) b.instructions.erase(std::remove_if(b.instructions.begin(), b.instructions.end(),
        [](const IRInstruction& i) { return i.opcode == IROpcode::Store && i.operation == "ssa.erased"; }), b.instructions.end());
    // Remove dead proven subobject paths backwards. Unproven/dynamic pointer
    // operations retain their address checks, even when their result is unused.
    std::vector<unsigned> uses(static_cast<std::size_t>(f.value_count) + 1, 0);
    for (auto& b : f.blocks) for (auto& i : b.instructions) {
        for (auto& v : i.operands) v = resolve(v, aliases);
        if (!erased.count(i.result.value)) for (const auto v : i.operands) ++uses.at(v.value);
    }
    std::vector<std::uint32_t> dead;
    for (const auto& a : addresses) if (!uses.at(a.first) && !blocked.count(a.second.first)) dead.push_back(a.first);
    while (!dead.empty()) {
        const auto v = dead.back(); dead.pop_back();
        if (!erased.insert(v).second) continue;
        for (const auto operand : defs.at(v).operands)
            if (--uses.at(operand.value) == 0 && addresses.count(operand.value) &&
                !blocked.count(addresses.at(operand.value).first)) dead.push_back(operand.value);
    }
    compact(f, aliases, erased); simplifyPhis(f);
    bool has_local_address = false;
    for (const auto& b : f.blocks) for (const auto& i : b.instructions) if (i.opcode == IROpcode::Address) {
        const auto local = locals.find(i.symbol_id);
        has_local_address = has_local_address || i.operation == "temporary" ||
            (local != locals.end() && local->second.stackOffset < 0);
    }
    if (!has_local_address) f.total_local_alloc_size = 0;
}

void scalarizeToFixedPoint(IRModule& module, const std::map<std::string, Analyzer::LocalSymbolTable>& locals, bool near_pointers) {
    const auto memoryCount = [&]() {
        std::size_t count = 0;
        for (const auto& f : module.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
            count += i.opcode == IROpcode::LoadIndirect || i.opcode == IROpcode::StoreIndirect;
        return count;
    };
    std::size_t work = 0;
    for (unsigned round = 0; round < 32; ++round) {
        const auto before = memoryCount();
        for (auto& f : module.functions) {
            for (const auto& b : f.blocks) work += b.instructions.size();
            if (work > 2000000) throw CompilerError("O2 scalar-cell convergence resource limit exceeded.", 1, 1);
            const auto symbols = locals.find(f.name);
            if (symbols != locals.end()) promote(f, symbols->second, near_pointers);
        }
        IRVerifier::verify(module);
        IRConditionalOptimizer::run(module);
        IRLocalOptimizer::run(module, locals);
        IRValueOptimizer::run(module);
        // Promoting an index or another cell can expose constant subobject
        // paths in a different array. Finish that chain in this invocation.
        if (memoryCount() == before) return;
    }
    throw CompilerError("O2 scalar-cell convergence resource limit exceeded.", 1, 1);
}

// Leaf scalar IR only: no memory, CACHE, recursion, bank state or implicit
// ABI effects are cloned. Constant arguments specialize the clone in the
// following local folding pass. Growth has both per-caller and module budgets.
void inlineLeaves(IRModule& module, const std::map<std::string, Analyzer::LocalSymbolTable>& locals, bool size_policy) {
    const auto snapshots = module.functions;
    std::map<std::string, const IRFunction*> functions;
    for (const auto& f : snapshots) functions.emplace(f.link_name.empty() ? f.name : f.link_name, &f);
    std::size_t module_growth = 0, module_nodes = 0;
    for (auto& caller : module.functions) {
        std::size_t caller_growth = 0, caller_nodes = 0;
        std::vector<Type> value_types(static_cast<std::size_t>(caller.value_count) + 1);
        for (const auto& b : caller.blocks) for (const auto& i : b.instructions)
            if (i.result.isValid()) value_types.at(i.result.value) = i.type;
        bool cached = caller.is_cached;
        for (const auto& b : caller.blocks) for (const auto& i : b.instructions) cached = cached || i.opcode == IROpcode::Cache;
        std::vector<IRValueId> aliases(static_cast<std::size_t>(caller.value_count) + 1);
        std::set<std::uint32_t> erased;
        for (auto& b : caller.blocks) {
            std::vector<IRInstruction> output;
            for (auto call : b.instructions) {
                for (auto& operand : call.operands) operand = resolve(operand, aliases);
                const auto found = functions.find(call.symbol);
                const IRFunction* callee = call.opcode == IROpcode::Call && found != functions.end() ? found->second : nullptr;
                bool legal = callee && callee->name != caller.name && callee->name != "main" && !callee->is_cached &&
                    callee->blocks.size() == 1 && sameScalar(callee->return_type, call.type) &&
                    call.operands.size() == callee->parameters.size();
                std::map<std::uint32_t, std::size_t> arguments;
                std::set<std::uint32_t> param_addresses;
                std::size_t cost = 0, nodes = 0;
                if (legal) {
                    for (std::size_t p = 0; p < call.operands.size(); ++p)
                        legal = legal && sameScalar(value_types.at(call.operands[p].value), callee->parameters[p].type);
                    const auto symbols = locals.find(callee->name);
                    const auto defs = definitions(*callee);
                    const auto& body = callee->blocks[0].instructions;
                    legal = !body.empty() && body.back().opcode == IROpcode::Return;
                    for (const auto& i : body) {
                        if (i.opcode == IROpcode::Address && i.operation.empty() && symbols != locals.end()) {
                            const auto symbol = symbols->second.find(i.symbol_id);
                            bool parameter = false;
                            if (symbol != symbols->second.end() && symbol->second.stackOffset > 0)
                                for (const auto& p : callee->parameters) parameter = parameter || p.name.lexeme == i.symbol;
                            legal = legal && parameter; param_addresses.insert(i.result.value);
                        } else if (i.opcode == IROpcode::LoadIndirect && !i.memory_volatile && scalar(i.type) && param_addresses.count(i.operands[0].value)) {
                            const auto& address = defs.at(i.operands[0].value);
                            bool matched = false;
                            for (std::size_t p = 0; p < callee->parameters.size(); ++p)
                                if (callee->parameters[p].name.lexeme == address.symbol && sameScalar(callee->parameters[p].type, i.type)) {
                                    arguments.emplace(i.result.value, p); matched = true;
                                }
                            legal = legal && matched;
                        } else if ((i.opcode == IROpcode::Constant || i.opcode == IROpcode::Binary || i.opcode == IROpcode::Unary ||
                                    i.opcode == IROpcode::Cast || i.opcode == IROpcode::BitExtract) && scalar(i.type)) {
                            for (const auto v : i.operands) legal = legal && scalar(defs.at(v.value).type);
                            // Helpers are too large for cache-sensitive duplication.
                            if (i.opcode == IROpcode::Binary && (i.operation == "/" || i.operation == "%")) cost += 20;
                            cost += i.opcode == IROpcode::Constant ? 0 : 1;
                            ++nodes;
                        } else if (i.opcode != IROpcode::Return) legal = false;
                    }
                }
                const auto limit = size_policy ? 2u : cached ? 4u : 12u;
                if (!legal || cost > limit || caller_growth + cost > 64 || module_growth + cost > 512 ||
                    caller_nodes + nodes > 256 || module_nodes + nodes > 2048) {
                    output.push_back(std::move(call)); continue;
                }
                std::map<std::uint32_t, IRValueId> map;
                for (const auto& arg : arguments) map.emplace(arg.first, call.operands.at(arg.second));
                IRValueId returned;
                for (auto i : callee->blocks[0].instructions) {
                    if (i.opcode == IROpcode::Address || arguments.count(i.result.value)) continue;
                    if (i.opcode == IROpcode::Return) { returned = map.at(i.operands.front().value); continue; }
                    for (auto& v : i.operands) v = map.at(v.value);
                    const auto old = i.result; i.result = newValue(caller); map.emplace(old.value, i.result);
                    value_types.push_back(i.type);
                    i.in_plot_context = call.in_plot_context; output.push_back(std::move(i));
                }
                aliases.resize(static_cast<std::size_t>(caller.value_count) + 1);
                aliases.at(call.result.value) = returned; erased.insert(call.result.value);
                caller_growth += cost; module_growth += cost;
                caller_nodes += nodes; module_nodes += nodes;
            }
            b.instructions = std::move(output);
        }
        compact(caller, aliases, erased);
    }
}

bool movable(const IRInstruction& i, const Definitions& defs) {
    if (i.is_live_range_split) return false;
    if (i.memory_volatile) return false;
    if (i.opcode == IROpcode::Address) return i.operation.empty();
    if (!scalar(i.type)) return false;
    for (const auto v : i.operands) if (!scalar(defs.at(v.value).type)) return false;
    if (i.opcode == IROpcode::Constant || i.opcode == IROpcode::Cast || i.opcode == IROpcode::Unary || i.opcode == IROpcode::BitExtract) return true;
    if (i.opcode != IROpcode::Binary || i.operation == "/" || i.operation == "%") return false;
    if (i.operation == "<<" || i.operation == ">>") {
        const auto& count = defs.at(i.operands[1].value);
        return count.opcode == IROpcode::Constant && count.immediate >= 0 && count.immediate < 16;
    }
    return true;
}
void hoist(IRFunction& f) {
    IRControlFlow cfg(f);
    std::sort(cfg.loops.begin(), cfg.loops.end(), [](const IRControlFlow::Loop& a, const IRControlFlow::Loop& b) { return a.blocks.size() < b.blocks.size(); });
    for (const auto& loop : cfg.loops) {
        std::vector<std::uint32_t> outside;
        for (const auto p : cfg.predecessors[loop.header]) if (!loop.blocks.count(p)) outside.push_back(p);
        if (outside.size() != 1 || cfg.successors[outside[0]].size() != 1) continue;
        const auto preheader = outside[0];
        const auto defs = definitions(f);
        std::vector<std::uint32_t> owner(defs.size(), IRBlockId::Invalid);
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) if (i.result.isValid()) owner[i.result.value] = b.id.value;
        std::vector<IRInstruction> moved;
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto b : loop.blocks) {
                auto& body = f.blocks[b].instructions;
                body.erase(std::remove_if(body.begin(), body.end(), [&](const IRInstruction& i) {
                    if (!i.result.isValid() || !movable(i, defs)) return false;
                    for (const auto v : i.operands) if (loop.blocks.count(owner.at(v.value))) return false;
                    owner[i.result.value] = preheader; moved.push_back(i); changed = true; return true;
                }), body.end());
            }
        }
        auto& pre = f.blocks[preheader].instructions;
        pre.insert(pre.end() - 1, moved.begin(), moved.end());
    }
}

// Carry a modular scaled index, not an unchecked machine pointer. The address
// operation still checks the original index magnitude, base, bank window and
// final alignment at its original program point, including on the last trip.
void strengthReduce(IRFunction& f) {
    IRControlFlow cfg(f);
    for (const auto& loop : cfg.loops) {
        if (loop.latches.size() != 1) continue;
        const auto latch = *loop.latches.begin();
        std::vector<std::uint32_t> outside;
        for (const auto p : cfg.predecessors[loop.header]) if (!loop.blocks.count(p)) outside.push_back(p);
        if (outside.size() != 1 || cfg.successors[outside[0]].size() != 1) continue;
        const auto preheader = outside[0];
        const auto defs = definitions(f);
        struct Ramp { IRInstruction phi; IRValueId initial; std::uint16_t step; };
        std::map<std::uint32_t, Ramp> ramps;
        for (const auto& phi : f.blocks[loop.header].instructions) {
            if (phi.opcode != IROpcode::Phi) break;
            if (phi.operands.size() != 2 || phi.type.pointer_level || phi.type.base != BaseType::WORD || !phi.type.is_unsigned || !phi.type.enum_name.empty()) continue;
            IRValueId initial, next;
            for (std::size_t e = 0; e < 2; ++e) {
                if (phi.targets[e].value == preheader) initial = phi.operands[e];
                if (phi.targets[e].value == latch) next = phi.operands[e];
            }
            if (!initial.isValid() || !next.isValid()) continue;
            const auto& update = defs.at(next.value);
            if (update.opcode != IROpcode::Binary || (update.operation != "+" && update.operation != "-") || update.operands[0].value != phi.result.value) continue;
            const auto& delta = defs.at(update.operands[1].value);
            if (delta.opcode != IROpcode::Constant) continue;
            const auto raw = static_cast<std::uint16_t>(delta.immediate);
            const auto step = update.operation == "+" ? raw : static_cast<std::uint16_t>(0 - raw);
            ramps.emplace(phi.result.value, Ramp{phi, initial, step});
        }
        std::map<std::pair<std::uint32_t, std::int64_t>, IRValueId> scaled;
        // Snapshot candidate positions before inserting PHIs/constants.
        struct Use { std::uint32_t block, value, index; std::int64_t stride; };
        std::vector<Use> uses;
        for (const auto b : loop.blocks) for (const auto& i : f.blocks[b].instructions)
            if (i.opcode == IROpcode::PointerOffset && i.operands.size() == 2 && !isFarPointer(i.type) &&
                (i.operation == "+" || i.operation == "-") && i.immediate >= 4 && ramps.count(i.operands[1].value))
                uses.push_back({b, i.result.value, i.operands[1].value, i.immediate});
        for (const auto& use : uses) {
            const auto key = std::make_pair(use.index, use.stride);
            auto value = scaled.find(key);
            if (value == scaled.end()) {
                if (scaled.size() == 4) continue; // Avoid speculative register pressure/code growth.
                const auto& ramp = ramps.at(use.index);
                auto literal = [&](std::uint16_t raw) {
                    IRInstruction i; i.opcode = IROpcode::Constant; i.type = ramp.phi.type;
                    i.immediate = raw; i.result = newValue(f); i.source = ramp.phi.source; return i;
                };
                auto stride = literal(static_cast<std::uint16_t>(use.stride));
                IRInstruction initial; initial.opcode = IROpcode::Binary; initial.type = ramp.phi.type;
                initial.result = newValue(f); initial.operation = "*"; initial.operands = {ramp.initial, stride.result}; initial.source = ramp.phi.source;
                auto delta = literal(static_cast<std::uint16_t>(static_cast<std::uint64_t>(ramp.step) * static_cast<std::uint64_t>(use.stride)));
                IRInstruction phi = ramp.phi; phi.result = newValue(f); phi.symbol_id = SymbolId{};
                IRInstruction next; next.opcode = IROpcode::Binary; next.type = ramp.phi.type; next.result = newValue(f);
                next.operation = "+"; next.operands = {phi.result, delta.result}; next.source = ramp.phi.source;
                for (std::size_t e = 0; e < phi.targets.size(); ++e) phi.operands[e] = phi.targets[e].value == preheader ? initial.result : next.result;
                auto& pre = f.blocks[preheader].instructions;
                pre.insert(pre.end() - 1, {stride, initial, delta});
                auto& body = f.blocks[latch].instructions; body.insert(body.end() - 1, next);
                f.blocks[loop.header].instructions.insert(f.blocks[loop.header].instructions.begin(), phi);
                value = scaled.emplace(key, phi.result).first;
            }
            for (auto& i : f.blocks[use.block].instructions) if (i.result.value == use.value) {
                i.operation = i.operation == "+" ? "scaled+" : "scaled-";
                i.operands.push_back(value->second); break;
            }
        }
    }
}
}

void IRGlobalOptimizer::run(IRModule& module, const std::map<std::string, Analyzer::LocalSymbolTable>& locals,
                            OptimizationLevel policy, IRCompactionPolicy compaction) {
    const bool size_policy = policy == OptimizationLevel::Size;
    const bool compact_ir = compaction == IRCompactionPolicy::Enabled;
    IRVerifier::verify(module);
    for (auto& f : module.functions) {
        IRControlFlow::exposeHardwareLoops(f);
        const auto symbols = locals.find(f.name);
        if (symbols != locals.end()) promote(f, symbols->second, compact_ir);
    }
    IRVerifier::verify(module);
    IRConditionalOptimizer::run(module);
    IRLocalOptimizer::run(module, locals);
    IRValueOptimizer::run(module);
    inlineLeaves(module, locals, size_policy);
    IRVerifier::verify(module);
    IRConditionalOptimizer::run(module);
    IRLocalOptimizer::run(module, locals);
    IRValueOptimizer::run(module);
    scalarizeToFixedPoint(module, locals, compact_ir);
    for (auto& f : module.functions) {
        simplifyPhis(f);
        if (!size_policy) { hoist(f); strengthReduce(f); }
    }
    if (!size_policy) IRValueOptimizer::reduceRecurrences(module);
    IRVerifier::verify(module);
    IRConditionalOptimizer::run(module);
    IRLocalOptimizer::run(module, locals);
    IRValueOptimizer::run(module);
    IRConditionalOptimizer::run(module);
    IRDivModFusion::run(module);
    if (size_policy) {
        IRSizeOptimizer::run(module);
        IRConditionalOptimizer::run(module);
        IRLocalOptimizer::run(module, locals);
    }
    if (compact_ir) {
        IRCompactOptimizer::run(module, locals, policy);
        // New counted-loop guards/counts can be invariant in an enclosing
        // loop. Finish safe scalar LICM before value numbering, so a second
        // full invocation does not newly hoist and merge those producers.
        if (!size_policy) for (auto& f : module.functions) hoist(f);
        IRValueOptimizer::run(module);
        IRConditionalOptimizer::run(module);
        IRLocalOptimizer::run(module, locals);
    }
    for (auto& f : module.functions) {
        IRControlFlow::splitPhiEdges(f);
        IRControlFlow::layoutHotBlocks(f);
    }
    // Fusion removes the second operation's dividend/divisor uses. Clean up
    // after final block layout so stable IDs are canonical on the first run.
    IRLocalOptimizer::run(module, locals);
    if (!size_policy) IRValueOptimizer::splitLoopLifetimes(module);
    IRVerifier::verify(module);
}
