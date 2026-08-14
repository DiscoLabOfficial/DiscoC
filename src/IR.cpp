#include "IR.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <queue>
#include <set>
#include <sstream>
#include <iterator>
#include <utility>

namespace {

constexpr std::size_t MaxIRBlocksPerFunction = 1'000'000;
constexpr std::uint32_t MaxIRValuesPerFunction = 1'000'000;

bool isValueOpcode(IROpcode opcode) {
    switch (opcode) {
        case IROpcode::Constant:
        case IROpcode::Address:
        case IROpcode::Load:
        case IROpcode::LoadIndirect:
        case IROpcode::Binary:
        case IROpcode::Unary:
        case IROpcode::Cast:
        case IROpcode::Call:
            return true;
        default:
            return false;
    }
}

bool isTerminatorOpcode(IROpcode opcode) {
    switch (opcode) {
        case IROpcode::Branch:
        case IROpcode::CondBranch:
        case IROpcode::Switch:
        case IROpcode::Return:
        case IROpcode::ReturnVoid:
        case IROpcode::Unreachable:
            return true;
        default:
            return false;
    }
}

std::string opcodeName(IROpcode opcode) {
    switch (opcode) {
        case IROpcode::Constant: return "const";
        case IROpcode::Address: return "address";
        case IROpcode::Load: return "load";
        case IROpcode::LoadIndirect: return "load.indirect";
        case IROpcode::Store: return "store";
        case IROpcode::StoreIndirect: return "store.indirect";
        case IROpcode::Binary: return "binary";
        case IROpcode::Unary: return "unary";
        case IROpcode::Cast: return "cast";
        case IROpcode::Call: return "call";
        case IROpcode::PlotBegin: return "plot.begin";
        case IROpcode::PlotEnd: return "plot.end";
        case IROpcode::Plot: return "plot";
        case IROpcode::SetColor: return "setcolor";
        case IROpcode::CMode: return "cmode";
        case IROpcode::Rpix: return "rpix";
        case IROpcode::HardwareLoop: return "hardware_loop";
        case IROpcode::HardwareLoopEnd: return "hardware_loop.end";
        case IROpcode::Branch: return "br";
        case IROpcode::CondBranch: return "condbr";
        case IROpcode::Switch: return "switch";
        case IROpcode::Return: return "return";
        case IROpcode::ReturnVoid: return "return.void";
        case IROpcode::Unreachable: return "unreachable";
    }
    return "unknown";
}

std::string typeName(const Type& type) {
    const std::string name = to_string(type);
    return name.empty() ? "none" : name;
}

void verifyTarget(const IRFunction& function, IRBlockId target, const Token& source) {
    if (!target.isValid() || target.value >= function.blocks.size()) {
        throw CompilerError("IR verifier: branch target is outside the function.",
                            source.line_number, source.col_number);
    }
}

bool isVoidType(const Type& type) {
    return type.base == BaseType::VOID;
}

bool isIntegerType(const Type& type) {
    return type.pointer_level == 0 &&
           (type.base == BaseType::BYTE || type.base == BaseType::WORD);
}

bool isLegalBinaryOperation(const std::string& operation) {
    return operation == "+" || operation == "-" || operation == "*" ||
           operation == "/" || operation == ">" || operation == "<" ||
           operation == ">=" || operation == "<=" || operation == "==" ||
           operation == "!=";
}

bool sameValueType(const Type& left, const Type& right) {
    return left.base == right.base &&
           left.structName == right.structName &&
           left.pointer_level == right.pointer_level &&
           (left.pointer_level == 0 || left.is_far == right.is_far);
}

} // namespace

bool IRInstruction::isTerminator() const {
    return isTerminatorOpcode(opcode);
}

bool IRInstruction::producesValue() const {
    return isValueOpcode(opcode) &&
           !(opcode == IROpcode::Call && type.base == BaseType::VOID);
}

void IRVerifier::fail(const std::string& message, const Token& source) {
    throw CompilerError(message, source.line_number, source.col_number);
}

void IRVerifier::verify(const IRModule& module) {
    for (const auto& function : module.functions) {
        verifyFunction(function);
    }
}

