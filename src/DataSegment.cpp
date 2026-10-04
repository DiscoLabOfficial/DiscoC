#include "IntegerLiteral.hpp"
#include "DataSegment.hpp"
#include "CompilerError.hpp"
#include "ConstantEvaluator.hpp"
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

namespace {
std::int64_t scalarInitializer(const Expr& expression) {
    std::int64_t constant = 0;
    if (ConstantEvaluator::evaluate(expression, constant)) return constant;
    if (const auto* literal = dynamic_cast<const LiteralExpr*>(&expression)) {
        if (literal->token.type == TokenType::KEYWORD_TRUE) return 1;
        if (literal->token.type == TokenType::KEYWORD_FALSE) return 0;
        return DiscoNumeric::parse(literal->token.lexeme, nullptr, 0);
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        if (unary->token.type == TokenType::MINUS) return -scalarInitializer(*unary->right);
    }
    if (const auto* cast = dynamic_cast<const CastExpr*>(&expression)) {
        const auto value = scalarInitializer(*cast->expression);
        if (cast->result_type.base == BaseType::BOOL && cast->result_type.pointer_level == 0) return value != 0;
        if (usesByteStorage(cast->result_type)) {
            const auto bits = static_cast<std::uint8_t>(value);
            return !cast->result_type.is_unsigned && bits >= 128 ? static_cast<int>(bits) - 256 : bits;
        }
        if (cast->result_type.pointer_level == 0) {
            const auto bits = static_cast<std::uint16_t>(value);
            return !cast->result_type.is_unsigned && bits >= 32768 ? static_cast<int>(bits) - 65536 : bits;
        }
        return value;
    }
    throw CompilerError("Static initializers must be scalar literals or casts of literals.", expression.token);
}
}

void DataSegmentManager::add(VarDeclStmt& stmt) {
    const auto elements = stmt.type.array_size > 0 ? static_cast<std::uint32_t>(stmt.type.array_size) : 1u;
    if (stmt.type.sizeInBytes <= 0 || static_cast<std::uint32_t>(stmt.type.sizeInBytes) > 65536u / elements)
        throw CompilerError("Static storage exceeds one RAM bank.", stmt.token);
    DataEntry entry;
    entry.label = stmt.token.lexeme;
    entry.type = stmt.type;
    entry.storage = AddressSpace::RAM;
    entry.id = stmt.symbol_id;
    entry.link_name = stmt.link_name;
    entry.is_extern = stmt.is_extern;
    if (stmt.is_extern) {
        m_entries.emplace(entry.label, std::move(entry));
        return;
    }
    entry.bytes.resize(static_cast<std::size_t>(stmt.type.sizeInBytes) * elements, 0);
    const auto store = [&](int offset, const Type& type, const Expr& expression) {
        const auto value = static_cast<std::uint32_t>(scalarInitializer(expression));
        if (isFarPointer(type)) {
            entry.bytes.at(static_cast<std::size_t>(offset)) = static_cast<std::uint8_t>(value >> 16);
            entry.bytes.at(static_cast<std::size_t>(offset + 2)) = static_cast<std::uint8_t>(value);
            entry.bytes.at(static_cast<std::size_t>(offset + 3)) = static_cast<std::uint8_t>(value >> 8);
        } else {
            entry.bytes.at(static_cast<std::size_t>(offset)) = static_cast<std::uint8_t>(value);
            if (type.sizeInBytes > 1) entry.bytes.at(static_cast<std::size_t>(offset + 1)) = static_cast<std::uint8_t>(value >> 8);
        }
    };
    for (const auto& element : stmt.aggregate_initializers) store(element.offset, element.type, *element.value);
    if (stmt.initializer) {
        const auto value = static_cast<std::uint32_t>(scalarInitializer(*stmt.initializer));
        if (isFarPointer(stmt.type)) {
            entry.bytes.at(0) = static_cast<std::uint8_t>(value >> 16);
            entry.bytes.at(2) = static_cast<std::uint8_t>(value);
            entry.bytes.at(3) = static_cast<std::uint8_t>(value >> 8);
        } else {
            entry.bytes.at(0) = static_cast<std::uint8_t>(value);
            if (entry.bytes.size() > 1) entry.bytes.at(1) = static_cast<std::uint8_t>(value >> 8);
        }
    }
    const auto label = entry.label;
    m_entries[label] = std::move(entry); // A definition replaces a compatible extern declaration.
}

void DataSegmentManager::add(ConstDataStmt& stmt) {
    if (stmt.is_array && stmt.initializers.empty()) throw CompilerError("ROM array requires a nonempty initializer.", stmt.token);
    if (m_entries.count(stmt.token.lexeme)) {
        throw CompilerError("Data label '" + stmt.token.lexeme + "' already defined.", stmt.token);
    }
    DataEntry entry;
    entry.label = stmt.token.lexeme;
    entry.type = stmt.type;
    if (stmt.is_array) entry.type.array_size = static_cast<int>(stmt.initializers.size());
    entry.link_name = stmt.linkage == Linkage::Internal ? std::string(1, '\x01') + stmt.token.lexeme : stmt.token.lexeme;
    for (const auto& expr : stmt.initializers) {
        if (auto* literal = dynamic_cast<LiteralExpr*>(expr.get())) {
            std::int64_t value = 0;
            try {
                if (literal->token.type == TokenType::KEYWORD_TRUE) value = 1;
                else if (literal->token.type == TokenType::KEYWORD_FALSE) value = 0;
                else {
                    std::size_t parsed = 0;
                    value = DiscoNumeric::parse(literal->token.lexeme, &parsed, 0);
                    if (parsed != literal->token.lexeme.size()) throw std::invalid_argument("trailing characters");
                }
            } catch (const std::exception&) {
                throw CompilerError("Invalid integer literal in ROM data.",
                                    literal->token);
            }
            if (stmt.type.base == BaseType::BOOL) {
                entry.bytes.push_back(value != 0 ? 1 : 0);
            } else if (stmt.type.base == BaseType::BYTE) {
                const auto min_value = stmt.type.is_unsigned ? 0 : -128;
                const auto max_value = stmt.type.is_unsigned ? 255 : 127;
                if (value < min_value || value > max_value) {
                    throw CompilerError("ROM data value is out of range for byte.",
                                        literal->token);
                }
                entry.bytes.push_back(static_cast<uint8_t>(value));
            } else if (stmt.type.base == BaseType::WORD) {
                const auto min_value = stmt.type.is_unsigned ? 0 : -32768;
                const auto max_value = stmt.type.is_unsigned ? 65535 : 32767;
                if (value < min_value || value > max_value) {
                    throw CompilerError("ROM data value is out of range for word.",
                                        literal->token);
                }
                entry.bytes.push_back(value & 0xFF);
                entry.bytes.push_back((value >> 8) & 0xFF);
            } else {
                throw CompilerError("ROM data requires bool, byte or word type.",
                                    stmt.token);
            }
        } else {
            throw CompilerError("ROM data initializers must be constant literals.", expr->token);
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
