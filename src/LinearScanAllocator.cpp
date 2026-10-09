#include "LinearScanAllocator.hpp"
#include "IRControlFlow.hpp"
#include "GSUCostModel.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>

namespace {

struct Interval {
    std::uint32_t value = IRValueId::Invalid;
    std::size_t start = 0;
    std::size_t end = 0;
};

struct ActiveInterval {
    std::uint32_t value = IRValueId::Invalid;
    std::size_t end = 0;
    std::uint8_t physical_register = 0;
};

using ValueEdge = std::pair<std::uint32_t, std::uint32_t>;
using ValueGraph = std::map<std::uint32_t, std::set<std::uint32_t>>;
using PhiAffinity = std::map<ValueEdge, std::uint64_t>;
ValueEdge edgeKey(std::uint32_t a, std::uint32_t b) { return {std::min(a, b), std::max(a, b)}; }

// Improve a valid coloring without introducing spills or weakening its exact
// interference. Swapping an entire two-color connected component (Kempe chain)
// preserves every conflict, including parallel PHIs and hardware backedges.
void improvePhiLocations(std::map<std::uint32_t, LinearScanLocation>& locations,
                         const ValueGraph& interference, const ValueGraph& preferences,
                         const PhiAffinity& affinity) {
    std::vector<ValueEdge> ordered;
    for (const auto& item : affinity) ordered.push_back(item.first);
    std::sort(ordered.begin(), ordered.end(), [&](const ValueEdge& a, const ValueEdge& b) {
        if (affinity.at(a) != affinity.at(b)) return affinity.at(a) > affinity.at(b);
        return a < b;
    });
    const std::set<std::uint32_t> empty;
    const auto neighbors = [&](const ValueGraph& graph, std::uint32_t value) -> const std::set<std::uint32_t>& {
        const auto found = graph.find(value);
        return found == graph.end() ? empty : found->second;
    };
    std::size_t work = 0;
    constexpr std::size_t MaxWork = 2500000;
    for (unsigned round = 0; round < 2; ++round) {
        bool changed = false;
        for (const auto& edge : ordered) for (unsigned direction = 0; direction < 2; ++direction) {
            const auto source = direction ? edge.second : edge.first;
            const auto target = direction ? edge.first : edge.second;
            const auto a = locations.at(source), b = locations.at(target);
            if (!a.has_register || !b.has_register || a.physical_register == b.physical_register) continue;
            std::set<std::uint32_t> component{source};
            std::vector<std::uint32_t> pending{source};
            while (!pending.empty()) {
                const auto value = pending.back(); pending.pop_back();
                for (const auto next : neighbors(interference, value)) {
                    if (++work > MaxWork) return;
                    const auto& location = locations.at(next);
                    if (location.has_register && (location.physical_register == a.physical_register ||
                        location.physical_register == b.physical_register) && component.insert(next).second)
                        pending.push_back(next);
                }
            }
            if (component.count(target)) continue; // This interchange cannot coalesce the requested edge.
            std::set<ValueEdge> affected;
            for (const auto value : component) for (const auto next : neighbors(preferences, value)) {
                if (++work > MaxWork) return;
                const auto key = edgeKey(value, next);
                if (affinity.count(key)) affected.insert(key);
            }
            const auto swapped = [&](std::uint32_t value) {
                const auto reg = locations.at(value).physical_register;
                return !component.count(value) ? reg : reg == a.physical_register ? b.physical_register : a.physical_register;
            };
            std::int64_t gain = 0;
            for (const auto& key : affected) {
                if (++work > MaxWork) return;
                const auto& x = locations.at(key.first); const auto& y = locations.at(key.second);
                if (!x.has_register || !y.has_register) continue;
                const bool before = x.physical_register == y.physical_register;
                const bool after = swapped(key.first) == swapped(key.second);
                gain += (static_cast<int>(after) - static_cast<int>(before)) * static_cast<std::int64_t>(affinity.at(key));
            }
            if (gain <= 0) continue;
            for (const auto value : component) locations.at(value).physical_register = swapped(value);
            changed = true;
        }
        if (!changed) break;
    }
    // Spilled PHIs also have copy costs. Reuse an existing preferred slot only
    // when no interfering neighbor uses it; never turn a register into a spill.
    for (unsigned round = 0; round < 2; ++round) for (auto& item : locations) {
        auto& location = item.second;
        if (location.has_register || location.spill_slot < 0) continue;
        std::set<int> occupied, choices{location.spill_slot};
        for (const auto other : neighbors(interference, item.first)) {
            if (++work > MaxWork) return;
            const auto slot = locations.at(other).spill_slot;
            if (slot >= 0) occupied.insert(slot);
        }
        for (const auto other : neighbors(preferences, item.first)) {
            if (++work > MaxWork) return;
            const auto slot = locations.at(other).spill_slot;
            if (slot >= 0 && !occupied.count(slot)) choices.insert(slot);
        }
        const auto score = [&](int slot) {
            std::uint64_t value = 0;
            for (const auto other : neighbors(preferences, item.first)) {
                const auto found = affinity.find(edgeKey(item.first, other));
                if (found != affinity.end() && locations.at(other).spill_slot == slot) value += found->second;
            }
            return value;
        };
        const auto score_cost = neighbors(preferences, item.first).size() + 1;
        if (score_cost > MaxWork - work) return;
        work += score_cost;
        auto best = score(location.spill_slot);
        for (const auto slot : choices) {
            if (score_cost > MaxWork - work) return;
            work += score_cost;
            const auto next = score(slot);
            if (next > best) { best = next; location.spill_slot = slot; }
        }
    }
}

std::vector<std::size_t> reversePostOrder(const IRFunction& function) {
    std::vector<std::vector<std::size_t>> successors(function.blocks.size());
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        const auto& instructions = function.blocks[index].instructions;
        if (instructions.empty()) continue;
        for (const auto target : instructions.back().targets) {
            if (target.isValid() && target.value < function.blocks.size()) {
                successors[index].push_back(target.value);
            }
        }
    }

