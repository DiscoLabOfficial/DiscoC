#pragma once

#include "ObjectFile.hpp"
#include "TargetConfig.hpp"
#include <cstddef>
#include <string>

// Assembles the ca65-like source emitted by AssemblyGenerator into DiscoC's
// relocatable object format.  This is intentionally a two-pass assembler:
// local branches are resolved after all labels are known, while references to
// exported or external symbols remain relocations for discld.  The target
// selects the instruction encoder; the driver itself is target-neutral.
class Assembler {
public:
    explicit Assembler(TargetKind target = TargetKind::GSU) : m_target(target) {}

    ObjectFile assemble(const std::string& source);

    // Returns the processor named by the first `.setcpu` directive in
    // `source` that names a supported target, or `fallback` when none does.
    static TargetKind detectTarget(const std::string& source, TargetKind fallback);

private:
    TargetKind m_target;
};
