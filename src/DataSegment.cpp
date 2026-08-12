#include "DataSegment.hpp"
#include "CompilerError.hpp"
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cstdint>

// Helper to convert integer to hex string for errors
template <typename T>
std::string to_hex_string(T i) {
    std::stringstream stream;
    // Cast to an unsigned integer type to ensure it's treated as a number, not a character.
    // Then set the stream state to output in hexadecimal.
    stream << "0x" << std::hex << static_cast<unsigned int>(i);
    return stream.str();
}

const std::map<std::string, DataEntry>& DataSegmentManager::getEntries() const {
    return m_entries;
}

void DataSegmentManager::add(ConstDataStmt& stmt) {
    if (m_entries.count(stmt.token.lexeme)) {
        throw CompilerError("Data label '" + stmt.token.lexeme + "' already defined.", stmt.token.line_number, stmt.token.col_number);
    }
    DataEntry entry;
    entry.label = stmt.token.lexeme;
    entry.type = stmt.type;
    for (const auto& expr : stmt.initializers) {
        if (auto* literal = dynamic_cast<LiteralExpr*>(expr.get())) {
            std::int64_t value = 0;
            try {
                std::size_t parsed = 0;
                value = std::stoll(literal->token.lexeme, &parsed, 0);
                if (parsed != literal->token.lexeme.size()) throw std::invalid_argument("trailing characters");
            } catch (const std::exception&) {
                throw CompilerError("Invalid integer literal in ROM data.",
                                    literal->token.line_number, literal->token.col_number);
            }
            if (stmt.type.base == BaseType::BYTE) {
                const auto min_value = stmt.type.is_unsigned ? 0 : -128;
                const auto max_value = stmt.type.is_unsigned ? 255 : 127;
                if (value < min_value || value > max_value) {
                    throw CompilerError("ROM data value is out of range for byte.",
                                        literal->token.line_number, literal->token.col_number);
                }
                entry.bytes.push_back(static_cast<uint8_t>(value));
            } else if (stmt.type.base == BaseType::WORD) {
                const auto min_value = stmt.type.is_unsigned ? 0 : -32768;
                const auto max_value = stmt.type.is_unsigned ? 65535 : 32767;
                if (value < min_value || value > max_value) {
                    throw CompilerError("ROM data value is out of range for word.",
                                        literal->token.line_number, literal->token.col_number);
                }
                entry.bytes.push_back(value & 0xFF);
                entry.bytes.push_back((value >> 8) & 0xFF);
            } else {
                throw CompilerError("ROM data requires byte or word type.",
                                    stmt.token.line_number, stmt.token.col_number);
            }
        } else {
            throw CompilerError("ROM data initializers must be constant literals.", expr->token.line_number, expr->token.col_number);
        }
    }
    m_entries[entry.label] = entry;
}

bool DataSegmentManager::hasSymbol(const std::string& label) const {
    return m_entries.count(label);
}

Type DataSegmentManager::getSymbolType(const std::string& label) const {
    if (m_entries.find(label) == m_entries.end()) throw std::runtime_error("Linker Error: Undefined data label '" + label + "'.");
    return m_entries.at(label).type;
}
