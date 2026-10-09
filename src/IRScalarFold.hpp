#pragma once

#include <array>
#include "IR.hpp"
#include "ConstantEvaluator.hpp"

// Shared O1/O2 scalar evaluation. Operand types are borrowed for this call
// only; callers must establish that the operand values are constants.
namespace IRScalarFold {
inline bool scalar(const Type& type) {
    return type.pointer_level == 0 && type.array_size == 0 &&
        (type.base == BaseType::BYTE || type.base == BaseType::WORD || type.base == BaseType::BOOL);
}
struct Operand {
    const Type* type = nullptr;
    std::int64_t value = 0;
};
inline bool evaluate(const IRInstruction& instruction, const std::array<Operand, 2>& operands,
                     std::int64_t& result) {
    if (!scalar(instruction.type) || instruction.operands.empty() || instruction.operands.size() > 2) return false;
    std::int64_t values[2] = {};
    for (std::size_t n = 0; n < instruction.operands.size(); ++n) {
        if (!operands[n].type || !scalar(*operands[n].type)) return false;
        values[n] = ConstantEvaluator::convert(operands[n].value, *operands[n].type);
    }
    const auto a = values[0], b = values[1];
    const auto& op = instruction.operation;
    if (instruction.opcode == IROpcode::BitExtract) {
        if (instruction.immediate < 0 || instruction.immediate > 15) return false;
        result = (static_cast<std::uint64_t>(a) >> static_cast<unsigned>(instruction.immediate)) & 1u;
    } else if (instruction.opcode == IROpcode::Cast) result = a;
    else if (instruction.opcode == IROpcode::Unary) {
        if (op == "-") result = -a;
        else if (op == "!") result = a == 0;
        else if (op == "~") result = static_cast<std::int64_t>(~static_cast<std::uint64_t>(a) & 0xffffu);
        else return false;
    } else if (instruction.opcode == IROpcode::Binary) {
        // Converted scalar inputs are at most 16 bits. Their products/shifts
        // fit int64_t, including signed minimum / -1 before modular wrapping.
        if (op == "+") result = a + b;
        else if (op == "-") result = a - b;
        else if (op == "*") result = a * b;
        else if (op == "/" || op == "%") {
            if (b == 0) return false; // Keep the original runtime fail-stop.
            result = op == "/" ? a / b : a % b;
        } else if (op == "&" || op == "|" || op == "^") {
            const auto x = static_cast<std::uint64_t>(a), y = static_cast<std::uint64_t>(b);
            result = static_cast<std::int64_t>((op == "&" ? x & y : op == "|" ? x | y : x ^ y) & 0xffffu);
        } else if (op == "<<" || op == ">>") {
            if (b < 0 || b > 15) return false;
            const auto divisor = std::int64_t{1} << static_cast<unsigned>(b);
            if (op == "<<") result = static_cast<std::int64_t>((static_cast<std::uint64_t>(a) & 0xffffu) * static_cast<std::uint64_t>(divisor));
            else result = a >= 0 ? a / divisor : -((-a + divisor - 1) / divisor);
        } else if (op == "==") result = a == b;
        else if (op == "!=") result = a != b;
        else if (op == "<") result = a < b;
        else if (op == "<=") result = a <= b;
        else if (op == ">") result = a > b;
        else if (op == ">=") result = a >= b;
        else return false;
    } else return false;
    result = ConstantEvaluator::convert(result, instruction.type);
    return true;
}
}