    std::vector<std::size_t> postorder;
    std::set<std::size_t> visited;
    const std::function<void(std::size_t)> visit = [&](std::size_t block) {
        if (!visited.insert(block).second) return;
        for (const auto successor : successors[block]) visit(successor);
        postorder.push_back(block);
    };
    if (function.entry.isValid() && function.entry.value < function.blocks.size()) {
        visit(function.entry.value);
    }
    std::reverse(postorder.begin(), postorder.end());
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        if (visited.insert(index).second) postorder.push_back(index);
    }
    return postorder;
}

bool isRematerializable(
    IRValueId value,
    const std::map<std::uint32_t, const IRInstruction*>& definitions,
    std::set<std::uint32_t>& active) {
    if (!value.isValid()) return false;
    const auto found = definitions.find(value.value);
    if (found == definitions.end() || !active.insert(value.value).second) return false;

    const auto& instruction = *found->second;
    bool result = false;
    switch (instruction.opcode) {
        case IROpcode::Constant:
        case IROpcode::Address:
            result = true;
            break;
        case IROpcode::Cast:
        case IROpcode::Unary:
            result = instruction.operands.size() == 1 &&
                     isRematerializable(instruction.operands.front(), definitions, active);
            break;
        case IROpcode::Binary:
            result = instruction.operands.size() == 2 &&
                     (instruction.operation == "+" || instruction.operation == "-" ||
                      instruction.operation == "*") &&
                     isRematerializable(instruction.operands[0], definitions, active) &&
                     isRematerializable(instruction.operands[1], definitions, active);
            break;
        default:
            result = false;
            break;
    }
    active.erase(value.value);
    return result;
}

} // namespace

