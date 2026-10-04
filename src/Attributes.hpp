#pragma once

#include <string>

enum class AttributeSite { TopLevel, Statement };

// These names have future contracts, not silently accepted semantics. Until
// lowering/ABI support exists the frontend must reject them explicitly.
inline bool isReservedAttribute(const std::string& name) {
    return name == "interrupt" || name == "naked" || name == "section" ||
           name == "bank" || name == "calling_convention" || name == "inline";
}
