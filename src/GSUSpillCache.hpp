#pragma once

#include <algorithm>
#include <map>
#include <vector>
#include "IR.hpp"

// Non-owning SSA IDs only. The frame remains authoritative across CFG edges
// and calls; a temporary copy may occupy a register only at points where exact
// liveness proves that no global allocation owns it. No memory-load CSE occurs.
class GSUSpillCache {
public:
    void clear() { m_copies.clear(); m_spare.clear(); }
    void enter(const std::vector<std::uint8_t>& spare) {
        m_spare = spare;
        for (auto i = m_copies.begin(); i != m_copies.end();) {
            if (std::find(spare.begin(), spare.end(), i->first) == spare.end()) i = m_copies.erase(i);
            else ++i;
        }
    }
    void clobber(std::uint8_t reg) { m_copies.erase(reg); }
    int find(IRValueId value) const {
        for (const auto& entry : m_copies) if (entry.second.value == value.value) return entry.first;
        return -1;
    }
    // Reuse dead copies, never evict a known useful copy just to add another
    // move. This bounds physical cache size to the three ABI-safe registers.
    int choose(const std::map<std::uint32_t, std::size_t>& remaining) const {
        for (const auto reg : m_spare) {
            if (reg != 5 && reg != 7 && reg != 8) continue;
            const auto copy = m_copies.find(reg);
            if (copy == m_copies.end()) return reg;
            const auto uses = remaining.find(copy->second.value);
            if (uses == remaining.end() || uses->second == 0) return reg;
        }
        return -1;
    }
    void record(std::uint8_t reg, IRValueId value) {
        if (value.isValid() && (reg == 5 || reg == 7 || reg == 8) &&
            std::find(m_spare.begin(), m_spare.end(), reg) != m_spare.end()) m_copies[reg] = value;
    }
private:
    std::vector<std::uint8_t> m_spare;
    std::map<std::uint8_t, IRValueId> m_copies;
};
