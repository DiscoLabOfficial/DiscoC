#pragma once

#include "IR.hpp"

// O2 operates on an owned copy, never on the analyzed AST/symbol table. Every
// public pass boundary is verified. Unsupported alias/aggregate cases retain
// memory, rather than guessing about ownership or pointer validity.
enum class IRCompactionPolicy { Enabled, Disabled };

class IRGlobalOptimizer {
public:
    static void run(IRModule& module, const std::map<std::string, Analyzer::LocalSymbolTable>& locals,
                    OptimizationLevel policy = OptimizationLevel::O2,
                    IRCompactionPolicy compaction = IRCompactionPolicy::Enabled);
};
