#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "IR.hpp"

struct LinearScanLocation {
    bool has_register = false;
    std::uint8_t physical_register = 0;
    std::size_t start = 0;
    std::size_t end = 0;
};

// Assigns non-overlapping IR value intervals to a fixed set of physical GSU
// registers. Values without a register are intentionally left available for
// rematerialization by the backend until explicit spill slots are introduced.
class LinearScanAllocator {
public:
    void run(const IRFunction& function,
             const std::vector<std::uint8_t>& allocatable_registers);

    const LinearScanLocation* find(IRValueId value) const;
    const std::map<std::uint32_t, LinearScanLocation>& locations() const;

private:
    std::map<std::uint32_t, LinearScanLocation> m_locations;
};