void IRVerifier::verifyFunction(const IRFunction& function) {
    if (function.name.empty()) {
        fail("IR verifier: function has no name.", Token(TokenType::UNKNOWN, "", 0, 0));
    }
    if (function.blocks.empty()) {
        fail("IR verifier: function has no basic blocks.", Token(TokenType::UNKNOWN, "", 0, 0));
    }
    if (!function.entry.isValid() || function.entry.value >= function.blocks.size()) {
        fail("IR verifier: function entry block is invalid.", Token(TokenType::UNKNOWN, "", 0, 0));
    }

    if (function.entry.value != 0) {
        fail("IR verifier: function entry block must be block zero.",
             Token(TokenType::UNKNOWN, "", 0, 0));
    }

    struct Definition {
        std::size_t block = 0;
        std::size_t instruction = 0;
        Token source = {TokenType::UNKNOWN, "", 0, 0};
    };

    std::map<std::uint32_t, Definition> definitions;
    std::vector<std::vector<std::size_t>> successors(function.blocks.size());
    std::vector<std::vector<std::size_t>> predecessors(function.blocks.size());

    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        const auto& block = function.blocks[index];
        if (!block.id.isValid() || block.id.value != index) {
            fail("IR verifier: basic-block IDs are not stable or contiguous.",
                 Token(TokenType::UNKNOWN, "", 0, 0));
        }
        if (block.instructions.empty()) {
            fail("IR verifier: basic block has no terminator.",
                 Token(TokenType::UNKNOWN, "", 0, 0));
        }

        for (std::size_t instruction_index = 0;
             instruction_index < block.instructions.size(); ++instruction_index) {
            const auto& instruction = block.instructions[instruction_index];
            if (instruction.isTerminator() &&
                instruction_index + 1 != block.instructions.size()) {
                fail("IR verifier: instruction appears after a terminator.", instruction.source);
            }
            if (instruction.producesValue()) {
                if (!instruction.result.isValid() ||
                    instruction.result.value > function.value_count) {
                    fail("IR verifier: value-producing instruction has an invalid result.",
                         instruction.source);
                }
                if (!definitions.emplace(instruction.result.value,
                                         Definition{index, instruction_index, instruction.source}).second) {
                    fail("IR verifier: value is defined more than once.", instruction.source);
                }
            } else if (instruction.result.isValid()) {
                fail("IR verifier: non-value instruction unexpectedly has a result.",
                     instruction.source);
            }
            for (const auto operand : instruction.operands) {
                if (!operand.isValid() || operand.value > function.value_count) {
                    fail("IR verifier: instruction uses an invalid value.", instruction.source);
                }
            }

            switch (instruction.opcode) {
                case IROpcode::Branch:
                    if (!instruction.operands.empty() || instruction.targets.size() != 1) {
                        fail("IR verifier: branch must have one target and no operands.", instruction.source);
                    }
                    break;
                case IROpcode::CondBranch:
                    if (instruction.operands.size() != 1 || instruction.targets.size() != 2) {
                        fail("IR verifier: conditional branch must have one condition and two targets.", instruction.source);
                    }
                    break;
                case IROpcode::Switch:
                    if (instruction.operands.size() != 1 || instruction.targets.empty()) {
                        fail("IR verifier: switch must have one condition and at least one target.", instruction.source);
                    }
                    if (instruction.case_values.size() +
                            (instruction.has_default_target ? 1u : 0u) !=
                        instruction.targets.size()) {
                        fail("IR verifier: switch case values and targets do not match.", instruction.source);
                    }
                    for (std::size_t case_index = 0;
                         case_index < instruction.case_values.size(); ++case_index) {
                        for (std::size_t previous = 0; previous < case_index; ++previous) {
                            if (instruction.case_values[case_index] == instruction.case_values[previous]) {
                                fail("IR verifier: switch contains duplicate case values.",
                                     instruction.source);
                            }
                        }
                    }
                    break;
                case IROpcode::Return:
                    if (instruction.operands.size() != 1) {
                        fail("IR verifier: return must have one value.", instruction.source);
                    }
                    break;
                case IROpcode::ReturnVoid:
                case IROpcode::Unreachable:
                    if (!instruction.operands.empty() || !instruction.targets.empty()) {
                        fail("IR verifier: void return/unreachable cannot have operands or targets.",
                             instruction.source);
                    }
                    break;
                default:
                    break;
            }
            for (const auto target : instruction.targets) {
                verifyTarget(function, target, instruction.source);
                successors[index].push_back(target.value);
                predecessors[target.value].push_back(index);
            }
        }

        if (!block.instructions.back().isTerminator()) {
            fail("IR verifier: basic block does not end in a terminator.",
                 block.instructions.back().source);
        }
    }

    if (definitions.size() != function.value_count) {
        fail("IR verifier: value IDs must form a contiguous definition sequence.",
             Token(TokenType::UNKNOWN, "", 0, 0));
    }
    for (std::uint32_t value = 1; value <= function.value_count; ++value) {
        if (definitions.find(value) == definitions.end()) {
            fail("IR verifier: value ID has no definition.",
                 Token(TokenType::UNKNOWN, "", 0, 0));
        }
    }

    std::vector<bool> reachable(function.blocks.size(), false);
    std::queue<std::size_t> worklist;
    reachable[function.entry.value] = true;
    worklist.push(function.entry.value);
    while (!worklist.empty()) {
        const auto block = worklist.front();
        worklist.pop();
        for (const auto successor : successors[block]) {
            if (!reachable[successor]) {
                reachable[successor] = true;
                worklist.push(successor);
            }
        }
    }

    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        if (!reachable[index]) {
            const auto& block = function.blocks[index];
            if (block.instructions.size() != 1 ||
                block.instructions.front().opcode != IROpcode::Unreachable) {
                fail("IR verifier: unreachable blocks must contain only 'unreachable'.",
                     block.instructions.front().source);
            }
        }
    }

    const std::size_t dominator_words =
        (function.blocks.size() + (sizeof(std::uint64_t) * 8u - 1u)) /
        (sizeof(std::uint64_t) * 8u);
    std::vector<std::vector<std::uint64_t>> dominators(
        function.blocks.size(), std::vector<std::uint64_t>(dominator_words, 0));
    const auto set_dominator = [&](std::vector<std::uint64_t>& bits, std::size_t index) {
        bits[index / 64u] |= std::uint64_t{1} << (index % 64u);
    };
    const auto has_dominator = [&](const std::vector<std::uint64_t>& bits, std::size_t index) {
        return (bits[index / 64u] & (std::uint64_t{1} << (index % 64u))) != 0;
    };
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        if (!reachable[index]) continue;
        if (index == function.entry.value) {
            set_dominator(dominators[index], index);
        } else {
            for (std::size_t candidate = 0; candidate < function.blocks.size(); ++candidate) {
                if (reachable[candidate]) set_dominator(dominators[index], candidate);
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t index = 0; index < function.blocks.size(); ++index) {
            if (!reachable[index] || index == function.entry.value) continue;

            std::vector<std::uint64_t> intersection(dominator_words, 0);
            bool has_predecessor = false;
            for (const auto predecessor : predecessors[index]) {
                if (!reachable[predecessor]) continue;
                if (!has_predecessor) {
                    intersection = dominators[predecessor];
                    has_predecessor = true;
                } else {
                    for (std::size_t word = 0; word < dominator_words; ++word) {
                        intersection[word] &= dominators[predecessor][word];
                    }
                }
            }
            set_dominator(intersection, index);
            if (intersection != dominators[index]) {
                dominators[index] = std::move(intersection);
                changed = true;
            }
        }
    }

    auto definitionType = [&](IRValueId value, const Token& source) -> const Type& {
        const auto definition = definitions.find(value.value);
        if (definition == definitions.end()) {
            fail("IR verifier: operand refers to an undefined value.", source);
        }
        return function.blocks[definition->second.block]
            .instructions[definition->second.instruction].type;
    };

    for (std::size_t block_index = 0; block_index < function.blocks.size(); ++block_index) {
        const auto& block = function.blocks[block_index];
        for (std::size_t instruction_index = 0;
             instruction_index < block.instructions.size(); ++instruction_index) {
            const auto& instruction = block.instructions[instruction_index];

            for (const auto operand : instruction.operands) {
                const auto definition = definitions.at(operand.value);
                if (definition.block == block_index) {
                    if (definition.instruction >= instruction_index) {
                        fail("IR verifier: value is used before its definition.", instruction.source);
                    }
                } else if (reachable[block_index] &&
                           (!reachable[definition.block] ||
                            !has_dominator(dominators[block_index], definition.block))) {
                    fail("IR verifier: value definition does not dominate its use.",
                         instruction.source);
                }
            }

            switch (instruction.opcode) {
                case IROpcode::LoadIndirect:
                    if (instruction.operands.size() != 1 ||
                        definitionType(instruction.operands.front(), instruction.source).pointer_level == 0) {
                        fail("IR verifier: load.indirect requires one pointer operand.", instruction.source);
                    }
                    break;
                case IROpcode::StoreIndirect:
                    if (instruction.operands.size() != 2 ||
                        definitionType(instruction.operands.front(), instruction.source).pointer_level == 0 ||
                        isVoidType(definitionType(instruction.operands.back(), instruction.source)) ||
                        !sameValueType(instruction.type,
                                       definitionType(instruction.operands.back(), instruction.source))) {
                        fail("IR verifier: store.indirect has incompatible operands.", instruction.source);
                    }
                    break;
                case IROpcode::Binary:
                    if (instruction.operands.size() != 2 || instruction.operation.empty() ||
                        isVoidType(definitionType(instruction.operands[0], instruction.source)) ||
                        isVoidType(definitionType(instruction.operands[1], instruction.source)) ||
                        !isLegalBinaryOperation(instruction.operation) ||
                        !isIntegerType(definitionType(instruction.operands[0], instruction.source)) ||
                        !isIntegerType(definitionType(instruction.operands[1], instruction.source)) ||
                        !sameValueType(definitionType(instruction.operands[0], instruction.source),
                                       definitionType(instruction.operands[1], instruction.source)) ||
                        !isIntegerType(instruction.type)) {
                        fail("IR verifier: binary instruction has invalid operands.", instruction.source);
                    }
                    break;
                case IROpcode::Unary:
                case IROpcode::Cast:
                    if (instruction.operands.size() != 1 ||
                        isVoidType(definitionType(instruction.operands.front(), instruction.source))) {
                        fail("IR verifier: unary/cast instruction has an invalid operand.", instruction.source);
                    }
                    break;
                case IROpcode::Call:
                    if (instruction.symbol.empty()) {
                        fail("IR verifier: call has no target symbol.", instruction.source);
                    }
                    for (const auto operand : instruction.operands) {
                        if (isVoidType(definitionType(operand, instruction.source))) {
                            fail("IR verifier: call cannot pass a void value.", instruction.source);
                        }
                    }
                    break;
                case IROpcode::CondBranch:
                    if (instruction.operands.size() != 1 || instruction.targets.size() != 2 ||
                        !isIntegerType(definitionType(instruction.operands.front(), instruction.source))) {
                        fail("IR verifier: conditional branch requires an integer condition.", instruction.source);
                    }
                    break;
                case IROpcode::Switch:
                    if (instruction.operands.size() != 1 ||
                        !isIntegerType(definitionType(instruction.operands.front(), instruction.source))) {
                        fail("IR verifier: switch requires an integer condition.", instruction.source);
                    }
                    break;
                case IROpcode::Return:
                    if (isVoidType(function.return_type) ||
                        !sameValueType(function.return_type,
                                       definitionType(instruction.operands.front(), instruction.source))) {
                        fail("IR verifier: return value does not match the function return type.",
                             instruction.source);
                    }
                    break;
                case IROpcode::ReturnVoid:
                    if (!isVoidType(function.return_type)) {
                        fail("IR verifier: non-void function must return a value.", instruction.source);
                    }
                    break;
                default:
                    break;
            }
        }
    }
}

