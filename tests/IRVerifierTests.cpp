#include "IR.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

Type wordType() {
    Type type;
    type.base = BaseType::WORD;
    type.sizeInBytes = 2;
    return type;
}

Type voidType() {
    Type type;
    type.base = BaseType::VOID;
    return type;
}

IRBasicBlock block(std::uint32_t id) {
    return IRBasicBlock{IRBlockId{id}, "b" + std::to_string(id), {}};
}

IRInstruction constant(std::uint32_t value, std::int64_t immediate = 1) {
    IRInstruction instruction;
    instruction.opcode = IROpcode::Constant;
    instruction.type = wordType();
    instruction.result = IRValueId{value};
    instruction.immediate = immediate;
    return instruction;
}

IRInstruction returnValue(std::uint32_t value) {
    IRInstruction instruction;
    instruction.opcode = IROpcode::Return;
    instruction.operands = {IRValueId{value}};
    return instruction;
}

IRFunction baseFunction(Type returnType = wordType()) {
    IRFunction function;
    function.name = "test";
    function.return_type = returnType;
    function.entry = IRBlockId{0};
    function.blocks.push_back(block(0));
    return function;
}

void expectFailure(const std::string& name, const IRModule& module,
                  const std::string& expectedMessage) {
    try {
        IRVerifier::verify(module);
    } catch (const CompilerError& error) {
        if (error.getMessage().find(expectedMessage) == std::string::npos) {
            throw std::runtime_error(name + " produced the wrong diagnostic: " +
                                     error.getMessage());
        }
        return;
    }
    throw std::runtime_error(name + " was accepted by the verifier");
}

void expectSuccess(const std::string& name, const IRModule& module) {
    try {
        IRVerifier::verify(module);
    } catch (const CompilerError& error) {
        throw std::runtime_error(name + " was rejected: " + error.getMessage());
    }
}

void testValidFunction() {
    auto function = baseFunction();
    function.value_count = 1;
    function.blocks[0].instructions = {constant(1), returnValue(1)};
    expectSuccess("valid function", IRModule{{std::move(function)}});
}

void testDuplicateDefinition() {
    auto function = baseFunction();
    function.value_count = 1;
    function.blocks[0].instructions = {constant(1), constant(1), returnValue(1)};
    expectFailure("duplicate definition", IRModule{{std::move(function)}},
                  "defined more than once");
}

void testUseBeforeDefinition() {
    auto function = baseFunction();
    function.value_count = 2;
    IRInstruction add;
    add.opcode = IROpcode::Binary;
    add.type = wordType();
    add.result = IRValueId{2};
    add.operands = {IRValueId{1}, IRValueId{1}};
    add.operation = "+";
    function.blocks[0].instructions = {add, constant(1), returnValue(2)};
    expectFailure("use before definition", IRModule{{std::move(function)}},
                  "used before its definition");
}

void testNonDominatingDefinition() {
    auto function = baseFunction();
    function.value_count = 2;
    auto thenBlock = block(1);
    auto elseBlock = block(2);

    IRInstruction branch;
    branch.opcode = IROpcode::CondBranch;
    branch.operands = {IRValueId{1}};
    branch.targets = {IRBlockId{1}, IRBlockId{2}};

    thenBlock.instructions = {constant(2), returnValue(2)};
    elseBlock.instructions = {returnValue(2)};
    function.blocks[0].instructions = {constant(1), branch};
    function.blocks.push_back(std::move(thenBlock));
    function.blocks.push_back(std::move(elseBlock));
    expectFailure("non-dominating definition", IRModule{{std::move(function)}},
                  "does not dominate its use");
}

void testReturnType() {
    auto function = baseFunction(voidType());
    function.value_count = 1;
    function.blocks[0].instructions = {constant(1), returnValue(1)};
    expectFailure("return type", IRModule{{std::move(function)}},
                  "return value does not match");
}

void testIndirectLoadRequiresPointer() {
    auto function = baseFunction();
    function.value_count = 2;
    IRInstruction load;
    load.opcode = IROpcode::LoadIndirect;
    load.type = wordType();
    load.result = IRValueId{2};
    load.operands = {IRValueId{1}};
    function.blocks[0].instructions = {constant(1), load, returnValue(2)};
    expectFailure("indirect load", IRModule{{std::move(function)}},
                  "load.indirect requires one pointer operand");
}

void testSyntheticUnreachableBlock() {
    auto function = baseFunction();
    function.value_count = 1;
    function.blocks[0].instructions = {constant(1), returnValue(1)};
    auto unreachable = block(1);
    IRInstruction marker;
    marker.opcode = IROpcode::Unreachable;
    unreachable.instructions.push_back(marker);
    function.blocks.push_back(std::move(unreachable));
    expectSuccess("synthetic unreachable block", IRModule{{std::move(function)}});
}

} // namespace

int main() {
    try {
        testValidFunction();
        testDuplicateDefinition();
        testUseBeforeDefinition();
        testNonDominatingDefinition();
        testReturnType();
        testIndirectLoadRequiresPointer();
        testSyntheticUnreachableBlock();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
