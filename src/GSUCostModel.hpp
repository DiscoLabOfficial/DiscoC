#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include "IRControlFlow.hpp"
#include "LinearScanAllocator.hpp"

// Relative selection/allocation costs, NOT an emulator or timing guarantee.
// GSU fetches one byte at a time: fast warm CACHE costs 1 clock/byte, uncached
// ROM/RAM costs 5. A cold 16-byte line costs another 80 clocks. RAM writes are
// buffered and may overlap fetch/ALU work; charging each transferred byte here
// is a conservative pressure proxy, not a prediction of every stall.
struct GSUCost {
    std::uint64_t fetch_bytes = 0, ram_bytes = 0;
    static std::uint64_t saturatedAdd(std::uint64_t a, std::uint64_t b) {
        return b > std::numeric_limits<std::uint64_t>::max() - a ? std::numeric_limits<std::uint64_t>::max() : a + b;
    }
    static std::uint64_t saturatedMultiply(std::uint64_t a, std::uint64_t b) {
        return b && a > std::numeric_limits<std::uint64_t>::max() / b ? std::numeric_limits<std::uint64_t>::max() : a * b;
    }
    void add(GSUCost cost, std::uint64_t weight = 1) {
        fetch_bytes = saturatedAdd(fetch_bytes, saturatedMultiply(cost.fetch_bytes, weight));
        ram_bytes = saturatedAdd(ram_bytes, saturatedMultiply(cost.ram_bytes, weight));
    }
    std::uint64_t pressureScore(unsigned fetch_clocks = 1) const {
        return saturatedAdd(saturatedMultiply(fetch_bytes, fetch_clocks), saturatedMultiply(ram_bytes, 5));
    }
};

class GSUCostModel {
public:
    static unsigned literalBytes(std::uint16_t value) { return value <= 127 || value >= 0xff80 ? 2 : 3; }
    static std::uint64_t blockWeight(unsigned depth) { return std::uint64_t{1} << (std::min(depth, 3u) * 6); }
    static GSUCost copy() { return {2, 0}; } // WITH/TO, not TO alone.
    static GSUCost spillLoad(int displacement, bool far = false) {
        return {literalBytes(static_cast<std::uint16_t>(displacement)) + 4u + (far ? 4u : 0u), far ? 4u : 2u};
    }
    static GSUCost spillStore(int displacement, bool far = false) {
        // Scalar stores keep R0 while FROM R9 / TO R3 / ADD R3 builds
        // the address; far pairs still preserve their offset through R6.
        return {literalBytes(static_cast<std::uint16_t>(displacement)) + (far ? 13u : 4u), far ? 4u : 2u};
    }
    static GSUCost preserveCall() { return {8, 4}; } // checked pushes/guard costs may add more.
    static bool scalar(const Type& type) {
        return !type.pointer_level && !type.array_size &&
            (type.base == BaseType::WORD || type.base == BaseType::BYTE || type.base == BaseType::BOOL);
    }
    static GSUCost allocation(const IRFunction& f, const LinearScanAllocator& allocator, const IRControlFlow& cfg,
                              bool size_policy = false) {
        GSUCost result;
        std::map<std::uint32_t, const IRInstruction*> definitions;
        for (const auto& b : f.blocks) for (const auto& i : b.instructions)
            if (i.result.isValid()) definitions.emplace(i.result.value, &i);
        const auto displacement = [&](const LinearScanLocation& location) {
            const auto slot = std::max(0, location.spill_slot);
            const auto bytes = std::min<std::uint64_t>(65535, static_cast<std::uint64_t>(std::max(0, f.total_local_alloc_size)) + 2u * (static_cast<unsigned>(slot) + 1u));
            return -static_cast<int>(bytes);
        };
        const auto read = [&](IRValueId value) {
            const auto* p = allocator.find(value);
            if (!p || p->rematerializable) return GSUCost{};
            return p->has_register ? copy() : spillLoad(displacement(*p), isFarPointer(definitions.at(value.value)->type));
        };
        const auto sameLocation = [&](IRValueId a, IRValueId b) {
            const auto* x = allocator.find(a); const auto* y = allocator.find(b);
            return a.value == b.value || (x && y && ((x->has_register && y->has_register && x->physical_register == y->physical_register) ||
                (!x->has_register && !y->has_register && x->spill_slot >= 0 && x->spill_slot == y->spill_slot)));
        };
        std::size_t position = 0;
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
            const auto weight = size_policy ? 1 : blockWeight(cfg.loop_depth.at(b.id.value));
            const auto* dest = i.result.isValid() ? allocator.find(i.result) : nullptr;
            if (i.opcode == IROpcode::Phi) {
                for (std::size_t n = 0; n < i.operands.size(); ++n) if (!sameLocation(i.result, i.operands[n])) {
                    const auto edge_weight = size_policy ? 1 : blockWeight(cfg.loop_depth.at(i.targets[n].value));
                    result.add(read(i.operands[n]), edge_weight);
                    if (dest && !dest->rematerializable) result.add(dest->has_register ? copy() : spillStore(displacement(*dest), isFarPointer(i.type)), edge_weight);
                }
            } else {
                for (const auto v : i.operands) result.add(read(v), weight);
                if (dest && !dest->rematerializable) result.add(dest->has_register ? copy() : spillStore(displacement(*dest), isFarPointer(i.type)), weight);
                if (i.opcode == IROpcode::Call) for (const auto& entry : allocator.locations())
                    if (entry.second.has_register && allocator.liveAcrossCall(IRValueId{entry.first}, position)) result.add(preserveCall(), weight);
            }
            ++position;
        }
        return result;
    }
};
