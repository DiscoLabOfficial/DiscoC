#include "LinearScanAllocator.hpp"

#include <algorithm>
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

} // namespace

void LinearScanAllocator::run(
    const IRFunction& function,
    const std::vector<std::uint8_t>& allocatable_registers) {
    m_locations.clear();

    std::map<std::uint32_t, Interval> intervals;
    std::size_t position = 0;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.result.isValid()) {
                if (!intervals.emplace(instruction.result.value,
                                       Interval{instruction.result.value, position, position}).second) {
                    throw std::runtime_error("Linear-scan allocator: value is defined more than once.");
                }
            }
            for (const auto operand : instruction.operands) {
                if (!operand.isValid()) {
                    throw std::runtime_error("Linear-scan allocator: instruction uses an invalid value.");
                }
                const auto found = intervals.find(operand.value);
                if (found == intervals.end()) {
                    throw std::runtime_error("Linear-scan allocator: use appears before its definition.");
                }
                found->second.end = std::max(found->second.end, position);
            }
            ++position;
        }
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
            victim_location.has_register = false;
            location.has_register = true;
            location.physical_register = victim->physical_register;
            victim->value = interval.value;
            victim->end = interval.end;
            m_locations[interval.value] = location;
        } else {
            // The backend rematerializes this value from its defining IR
            // instruction. The interval remains recorded for deterministic
            // spill decisions and diagnostics.
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
