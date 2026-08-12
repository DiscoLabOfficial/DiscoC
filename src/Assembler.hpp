#pragma once

#include "ObjectFile.hpp"
#include <cstddef>
#include <string>

// Assembles the ca65-like source emitted by AssemblyGenerator into DiscoC's
// relocatable object format.  This is intentionally a two-pass assembler:
// local branches are resolved after all labels are known, while references to
// exported or external symbols remain relocations for discld.
class Assembler {
public:
    ObjectFile assemble(const std::string& source);
};
