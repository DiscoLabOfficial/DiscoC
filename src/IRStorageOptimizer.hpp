#pragma once

#include "IR.hpp"

enum class IRImmutableArrayPolicy { Disabled, CopiedNearRamPayload };

struct IRStorageOptions {
    // Enabled only when the backend/linker can represent private DATA in the
    // copied payload's near RAM bank. This never requests ROM-buffer reads.
    IRImmutableArrayPolicy immutable_arrays = IRImmutableArrayPolicy::Disabled;
};

// Final, candidate-owned storage plan. No analyzer table is mutated. Unknown
// flat-address aliases retain the entire original layout; there is no implied
// restrict/provenance rule. Repeated calls on an unchanged final plan are
// idempotent. After any IR/CFG mutation, discard the plan AND restore the
// original unplanned IR before rebuilding: pooled initializers were removed.
class IRStorageOptimizer {
public:
    static void run(IRModule& module,
                    const std::map<std::string, Analyzer::LocalSymbolTable>& original_locals,
                    IRStorageOptions options = {});
};
