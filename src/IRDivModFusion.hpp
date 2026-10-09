#pragma once

#include "IR.hpp"

// Fuses only resolved, immutable SSA operands. A dominating computation owns
// both components; no division is speculated onto a previously untaken path.
class IRDivModFusion {
public:
    static void run(IRModule& module);
};
