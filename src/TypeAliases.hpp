#pragma once

#include "Token.hpp"
#include "Types.hpp"
#include <cstddef>
#include <map>

constexpr std::size_t MaxTypeAliases = 4096;

// Aliases are transparent canonical types, not new ABI or conversion domains.
// Both the type and diagnostic token are owned values.
struct TypeAliasBinding {
    Type type;
    Token declaration;
};
using TypeAliasTable = std::map<std::string, TypeAliasBinding>;

inline TypeAliasTable builtinTypeAliases() {
    TypeAliasTable result;
    for (const auto& name : {"u8", "i8", "u16", "i16"}) {
        Type type;
        type.base = std::string(name).size() == 2 ? BaseType::BYTE : BaseType::WORD;
        type.sizeInBytes = type.base == BaseType::BYTE ? 1 : 2;
        type.is_unsigned = name[0] == 'u';
        type.space = AddressSpace::RAM;
        result.emplace(name, TypeAliasBinding{type, Token(TokenType::IDENTIFIER, name, 0, 0)});
    }
    return result;
}
