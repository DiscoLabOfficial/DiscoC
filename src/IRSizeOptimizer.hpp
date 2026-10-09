#pragma once
#include "IR.hpp"

// Exact, bounded suffix sharing. No speculative execution or memory CSE:
// mutually incoming paths execute their identical tail once at the same point.
class IRSizeOptimizer {
public:
    static void run(IRModule& module);
};
