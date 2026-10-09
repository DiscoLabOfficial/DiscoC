#pragma once
#include "IR.hpp"
#include "TargetConfig.hpp"

// Bounded, typed rerolling. Never infer repetition from textual assembly or
// exchange ROM and RAM reads. Rebuild analyses after each CFG mutation.
class IRCompactOptimizer {
public:
    static void run(IRModule& module,
        const std::map<std::string, Analyzer::LocalSymbolTable>& locals,
        OptimizationLevel policy);
};
