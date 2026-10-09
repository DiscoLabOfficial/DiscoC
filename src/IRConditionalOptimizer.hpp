#pragma once

#include "IR.hpp"

// Bounded SCCP followed by CFG cleanup, on the O2-owned SSA module only.
// Unknown is an analysis state, never a source-language undef value.
class IRConditionalOptimizer {
public:
    static void run(IRModule& module);
};
