#include "ProjectManifest.hpp"
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace DiscoProject {
namespace {
bool bare(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-';
}
int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
void appendUtf8(std::string& output, std::uint32_t point) {
    if (point <= 0x7f) output += static_cast<char>(point);
    else if (point <= 0x7ff) {
        output += static_cast<char>(0xc0 | (point >> 6));
        output += static_cast<char>(0x80 | (point & 63));
    } else if (point <= 0xffff) {
        output += static_cast<char>(0xe0 | (point >> 12));
        output += static_cast<char>(0x80 | ((point >> 6) & 63));
        output += static_cast<char>(0x80 | (point & 63));
    } else {
        output += static_cast<char>(0xf0 | (point >> 18));
        output += static_cast<char>(0x80 | ((point >> 12) & 63));
        output += static_cast<char>(0x80 | ((point >> 6) & 63));
        output += static_cast<char>(0x80 | (point & 63));
    }
}
class Reader {
public:
    Reader(const std::string& source, const std::string& path) : m_source(source) { m_result.path = path; }
    Manifest read() {
        if (m_source.size() > MaxManifestBytes) fail("Manifest exceeds 64 KiB.");
        validateUtf8();
        std::string table;
        std::set<std::string> tables;
        skipLines();
        while (!end()) {
            if (peek() == '[') {
                advance();
                if (peek() == '[') fail("Array-of-table syntax is not supported by the manifest schema.");
                table = key();
                require(']', "Expected ']' after table name.");
                if (table == "target.superfx") table = "target.gsu";
                if (table != "project" && table != "compiler" && table != "runtime" && table != "output" &&
                    table != "target.gsu" && table != "target.spc700")
                    fail("Unknown manifest table '" + table + "'.");
                if (!tables.insert(table).second) fail("Duplicate table '" + table + "'.");
                if (tables.size() > 8) fail("Manifest table limit exceeded.");
                finishLine();
            } else {
                const auto name = key();
                if (name.find('.') != std::string::npos) fail("Use table headers, not dotted assignment keys.");
                require('=', "Expected '=' after key.");
                horizontal();
                auto value = readValue();
                const auto full_name = table.empty() ? name : table + "." + name;
                if (!m_result.values.emplace(full_name, std::move(value)).second)
                    fail("Duplicate key '" + full_name + "'.");
                if (m_result.values.size() > 64) fail("Manifest key limit exceeded.");
                finishLine();
            }
            skipLines();
        }
        return std::move(m_result);
    }
private:
    const std::string& m_source; // Borrowed for this synchronous read only.
    Manifest m_result;
    std::size_t m_cursor = 0, m_line = 1, m_column = 1;
    bool end() const { return m_cursor == m_source.size(); }
    char peek() const { return end() ? '\0' : m_source[m_cursor]; }
    char advance() {
        if (end()) fail("Unexpected end of manifest.");
        const char c = m_source[m_cursor++];
        if (c == '\n') { ++m_line; m_column = 1; } else ++m_column;
        return c;
    }
    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error(m_result.path + ':' + std::to_string(m_line) + ':' +
            std::to_string(m_column) + ": manifest error: " + message);
    }
    [[noreturn]] void failAt(std::size_t offset, const std::string& message) {
        m_line = m_column = 1;
        for (std::size_t index = 0; index < offset; ++index) {
            if (m_source[index] == '\n') { ++m_line; m_column = 1; } else ++m_column;
        }
        fail(message);
    }
    void validateUtf8() {
        for (std::size_t index = 0; index < m_source.size();) {
            const auto start = index;
            const auto first = static_cast<unsigned char>(m_source[index++]);
            if (first < 0x80) {
                if ((first < 32 && first != '\t' && first != '\n' && first != '\r') || first == 127)
                    failAt(start, "Invalid control character.");
                if (first == '\r' && (index == m_source.size() || m_source[index] != '\n'))
                    failAt(start, "Carriage return must be followed by a newline.");
                continue;
            }
            const unsigned length = first >= 0xf0 && first <= 0xf4 ? 4 :
                first >= 0xe0 && first <= 0xef ? 3 : first >= 0xc2 && first <= 0xdf ? 2 : 0;
            if (!length || m_source.size() - index < length - 1) failAt(start, "Invalid UTF-8.");
            std::uint32_t point = first & (0x7f >> length);
            for (unsigned byte = 1; byte < length; ++byte) {
                const auto continuation = static_cast<unsigned char>(m_source[index++]);
                if ((continuation & 0xc0) != 0x80) failAt(index - 1, "Invalid UTF-8.");
                point = (point << 6) | (continuation & 63);
            }
            if (point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) ||
                point < (length == 2 ? 0x80u : length == 3 ? 0x800u : 0x10000u)) failAt(start, "Invalid UTF-8.");
        }
    }
    void horizontal() { while (peek() == ' ' || peek() == '\t') advance(); }
    void comment() { if (peek() == '#') while (!end() && peek() != '\n' && peek() != '\r') advance(); }
    void newline() { if (peek() == '\r') advance(); require('\n', "Expected newline."); }
    void skipLines() {
        while (true) {
            horizontal(); comment();
            if (peek() != '\n' && peek() != '\r') return;
            newline();
        }
    }
    void finishLine() {
        horizontal(); comment();
        if (!end()) newline();
    }
    void require(char c, const std::string& message) { horizontal(); if (peek() != c || end()) fail(message); advance(); }
    std::string key() {
        horizontal();
        std::string result;
        do {
            if (!bare(peek())) fail("Expected bare key/table name.");
            while (bare(peek())) result += advance();
            horizontal();
            if (peek() != '.') break;
            result += advance(); horizontal();
        } while (true);
        if (result.size() > 128) fail("Key/table name exceeds 128 bytes.");
        return result;
    }
    std::string string() {
        const char quote = advance();
        std::string result;
        while (!end() && peek() != quote) {
            const auto c = advance();
            if (c == '\n' || c == '\r') fail("Multiline strings are not supported in manifests.");
            if (c != '\\' || quote == '\'') result += c;
            else {
                const auto escaped = advance();
                switch (escaped) {
                    case 'b': result += '\b'; break;
                    case 't': result += '\t'; break;
                    case 'n': result += '\n'; break;
                    case 'f': result += '\f'; break;
                    case 'r': result += '\r'; break;
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case 'u': case 'U': {
                        std::uint32_t point = 0;
                        for (unsigned index = 0; index < (escaped == 'u' ? 4u : 8u); ++index) {
                            const auto value = digit(advance());
                            if (value < 0) fail("Invalid Unicode escape.");
                            point = (point << 4) | static_cast<unsigned>(value);
                        }
                        if (point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) fail("Invalid Unicode scalar.");
                        appendUtf8(result, point); break;
                    }
                    default: fail("Invalid string escape.");
                }
            }
            if (result.size() > MaxProjectPathBytes) fail("String exceeds 4096 bytes.");
        }
        require(quote, "Unterminated string.");
        return result;
    }
    std::int64_t integer(const std::string& text) {
        std::size_t index = 0;
        bool negative = false, signed_token = false;
        if (text.empty()) fail("Expected integer or boolean.");
        if (text[index] == '+' || text[index] == '-') { signed_token = true; negative = text[index++] == '-'; }
        unsigned radix = 10;
        if (text.size() - index >= 2 && text[index] == '0') {
            const auto prefix = text[index + 1];
            if (prefix == 'x' || prefix == 'o' || prefix == 'b') {
                if (signed_token) fail("Non-decimal TOML integers cannot have a sign.");
                radix = prefix == 'x' ? 16 : prefix == 'o' ? 8 : 2; index += 2;
            } else if (text.size() - index > 1) fail("Leading zero in decimal integer.");
        }
        const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + (negative ? 1u : 0u);
        std::uint64_t value = 0;
        bool previous_digit = false;
        for (; index < text.size(); ++index) {
            const auto c = text[index];
            if (c == '_') {
                if (!previous_digit || index + 1 == text.size() || digit(text[index + 1]) < 0 ||
                    static_cast<unsigned>(digit(text[index + 1])) >= radix) fail("Invalid numeric underscore.");
                previous_digit = false; continue;
            }
            const auto next = digit(c);
            if (next < 0 || static_cast<unsigned>(next) >= radix) fail("Expected a TOML integer or boolean (no floats/dates).");
            if (value > (limit - static_cast<unsigned>(next)) / radix) fail("Integer exceeds signed 64-bit range.");
            value = value * radix + static_cast<unsigned>(next); previous_digit = true;
        }
        if (!previous_digit) fail("Expected integer digits.");
        if (negative && value == limit) return std::numeric_limits<std::int64_t>::min();
        return negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
    }
    Value readValue() {
        Value result; result.line = m_line; result.column = m_column;
        if (peek() == '"' || peek() == '\'') result.text = string();
        else if (peek() == '[') {
            result.kind = Value::Kind::Strings; advance(); skipLines();
            while (peek() != ']') {
                if (peek() != '"' && peek() != '\'') fail("Sources must be an array of quoted strings.");
                if (result.strings.size() == MaxProjectSources) fail("Source count exceeds 128.");
                result.strings.push_back(string()); skipLines();
                if (peek() == ']') break;
                require(',', "Expected ',' between array entries."); skipLines();
            }
            require(']', "Unterminated array.");
        } else {
            std::string text;
            while (!end() && peek() != ' ' && peek() != '\t' && peek() != '\n' && peek() != '\r' && peek() != '#') text += advance();
            if (text == "true" || text == "false") { result.kind = Value::Kind::Boolean; result.boolean = text == "true"; }
            else { result.kind = Value::Kind::Integer; result.integer = integer(text); }
        }
        return result;
    }
};
} // namespace

Manifest Manifest::parse(const std::string& source, const std::string& path) {
    auto result = Reader(source, path).read(); result.validate(); return result;
}
Manifest Manifest::read(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open manifest: " + path);
    const auto size = file.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(MaxManifestBytes)) throw std::runtime_error("Manifest exceeds 64 KiB or cannot be read: " + path);
    std::string source(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if (!source.empty() && !file.read(&source[0], static_cast<std::streamsize>(source.size()))) throw std::runtime_error("Truncated manifest: " + path);
    return parse(source, path);
}
} // namespace DiscoProject
