#pragma once

#include "ObjectFile.hpp"
#include <stdexcept>

namespace GSUCodeLayout {
// A private, version-7-compatible layout hint, not an executable symbol.
// Older linkers can ignore it without changing program semantics. The current
// linker honors it at the final origin, including startup and preceding objects.
constexpr const char* CacheAlignmentSymbol = "\x01" "__disco_cache_align16";

inline unsigned alignment(const ObjectFile& object) {
    for (const auto& symbol : object.symbol_table) {
        if (symbol.name != CacheAlignmentSymbol) continue;
        if (object.config.target != TargetKind::GSU || symbol.section != SymbolSection::CODE || symbol.offset != 0)
            throw std::runtime_error("Invalid GSU CACHE alignment hint.");
        return 16;
    }
    return 1;
}
}
