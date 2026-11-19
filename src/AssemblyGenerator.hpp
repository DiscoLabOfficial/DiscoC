#pragma once
#include "ObjectFile.hpp"
#include <string>

// Borrowed encoded object, not a second AST backend. It must outlive generate().
// With relocations the listing is relocatable; otherwise it is a linked listing.
class AssemblyGenerator {
public:
    explicit AssemblyGenerator(const ObjectFile& object) : m_object(object) {}
    std::string generate() const;
private:
    std::string generateSPC700() const;
    const ObjectFile& m_object;
};
