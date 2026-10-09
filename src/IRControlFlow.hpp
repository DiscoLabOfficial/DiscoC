#pragma once

#include <set>
#include "IR.hpp"

// Owned analysis snapshot. Rebuild after CFG/value mutations; no instruction
// pointers survive a rewrite. PHI inputs are uses on predecessor edges.
class IRControlFlow {
public:
    explicit IRControlFlow(const IRFunction& function);
    bool dominates(std::size_t dominator, std::size_t block) const;

    std::vector<std::vector<std::uint32_t>> successors, predecessors, children, frontier;
    std::vector<bool> reachable;
    std::vector<std::uint32_t> immediate_dominator;
    struct Loop { std::uint32_t header; std::set<std::uint32_t> blocks, latches; };
    std::vector<Loop> loops;
    std::vector<unsigned> loop_depth;
    std::vector<std::set<std::uint32_t>> live_in, live_out;

    // Normalizes the old instruction-pair representation without changing O0
    // or O1. Explicit setup/end/leave also preserve nested R12/R13 lifetimes.
    static void exposeHardwareLoops(IRFunction& function);
    // Branch edge blocks keep PHI parallel copies off untaken conditional
    // edges. LOOP backedges use the same rule, through their R13 destination.
    static void splitPhiEdges(IRFunction& function);
    // Keep natural loops (including PHI backedge copies) contiguous. IDs and
    // every explicit CFG/PHI/hardware-loop reference are remapped together.
    static void layoutHotBlocks(IRFunction& function);

private:
    std::vector<std::vector<std::uint64_t>> m_dominators;
};
