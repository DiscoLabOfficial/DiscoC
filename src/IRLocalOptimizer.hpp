#pragma once

#include "IR.hpp"

// A backend-local O1 pass, not mem2reg or global alias analysis. The caller
// owns the module; symbol tables are borrowed only for this invocation.
// Requires verified IR and verifies the rewritten module before returning.
class IRLocalOptimizer {
public:
    static void run(IRModule& module,
                    const std::map<std::string, Analyzer::LocalSymbolTable>& locals);
};
