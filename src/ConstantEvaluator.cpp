#include "IntegerLiteral.hpp"
#include "ConstantEvaluator.hpp"
#include "CompilerError.hpp"

std::int64_t ConstantEvaluator::convert(std::int64_t value, const Type& type) {
    if (type.pointer_level > 0) return value;
    if (type.base == BaseType::BOOL) return value != 0;
    const unsigned width = type.base == BaseType::BYTE ? 8u : 16u;
    const std::uint64_t modulus = std::uint64_t{1} << width;
    const auto bits = static_cast<std::uint64_t>(value) & (modulus - 1u);
    return !type.is_unsigned && bits >= modulus / 2u
        ? static_cast<std::int64_t>(bits) - static_cast<std::int64_t>(modulus)
        : static_cast<std::int64_t>(bits);
}

bool ConstantEvaluator::evaluate(const Expr& expression, std::int64_t& value) {
    if (expression.is_constant) { value = expression.constant_value; return true; }
    if (const auto* literal = dynamic_cast<const LiteralExpr*>(&expression)) {
        if (literal->token.type == TokenType::KEYWORD_TRUE) value = 1;
        else if (literal->token.type == TokenType::KEYWORD_FALSE) value = 0;
        else {
            try {
                std::size_t consumed = 0;
                value = DiscoNumeric::parse(literal->token.lexeme, &consumed, 0);
                if (consumed != literal->token.lexeme.size()) return false;
            } catch (const std::exception&) { return false; }
        }
        return true;
    }
    if (const auto* cast = dynamic_cast<const CastExpr*>(&expression)) {
        if (!evaluate(*cast->expression, value)) return false;
        value = convert(value, cast->result_type);
        return true;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expression)) {
        if (!evaluate(*unary->right, value)) return false;
        if (unary->token.type == TokenType::MINUS) value = -value;
        else if (unary->token.type == TokenType::BANG) value = value == 0;
        else if (unary->token.type == TokenType::TILDE) value = static_cast<std::int64_t>(~static_cast<std::uint64_t>(value) & 0xffffu);
        else return false;
        value = convert(value, unary->result_type);
        return true;
    }
    const auto* binary = dynamic_cast<const BinaryExpr*>(&expression);
    if (!binary || binary->pointer_stride > 0 || binary->result_type.pointer_level > 0) return false;
    std::int64_t left = 0, right = 0;
    if (!evaluate(*binary->left, left)) return false;
    const auto operation = binary->token.type;
    if (operation == TokenType::AND_AND && left == 0) { value = 0; return true; }
    if (operation == TokenType::OR_OR && left != 0) { value = 1; return true; }
    if (!evaluate(*binary->right, right)) return false;
    // Typed scalar operands are at most 16 bits; int64_t safely holds their
    // product, signed minimum/-1, and the largest legal shifted result.
    switch (operation) {
        case TokenType::PLUS: value = left + right; break;
        case TokenType::MINUS: value = left - right; break;
        case TokenType::STAR: value = left * right; break;
        case TokenType::SLASH:
        case TokenType::PERCENT:
            if (right == 0) throw CompilerError("Division by zero in constant expression.", expression.token);
            value = operation == TokenType::SLASH ? left / right : left % right; break;
        case TokenType::AMPERSAND: value = static_cast<std::int64_t>(static_cast<std::uint64_t>(left) & static_cast<std::uint64_t>(right) & 0xffffu); break;
        case TokenType::PIPE: value = static_cast<std::int64_t>((static_cast<std::uint64_t>(left) | static_cast<std::uint64_t>(right)) & 0xffffu); break;
        case TokenType::CARET: value = static_cast<std::int64_t>((static_cast<std::uint64_t>(left) ^ static_cast<std::uint64_t>(right)) & 0xffffu); break;
        case TokenType::SHIFT_LEFT:
        case TokenType::SHIFT_RIGHT:
            if (right < 0 || right > 15) throw CompilerError("Constant shift count must be in 0..15.", expression.token);
            if (operation == TokenType::SHIFT_LEFT) value = static_cast<std::int64_t>((static_cast<std::uint64_t>(left) & 0xffffu) << static_cast<unsigned>(right));
            else if (left >= 0) value = left / (std::int64_t{1} << static_cast<unsigned>(right));
            else value = -((-left + (std::int64_t{1} << static_cast<unsigned>(right)) - 1) / (std::int64_t{1} << static_cast<unsigned>(right)));
            break;
        case TokenType::EQUAL_EQUAL: value = left == right; break;
        case TokenType::BANG_EQUAL: value = left != right; break;
        case TokenType::LESS: value = left < right; break;
        case TokenType::LESS_EQUAL: value = left <= right; break;
        case TokenType::GREATER: value = left > right; break;
        case TokenType::GREATER_EQUAL: value = left >= right; break;
        case TokenType::AND_AND: value = left != 0 && right != 0; break;
        case TokenType::OR_OR: value = left != 0 || right != 0; break;
        default: return false;
    }
    value = convert(value, expression.result_type);
    return true;
}
