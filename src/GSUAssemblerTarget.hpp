#pragma once

#include "AssemblerTarget.hpp"

#include <memory>

namespace AsmSupport {

// SuperFX (GSU) instruction encoder for discas. Its accepted syntax and the
// emitted bytes are the canonical output of AssemblyGenerator; see
// docs/assembler.md and casfx.inc for the mnemonic set.
std::unique_ptr<TargetEncoder> makeGSUTargetEncoder();

} // namespace AsmSupport
