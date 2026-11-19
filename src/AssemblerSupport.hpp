#pragma once

// Target-neutral pieces of the discas line assembler. The driver in
// Assembler.cpp owns parsing, sections, labels, layout passes and symbol
// export; target encoders (GSUAssemblerTarget, ...) only translate one parsed
// statement into bytes and relocations. Everything here is header-only and
// stateless so each encoder can share the exact same operand syntax rules.

#include "CompilerError.hpp"
#include "ObjectFile.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace AsmSupport {

enum class Section { None, Code, Data, Ram, ShortRam };

struct ParsedLine {
    int line_number = 0;
    Section section = Section::None;
    std::string label;
    std::string operation;
    std::vector<std::string> operands;
};

constexpr char InternalSymbolPrefix = '\x01';

[[noreturn]] inline void fail(const std::string& message, int line) {
    throw CompilerError("Assembler: " + message, line, 1);
}

inline SymbolSection symbolSection(Section section) {
    return section == Section::Code ? SymbolSection::CODE : section == Section::Data ? SymbolSection::DATA :
        section == Section::Ram ? SymbolSection::RAM : SymbolSection::SHORT_RAM;
}

inline std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline bool isIdentifier(const std::string& value) {
    if (value.empty()) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!(std::isalpha(first) || value.front() == '_' || value.front() == '.' || value.front() == '$')) {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (!(std::isalnum(c) || value[i] == '_' || value[i] == '.' || value[i] == '$')) return false;
    }
    return true;
}

// Splits on top-level commas. Parentheses and double quotes group text.
inline std::vector<std::string> splitOperands(const std::string& text) {
    std::vector<std::string> result;
    std::size_t start = 0;
    int parentheses = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '"') quoted = !quoted;
        if (quoted) continue;
        if (text[i] == '(') ++parentheses;
        if (text[i] == ')') --parentheses;
        if (text[i] == ',' && parentheses == 0) {
            result.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    const auto tail = trim(text.substr(start));
    if (!tail.empty()) result.push_back(tail);
    return result;
}

inline std::string withoutHash(std::string value) {
    value = trim(value);
    if (!value.empty() && value.front() == '#') value.erase(value.begin());
    return trim(value);
}

inline bool parseNumber(const std::string& text, std::int64_t& value) {
    std::string token = withoutHash(text);
    if (token.empty()) return false;

    bool negative = false;
    if (token.front() == '-') {
        negative = true;
        token.erase(token.begin());
    } else if (token.front() == '+') {
        token.erase(token.begin());
    }
    if (token.empty()) return false;

    int base = 0;
    if (token.front() == '$') {
        base = 16;
        token.erase(token.begin());
    } else if (token.size() > 1 && token.front() == '%') {
        base = 2;
        token.erase(token.begin());
    }
    if (token.empty()) return false;

    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoll(token, &consumed, base);
        // The sign was removed above. Reject a second embedded sign before
        // negation so malformed input cannot negate INT64_MIN.
        if (consumed != token.size() || parsed < 0) return false;
        value = negative ? -parsed : parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

inline void emitByte(std::vector<std::uint8_t>* output, std::uint8_t value) {
    if (output) output->push_back(value);
}

inline void emitWord(std::vector<std::uint8_t>* output, std::uint16_t value) {
    emitByte(output, static_cast<std::uint8_t>(value & 0xff));
    emitByte(output, static_cast<std::uint8_t>((value >> 8) & 0xff));
}

inline void requireOperands(const ParsedLine& line, std::size_t count) {
    if (line.operands.size() != count) {
        fail(line.operation + " expects " + std::to_string(count) + " operand(s).", line.line_number);
    }
}

} // namespace AsmSupport