IRFunction& IRLowerer::currentFunction() {
    if (m_module.functions.empty() || m_current_function >= m_module.functions.size()) {
        throw CompilerError("IR lowering: no current function.", 0, 0);
    }
    return m_module.functions.at(m_current_function);
}

const IRBasicBlock& IRLowerer::currentBlock() const {
    if (!m_current_block.isValid()) {
        throw CompilerError("IR lowering: no current basic block.", 0, 0);
    }
    return m_module.functions.at(m_current_function).blocks.at(m_current_block.value);
}

IRBasicBlock& IRLowerer::currentBlock() {
    if (!m_current_block.isValid()) {
        throw CompilerError("IR lowering: no current basic block.", 0, 0);
    }
    return m_module.functions.at(m_current_function).blocks.at(m_current_block.value);
}

IRBlockId IRLowerer::createBlock(const std::string& label) {
    auto& function = currentFunction();
    if (function.blocks.size() >= MaxIRBlocksPerFunction) {
        throw CompilerError("IR lowering: function has too many basic blocks.", 0, 0);
    }
    const auto id = IRBlockId{static_cast<std::uint32_t>(function.blocks.size())};
    function.blocks.push_back({id, label, {}});
    return id;
}

IRValueId IRLowerer::createValue() {
    auto& function = currentFunction();
    if (function.value_count >= MaxIRValuesPerFunction) {
        throw CompilerError("IR lowering: function has too many values.", 0, 0);
    }
    ++function.value_count;
    return IRValueId{function.value_count};
}

