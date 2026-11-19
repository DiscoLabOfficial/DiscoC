#pragma once

#include "AssemblerTarget.hpp"

#include <memory>

namespace AsmSupport {

// SPC-700 instruction encoder for discas, driven by the SPC700Isa opcode map.
// See docs/assembler.md for the accepted operand syntax.
std::unique_ptr<TargetEncoder> makeSPC700TargetEncoder();

} // namespace AsmSupport