void LinearScanAllocator::run(
    const IRFunction& function,
    const std::vector<std::uint8_t>& allocatable_registers) {
    m_locations.clear(); m_call_live.clear(); m_spare_registers.clear();

    const auto block_order = reversePostOrder(function);
    std::map<std::uint32_t, Interval> intervals;
    std::map<std::uint32_t, const IRInstruction*> definitions;

    std::size_t position = 0;
    for (const auto block_index : block_order) {
        const auto& block = function.blocks.at(block_index);
        for (const auto& instruction : block.instructions) {
            if (instruction.result.isValid()) {
                if (!intervals.emplace(instruction.result.value,
                                       Interval{instruction.result.value, position, position}).second) {
                    throw std::runtime_error("Linear-scan allocator: value is defined more than once.");
                }
                definitions[instruction.result.value] = &instruction;
            }
            ++position;
        }
    }

    position = 0;
    for (const auto block_index : block_order) {
        const auto& block = function.blocks.at(block_index);
        for (const auto& instruction : block.instructions) {
            for (const auto operand : instruction.operands) {
                if (!operand.isValid()) {
                    throw std::runtime_error("Linear-scan allocator: instruction uses an invalid value.");
                }
                const auto found = intervals.find(operand.value);
                if (found == intervals.end()) {
                    throw std::runtime_error("Linear-scan allocator: instruction uses an undefined value.");
                }
                found->second.end = std::max(found->second.end, position);
            }
            ++position;
        }
    }

    std::map<std::uint32_t, bool> rematerializable;
    std::map<std::uint32_t, std::size_t> use_counts;
    position = 0;
    for (const auto block_index : block_order) {
        for (const auto& instruction : function.blocks.at(block_index).instructions) {
            for (const auto operand : instruction.operands) ++use_counts[operand.value];
            ++position;
        }
    }
    for (const auto& pair : intervals) {
        std::set<std::uint32_t> active_values;
        const bool pure = isRematerializable(IRValueId{pair.first}, definitions, active_values);
        // The IR backend materializes lazy values at their use site. Repeating
        // a side-effecting definition once is therefore equivalent for a
        // single-use value; repeating it for multiple uses is not.
        // A pixel read is also a cache flush: it cannot move across graphics
        // effects or be dropped even when the SSA result has no users.
        rematerializable[pair.first] = !definitions.at(pair.first)->hardwareEffects().observable() &&
            (pure || use_counts[pair.first] <= 1);
    }

    std::vector<Interval> ordered;
    ordered.reserve(intervals.size());
    for (const auto& pair : intervals) ordered.push_back(pair.second);
    std::sort(ordered.begin(), ordered.end(), [](const Interval& left, const Interval& right) {
        if (left.start != right.start) return left.start < right.start;
        return left.value < right.value;
    });

    std::vector<std::uint8_t> free_registers = allocatable_registers;
    // The cursor and ROM-buffer pointer are persistent hardware state, not
    // spare scalar temporaries. Reserve their registers for the whole function.
    std::uint16_t reserved = 0;
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
        const auto effects = instruction.hardwareEffects();
        reserved |= effects.reads_registers | effects.writes_registers;
    }
    free_registers.erase(std::remove_if(free_registers.begin(), free_registers.end(), [&](std::uint8_t reg) {
        if (reg > 15) throw std::runtime_error("Linear-scan allocator: invalid physical register.");
        return (reserved & (1u << reg)) != 0;
    }), free_registers.end());
    std::sort(free_registers.begin(), free_registers.end());
    free_registers.erase(std::unique(free_registers.begin(), free_registers.end()),
                         free_registers.end());

    std::vector<ActiveInterval> active;
    for (const auto& interval : ordered) {
        // A far value is a pair, not a scalar register. The backend owns an
        // aligned four-byte frame slot for it, including observable results.
        if (isFarPointer(definitions.at(interval.value)->type)) continue;
        for (std::size_t index = 0; index < active.size();) {
            if (active[index].end < interval.start) {
                free_registers.push_back(active[index].physical_register);
                active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
            } else {
                ++index;
            }
        }
        std::sort(free_registers.begin(), free_registers.end());

        LinearScanLocation location;
        location.start = interval.start;
        location.end = interval.end;
        location.rematerializable = rematerializable.at(interval.value);
        if (!free_registers.empty()) {
            location.has_register = true;
            location.physical_register = free_registers.front();
            free_registers.erase(free_registers.begin());
            active.push_back({interval.value, interval.end, location.physical_register});
            m_locations[interval.value] = location;
            continue;
        }

        auto victim = std::max_element(active.begin(), active.end(),
            [](const ActiveInterval& left, const ActiveInterval& right) {
                if (left.end != right.end) return left.end < right.end;
                return left.value < right.value;
            });
        if (victim != active.end() && victim->end > interval.end) {
            auto& victim_location = m_locations.at(victim->value);
            if (!victim_location.rematerializable) {
                throw std::runtime_error(
                    "Linear-scan allocator: live observable value requires a spill slot.");
            }
            victim_location.has_register = false;
            location.has_register = true;
            location.physical_register = victim->physical_register;
            victim->value = interval.value;
            victim->end = interval.end;
            m_locations[interval.value] = location;
        } else {
            if (!location.rematerializable) {
                throw std::runtime_error(
                    "Linear-scan allocator: value with side effects requires a spill slot.");
            }
            m_locations[interval.value] = location;
        }
    }
}