IRValueId IRLowerer::emitValue(IROpcode opcode, const Type& type, const Token& source,
                               const std::vector<IRValueId>& operands,
                               const std::string& operation,
                               const std::string& symbol,
                               std::int64_t immediate,
                               SymbolId symbol_id) {
    IRInstruction instruction;
    instruction.opcode = opcode;
    instruction.type = type;
    instruction.source = source;
    instruction.operands = operands;
    instruction.operation = operation;
    instruction.symbol = symbol;
    instruction.symbol_id = symbol_id;
    instruction.immediate = immediate;
    if (isValueOpcode(opcode) &&
        !(opcode == IROpcode::Call && type.base == BaseType::VOID)) {
        instruction.result = createValue();
    }
    const auto result = instruction.result;
    emitInstruction(std::move(instruction));
    return result;
}

void IRLowerer::emitInstruction(IRInstruction instruction) {
    if (isTerminated()) {
        throw CompilerError("IR lowering: cannot append instruction after a terminator.",
                            instruction.source.line_number, instruction.source.col_number);
    }
    currentBlock().instructions.push_back(std::move(instruction));
}

void IRLowerer::emitBranch(IRBlockId target, const Token& source) {
    IRInstruction instruction;
    instruction.opcode = IROpcode::Branch;
    instruction.targets.push_back(target);
    instruction.source = source;
    emitInstruction(std::move(instruction));
}

