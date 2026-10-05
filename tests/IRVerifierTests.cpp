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

void testInvalidBinaryOperation() {
    auto function = baseFunction();
    function.value_count = 3;
    IRInstruction add;
    add.opcode = IROpcode::Binary;
    add.type = wordType();
    add.result = IRValueId{3};
    add.operands = {IRValueId{1}, IRValueId{2}};
    add.operation = "**";
    function.blocks[0].instructions = {constant(1), constant(2), add, returnValue(3)};
    expectFailure("invalid binary operation", IRModule{{std::move(function)}},
                  "binary instruction has invalid operands");
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

void testPointerRepresentation() {
    auto function = baseFunction();
    auto address = constant(1);
    address.type.pointer_level = 1;
    address.type.is_far = true;
    address.type.space = AddressSpace::RAM;
    function.value_count = 1;
    function.blocks[0].instructions = {address, returnValue(1)};
    expectFailure("far width", IRModule{{function}}, "invalid pointer representation");
    address.type.sizeInBytes = 4;
    address.immediate = 0x721000;
    function.blocks[0].instructions[0] = address;
    expectFailure("far domain", IRModule{{function}}, "invalid pointer constant");
    address.type.pointer_level = MaxPointerDepth + 1;
    function.blocks[0].instructions[0] = address;
    expectFailure("pointer depth", IRModule{{function}}, "invalid pointer representation");
}

void testPointerOffset() {
    auto function = baseFunction();
    auto pointer = constant(1, 0x1000);
    pointer.type = pointerTo(wordType(), AddressSpace::RAM);
    IRInstruction offset;
    offset.opcode = IROpcode::PointerOffset;
    offset.type = pointer.type;
    offset.result = IRValueId{3};
    offset.operands = {IRValueId{1}, IRValueId{2}};
    offset.operation = "+";
    offset.immediate = 1; // word* must scale by two, not one.
    function.return_type = pointer.type;
    function.value_count = 3;
    function.blocks[0].instructions = {pointer, constant(2), offset, returnValue(3)};
    expectFailure("pointer stride", IRModule{{function}}, "stride does not match");
    function.blocks[0].instructions[2].immediate = 2;
    expectSuccess("typed pointer offset", IRModule{{function}});
}

void testAccessQualifiers() {
    auto function = baseFunction();
    function.value_count = 2;
    auto address = constant(1, 0x100);
    Type object = wordType();
    object.is_volatile = true;
    address.type = pointerTo(object, AddressSpace::RAM);
    IRInstruction load;
    load.opcode = IROpcode::LoadIndirect;
    load.result = IRValueId{2};
    load.type = wordType();
    load.operands = {IRValueId{1}};
    function.blocks[0].instructions = {address, load, returnValue(2)};
    expectFailure("missing volatile load effect", IRModule{{function}}, "volatile load metadata");
    function.blocks[0].instructions[1].memory_volatile = true;
    expectSuccess("volatile load effect", IRModule{{function}});
    function.blocks[0].instructions[1].type.base = BaseType::BYTE;
    function.blocks[0].instructions[1].type.sizeInBytes = 1;
    expectFailure("incorrect load width", IRModule{{function}}, "load.indirect requires");

    object.is_const = true;
    address.type = pointerTo(object, AddressSpace::RAM);
    IRInstruction store;
    store.opcode = IROpcode::StoreIndirect;
    store.type = wordType();
    store.memory_volatile = true;
    store.operands = {IRValueId{1}, IRValueId{2}};
    function.blocks[0].instructions = {address, constant(2), store, returnValue(2)};
    expectFailure("const store", IRModule{{function}}, "const-qualified address");
    function.blocks[0].instructions[2].operation = "declare";
    expectSuccess("const volatile initialization", IRModule{{function}});
    function.blocks[0].instructions[1].memory_volatile = true;
    expectFailure("volatile on arithmetic value", IRModule{{function}}, "non-memory instruction");
}

void testTargetCapabilities() {
    auto function = baseFunction();
    function.value_count = 1;
    function.blocks[0].instructions = {constant(1), returnValue(1)};
    IRModule module{{function}, TargetKind::SPC700};
    expectSuccess("common SPC700 IR", module);

    module.functions[0].is_cached = true;
    expectFailure("cached SPC700 function", module, "instruction-cache capability");
    module.functions[0].is_cached = false;
    IRInstruction cache;
    cache.opcode = IROpcode::Cache;
    module.functions[0].blocks[0].instructions.insert(
        module.functions[0].blocks[0].instructions.begin(), cache);
    expectFailure("SPC700 cache instruction", module, "instruction-cache capability");
    module.target = TargetKind::GSU;
    expectSuccess("GSU cache instruction", module);
    cache.operands = {IRValueId{1}};
    module.functions[0].blocks[0].instructions = {constant(1), cache, returnValue(1)};
    expectFailure("cache with operands", module, "cache has no operands or targets");

    auto coordinate = constant(1);
    coordinate.opcode = IROpcode::PlotCoordinateRead;
    coordinate.immediate = 0;
    coordinate.in_plot_context = true;
    function.blocks[0].instructions = {coordinate, returnValue(1)};
    module = IRModule{{function}, TargetKind::GSU};
    expectSuccess("GSU plot coordinate", module);
    module.target = TargetKind::SPC700;
    expectFailure("SPC700 plot coordinate", module, "graphics capability");
    module.target = TargetKind::GSU;
    module.functions[0].blocks[0].instructions[0].in_plot_context = false;
    expectFailure("coordinate outside plot", module, "invalid plot coordinate access");
    module.functions[0].blocks[0].instructions[0].in_plot_context = true;
    module.functions[0].blocks[0].instructions[0].immediate = 2;
    expectFailure("invalid coordinate selector", module, "invalid plot coordinate access");

    IRInstruction hardware;
    hardware.opcode = IROpcode::HardwareLoopEnd;
    module = IRModule{{function}, TargetKind::SPC700};
    module.functions[0].blocks[0].instructions[0] = hardware;
    expectFailure("SPC700 hardware loop", module, "hardware-loops capability");

    Type far_pointer = pointerTo(wordType(), AddressSpace::RAM);
    far_pointer.is_far = true;
    far_pointer.pointer_reach[0] = true;
    far_pointer.sizeInBytes = 4;
    function.return_type = far_pointer;
    auto pointer = constant(1, 0x701000);
    pointer.type = far_pointer;
    function.blocks[0].instructions = {pointer, returnValue(1)};
    module = IRModule{{function}, TargetKind::GSU};
    expectSuccess("GSU far-data IR", module);
    module.target = TargetKind::SPC700;
    expectFailure("SPC700 far-data IR", module, "far-data capability");
}

void testGraphicsEffects() {
    auto function = baseFunction(voidType());
    IRInstruction pixel; pixel.opcode = IROpcode::Plot; pixel.in_plot_context = true;
    IRInstruction flush; flush.opcode = IROpcode::Rpix; flush.type = voidType();
    IRInstruction end; end.opcode = IROpcode::ReturnVoid;
    function.blocks[0].instructions = {pixel, flush, end};
    IRModule module{{function}};
    expectSuccess("stateful pixel and discarded rpix", module);
    const auto effects = pixel.hardwareEffects(), read = flush.hardwareEffects();
    if (effects.reads_registers != 6 || effects.writes_registers != 2 || !effects.reads_color ||
        !effects.reads_por || !effects.writes_framebuffer || !effects.pixel_cache ||
        !read.observable() || read.reads_registers != 6 || read.writes_registers != 0 ||
        !read.reads_framebuffer || !read.writes_framebuffer || !read.pixel_cache)
        throw std::runtime_error("Incorrect PLOT/RPIX effects");
    module.functions[0].blocks[0].instructions[0].in_plot_context = false;
    expectFailure("pixel outside context", module, "pixel requires");
    function.blocks[0].instructions = {constant(1), pixel, end}; function.value_count = 1;
    function.blocks[0].instructions[1].operands = {IRValueId{1}};
    expectFailure("pixel with explicit coordinates", IRModule{{function}}, "pixel requires");
    auto rpix = flush; rpix.type = Type{BaseType::BYTE, "", 1, false}; rpix.result = IRValueId{1}; rpix.in_plot_context = true;
    function.blocks[0].instructions = {rpix, end};
    expectSuccess("value-producing rpix", IRModule{{function}});
    rpix.in_plot_context = false; function.blocks[0].instructions = {rpix, end};
    expectFailure("read_pixel outside context", IRModule{{function}}, "rpix must");
    IRInstruction color; color.opcode = IROpcode::SetColor; color.in_plot_context = true;
    color.operation = "rom.byte"; color.operands = {IRValueId{1}};
    auto address = constant(1, 0x100); address.type = pointerTo(Type{BaseType::BYTE, "", 1, false}, AddressSpace::RAM);
    function.blocks[0].instructions = {address, color, end};
    expectFailure("RAM cannot use GETC color path", IRModule{{function}}, "direct ROM color");
    Type volatile_byte{BaseType::BYTE, "", 1, false}; volatile_byte.is_volatile = true;
    address.type = pointerTo(volatile_byte, AddressSpace::ROM);
    function.blocks[0].instructions = {address, color, end};
    expectFailure("volatile ROM keeps an ordinary load", IRModule{{function}}, "direct ROM color");
    const auto rom_effect = color.hardwareEffects();
    if (!rom_effect.rom_buffer || !rom_effect.writes_color || !rom_effect.reads_color ||
        !rom_effect.reads_por || rom_effect.writes_registers != (1u << 14))
        throw std::runtime_error("GETC must use ROM buffer and update COLR under POR");
    color.operation.clear();
    const auto value_effect = color.hardwareEffects();
    if (!value_effect.writes_color || !value_effect.reads_color || !value_effect.reads_por ||
        value_effect.rom_buffer || value_effect.writes_registers != 0)
        throw std::runtime_error("COLOR must update COLR without a ROM-buffer dependency");
    auto cursor = constant(1);
    cursor.opcode = IROpcode::PlotCoordinateRead; cursor.in_plot_context = true;
    cursor.targets = {IRBlockId{0}};
    function.blocks[0].instructions = {cursor, end};
    expectFailure("cursor access is not a control transfer", IRModule{{function}}, "invalid plot coordinate access");
    cursor.targets.clear(); cursor.operation = "legacy";
    function.blocks[0].instructions = {cursor, end};
    expectFailure("cursor access has no hidden operation", IRModule{{function}}, "invalid plot coordinate access");
    IRInstruction mode; mode.opcode = IROpcode::CMode; mode.in_plot_context = true; mode.immediate = 32;
    function.value_count = 0; function.blocks[0].instructions = {mode, end};
    expectFailure("invalid CMODE mask", IRModule{{function}}, "five-bit POR mask");
    mode.immediate = 1; mode.targets = {IRBlockId{0}};
    function.blocks[0].instructions = {mode, end};
    expectFailure("CMODE is not a control transfer", IRModule{{function}}, "five-bit POR mask");
}

} // namespace

int main() {
    try {
        testGraphicsEffects();
        testValidFunction();
        testDuplicateDefinition();
        testUseBeforeDefinition();
        testNonDominatingDefinition();
        testReturnType();
        testIndirectLoadRequiresPointer();
        testInvalidBinaryOperation();
        testSyntheticUnreachableBlock();
        testPointerRepresentation();
        testPointerOffset();
        testAccessQualifiers();
        testTargetCapabilities();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
