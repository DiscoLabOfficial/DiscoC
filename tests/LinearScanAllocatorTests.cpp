#include "LinearScanAllocator.hpp"

#include <iostream>
#include <cstdlib>
#include <stdexcept>

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
    instruction.operation = "+";
    return instruction;
}

IRInstruction load(std::uint32_t result, std::uint32_t address) {
    IRInstruction instruction;
    instruction.opcode = IROpcode::LoadIndirect;
    instruction.result = IRValueId{result};
    instruction.operands = {IRValueId{address}};
    return instruction;
}

IRInstruction branch(std::uint32_t target) {
    IRInstruction instruction;
    instruction.opcode = IROpcode::Branch;
    instruction.targets = {IRBlockId{target}};
    return instruction;
}

IRInstruction ret(std::uint32_t value) {
    IRInstruction instruction;
    instruction.opcode = IROpcode::Return;
    instruction.operands = {IRValueId{value}};
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

    IRFunction reordered;
    reordered.entry = IRBlockId{0};
    reordered.blocks.push_back({IRBlockId{0}, "entry", {
        branch(2)
    }});
    reordered.blocks.push_back({IRBlockId{1}, "use", {
        binary(2, 1, 1),
        ret(2)
    }});
    reordered.blocks.push_back({IRBlockId{2}, "definition", {
        constant(1),
        branch(1)
    }});
    reordered.value_count = 2;
    allocator.run(reordered, {5});
    require(allocator.find(IRValueId{1}) != nullptr, "RPO handles definitions in later physical blocks");

    IRFunction observable;
    observable.blocks.push_back({IRBlockId{0}, "entry", {
        constant(1),
        load(2, 1),
        binary(3, 2, 1),
        binary(4, 2, 1),
        ret(4)
    }});
    observable.entry = IRBlockId{0};
    observable.value_count = 4;
    bool rejected_observable_spill = false;
    try {
        allocator.run(observable, {});
    } catch (const std::runtime_error&) {
        rejected_observable_spill = true;
    }
    require(rejected_observable_spill, "multi-use loads are not silently rematerialized");
    IRInstruction pixel; pixel.opcode = IROpcode::Plot; pixel.in_plot_context = true;
    IRFunction graphics;
    graphics.entry = IRBlockId{0}; graphics.value_count = 1;
    graphics.blocks.push_back({IRBlockId{0}, "entry", {constant(1), pixel, ret(1)}});
    allocator.run(graphics, {1, 2, 5});
    require(allocator.find(IRValueId{1})->physical_register == 5, "PLOT reserves R1/R2, including its automatic X update");
    auto rpix = constant(1); rpix.opcode = IROpcode::Rpix; rpix.type = Type{BaseType::BYTE, "", 1, false};
    graphics.blocks[0].instructions = {rpix, ret(1)};
    bool rejected_pixel_rematerialization = false;
    try { allocator.run(graphics, {}); } catch (const std::runtime_error&) { rejected_pixel_rematerialization = true; }
    require(rejected_pixel_rematerialization, "Single-use RPIX is not movable/rematerializable");
    return 0;
}