void IRLowerer::emitConditionalBranch(IRValueId condition, IRBlockId true_target,
                                      IRBlockId false_target, const Token& source) {
    IRInstruction instruction;
    instruction.opcode = IROpcode::CondBranch;
    instruction.operands.push_back(condition);
    instruction.targets.push_back(true_target);
    instruction.targets.push_back(false_target);
    instruction.source = source;
    emitInstruction(std::move(instruction));
}

bool IRLowerer::isTerminated() const {
    const auto& block = currentBlock();
    return !block.instructions.empty() && block.instructions.back().isTerminator();
}

void IRLowerer::requireFunction(const Token& source) const {
    if (m_module.functions.empty()) {
        throw CompilerError("IR lowering: statement is outside a function.",
                            source.line_number, source.col_number);
    }
}

IRValueId IRLowerer::lowerExpression(Expr& expr) {
    expr.accept(*this, nullptr);
    if (!m_last_value.isValid() && expr.result_type.base != BaseType::VOID) {
        throw CompilerError("IR lowering: expression did not produce a value.",
                            expr.token.line_number, expr.token.col_number);
    }
    return m_last_value;
}

IRValueId IRLowerer::lowerAddress(Expr& expr) {
    Type address_type = expr.result_type;
    address_type.pointer_level += 1;

    if (auto* variable = dynamic_cast<VariableExpr*>(&expr)) {
        return emitValue(IROpcode::Address, address_type, expr.token, {}, {},
                         variable->token.lexeme, 0, variable->symbol_id);
    }
    if (auto* dereference = dynamic_cast<DereferenceExpr*>(&expr)) {
        return lowerExpression(*dereference->right);
    }
    if (auto* subscript = dynamic_cast<SubscriptExpr*>(&expr)) {
        IRValueId base;
        if (dynamic_cast<VariableExpr*>(subscript->array.get()) ||
            dynamic_cast<MemberAccessExpr*>(subscript->array.get()) ||
            dynamic_cast<SubscriptExpr*>(subscript->array.get())) {
            base = lowerAddress(*subscript->array);
        } else {
            base = lowerExpression(*subscript->array);
        }
        const auto index = lowerExpression(*subscript->index);
        if (subscript->element_size <= 0) {
            throw CompilerError("IR lowering: subscript has no validated element stride.",
                                subscript->token.line_number, subscript->token.col_number);
        }
        const auto element_size = subscript->element_size;
        return emitValue(IROpcode::Address, address_type, expr.token, {base, index}, "index",
                         {}, element_size);
    }
    if (auto* member = dynamic_cast<MemberAccessExpr*>(&expr)) {
        const auto object = lowerAddress(*member->object);
        return emitValue(IROpcode::Address, address_type, expr.token, {object}, "member",
                         {}, member->member_offset);
    }

    throw CompilerError("IR lowering: expression is not an assignable address.",
                        expr.token.line_number, expr.token.col_number);
}

void IRLowerer::lowerStatementList(const std::vector<std::unique_ptr<Stmt>>& statements) {
    for (const auto& statement : statements) {
        if (isTerminated()) {
            break;
        }
        statement->accept(*this);
    }
}

IRModule IRLowerer::lower(const std::vector<std::unique_ptr<Stmt>>& program) {
    m_module = IRModule{};
    m_current_block = IRBlockId{};
    m_last_value = IRValueId{};
    m_break_targets.clear();

    for (const auto& statement : program) {
        if (auto* function = dynamic_cast<FunctionDeclStmt*>(statement.get())) {
            function->accept(*this);
        }
    }
    return std::move(m_module);
}

