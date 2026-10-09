#pragma once

#include "IR.hpp"

// O2 scalar proofs only. No memory value numbering, speculative faults or
// hardware-state assumptions. Each public boundary accepts/verifies owned IR.
class IRValueOptimizer {
public:
    static void run(IRModule& module);
    static void reduceRecurrences(IRModule& module);
    static void splitLoopLifetimes(IRModule& module);
};