const LinearScanLocation* LinearScanAllocator::find(IRValueId value) const {
    const auto found = m_locations.find(value.value);
    return found == m_locations.end() ? nullptr : &found->second;
}

void LinearScanAllocator::runEager(
    const IRFunction& function, const std::vector<std::uint8_t>& allocatable_registers) {
    m_locations.clear(); m_call_live.clear(); m_spare_registers.clear();
    std::map<std::uint32_t, std::size_t> defining_block;
    std::set<std::uint32_t> cross_block;
    // HardwareLoopEnd has a hidden backedge inside its physical IR block.
    // Keep frame-backed values until that liveness edge is represented here.
    bool hardware_loop = false;
    std::size_t position = 0;
    std::uint16_t reserved = 0;
    for (std::size_t block = 0; block < function.blocks.size(); ++block) {
        for (const auto& instruction : function.blocks[block].instructions) {
            const auto effects = instruction.hardwareEffects();
            reserved |= effects.reads_registers | effects.writes_registers;
            hardware_loop = hardware_loop || instruction.opcode == IROpcode::HardwareLoop;
            if (instruction.result.isValid()) {
                if (!defining_block.emplace(instruction.result.value, block).second)
                    throw std::runtime_error("Eager allocator: duplicate value definition.");
                LinearScanLocation location;
                location.start = location.end = position;
                location.rematerializable = instruction.opcode == IROpcode::Constant ||
                    (instruction.opcode == IROpcode::Address && instruction.operation.empty());
                // Far values retain the backend's two-word representation.
                if (isFarPointer(instruction.type)) cross_block.insert(instruction.result.value);
                m_locations.emplace(instruction.result.value, location);
            }
            ++position;
        }
    }
    position = 0;
    for (std::size_t block = 0; block < function.blocks.size(); ++block) {
        for (const auto& instruction : function.blocks[block].instructions) {
            for (const auto operand : instruction.operands) {
                auto found = m_locations.find(operand.value);
                if (found == m_locations.end()) throw std::runtime_error("Eager allocator: undefined operand.");
                found->second.end = std::max(found->second.end, position);
                if (defining_block.at(operand.value) != block) cross_block.insert(operand.value);
            }
            ++position;
        }
    }
    std::vector<std::uint8_t> registers;
    for (const auto reg : allocatable_registers) {
        if (reg > 15) throw std::runtime_error("Eager allocator: invalid physical register.");
        if (!(reserved & (1u << reg))) registers.push_back(reg);
    }
    std::sort(registers.begin(), registers.end());
    registers.erase(std::unique(registers.begin(), registers.end()), registers.end());
    std::vector<std::uint32_t> ordered;
    for (const auto& item : m_locations) ordered.push_back(item.first);
    std::sort(ordered.begin(), ordered.end(), [&](std::uint32_t left, std::uint32_t right) {
        return m_locations.at(left).start < m_locations.at(right).start;
    });
    std::vector<ActiveInterval> active;
    for (const auto value : ordered) {
        auto& location = m_locations.at(value);
        if (hardware_loop || cross_block.count(value) || location.rematerializable || location.end == location.start) continue;
        for (std::size_t index = 0; index < active.size();) {
            if (active[index].end < location.start) {
                registers.push_back(active[index].physical_register);
                active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
            } else ++index;
        }
        if (registers.empty()) continue; // Real spill; never repeat an observable definition.
        std::sort(registers.begin(), registers.end());
        location.has_register = true;
        location.physical_register = registers.front();
        registers.erase(registers.begin());
        active.push_back({value, location.end, location.physical_register});
    }
}

