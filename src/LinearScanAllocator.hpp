#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "IR.hpp"

struct LinearScanLocation {
    bool has_register = false;
    bool rematerializable = false;
    std::uint8_t physical_register = 0;
    std::size_t start = 0;
    std::size_t end = 0;
};

// Assigns non-overlapping IR value intervals to a fixed set of physical GSU
// registers. Values without a register may only be rematerialized when their
// defining expression is pure and repeatable. Observable values fail
// explicitly until a real spill-slot implementation is available.
class LinearScanAllocator {
public:
    void run(const IRFunction& function,
             const std::vector<std::uint8_t>& allocatable_registers);

    const LinearScanLocation* find(IRValueId value) const;
    const std::map<std::uint32_t, LinearScanLocation>& locations() const;

private:
    std::map<std::uint32_t, LinearScanLocation> m_locations;
};
