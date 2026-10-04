#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

constexpr int MaxPointerDepth = 32;
// An l-value of the deepest source type still needs a storage-address type.
constexpr int MaxAddressPointerDepth = MaxPointerDepth + 1;

// Stable identity for a declaration within one analyzed compilation unit.
// Names are not sufficient because nested lexical scopes may legally shadow
// one another.
struct SymbolId {
    static constexpr std::uint32_t Invalid = 0;
    std::uint32_t value = Invalid;

    bool isValid() const { return value != Invalid; }
    friend bool operator==(SymbolId lhs, SymbolId rhs) { return lhs.value == rhs.value; }
    friend bool operator!=(SymbolId lhs, SymbolId rhs) { return !(lhs == rhs); }
    friend bool operator<(SymbolId lhs, SymbolId rhs) { return lhs.value < rhs.value; }
};

enum class AddressSpace {
    NONE, // Default, uninitialized state
    RAM,
    ROM
};

// Represents the fundamental size of a type.
enum class BaseType { NONE, BYTE, WORD, VOID, STRUCT, BOOL };

struct AccessQualifiers {
    bool is_const = false;
    bool is_volatile = false;
    friend bool operator==(AccessQualifiers a, AccessQualifiers b) {
        return a.is_const == b.is_const && a.is_volatile == b.is_volatile;
    }
};

enum class Linkage { External, Internal };

// Represents the full semantic type.
struct Type {
    BaseType base = BaseType::NONE;
    std::string structName; // Used if base is STRUCT
    int sizeInBytes = 0; // Filled in by Analyzer for all types
    bool is_unsigned = false;
    int pointer_level = 0;
    bool is_far = false;
    int array_size = 0;
    AddressSpace space = AddressSpace::NONE; // Default to NONE
    // Innermost first. Each pointer level has its own representation and
    // pointee address space; taking &p must not inherit p's far qualifier.
    std::vector<bool> pointer_reach = {};
    std::vector<AddressSpace> pointer_spaces = {};
    // Qualify this object independently of its location. Each pointer layer
    // retains the qualifiers of the object reached by dereferencing it.
    bool is_const = false;
    bool is_volatile = false;
    std::vector<AccessQualifiers> pointee_qualifiers = {};
    int alignment = 0;
    int aggregate_size = 0;
    std::string enum_name = {};
};

inline int storageAlignment(const Type& type) {
    if (type.pointer_level > 0) return 2;
    if (type.base == BaseType::BYTE || type.base == BaseType::BOOL || type.base == BaseType::VOID) return 1;
    if (type.base == BaseType::STRUCT && type.alignment > 0) return type.alignment;
    return 2;
}

inline bool isFarPointer(const Type& type) {
    return type.pointer_level > 0 && (type.pointer_reach.empty()
        ? type.is_far : type.pointer_reach.back());
}

inline bool isVoidValue(const Type& type) { return type.base == BaseType::VOID && type.pointer_level == 0; }

inline void normalizePointerLayers(Type& type) {
    if (type.pointer_level < 0 || type.pointer_level > MaxAddressPointerDepth ||
        (!type.pointer_reach.empty() && type.pointer_reach.size() != static_cast<std::size_t>(type.pointer_level)) ||
        (!type.pointer_spaces.empty() && type.pointer_spaces.size() != static_cast<std::size_t>(type.pointer_level)) ||
        (!type.pointee_qualifiers.empty() && type.pointee_qualifiers.size() != static_cast<std::size_t>(type.pointer_level)))
        throw std::logic_error("Invalid pointer layer metadata.");
    if (type.pointer_level == 0) {
        type.pointer_reach.clear();
        type.pointer_spaces.clear();
        type.pointee_qualifiers.clear();
        type.is_far = false;
        return;
    }
    if (type.pointer_reach.empty()) {
        type.pointer_reach.assign(static_cast<std::size_t>(type.pointer_level), false);
        type.pointer_reach.back() = type.is_far;
    }
    if (type.pointer_spaces.empty()) {
        type.pointer_spaces.assign(static_cast<std::size_t>(type.pointer_level), AddressSpace::RAM);
        type.pointer_spaces.front() = type.space;
    }
    if (type.pointee_qualifiers.empty())
        type.pointee_qualifiers.resize(static_cast<std::size_t>(type.pointer_level));
    type.is_far = type.pointer_reach.back();
    type.space = type.pointer_spaces.back();
    type.sizeInBytes = type.is_far ? 4 : 2;
}