const std::map<std::uint32_t, LinearScanLocation>& LinearScanAllocator::locations() const {
    return m_locations;
}

void LinearScanAllocator::runGlobal(const IRFunction& function,
                                    const std::vector<std::uint8_t>& allocatable_registers,
                                    OptimizationLevel policy) {
    const bool size_policy = policy == OptimizationLevel::Size;
    runGlobalImpl(function, allocatable_registers, false, size_policy);
    // Keep the previous coloring unless a bounded candidate demonstrably wins
    // the same ISA-aware proxy. No source-driven unbounded search/recoloring.
    if (function.value_count <= 4096 && function.blocks.size() <= 256) {
        IRControlFlow cfg(function);
        const auto before = GSUCostModel::allocation(function, *this, cfg, size_policy);
        auto original = *this;
        runGlobalImpl(function, allocatable_registers, true, size_policy);
        const auto after = GSUCostModel::allocation(function, *this, cfg, size_policy);
        const bool keep_original = size_policy ? after.fetch_bytes >= before.fetch_bytes :
            after.pressureScore() >= before.pressureScore() || after.pressureScore(5) > before.pressureScore(5);
        if (keep_original) *this = std::move(original);
    }
    IRControlFlow cfg(function);
    std::size_t end = 0, work = 0;
    for (const auto& b : function.blocks) {
        end += b.instructions.size();
        auto live = cfg.live_out.at(b.id.value);
        auto point = end;
        for (auto i = b.instructions.rbegin(); i != b.instructions.rend(); ++i) {
            // Empty live sets must not permit an unbounded map of empty
            // vectors. This optional copy optimization has a safe fallback.
            if (++work > 1000000) { m_spare_registers.clear(); return; }
            --point;
            if (i->opcode != IROpcode::Phi) for (const auto v : i->operands) live.insert(v.value);
            // The destination and all operands are busy during emission, even
            // if the result immediately dies or the old value dies here.
            if (i->result.isValid()) live.insert(i->result.value);
            std::set<std::uint8_t> busy;
            for (const auto value : live) {
                if (++work > 1000000) { m_spare_registers.clear(); return; }
                const auto* location = find(IRValueId{value});
                if (location && location->has_register) busy.insert(location->physical_register);
            }
            auto& spare = m_spare_registers[point];
            for (const auto reg : allocatable_registers) if (!busy.count(reg)) spare.push_back(reg);
            if (i->result.isValid()) live.erase(i->result.value);
        }
    }
}

std::vector<std::uint8_t> LinearScanAllocator::spareRegisters(std::size_t position) const {
    const auto found = m_spare_registers.find(position);
    return found == m_spare_registers.end() ? std::vector<std::uint8_t>{} : found->second;
}

