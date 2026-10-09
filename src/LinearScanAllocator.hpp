#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "IR.hpp"

struct LinearScanLocation {
    bool has_register = false;
    bool rematerializable = false;
    std::uint8_t physical_register = 0;
    std::size_t start = 0;
    std::size_t end = 0;
    int spill_slot = -1; // O2 reusable two-byte frame class; far pairs stay unique.
};

// Assigns IR values to an explicit safe set of GSU registers. The baseline/O1
// strategies use conservative intervals; O2 uses exact CFG interference and
// reusable spill classes. Backend frame slots preserve observable definitions.
class LinearScanAllocator {
public:
    void run(const IRFunction& function,
             const std::vector<std::uint8_t>& allocatable_registers);

    // Eager definitions, matching physical block emission. Cross-block and
    // far values require real frame slots; only constants/plain addresses can
    // be recreated. Hidden hardware-loop backedges conservatively spill.
    void runEager(const IRFunction& function,
                  const std::vector<std::uint8_t>& allocatable_registers);
    // CFG/PHI-edge-aware interference with loop-weighted spill priority. Fixed
    // GSU registers remain unavailable even when not mentioned in the IR.
    void runGlobal(const IRFunction& function,
                   const std::vector<std::uint8_t>& allocatable_registers,
                   OptimizationLevel policy = OptimizationLevel::O2);
    bool liveAcrossCall(IRValueId value, std::size_t position) const;

    const LinearScanLocation* find(IRValueId value) const;
    const std::map<std::uint32_t, LinearScanLocation>& locations() const;
    // Registers with no live assigned value at this IR point. Includes result
    // interference and PHI edge uses; never lends special hardware registers.
    std::vector<std::uint8_t> spareRegisters(std::size_t position) const;

private:
    void runGlobalImpl(const IRFunction& function,
                       const std::vector<std::uint8_t>& registers, bool cost_priority, bool size_policy);
    std::map<std::uint32_t, LinearScanLocation> m_locations;
    std::map<std::size_t, std::set<std::uint32_t>> m_call_live;
    std::map<std::size_t, std::vector<std::uint8_t>> m_spare_registers;
};