inline Type pointeeType(Type type) {
    if (type.pointer_level <= 0) throw std::logic_error("A non-pointer has no pointee type.");
    normalizePointerLayers(type);
    const auto space = type.space;
    const auto qualifiers = type.pointee_qualifiers.back();
    --type.pointer_level;
    type.pointer_reach.pop_back();
    type.pointer_spaces.pop_back();
    type.pointee_qualifiers.pop_back();
    type.is_const = qualifiers.is_const;
    type.is_volatile = qualifiers.is_volatile;
    if (type.pointer_level > 0) normalizePointerLayers(type);
    else {
        type.is_far = false;
        type.space = space;
        type.sizeInBytes = type.base == BaseType::BYTE || type.base == BaseType::BOOL ? 1 :
            type.base == BaseType::VOID ? 0 : type.base == BaseType::STRUCT && type.aggregate_size > 0 ? type.aggregate_size : 2;
    }
    return type;
}

inline Type pointerTo(Type type, AddressSpace space, bool far = false) {
    normalizePointerLayers(type);
    ++type.pointer_level;
    type.array_size = 0;
    type.pointer_reach.push_back(far);
    type.pointer_spaces.push_back(space);
    type.pointee_qualifiers.push_back({type.is_const, type.is_volatile});
    type.is_const = false;
    type.is_volatile = false;
    normalizePointerLayers(type);
    return type;
}

inline bool samePointerLayers(Type left, Type right) {
    normalizePointerLayers(left);
    normalizePointerLayers(right);
    return left.pointer_reach == right.pointer_reach && left.pointer_spaces == right.pointer_spaces &&
           left.pointee_qualifiers == right.pointee_qualifiers;
}

inline Type valueType(Type type) {
    type.is_const = false;
    type.is_volatile = false;
    return type;
}

// Storage width describes the value, not the pointee. Near pointers occupy
// one word; far pointers occupy a zero-padded bank word followed by an offset.
inline bool usesByteStorage(const Type& type) {
    return type.pointer_level == 0 && type.sizeInBytes == 1;
}

// Information stored about a declared variable.
struct Symbol {
    Type type;
    int stackOffset;
    std::string initial_literal_value;
    SymbolId id;
};

// Helper for debugging.
inline std::string to_string(const Type& type) {
    std::string s;
    const AccessQualifiers leaf = type.pointer_level > 0 && !type.pointee_qualifiers.empty()
        ? type.pointee_qualifiers.front() : AccessQualifiers{type.is_const, type.is_volatile};
    if (leaf.is_const) s += "const ";
    if (leaf.is_volatile) s += "volatile ";
    if (type.space == AddressSpace::ROM) s += "rom ";
    if (type.is_unsigned) s += "unsigned ";
    switch (type.base) {
        case BaseType::BYTE: s += "byte"; break;
        case BaseType::BOOL: s += "bool"; break;
        case BaseType::WORD: s += "word"; break;
		case BaseType::STRUCT: s += "struct " + type.structName; break;
        case BaseType::VOID: s += "void"; break;
        default: break;
    }
    if (isFarPointer(type)) s = "far " + s;
    for (int i = 0; i < type.pointer_level; ++i) {
        s += "*";
        if (i + 1 < type.pointer_level && !type.pointer_reach.empty() &&
            type.pointer_reach[static_cast<std::size_t>(i)]) s += " far ";
        const auto qualifiers = i + 1 == type.pointer_level
            ? AccessQualifiers{type.is_const, type.is_volatile}
            : type.pointee_qualifiers.empty() ? AccessQualifiers{} : type.pointee_qualifiers[static_cast<std::size_t>(i + 1)];
        if (qualifiers.is_const) s += " const ";
        if (qualifiers.is_volatile) s += " volatile ";
    }
    if (type.array_size > 0) {
        s += "[" + std::to_string(type.array_size) + "]";
    }
    return s;
}
