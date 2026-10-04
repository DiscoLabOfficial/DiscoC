#pragma once
#include "AST.hpp"

// Operates on semantically typed expressions. Never executes loads, calls,
// stores, or hardware operations. All arithmetic is bounded and modular.
class ConstantEvaluator {
public:
    static bool evaluate(const Expr& expression, std::int64_t& value);
    static std::int64_t convert(std::int64_t value, const Type& type);
};