void IRLowerer::visit(LiteralExpr& expr, const Type*) {
    const auto value = std::stoll(expr.token.lexeme, nullptr, 0);
    m_last_value = emitValue(IROpcode::Constant, expr.result_type, expr.token, {}, {}, {}, value);
}

void IRLowerer::visit(VariableExpr& expr, const Type*) {
    const auto address = lowerAddress(expr);
    m_last_value = emitValue(IROpcode::LoadIndirect, expr.result_type, expr.token, {address});
}

void IRLowerer::visit(BinaryExpr& expr, const Type*) {
    const auto left = lowerExpression(*expr.left);
    const auto right = lowerExpression(*expr.right);
    m_last_value = emitValue(IROpcode::Binary, expr.result_type, expr.token, {left, right},
                             expr.token.lexeme);
}

void IRLowerer::visit(AssignExpr& expr, const Type*) {
    const auto address = lowerAddress(*expr.name);
    const auto value = lowerExpression(*expr.value);
    IRInstruction instruction;
    instruction.opcode = IROpcode::StoreIndirect;
    instruction.type = expr.result_type;
    instruction.operation = "assign";
    instruction.operands = {address, value};
    instruction.source = expr.token;
    emitInstruction(std::move(instruction));
    m_last_value = value;
}

void IRLowerer::visit(UnaryExpr& expr, const Type*) {
    const auto value = lowerExpression(*expr.right);
    m_last_value = emitValue(IROpcode::Unary, expr.result_type, expr.token, {value},
                             expr.token.lexeme);
}

void IRLowerer::visit(AddressOfExpr& expr, const Type*) {
    m_last_value = lowerAddress(*expr.right);
}

