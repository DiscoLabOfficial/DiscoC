#pragma once

#include "IR.hpp"

// O2 operates on an owned copy, never on the analyzed AST/symbol table. Every
// public pass boundary is verified. Unsupported alias/aggregate cases retain
// memory, rather than guessing about ownership or pointer validity.
// PointerPromotionOnly retains checked near-pointer SSA without initializer
// batching or counted-loop conversion; Enabled/Disabled keep their full policy.
enum class IRCompactionPolicy { Enabled, Disabled, PointerPromotionOnly };

class IRGlobalOptimizer {
public:
    static void run(IRModule& module, const std::map<std::string, Analyzer::LocalSymbolTable>& locals,
                    OptimizationLevel policy = OptimizationLevel::O2,
                    IRCompactionPolicy compaction = IRCompactionPolicy::Enabled);
};
