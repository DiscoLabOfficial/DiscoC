#include "LinearScanAllocator.hpp"

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
    m_locations.clear();

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
        rematerializable[pair.first] = pure || use_counts[pair.first] <= 1;
    }

    std::vector<Interval> ordered;
    ordered.reserve(intervals.size());
    for (const auto& pair : intervals) ordered.push_back(pair.second);
    std::sort(ordered.begin(), ordered.end(), [](const Interval& left, const Interval& right) {
        if (left.start != right.start) return left.start < right.start;
        return left.value < right.value;
    });

    std::vector<std::uint8_t> free_registers = allocatable_registers;
    std::sort(free_registers.begin(), free_registers.end());
    free_registers.erase(std::unique(free_registers.begin(), free_registers.end()),
                         free_registers.end());

    std::vector<ActiveInterval> active;
    for (const auto& interval : ordered) {
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

const std::map<std::uint32_t, LinearScanLocation>& LinearScanAllocator::locations() const {
    return m_locations;
}