void IRLowerer::visit(DereferenceExpr& expr, const Type*) {
    const auto address = lowerExpression(*expr.right);
    IRInstruction instruction;
    instruction.opcode = IROpcode::LoadIndirect;
    instruction.type = expr.result_type;
    instruction.operands = {address};
    instruction.operation = expr.right->result_type.is_far ? "far" : "";
    instruction.source = expr.token;
    instruction.result = createValue();
    m_last_value = instruction.result;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(SubscriptExpr& expr, const Type*) {
    const auto address = lowerAddress(expr);
    m_last_value = emitValue(IROpcode::LoadIndirect, expr.result_type, expr.token, {address});
}

void IRLowerer::visit(MemberAccessExpr& expr, const Type*) {
    const auto address = lowerAddress(expr);
    m_last_value = emitValue(IROpcode::LoadIndirect, expr.result_type, expr.token, {address});
}

void IRLowerer::visit(CallExpr& expr, const Type*) {
    auto* callee = dynamic_cast<VariableExpr*>(expr.callee.get());
    if (!callee) {
        throw CompilerError("IR lowering: dynamic function calls are not supported.",
                            expr.token.line_number, expr.token.col_number);
    }
    std::vector<IRValueId> arguments;
    for (const auto& argument : expr.arguments) {
        arguments.push_back(lowerExpression(*argument));
    }
    m_last_value = emitValue(IROpcode::Call, expr.result_type, expr.token, arguments, {},
                             callee->token.lexeme);
}

void IRLowerer::visit(CastExpr& expr, const Type*) {
    const auto value = lowerExpression(*expr.expression);
    m_last_value = emitValue(IROpcode::Cast, expr.result_type, expr.token,
                             std::vector<IRValueId>{value});
}

void IRLowerer::visit(ReturnStmt& stmt) {
    requireFunction(stmt.token);
    if (stmt.value) {
        IRInstruction instruction;
        instruction.opcode = IROpcode::Return;
        instruction.operands.push_back(lowerExpression(*stmt.value));
        instruction.source = stmt.token;
        emitInstruction(std::move(instruction));
    } else {
        IRInstruction instruction;
        instruction.opcode = IROpcode::ReturnVoid;
        instruction.source = stmt.token;
        emitInstruction(std::move(instruction));
    }
}

void IRLowerer::visit(VarDeclStmt& stmt) {
    requireFunction(stmt.token);
    if (!stmt.initializer) {
        return;
    }
    VariableExpr variable(stmt.token);
    variable.result_type = stmt.type;
    variable.symbol_id = stmt.symbol_id;
    const auto address = lowerAddress(variable);
    const auto value = lowerExpression(*stmt.initializer);
    IRInstruction instruction;
    instruction.opcode = IROpcode::StoreIndirect;
    instruction.type = stmt.type;
    instruction.operation = "declare";
    instruction.operands = {address, value};
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(FunctionDeclStmt& stmt) {
    if (stmt.is_prototype) return;

    m_module.functions.push_back({});
    m_current_function = m_module.functions.size() - 1;
    auto& function = currentFunction();
    function.name = stmt.token.lexeme;
    function.return_type = stmt.returnType;
    function.parameters = stmt.params;
    function.total_local_alloc_size = stmt.total_local_alloc_size;
    function.is_cached = stmt.is_cached;
    function.needs_implicit_return = stmt.needs_implicit_return;
    m_current_block = createBlock("entry");
    function.entry = m_current_block;
    lowerStatementList(stmt.body);

    if (!isTerminated()) {
        if (stmt.returnType.base == BaseType::VOID) {
            IRInstruction instruction;
            instruction.opcode = IROpcode::ReturnVoid;
            instruction.source = stmt.token;
            emitInstruction(std::move(instruction));
        } else {
            throw CompilerError("IR lowering: non-void function reaches its end.",
                                stmt.token.line_number, stmt.token.col_number);
        }
    }
}

void IRLowerer::visit(IfStmt& stmt) {
    requireFunction(stmt.token);
    const auto condition = lowerExpression(*stmt.condition);
    const auto then_block = createBlock("if.then");
    const auto else_block = stmt.elseBranch ? createBlock("if.else") : IRBlockId{};
    const auto end_block = createBlock("if.end");
    emitConditionalBranch(condition, then_block, stmt.elseBranch ? else_block : end_block,
                          stmt.token);

    m_current_block = then_block;
    stmt.thenBranch->accept(*this);
    const bool then_terminated = isTerminated();
    if (!then_terminated) {
        emitBranch(end_block, stmt.token);
    }

    if (stmt.elseBranch) {
        m_current_block = else_block;
        stmt.elseBranch->accept(*this);
        const bool else_terminated = isTerminated();
        if (!else_terminated) {
            emitBranch(end_block, stmt.token);
        }
        m_current_block = end_block;
        if (then_terminated && else_terminated) {
            IRInstruction instruction;
            instruction.opcode = IROpcode::Unreachable;
            instruction.source = stmt.token;
            emitInstruction(std::move(instruction));
        }
    } else {
        m_current_block = end_block;
    }
}

void IRLowerer::visit(BlockStmt& stmt) {
    requireFunction(stmt.token);
    lowerStatementList(stmt.statements);
}

void IRLowerer::visit(WhileStmt& stmt) {
    requireFunction(stmt.token);
    const auto condition_block = createBlock("while.cond");
    const auto body_block = createBlock("while.body");
    const auto end_block = createBlock("while.end");
    emitBranch(condition_block, stmt.token);

    m_current_block = condition_block;
    const auto condition = lowerExpression(*stmt.condition);
    emitConditionalBranch(condition, body_block, end_block, stmt.token);

    m_break_targets.push_back(end_block);
    m_current_block = body_block;
    stmt.body->accept(*this);
    if (!isTerminated()) {
        emitBranch(condition_block, stmt.token);
    }
    m_break_targets.pop_back();
    m_current_block = end_block;
}

void IRLowerer::visit(ExpressionStmt& stmt) {
    requireFunction(stmt.token);
    lowerExpression(*stmt.expression);
}

void IRLowerer::visit(PlotStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::Plot;
    const auto x = lowerExpression(*stmt.x);
    const auto y = lowerExpression(*stmt.y);
    instruction.operands = {x, y};
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(PlotBeginStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::PlotBegin;
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(PlotEndStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::PlotEnd;
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(SetColorStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::SetColor;
    instruction.operands = {lowerExpression(*stmt.color_value)};
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(CmodeStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::CMode;
    instruction.operands = {lowerExpression(*stmt.options_value)};
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(RpixStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::Rpix;
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(HardwareLoopStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::HardwareLoop;
    instruction.operands = {lowerExpression(*stmt.count)};
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
    stmt.body->accept(*this);
    if (isTerminated()) {
        throw CompilerError("IR lowering: hardware loop body cannot terminate with return or branch.",
                            stmt.token.line_number, stmt.token.col_number);
    }
    IRInstruction end;
    end.opcode = IROpcode::HardwareLoopEnd;
    end.source = stmt.token;
    emitInstruction(std::move(end));
}

void IRLowerer::visit(StructDefStmt&) {}
void IRLowerer::visit(ConstDataStmt&) {}

void IRLowerer::lowerSwitchBody(SwitchStmt& stmt, IRValueId condition, IRBlockId end_block) {
    struct Arm {
        IRBlockId block;
        std::size_t statement_index;
        bool is_default;
        std::int64_t value;
    };

    std::vector<Arm> arms;
    for (std::size_t index = 0; index < stmt.body->statements.size(); ++index) {
        if (auto* case_stmt = dynamic_cast<CaseStmt*>(stmt.body->statements[index].get())) {
            const auto* literal = dynamic_cast<LiteralExpr*>(case_stmt->value.get());
            if (!literal) {
                throw CompilerError("IR lowering: switch cases must be integer literals.",
                                    case_stmt->token.line_number, case_stmt->token.col_number);
            }
            arms.push_back({createBlock("switch.case"), index, false,
                            std::stoll(literal->token.lexeme, nullptr, 0)});
        } else if (dynamic_cast<DefaultStmt*>(stmt.body->statements[index].get())) {
            arms.push_back({createBlock("switch.default"), index, true, 0});
        }
    }

    IRInstruction dispatch;
    dispatch.opcode = IROpcode::Switch;
    dispatch.operands.push_back(condition);
    dispatch.source = stmt.token;
    for (const auto& arm : arms) {
        if (!arm.is_default) {
            dispatch.targets.push_back(arm.block);
            dispatch.case_values.push_back(arm.value);
        }
    }
    for (const auto& arm : arms) {
        if (arm.is_default) {
            dispatch.targets.push_back(arm.block);
            dispatch.has_default_target = true;
            break;
        }
    }
    if (dispatch.targets.empty() || !dispatch.has_default_target) {
        dispatch.targets.push_back(end_block);
        dispatch.has_default_target = true;
    }
    emitInstruction(std::move(dispatch));

    m_break_targets.push_back(end_block);
    for (std::size_t arm_index = 0; arm_index < arms.size(); ++arm_index) {
        const auto& arm = arms[arm_index];
        m_current_block = arm.block;
        const std::size_t first_statement = arm.statement_index + 1;
        const std::size_t next_label = arm_index + 1 < arms.size()
            ? arms[arm_index + 1].statement_index
            : stmt.body->statements.size();
        for (std::size_t index = first_statement; index < next_label; ++index) {
            if (isTerminated()) {
                break;
            }
            stmt.body->statements[index]->accept(*this);
        }
        if (!isTerminated()) {
            const auto next = arm_index + 1 < arms.size() ? arms[arm_index + 1].block : end_block;
            emitBranch(next, stmt.token);
        }
    }
    m_break_targets.pop_back();
    m_current_block = end_block;
}

void IRLowerer::visit(SwitchStmt& stmt) {
    requireFunction(stmt.token);
    const auto condition = lowerExpression(*stmt.condition);
    const auto end_block = createBlock("switch.end");
    lowerSwitchBody(stmt, condition, end_block);
}

void IRLowerer::visit(CaseStmt&) {}
void IRLowerer::visit(DefaultStmt&) {}

void IRLowerer::visit(BreakStmt& stmt) {
    requireFunction(stmt.token);
    if (m_break_targets.empty()) {
        throw CompilerError("IR lowering: break is outside a loop or switch.",
                            stmt.token.line_number, stmt.token.col_number);
    }
    emitBranch(m_break_targets.back(), stmt.token);
}

std::string dumpIR(const IRModule& module) {
    std::ostringstream output;
    for (const auto& function : module.functions) {
        output << "function " << function.name << "() -> " << typeName(function.return_type)
               << " {\n";
        for (const auto& block : function.blocks) {
            output << "  " << block.label << " (b" << block.id.value << "):\n";
            for (const auto& instruction : block.instructions) {
                output << "    ";
                if (instruction.result.isValid()) {
                    output << "%" << instruction.result.value << " = ";
                }
                output << opcodeName(instruction.opcode);
                if (!instruction.operation.empty()) {
                    output << " " << instruction.operation;
                }
                if (!instruction.symbol.empty()) {
                    output << " @" << instruction.symbol;
                    if (instruction.symbol_id.isValid()) {
                        output << "#" << instruction.symbol_id.value;
                    }
                }
                if (instruction.opcode == IROpcode::Constant) {
                    output << " " << instruction.immediate;
                }
                if (!instruction.operands.empty()) {
                    output << " ";
                    for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                        if (index != 0) output << ", ";
                        output << "%" << instruction.operands[index].value;
                    }
                }
                if (!instruction.targets.empty()) {
                    output << " -> ";
                    for (std::size_t index = 0; index < instruction.targets.size(); ++index) {
                        if (index != 0) output << ", ";
                        output << "b" << instruction.targets[index].value;
                    }
                }
                output << "\n";
            }
        }
        output << "}\n";
    }
    return output.str();
}