void LinearScanAllocator::runGlobalImpl(const IRFunction& function,
                                      const std::vector<std::uint8_t>& allocatable_registers, bool cost_priority, bool size_policy) {
    m_locations.clear(); m_call_live.clear(); m_spare_registers.clear();
    IRControlFlow cfg(function);
    std::vector<std::size_t> begin(function.blocks.size()), end(function.blocks.size());
    std::map<std::uint32_t, std::uint64_t> weight;
    std::set<std::uint32_t> far;
    std::size_t position = 0;
    for (const auto& block : function.blocks) {
        begin[block.id.value] = position;
        for (const auto& i : block.instructions) {
            if (i.result.isValid()) {
                LinearScanLocation location; location.start = location.end = position;
                location.rematerializable = i.opcode == IROpcode::Constant || (i.opcode == IROpcode::Address && i.operation.empty());
                m_locations.emplace(i.result.value, location);
                if (isFarPointer(i.type)) far.insert(i.result.value);
            }
            ++position;
        }
        end[block.id.value] = position - 1;
    }
    std::size_t call_live_entries = 0;
    const auto touch = [&](std::uint32_t value, std::size_t point, unsigned depth, bool use) {
        auto& location = m_locations.at(value);
        location.start = std::min(location.start, point); location.end = std::max(location.end, point);
        if (use) weight[value] += (size_policy ? 1 : GSUCostModel::blockWeight(depth)) * (cost_priority ?
            size_policy ? GSUCostModel::spillLoad(-2).fetch_bytes - GSUCostModel::copy().fetch_bytes :
            GSUCostModel::spillLoad(-2).pressureScore() - GSUCostModel::copy().pressureScore() : 1);
    };
    for (const auto& block : function.blocks) {
        const auto b = block.id.value;
        position = begin[b];
        for (const auto& i : block.instructions) {
            for (std::size_t o = 0; o < i.operands.size(); ++o) {
                const auto edge = i.opcode == IROpcode::Phi ? i.targets.at(o).value : b;
                touch(i.operands[o].value, i.opcode == IROpcode::Phi ? end.at(edge) : position, cfg.loop_depth.at(edge), true);
            }
            ++position;
        }
        // Merely surviving an inner loop is not a hot use. Otherwise cold
        // outer state would displace the induction/width used every pixel.
        for (const auto value : cfg.live_in[b]) touch(value, begin[b], cfg.loop_depth[b], false);
        for (const auto value : cfg.live_out[b]) touch(value, end[b], cfg.loop_depth[b], false);
        auto live = cfg.live_out[b];
        position = end[b];
        for (auto i = block.instructions.rbegin(); i != block.instructions.rend(); ++i) {
            if (i->result.isValid()) live.erase(i->result.value);
            if (i->opcode == IROpcode::Call) {
                auto& saved = m_call_live[position];
                for (const auto value : live) {
                    if (m_locations.at(value).rematerializable || far.count(value)) continue;
                    if (++call_live_entries > 1000000)
                        throw CompilerError("O2 call-liveness resource limit exceeded.", i->source);
                    saved.insert(value);
                    if (cost_priority) {
                        const auto penalty = size_policy ? GSUCostModel::preserveCall().fetch_bytes :
                            GSUCostModel::preserveCall().pressureScore() * GSUCostModel::blockWeight(cfg.loop_depth.at(b));
                        auto& priority = weight[value];
                        priority = priority > penalty ? priority - penalty : 1;
                    }
                }
            }
            if (i->opcode != IROpcode::Phi) for (const auto operand : i->operands) live.insert(operand.value);
            if (position) --position;
        }
    }
    std::vector<std::uint8_t> registers;
    for (const auto reg : allocatable_registers) {
        if (reg != 5 && reg != 7 && reg != 8) throw std::runtime_error("Global allocator: register has a fixed GSU/ABI role.");
        if (std::find(registers.begin(), registers.end(), reg) == registers.end()) registers.push_back(reg);
    }
    std::sort(registers.begin(), registers.end());
    std::vector<std::uint32_t> ordered;
    for (const auto& entry : m_locations) if (!entry.second.rematerializable && !far.count(entry.first) && weight.count(entry.first)) ordered.push_back(entry.first);
    std::sort(ordered.begin(), ordered.end(), [&](std::uint32_t a, std::uint32_t b) {
        if (weight.at(a) != weight.at(b)) return weight.at(a) > weight.at(b);
        if (m_locations.at(a).start != m_locations.at(b).start) return m_locations.at(a).start < m_locations.at(b).start;
        return a < b;
    });
    // An interval hull spanning physical blocks can falsely overlap values
    // on mutually exclusive edges. Color the actual CFG interference instead;
    // keep intervals for diagnostics and O0/O1, not as a global spill oracle.
    ValueGraph interference, preferences;
    PhiAffinity affinity;
    std::size_t edges = 0;
    const auto eligible = [&](std::uint32_t value) {
        const auto& location = m_locations.at(value);
        return !location.rematerializable && !far.count(value) && weight.count(value);
    };
    const auto conflict = [&](std::uint32_t a, std::uint32_t b) {
        if (a == b || !eligible(a) || !eligible(b)) return;
        if (interference[a].insert(b).second) {
            interference[b].insert(a);
            if (++edges > 1000000) throw CompilerError("O2 register-interference resource limit exceeded.", 1, 1);
        }
    };
    for (const auto& block : function.blocks) {
        std::set<std::uint32_t> live;
        for (const auto value : cfg.live_out[block.id.value]) if (eligible(value)) live.insert(value);
        for (auto i = block.instructions.rbegin(); i != block.instructions.rend(); ++i) {
            if (i->result.isValid()) {
                if (eligible(i->result.value)) for (const auto value : live) conflict(i->result.value, value);
                live.erase(i->result.value);
            }
            if (i->opcode == IROpcode::Phi) {
                for (std::size_t n = 0; n < i->operands.size(); ++n) {
                    const auto v = i->operands[n];
                    preferences[i->result.value].insert(v.value); preferences[v.value].insert(i->result.value);
                    if (eligible(i->result.value) && eligible(v.value) && i->result.value != v.value)
                        affinity[edgeKey(i->result.value, v.value)] +=
                            size_policy ? 1 : GSUCostModel::blockWeight(cfg.loop_depth.at(i->targets.at(n).value));
                }
            } else for (const auto v : i->operands) if (eligible(v.value)) live.insert(v.value);
        }
    }
    for (const auto value : ordered) {
        auto& location = m_locations.at(value);
        std::vector<std::uint8_t> choices = registers;
        std::map<std::uint8_t, std::uint64_t> scores;
        for (const auto reg : registers) scores.emplace(reg, 0);
        for (const auto preferred : preferences[value]) {
            const auto& other = m_locations.at(preferred);
            const auto edge = affinity.find(edgeKey(value, preferred));
            if (other.has_register && edge != affinity.end()) scores[other.physical_register] += edge->second;
        }
        // Incoming backedges are hot; ascending SSA IDs otherwise favor a cold
        // preheader copy. Ranking changes only choices, never interference.
        std::sort(choices.begin(), choices.end(), [&](std::uint8_t a, std::uint8_t b) {
            if (scores.at(a) != scores.at(b)) return scores.at(a) > scores.at(b);
            return a < b;
        });
        for (const auto reg : choices) {
            bool occupied = false;
            for (const auto neighbor : interference[value]) {
                const auto& other = m_locations.at(neighbor);
                occupied = occupied || (other.has_register && other.physical_register == reg);
            }
            if (occupied) continue;
            location.has_register = true; location.physical_register = reg;
            break;
        }
    }
    for (const auto value : ordered) {
        auto& location = m_locations.at(value);
        if (location.has_register) continue;
        std::set<int> occupied;
        for (const auto neighbor : interference[value]) {
            const auto slot = m_locations.at(neighbor).spill_slot;
            if (slot >= 0) occupied.insert(slot);
        }
        for (const auto preferred : preferences[value]) {
            const auto slot = m_locations.at(preferred).spill_slot;
            if (slot >= 0 && !occupied.count(slot)) { location.spill_slot = slot; break; }
        }
        if (location.spill_slot < 0) {
            int slot = 0;
            while (occupied.count(slot)) ++slot;
            location.spill_slot = slot;
        }
    }
    improvePhiLocations(m_locations, interference, preferences, affinity);
}

bool LinearScanAllocator::liveAcrossCall(IRValueId value, std::size_t position) const {
    const auto call = m_call_live.find(position);
    return call != m_call_live.end() && call->second.count(value.value) != 0;
}
