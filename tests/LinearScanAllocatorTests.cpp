#include "LinearScanAllocator.hpp"

#include <iostream>
#include <cstdlib>

namespace {

IRInstruction constant(std::uint32_t result) {
    IRInstruction instruction;
    instruction.opcode = IROpcode::Constant;
    instruction.result = IRValueId{result};
    return instruction;
}

IRInstruction binary(std::uint32_t result, std::uint32_t left, std::uint32_t right) {
    IRInstruction instruction;
    instruction.opcode = IROpcode::Binary;
    instruction.result = IRValueId{result};
    instruction.operands = {IRValueId{left}, IRValueId{right}};
    return instruction;
}

} // namespace

int main() {
    const auto require = [](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "linear scan test failure: " << message << '\n';
            std::exit(1);
        }
    };
    IRFunction function;
    function.blocks.push_back({IRBlockId{0}, "entry", {
        constant(1),
        constant(2),
        binary(3, 1, 2),
        binary(4, 3, 1),
    }});

    LinearScanAllocator allocator;
    allocator.run(function, {5, 7});

    const auto* first = allocator.find(IRValueId{1});
    const auto* second = allocator.find(IRValueId{2});
    const auto* third = allocator.find(IRValueId{3});
    const auto* fourth = allocator.find(IRValueId{4});
    require(first != nullptr && second != nullptr && third != nullptr && fourth != nullptr, "all values have locations");
    require(first->has_register && first->physical_register == 5, "first value uses R5");
    require(second->has_register && second->physical_register == 7, "second value uses R7");
    require(!third->has_register, "third value spills");
    require(fourth->has_register && fourth->physical_register == 7, "fourth value reuses R7");
    require(second->end < fourth->start, "intervals are ordered");
    return 0;
}
