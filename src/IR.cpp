#include "IntegerLiteral.hpp"
#include "IR.hpp"
#include "GsuPointer.hpp"

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
        case IROpcode::Phi:
        case IROpcode::Rpix:
        case IROpcode::PlotCoordinateRead:
        case IROpcode::Constant:
        case IROpcode::Address:
        case IROpcode::PointerOffset:
        case IROpcode::PointerCompare:
        case IROpcode::Load:
        case IROpcode::LoadIndirect:
        case IROpcode::Binary:
        case IROpcode::DivMod:
        case IROpcode::DivModResult:
        case IROpcode::BitExtract:
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
        case IROpcode::Phi: return "phi";
        case IROpcode::Constant: return "const";
        case IROpcode::Address: return "address";
        case IROpcode::PointerOffset: return "pointer.offset";
        case IROpcode::PointerCompare: return "pointer.compare";
        case IROpcode::Load: return "load";
        case IROpcode::LoadIndirect: return "load.indirect";
        case IROpcode::Store: return "store";
        case IROpcode::StoreIndirect: return "store.indirect";
        case IROpcode::MemoryInitialize: return "memory.initialize";
        case IROpcode::Binary: return "binary";
        case IROpcode::DivMod: return "divmod";
        case IROpcode::DivModResult: return "divmod.result";
        case IROpcode::BitExtract: return "bit.extract";
        case IROpcode::Unary: return "unary";
        case IROpcode::Cast: return "cast";
        case IROpcode::Call: return "call";
        case IROpcode::PlotCoordinateRead: return "cursor.read";
        case IROpcode::PlotCoordinateWrite: return "cursor.write";
        case IROpcode::PlotBegin: return "plot.begin";
        case IROpcode::PlotEnd: return "plot.end";
        case IROpcode::Plot: return "pixel";
        case IROpcode::SetColor: return "color";
        case IROpcode::CMode: return "cmode";
        case IROpcode::Rpix: return "rpix";
        case IROpcode::Cache: return "cache";
        case IROpcode::HardwareLoop: return "hardware_loop";
        case IROpcode::HardwareLoopEnd: return "hardware_loop.end";
        case IROpcode::HardwareLoopLeave: return "hardware_loop.leave";
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
                            source);
    }
}

bool isVoidType(const Type& type) {
    return type.base == BaseType::VOID && type.pointer_level == 0;
}

bool isIntegerType(const Type& type) {
    return type.pointer_level == 0 &&
           (type.base == BaseType::BYTE || type.base == BaseType::WORD || type.base == BaseType::BOOL);
}

bool isLegalBinaryOperation(const std::string& operation) {
    return operation == "+" || operation == "-" || operation == "*" ||
           operation == "%" || operation == "&" || operation == "|" || operation == "^" ||
           operation == "<<" || operation == ">>" ||
           operation == "/" || operation == ">" || operation == "<" ||
           operation == ">=" || operation == "<=" || operation == "==" ||
           operation == "!=";
}

bool sameValueType(const Type& left, const Type& right) {
    return left.base == right.base &&
           left.structName == right.structName &&
           left.is_unsigned == right.is_unsigned &&
           left.pointer_level == right.pointer_level &&
           left.array_size == right.array_size &&
           (left.pointer_level == 0 ||
            (samePointerLayers(left, right) && left.space == right.space));
}

} // namespace

bool IRInstruction::isTerminator() const {
    return isTerminatorOpcode(opcode) ||
        ((opcode == IROpcode::HardwareLoop || opcode == IROpcode::HardwareLoopEnd) && !targets.empty());
}

bool IRInstruction::producesValue() const {
    return isValueOpcode(opcode) &&
           !((opcode == IROpcode::Call || opcode == IROpcode::Rpix) && isVoidValue(type));
}

void IRVerifier::fail(const std::string& message, const Token& source) {
    throw CompilerError(message, source);
}

void IRVerifier::verify(const IRModule& module) {
    try { module.bitmap.validate(); } catch (const std::exception& error) { fail(std::string("IR verifier: ") + error.what(), Token(TokenType::UNKNOWN, "", 1, 1)); }
    if (module.bitmap.enabled && !supportsCapability(module.target, TargetCapability::Graphics))
        fail("IR verifier: target lacks graphics capability for bitmap configuration.", Token(TokenType::UNKNOWN, "", 1, 1));
    const auto check_type = [&](const Type& type, const Token& source) {
        if ((isFarPointer(type) || std::find(type.pointer_reach.begin(), type.pointer_reach.end(), true) != type.pointer_reach.end()) &&
            !supportsCapability(module.target, TargetCapability::FarData))
            fail("IR verifier: target lacks far-data capability.", source);
    };
    for (const auto& function : module.functions) {
        check_type(function.return_type, Token(TokenType::UNKNOWN, function.name, 1, 1));
        for (const auto& parameter : function.parameters) check_type(parameter.type, parameter.name);
        if (function.is_cached && !supportsCapability(module.target, TargetCapability::InstructionCache))
            fail("IR verifier: target lacks instruction-cache capability.", Token(TokenType::UNKNOWN, "", 1, 1));
        for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
            check_type(instruction.type, instruction.source);
            const auto opcode = instruction.opcode;
            const bool graphics = instruction.in_plot_context || opcode == IROpcode::PlotCoordinateRead ||
                opcode == IROpcode::PlotCoordinateWrite || opcode == IROpcode::PlotBegin || opcode == IROpcode::PlotEnd ||
                opcode == IROpcode::Plot || opcode == IROpcode::SetColor || opcode == IROpcode::CMode || opcode == IROpcode::Rpix;
            if (graphics && !supportsCapability(module.target, TargetCapability::Graphics))
                fail("IR verifier: target lacks graphics capability.", instruction.source);
            if (opcode == IROpcode::Cache && !supportsCapability(module.target, TargetCapability::InstructionCache))
                fail("IR verifier: target lacks instruction-cache capability.", instruction.source);
            if ((opcode == IROpcode::HardwareLoop || opcode == IROpcode::HardwareLoopEnd || opcode == IROpcode::HardwareLoopLeave) &&
                !supportsCapability(module.target, TargetCapability::HardwareLoops))
                fail("IR verifier: target lacks hardware-loops capability.", instruction.source);
        }
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

    const auto verifyPointerType = [&](const Type& type, const Token& source, int maximum_depth) {
        if (type.pointer_level < 0 || type.pointer_level > maximum_depth ||
            (!type.pointer_reach.empty() && type.pointer_reach.size() != static_cast<std::size_t>(type.pointer_level)) ||
            (!type.pointer_spaces.empty() && type.pointer_spaces.size() != static_cast<std::size_t>(type.pointer_level)) ||
            (!type.pointee_qualifiers.empty() && type.pointee_qualifiers.size() != static_cast<std::size_t>(type.pointer_level)) ||
            (!type.pointer_reach.empty() && type.is_far != type.pointer_reach.back()) ||
            (!type.pointer_spaces.empty() && type.space != type.pointer_spaces.back()) ||
            (type.pointer_level > 0 && type.sizeInBytes != (isFarPointer(type) ? 4 : 2)))
            fail("IR verifier: invalid pointer representation or layer metadata.", source);
    };
    verifyPointerType(function.return_type, Token(TokenType::UNKNOWN, "", 0, 0), MaxPointerDepth);
    for (const auto& parameter : function.parameters)
        verifyPointerType(parameter.type, Token(TokenType::UNKNOWN, "", 0, 0), MaxPointerDepth);

    struct Definition {
        std::size_t block = 0;
        std::size_t instruction = 0;
        Token source = {TokenType::UNKNOWN, "", 0, 0};
    };

    std::map<std::uint32_t, Definition> definitions;
    std::vector<std::vector<std::size_t>> successors(function.blocks.size());
    std::vector<std::vector<std::size_t>> predecessors(function.blocks.size());
    struct HardwareScope {
        const IRInstruction* setup = nullptr;
        const IRInstruction* end = nullptr;
        const IRInstruction* leave = nullptr;
        std::size_t setup_block = 0, end_block = 0, leave_block = 0;
    };
    // Borrowed only during this read-only verification; no IR is mutated.
    std::map<std::uint32_t, HardwareScope> hardware_scopes;

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
            const auto& type = instruction.type;
            if (!instruction.initialization_values.empty() && instruction.opcode != IROpcode::MemoryInitialize)
                fail("IR verifier: initializer values belong to memory.initialize only.", instruction.source);
            if (instruction.compiler_generated_loop && instruction.opcode != IROpcode::HardwareLoop)
                fail("IR verifier: generated-loop metadata belongs to setup only.", instruction.source);
            if (instruction.loop_id && instruction.opcode != IROpcode::HardwareLoop &&
                instruction.opcode != IROpcode::HardwareLoopEnd && instruction.opcode != IROpcode::HardwareLoopLeave)
                fail("IR verifier: loop ID belongs to hardware-loop operations only.", instruction.source);
            if (instruction.loop_target.isValid()) {
                if (instruction.opcode != IROpcode::HardwareLoop || instruction.targets.size() != 1)
                    fail("IR verifier: loop target belongs to explicit hardware-loop setup only.", instruction.source);
                verifyTarget(function, instruction.loop_target, instruction.source);
            }
            if (instruction.opcode == IROpcode::Phi) {
                if (instruction_index != 0 && block.instructions[instruction_index - 1].opcode != IROpcode::Phi)
                    fail("IR verifier: phi instructions must precede ordinary instructions.", instruction.source);
                if (instruction.operands.empty() || instruction.operands.size() != instruction.targets.size() ||
                    type.array_size != 0 || (!isIntegerType(type) && !(type.pointer_level > 0 && !isFarPointer(type))) || !instruction.operation.empty())
                    fail("IR verifier: invalid phi incoming values.", instruction.source);
            }
            if (instruction.memory_volatile && instruction.opcode != IROpcode::LoadIndirect &&
                instruction.opcode != IROpcode::StoreIndirect)
                fail("IR verifier: volatile metadata on a non-memory instruction.", instruction.source);
            if (instruction.operation == "null" && instruction.opcode == IROpcode::Constant &&
                (instruction.immediate != 0 || type.pointer_level == 0 || !instruction.operands.empty()))
                fail("IR verifier: invalid null pointer constant.", instruction.source);
            verifyPointerType(type, instruction.source,
                instruction.opcode == IROpcode::Address ? MaxAddressPointerDepth : MaxPointerDepth);
            if (instruction.opcode == IROpcode::Constant && type.pointer_level > 0 && instruction.operation != "null") {
                try {
                    if (instruction.immediate < 0 || instruction.immediate > (isFarPointer(type) ? 0xffffff : 0xffff))
                        throw std::runtime_error("Pointer address exceeds its representation.");
                    const auto element = pointeeType(type);
                    GsuPointer::validate(static_cast<std::uint32_t>(instruction.immediate), type.space, isFarPointer(type),
                                          element.sizeInBytes > 0 ? element.sizeInBytes : 1, storageAlignment(element));
                } catch (const std::exception& error) {
                    fail(std::string("IR verifier: invalid pointer constant: ") + error.what(), instruction.source);
                }
            }
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
                case IROpcode::HardwareLoop:
                    if (instruction.operands.size() != 1 || instruction.targets.size() > 1 ||
                        (!instruction.targets.empty() && !instruction.loop_target.isValid()) ||
                        !instruction.loop_id || !instruction.operation.empty())
                        fail("IR verifier: invalid hardware-loop setup.", instruction.source);
                    if (hardware_scopes[instruction.loop_id].setup)
                        fail("IR verifier: duplicate hardware-loop setup ID.", instruction.source);
                    hardware_scopes[instruction.loop_id].setup = &instruction;
                    hardware_scopes[instruction.loop_id].setup_block = index;
                    break;
                case IROpcode::HardwareLoopEnd:
                    if (!instruction.operands.empty() || (!instruction.targets.empty() && instruction.targets.size() != 2) ||
                        !instruction.loop_id || !instruction.operation.empty())
                        fail("IR verifier: hardware-loop end requires backedge and exit.", instruction.source);
                    if (hardware_scopes[instruction.loop_id].end)
                        fail("IR verifier: duplicate hardware-loop end ID.", instruction.source);
                    hardware_scopes[instruction.loop_id].end = &instruction;
                    hardware_scopes[instruction.loop_id].end_block = index;
                    break;
                case IROpcode::HardwareLoopLeave:
                    if (!instruction.operands.empty() || !instruction.targets.empty() ||
                        !instruction.loop_id || !instruction.operation.empty())
                        fail("IR verifier: invalid hardware-loop leave.", instruction.source);
                    if (hardware_scopes[instruction.loop_id].leave)
                        fail("IR verifier: duplicate hardware-loop leave ID.", instruction.source);
                    hardware_scopes[instruction.loop_id].leave = &instruction;
                    hardware_scopes[instruction.loop_id].leave_block = index;
                    break;
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
                if (instruction.opcode != IROpcode::Phi) {
                    successors[index].push_back(target.value);
                    predecessors[target.value].push_back(index);
                }
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

    for (const auto& item : hardware_scopes) {
        const auto& scope = item.second;
        if (!scope.setup || !scope.end || scope.setup->targets.empty() != scope.end->targets.empty() ||
            (scope.setup->targets.empty() ? scope.leave != nullptr : scope.leave == nullptr))
            fail("IR verifier: unmatched hardware-loop setup/end/leave.", Token(TokenType::UNKNOWN, "", 0, 0));
        if (!scope.setup->targets.empty() &&
            (scope.setup->loop_target.value != scope.end->targets[0].value ||
             !has_dominator(dominators[scope.end_block], scope.setup_block) ||
             !has_dominator(dominators[scope.leave_block], scope.end_block)))
            fail("IR verifier: hardware-loop backedge or exit does not match its setup.", scope.setup->source);
    }
    if (!hardware_scopes.empty()) {
        // Every join must agree on the exact active R12/R13 save stack. This
        // catches bypassed restores, incorrectly nested loops and backedges
        // into setup, even when ordinary value dominance would accept them.
        std::vector<std::vector<std::uint32_t>> scopes(function.blocks.size());
        std::vector<bool> assigned(function.blocks.size(), false);
        std::queue<std::size_t> pending;
        assigned[function.entry.value] = true; pending.push(function.entry.value);
        std::size_t entries = 0;
        while (!pending.empty()) {
            const auto b = pending.front(); pending.pop();
            auto active = scopes[b];
            for (const auto& instruction : function.blocks[b].instructions) {
                if (instruction.opcode == IROpcode::HardwareLoop) {
                    if (active.size() >= 256)
                        fail("IR verifier: hardware-loop nesting limit exceeded.", instruction.source);
                    active.push_back(instruction.loop_id);
                } else if (instruction.opcode == IROpcode::HardwareLoopEnd || instruction.opcode == IROpcode::HardwareLoopLeave) {
                    if (active.empty() || active.back() != instruction.loop_id)
                        fail("IR verifier: mismatched hardware-loop scope stack.", instruction.source);
                    if (instruction.opcode == IROpcode::HardwareLoopLeave || instruction.targets.empty()) active.pop_back();
                }
            }
            if (successors[b].empty() && !active.empty())
                fail("IR verifier: function exits with an active hardware-loop scope.", function.blocks[b].instructions.back().source);
            for (const auto s : successors[b]) {
                if (assigned[s]) {
                    if (scopes[s] != active)
                        fail("IR verifier: inconsistent hardware-loop scopes at CFG join.", function.blocks[s].instructions.front().source);
                } else {
                    entries += active.size();
                    if (entries > 1000000)
                        fail("IR verifier: hardware-loop scope analysis limit exceeded.", function.blocks[b].instructions.back().source);
                    scopes[s] = active; assigned[s] = true; pending.push(s);
                }
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

            if (instruction.opcode == IROpcode::Phi) {
                std::set<std::size_t> incoming;
                const std::set<std::size_t> expected(predecessors[block_index].begin(), predecessors[block_index].end());
                for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
                    const auto edge = instruction.targets[i].value;
                    const auto definition = definitions.at(instruction.operands[i].value);
                    if (!incoming.insert(edge).second || !expected.count(edge) || !reachable[edge] ||
                        !has_dominator(dominators[edge], definition.block) ||
                        !sameValueType(instruction.type, definitionType(instruction.operands[i], instruction.source)) ||
                        instruction.type.enum_name != definitionType(instruction.operands[i], instruction.source).enum_name)
                        fail("IR verifier: phi value must match its type and dominate its predecessor edge.", instruction.source);
                }
                if (incoming != expected || block_index == function.entry.value)
                    fail("IR verifier: phi must cover every predecessor exactly once.", instruction.source);
            }
            for (const auto operand : instruction.operands) {
                if (instruction.opcode == IROpcode::Phi) continue;
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

            if (instruction.is_live_range_split &&
                (instruction.opcode != IROpcode::Cast || instruction.operands.size() != 1 ||
                 !isIntegerType(instruction.type) || instruction.type.array_size != 0 || !instruction.operation.empty() ||
                 !sameValueType(instruction.type, definitionType(instruction.operands.front(), instruction.source)) ||
                 instruction.type.enum_name != definitionType(instruction.operands.front(), instruction.source).enum_name))
                fail("IR verifier: live-range split requires a representation-identical scalar copy.", instruction.source);

            switch (instruction.opcode) {
                case IROpcode::MemoryInitialize: {
                    const auto& type = instruction.type;
                    if (instruction.operands.size() != 1 || instruction.initialization_values.size() < 4 ||
                        instruction.initialization_values.size() > 256 || !instruction.targets.empty() ||
                        !isIntegerType(type) || (type.sizeInBytes != 1 && type.sizeInBytes != 2) || type.array_size || type.is_volatile || instruction.memory_volatile ||
                        instruction.operation != "declare" || instruction.immediate <= 0 ||
                        instruction.immediate > function.total_local_alloc_size ||
                        instruction.initialization_values.size() > static_cast<std::size_t>(instruction.immediate / type.sizeInBytes))
                        fail("IR verifier: invalid bounded memory initializer.", instruction.source);
                    const auto& origin = definitions.at(instruction.operands[0].value);
                    const auto& address = function.blocks.at(origin.block).instructions.at(origin.instruction);
                    if (address.opcode != IROpcode::Address || !address.operation.empty() || !address.symbol_id.isValid() ||
                        address.type.pointer_level != 1 || isFarPointer(address.type) || address.type.space != AddressSpace::RAM ||
                        !sameValueType(type, pointeeType(address.type)) || pointeeType(address.type).is_volatile)
                        fail("IR verifier: memory initializer requires a local near RAM address.", instruction.source);
                    for (const auto value : instruction.initialization_values)
                        if ((usesByteStorage(type) && value > 255) || (type.base == BaseType::BOOL && value > 1))
                            fail("IR verifier: initializer value exceeds its storage width.", instruction.source);
                    break;
                }
                case IROpcode::Cache:
                    if (!instruction.operands.empty() || !instruction.targets.empty()) fail("IR verifier: cache has no operands or targets.", instruction.source);
                    break;
                case IROpcode::Plot:
                    if (!instruction.in_plot_context || !instruction.operands.empty() || !instruction.targets.empty())
                        fail("IR verifier: pixel requires a plot context and no explicit coordinates.", instruction.source);
                    break;
                case IROpcode::Rpix:
                    if (!instruction.operands.empty() || !instruction.targets.empty() ||
                        (instruction.producesValue() && (!instruction.in_plot_context || instruction.type.pointer_level != 0 || instruction.type.base != BaseType::BYTE)))
                        fail("IR verifier: rpix must discard its result or return a byte in a plot context.", instruction.source);
                    break;
                case IROpcode::SetColor:
                    if (!instruction.in_plot_context || instruction.operands.size() != 1 || !instruction.targets.empty())
                        fail("IR verifier: color requires one source and a plot context.", instruction.source);
                    if (instruction.operation == "rom.byte") {
                        const auto& address = definitionType(instruction.operands[0], instruction.source);
                        if (address.pointer_level == 0 || address.space != AddressSpace::ROM || pointeeType(address).base != BaseType::BYTE || pointeeType(address).pointer_level != 0 || pointeeType(address).is_volatile)
                            fail("IR verifier: direct ROM color must address a ROM byte.", instruction.source);
                    } else if (!instruction.operation.empty() || !isIntegerType(definitionType(instruction.operands[0], instruction.source)))
                        fail("IR verifier: color requires an integer value or direct ROM byte.", instruction.source);
                    break;
                case IROpcode::CMode:
                    if (!instruction.in_plot_context || !instruction.operands.empty() || !instruction.targets.empty() || instruction.immediate < 0 || instruction.immediate > 31)
                        fail("IR verifier: cmode requires a constant five-bit POR mask in a plot context.", instruction.source);
                    break;
                case IROpcode::PlotCoordinateRead:
                case IROpcode::PlotCoordinateWrite:
                    if (!instruction.in_plot_context || !instruction.targets.empty() || !instruction.operation.empty() ||
                        instruction.immediate < 0 || instruction.immediate > 1 ||
                        instruction.type.pointer_level != 0 || instruction.type.base != BaseType::WORD ||
                        instruction.type.is_unsigned || instruction.operands.size() !=
                            (instruction.opcode == IROpcode::PlotCoordinateRead ? 0u : 1u) ||
                        (instruction.opcode == IROpcode::PlotCoordinateWrite &&
                         !sameValueType(instruction.type, definitionType(instruction.operands.front(), instruction.source))))
                        fail("IR verifier: invalid plot coordinate access.", instruction.source);
                    break;
                case IROpcode::Address:
                    if (instruction.type.pointer_level == 0 ||
                        (instruction.operation.empty() &&
                         (!instruction.operands.empty() || instruction.symbol.empty())) ||
                        (instruction.operation == "member" &&
                         (instruction.operands.size() != 1 || instruction.immediate < 0 ||
                          instruction.immediate > 65528 ||
                          definitionType(instruction.operands[0], instruction.source).pointer_level == 0)) ||
                        (instruction.operation == "temporary" && (!instruction.operands.empty() || instruction.immediate >= 0 ||
                            instruction.immediate < -function.total_local_alloc_size || instruction.immediate % 2 != 0)) ||
                        (!instruction.operation.empty() && instruction.operation != "member" && instruction.operation != "temporary"))
                        fail("IR verifier: address has an invalid operation or operands.", instruction.source);
                    break;
                case IROpcode::PointerCompare:
                    if (instruction.operands.size() != 2 || instruction.type.base != BaseType::BOOL || instruction.type.pointer_level != 0 ||
                        (instruction.operation != "==" && instruction.operation != "!=") ||
                        definitionType(instruction.operands[0], instruction.source).pointer_level == 0 ||
                        definitionType(instruction.operands[1], instruction.source).pointer_level == 0 ||
                        definitionType(instruction.operands[0], instruction.source).space != definitionType(instruction.operands[1], instruction.source).space ||
                        isFarPointer(definitionType(instruction.operands[0], instruction.source)) != isFarPointer(definitionType(instruction.operands[1], instruction.source)))
                        fail("IR verifier: incompatible pointer comparison.", instruction.source);
                    break;
                case IROpcode::PointerOffset:
                    if (instruction.operands.size() != ((instruction.operation == "scaled+" || instruction.operation == "scaled-") ? 3u : 2u) || instruction.immediate <= 0 ||
                        instruction.immediate > 65528 ||
                        definitionType(instruction.operands[0], instruction.source).pointer_level == 0 ||
                        !sameValueType(instruction.type, definitionType(instruction.operands[0], instruction.source)) ||
                        !isIntegerType(definitionType(instruction.operands[1], instruction.source)) ||
                        (instruction.operation != "+" && instruction.operation != "-" && instruction.operation != "scaled+" && instruction.operation != "scaled-"))
                        fail("IR verifier: pointer.offset has incompatible operands or stride.", instruction.source);
                    if (instruction.operands.size() == 3 &&
                        (isFarPointer(instruction.type) || instruction.immediate < 4 ||
                         definitionType(instruction.operands[1], instruction.source).base != BaseType::WORD ||
                         !definitionType(instruction.operands[1], instruction.source).is_unsigned ||
                         !sameValueType(definitionType(instruction.operands[1], instruction.source), definitionType(instruction.operands[2], instruction.source))))
                        fail("IR verifier: scaled offset requires matching unsigned words and a near address.", instruction.source);
                    if ((instruction.type.base != BaseType::STRUCT || instruction.type.pointer_level > 1) &&
                        instruction.immediate != pointeeType(instruction.type).sizeInBytes)
                        fail("IR verifier: pointer.offset stride does not match the pointee.", instruction.source);
                    break;
                case IROpcode::LoadIndirect:
                    if (instruction.operands.size() != 1 ||
                        definitionType(instruction.operands.front(), instruction.source).pointer_level == 0 ||
                        !sameValueType(instruction.type, pointeeType(definitionType(instruction.operands.front(), instruction.source)))) {
                        fail("IR verifier: load.indirect requires one pointer operand.", instruction.source);
                    }
                    if (instruction.memory_volatile != pointeeType(definitionType(instruction.operands.front(), instruction.source)).is_volatile)
                        fail("IR verifier: volatile load metadata does not match its address.", instruction.source);
                    break;
                case IROpcode::StoreIndirect:
                    if (instruction.operands.size() != 2 ||
                        definitionType(instruction.operands.front(), instruction.source).pointer_level == 0 ||
                        isVoidType(definitionType(instruction.operands.back(), instruction.source)) ||
                        !sameValueType(instruction.type,
                                       definitionType(instruction.operands.back(), instruction.source)) ||
                        !sameValueType(instruction.type, pointeeType(definitionType(instruction.operands.front(), instruction.source))) ||
                        definitionType(instruction.operands.front(), instruction.source).space == AddressSpace::ROM) {
                        fail("IR verifier: store.indirect has incompatible operands.", instruction.source);
                    }
                    if (instruction.memory_volatile != pointeeType(definitionType(instruction.operands.front(), instruction.source)).is_volatile)
                        fail("IR verifier: volatile store metadata does not match its address.", instruction.source);
                    if (pointeeType(definitionType(instruction.operands.front(), instruction.source)).is_const && instruction.operation != "declare")
                        fail("IR verifier: store through a const-qualified address.", instruction.source);
                    break;
                case IROpcode::DivMod:
                    if (instruction.operands.size() != 2 || !instruction.targets.empty() ||
                        (instruction.operation != "/" && instruction.operation != "%") ||
                        instruction.type.base != BaseType::WORD || instruction.type.sizeInBytes != 2 ||
                        instruction.type.pointer_level != 0 || instruction.type.array_size != 0 ||
                        !sameValueType(instruction.type, definitionType(instruction.operands[0], instruction.source)) ||
                        !sameValueType(instruction.type, definitionType(instruction.operands[1], instruction.source)) ||
                        instruction.type.enum_name != definitionType(instruction.operands[0], instruction.source).enum_name ||
                        instruction.type.enum_name != definitionType(instruction.operands[1], instruction.source).enum_name)
                        fail("IR verifier: divmod requires two matching words and a primary component.", instruction.source);
                    break;
                case IROpcode::DivModResult: {
                    if (instruction.operands.size() != 1 || !instruction.targets.empty() ||
                        (instruction.operation != "/" && instruction.operation != "%"))
                        fail("IR verifier: divmod.result requires one pair and the other component.", instruction.source);
                    const auto& origin = definitions.at(instruction.operands[0].value);
                    const auto& pair = function.blocks.at(origin.block).instructions.at(origin.instruction);
                    if (pair.opcode != IROpcode::DivMod || pair.operation == instruction.operation ||
                        !sameValueType(instruction.type, pair.type) || instruction.type.enum_name != pair.type.enum_name)
                        fail("IR verifier: divmod.result must select the matching pair's other component.", instruction.source);
                    break;
                }
                case IROpcode::Binary:
                    if (instruction.operands.size() != 2 || instruction.operation.empty() ||
                        isVoidType(definitionType(instruction.operands[0], instruction.source)) ||
                        isVoidType(definitionType(instruction.operands[1], instruction.source)) ||
                        !isLegalBinaryOperation(instruction.operation) ||
                        !isIntegerType(definitionType(instruction.operands[0], instruction.source)) ||
                        !isIntegerType(definitionType(instruction.operands[1], instruction.source)) ||
                        ((instruction.operation != "<<" && instruction.operation != ">>") &&
                         !sameValueType(definitionType(instruction.operands[0], instruction.source),
                                       definitionType(instruction.operands[1], instruction.source))) ||
                        !isIntegerType(instruction.type)) {
                        fail("IR verifier: binary instruction has invalid operands.", instruction.source);
                    }
                    break;
                case IROpcode::BitExtract:
                    if (instruction.operands.size() != 1 || instruction.immediate < 0 || instruction.immediate > 15 ||
                        !instruction.operation.empty() || !instruction.targets.empty() || instruction.memory_volatile ||
                        instruction.type.pointer_level != 0 ||
                        instruction.type.array_size != 0 || instruction.type.base != BaseType::WORD ||
                        !sameValueType(instruction.type, definitionType(instruction.operands.front(), instruction.source))) {
                        fail("IR verifier: bit.extract requires one matching word and a bit in 0..15.", instruction.source);
                    }
                    break;
                case IROpcode::HardwareLoop:
                    if (!isIntegerType(definitionType(instruction.operands.front(), instruction.source)))
                        fail("IR verifier: hardware-loop count must be an integer.", instruction.source);
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
        !(opcode == IROpcode::Call && isVoidValue(type))) {
        instruction.result = createValue();
    }
    const auto result = instruction.result;
    emitInstruction(std::move(instruction));
    return result;
}

void IRLowerer::emitInstruction(IRInstruction instruction) {
    instruction.in_plot_context = m_plot_context;
    if (isTerminated()) {
        throw CompilerError("IR lowering: cannot append instruction after a terminator.",
                            instruction.source);
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
                            source);
    }
}

IRValueId IRLowerer::lowerExpression(Expr& expr) {
    expr.accept(*this, nullptr);
    if (!m_last_value.isValid() && !isVoidValue(expr.result_type)) {
        throw CompilerError("IR lowering: expression did not produce a value.",
                            expr.token);
    }
    return m_last_value;
}

IRValueId IRLowerer::lowerAddress(Expr& expr) {
    Type address_type = expr.address_type.pointer_level > 0 ? expr.address_type :
        pointerTo(expr.result_type, expr.result_type.space);

    if (auto* variable = dynamic_cast<VariableExpr*>(&expr)) {
        if (variable->is_array_decay) address_type = expr.result_type;
        else if (expr.address_type.pointer_level == 0)
            address_type = pointerTo(expr.result_type, variable->symbol_id.isValid() ? AddressSpace::RAM : expr.result_type.space);
        return emitValue(IROpcode::Address, address_type, expr.token, {}, {},
                         variable->token.lexeme, 0, variable->symbol_id);
    }
    if (auto* dereference = dynamic_cast<DereferenceExpr*>(&expr)) {
        return lowerExpression(*dereference->right);
    }
    if (auto* subscript = dynamic_cast<SubscriptExpr*>(&expr)) {
        IRValueId base;
        if (subscript->array->result_type.array_size > 0) {
            base = lowerAddress(*subscript->array);
        } else {
            base = lowerExpression(*subscript->array);
        }
        const auto index = lowerExpression(*subscript->index);
        if (subscript->element_size <= 0) {
            throw CompilerError("IR lowering: subscript has no validated element stride.",
                                subscript->token);
        }
        const auto element_size = subscript->element_size;
        return emitValue(IROpcode::PointerOffset, address_type, expr.token, {base, index}, "+",
                         {}, element_size);
    }
    if (auto* member = dynamic_cast<MemberAccessExpr*>(&expr)) {
        const auto object = lowerAddress(*member->object);
        return emitValue(IROpcode::Address, address_type, expr.token, {object}, "member",
                         {}, member->member_offset);
    }

    throw CompilerError("IR lowering: expression is not an assignable address.",
                        expr.token);
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
    m_next_hardware_loop = 0;
    m_module.target = m_target;
    m_current_block = IRBlockId{};
    m_last_value = IRValueId{};
    m_break_targets.clear();
    m_continue_targets.clear();

    for (const auto& statement : program) {
        if (auto* function = dynamic_cast<FunctionDeclStmt*>(statement.get())) {
            function->accept(*this);
        }
    }
    return std::move(m_module);
}

void IRLowerer::visit(LiteralExpr& expr, const Type*) {
    const auto value = expr.token.type == TokenType::KEYWORD_TRUE ? 1 :
        expr.token.type == TokenType::KEYWORD_FALSE ? 0 : DiscoNumeric::parse(expr.token.lexeme, nullptr, 0);
    m_last_value = emitValue(IROpcode::Constant, expr.result_type, expr.token, {}, {}, {}, value);
}

void IRLowerer::visit(PlotCoordinateExpr& expr, const Type*) {
    m_last_value = emitValue(IROpcode::PlotCoordinateRead, expr.result_type, expr.token, {}, {}, {}, expr.is_y ? 1 : 0);
}

void IRLowerer::visit(ReadPixelExpr& expr, const Type*) {
    if (expr.x) {
        IRInstruction x; x.opcode = IROpcode::PlotCoordinateWrite; x.type = Type{BaseType::WORD, "", 2, false};
        x.operands = {lowerExpression(*expr.x)}; x.source = expr.token; emitInstruction(std::move(x));
        IRInstruction y; y.opcode = IROpcode::PlotCoordinateWrite; y.type = Type{BaseType::WORD, "", 2, false};
        y.immediate = 1; y.operands = {lowerExpression(*expr.y)}; y.source = expr.token; emitInstruction(std::move(y));
    }
    m_last_value = emitValue(IROpcode::Rpix, expr.result_type, expr.token);
}
void IRLowerer::visit(BitmapDeclStmt&) {}
void IRLowerer::visit(UseBitmapStmt& stmt) { m_module.bitmap = stmt.config; }

void IRLowerer::visit(InitializerListExpr&, const Type*) { throw CompilerError("Unresolved initializer list reached IR lowering.", 1, 1); }
void IRLowerer::visit(StringExpr&, const Type*) { throw CompilerError("Unresolved string storage reached IR lowering.", 1, 1); }
void IRLowerer::visit(NullExpr& expr, const Type*) {
    m_last_value = emitValue(IROpcode::Constant, expr.result_type, expr.token, {}, "null", {}, 0);
}
void IRLowerer::visit(UpdateExpr& expr, const Type*) {
    const auto* coordinate = dynamic_cast<const PlotCoordinateExpr*>(expr.target.get());
    IRValueId address;
    if (!coordinate) address = lowerAddress(*expr.target);
    const auto old_value = coordinate ? lowerExpression(*expr.target) :
        emitValue(IROpcode::LoadIndirect, expr.target->result_type, expr.token, {address});
    if (!coordinate) currentBlock().instructions.back().memory_volatile = expr.target->result_type.is_volatile;
    auto operand = old_value;
    if (!sameValueType(expr.target->result_type, expr.operation_type))
        operand = emitValue(IROpcode::Cast, expr.operation_type, expr.token, {operand});
    const auto rhs = lowerExpression(*expr.value);
    auto value = emitValue(expr.pointer_stride ? IROpcode::PointerOffset : IROpcode::Binary,
        expr.operation_type, expr.token, {operand, rhs}, expr.operation.lexeme, {}, expr.pointer_stride);
    if (!sameValueType(expr.operation_type, expr.result_type))
        value = emitValue(IROpcode::Cast, expr.result_type, expr.token, {value});
    IRInstruction store;
    store.opcode = coordinate ? IROpcode::PlotCoordinateWrite : IROpcode::StoreIndirect;
    store.type = expr.result_type;
    store.operands = coordinate ? std::vector<IRValueId>{value} : std::vector<IRValueId>{address, value};
    store.immediate = coordinate && coordinate->is_y ? 1 : 0;
    store.memory_volatile = !coordinate && expr.target->result_type.is_volatile;
    if (!coordinate) store.operation = "assign";
    store.source = expr.token;
    emitInstruction(std::move(store));
    m_last_value = expr.postfix ? old_value : value;
}

void IRLowerer::visit(LayoutQueryExpr& expr, const Type*) {
    m_last_value = emitValue(IROpcode::Constant, expr.result_type, expr.token, {}, {}, {}, expr.constant_value);
}
void IRLowerer::visit(EnumDeclStmt&) {}
void IRLowerer::visit(StaticAssertStmt&) {}
void IRLowerer::visit(TypeAliasDeclStmt&) {}

void IRLowerer::visit(VariableExpr& expr, const Type*) {
    if (expr.is_constant) {
        m_last_value = emitValue(IROpcode::Constant, expr.result_type, expr.token, {}, {}, {}, expr.constant_value);
        return;
    }
    const auto address = lowerAddress(expr);
    if (expr.is_array_decay) { m_last_value = address; return; }
    m_last_value = emitValue(IROpcode::LoadIndirect, expr.result_type, expr.token, {address});
    currentBlock().instructions.back().memory_volatile = expr.result_type.is_volatile;
}

void IRLowerer::visit(BinaryExpr& expr, const Type*) {
    if (expr.token.type == TokenType::AND_AND || expr.token.type == TokenType::OR_OR) {
        m_last_value = lowerLogical(expr);
        return;
    }
    const auto left = lowerExpression(*expr.left);
    const auto right = lowerExpression(*expr.right);
    m_last_value = emitValue(expr.left->result_type.pointer_level > 0 && expr.pointer_stride == 0 ? IROpcode::PointerCompare :
                             expr.pointer_stride > 0 ? IROpcode::PointerOffset : IROpcode::Binary,
                             expr.result_type, expr.token, {left, right}, expr.token.lexeme,
                             {}, expr.pointer_stride);
}

IRValueId IRLowerer::lowerLogical(BinaryExpr& expr) {
    auto& function = currentFunction();
    if (function.total_local_alloc_size == 0) function.total_local_alloc_size = 2;
    if (function.total_local_alloc_size > 65524)
        throw CompilerError("Logical expression frame exceeds one bank.", expr.token);
    function.total_local_alloc_size += 2;
    const Type boolean{BaseType::BOOL, "", 1, false};
    const auto address = emitValue(IROpcode::Address, pointerTo(boolean, AddressSpace::RAM),
        expr.token, {}, "temporary", {}, -(function.total_local_alloc_size - 2));
    const auto left = lowerExpression(*expr.left);
    const auto rhs = createBlock("logical.rhs"), skip = createBlock("logical.skip"), end = createBlock("logical.end");
    const bool conjunction = expr.token.type == TokenType::AND_AND;
    emitConditionalBranch(left, conjunction ? rhs : skip, conjunction ? skip : rhs, expr.token);
    const auto store = [&](IRValueId value) {
        IRInstruction instruction;
        instruction.opcode = IROpcode::StoreIndirect;
        instruction.type = boolean;
        instruction.operands = {address, value};
        instruction.source = expr.token;
        emitInstruction(std::move(instruction));
    };
    m_current_block = skip;
    store(emitValue(IROpcode::Constant, boolean, expr.token, {}, {}, {}, conjunction ? 0 : 1));
    emitBranch(end, expr.token);
    m_current_block = rhs;
    store(lowerExpression(*expr.right));
    emitBranch(end, expr.token);
    m_current_block = end;
    return emitValue(IROpcode::LoadIndirect, boolean, expr.token, {address});
}

void IRLowerer::visit(AssignExpr& expr, const Type*) {
    if (const auto* coordinate = dynamic_cast<const PlotCoordinateExpr*>(expr.name.get())) {
        const auto value = lowerExpression(*expr.value);
        IRInstruction instruction;
        instruction.opcode = IROpcode::PlotCoordinateWrite;
        instruction.type = expr.result_type;
        instruction.operands = {value};
        instruction.immediate = coordinate->is_y ? 1 : 0;
        instruction.source = expr.token;
        emitInstruction(std::move(instruction));
        m_last_value = value;
        return;
    }
    const auto address = lowerAddress(*expr.name);
    const auto value = lowerExpression(*expr.value);
    IRInstruction instruction;
    instruction.opcode = IROpcode::StoreIndirect;
    instruction.type = expr.result_type;
    instruction.operation = "assign";
    instruction.memory_volatile = expr.name->result_type.is_volatile;
    instruction.operands = {address, value};
    instruction.source = expr.token;
    emitInstruction(std::move(instruction));
    m_last_value = value;
}

void IRLowerer::visit(UnaryExpr& expr, const Type*) {
    if (expr.token.type == TokenType::MINUS && dynamic_cast<LiteralExpr*>(expr.right.get()) &&
        expr.right->token.type == TokenType::LITERAL_INTEGER) {
        m_last_value = emitValue(IROpcode::Constant, expr.result_type, expr.token, {}, {}, {},
            -DiscoNumeric::parse(expr.right->token.lexeme, nullptr, 0));
        return;
    }
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
    instruction.memory_volatile = expr.result_type.is_volatile;
    instruction.type = expr.result_type;
    instruction.operands = {address};
    instruction.source = expr.token;
    instruction.result = createValue();
    m_last_value = instruction.result;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(SubscriptExpr& expr, const Type*) {
    const auto address = lowerAddress(expr);
    m_last_value = emitValue(IROpcode::LoadIndirect, expr.result_type, expr.token, {address});
    currentBlock().instructions.back().memory_volatile = expr.result_type.is_volatile;
}

void IRLowerer::visit(MemberAccessExpr& expr, const Type*) {
    const auto address = lowerAddress(expr);
    m_last_value = emitValue(IROpcode::LoadIndirect, expr.result_type, expr.token, {address});
    currentBlock().instructions.back().memory_volatile = expr.result_type.is_volatile;
}

void IRLowerer::visit(CallExpr& expr, const Type*) {
    auto* callee = dynamic_cast<VariableExpr*>(expr.callee.get());
    if (!callee) {
        throw CompilerError("IR lowering: dynamic function calls are not supported.",
                            expr.token);
    }
    std::vector<IRValueId> arguments;
    for (const auto& argument : expr.arguments) {
        arguments.push_back(lowerExpression(*argument));
    }
    m_last_value = emitValue(IROpcode::Call, expr.result_type, expr.token, arguments, {},
                             expr.resolved_symbol.empty() ? callee->token.lexeme : expr.resolved_symbol);
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
    if (stmt.is_global || stmt.is_constexpr) return;
    requireFunction(stmt.token);
    if (!stmt.initializer && stmt.aggregate_initializers.empty()) {
        return;
    }
    VariableExpr variable(stmt.token);
    variable.result_type = stmt.type;
    variable.symbol_id = stmt.symbol_id;
    const auto address = lowerAddress(variable);
    for (const auto& element : stmt.aggregate_initializers) {
        const auto target = emitValue(IROpcode::Address, pointerTo(element.type, AddressSpace::RAM),
            stmt.token, {address}, "member", {}, element.offset);
        const auto value = lowerExpression(*element.value);
        IRInstruction store; store.opcode = IROpcode::StoreIndirect; store.type = element.type;
        store.operands = {target, value}; store.operation = "declare"; store.source = stmt.token;
        store.memory_volatile = element.type.is_volatile; emitInstruction(std::move(store));
    }
    if (!stmt.initializer) return;
    const auto value = lowerExpression(*stmt.initializer);
    IRInstruction instruction;
    instruction.opcode = IROpcode::StoreIndirect;
    instruction.type = stmt.type;
    instruction.operation = "declare";
    instruction.memory_volatile = stmt.type.is_volatile;
    instruction.operands = {address, value};
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(FunctionDeclStmt& stmt) {
    if (stmt.is_prototype) return;
    m_plot_context = false;

    m_module.functions.push_back({});
    m_current_function = m_module.functions.size() - 1;
    auto& function = currentFunction();
    function.name = stmt.token.lexeme;
    function.link_name = stmt.link_name;
    function.return_type = stmt.returnType;
    function.parameters = stmt.params;
    function.total_local_alloc_size = stmt.total_local_alloc_size;
    function.is_cached = stmt.is_cached;
    function.needs_implicit_return = stmt.needs_implicit_return;
    m_current_block = createBlock("entry");
    function.entry = m_current_block;
    lowerStatementList(stmt.body);

    if (!isTerminated()) {
        if (isVoidValue(stmt.returnType)) {
            IRInstruction instruction;
            instruction.opcode = IROpcode::ReturnVoid;
            instruction.source = stmt.token;
            emitInstruction(std::move(instruction));
        } else {
            throw CompilerError("IR lowering: non-void function reaches its end.",
                                stmt.token);
        }
    }
}

void IRLowerer::visit(IfStmt& stmt) {
    requireFunction(stmt.token);
    const bool incoming_plot_context = m_plot_context;
    const auto condition = lowerExpression(*stmt.condition);
    const auto then_block = createBlock("if.then");
    const auto else_block = stmt.elseBranch ? createBlock("if.else") : IRBlockId{};
    const auto end_block = createBlock("if.end");
    emitConditionalBranch(condition, then_block, stmt.elseBranch ? else_block : end_block,
                          stmt.token);

    m_current_block = then_block;
    stmt.thenBranch->accept(*this);
    const bool then_terminated = isTerminated();
    const bool then_plot_context = m_plot_context;
    if (!then_terminated) {
        emitBranch(end_block, stmt.token);
    }

    if (stmt.elseBranch) {
        m_plot_context = incoming_plot_context;
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
    m_plot_context = stmt.elseBranch ? then_plot_context : incoming_plot_context;
}

void IRLowerer::visit(BlockStmt& stmt) {
    requireFunction(stmt.token);
    lowerStatementList(stmt.statements);
}

void IRLowerer::visit(ForStmt& stmt) {
    requireFunction(stmt.token);
    if (stmt.initializer) stmt.initializer->accept(*this);
    const auto condition_block = createBlock("for.condition"), body_block = createBlock("for.body");
    const auto increment_block = createBlock("for.increment"), end_block = createBlock("for.end");
    emitBranch(condition_block, stmt.token);
    m_current_block = condition_block;
    if (stmt.is_cached) { IRInstruction cache; cache.opcode = IROpcode::Cache; cache.source = stmt.token; emitInstruction(std::move(cache)); }
    emitConditionalBranch(lowerExpression(*stmt.condition), body_block, end_block, stmt.token);
    m_current_block = body_block;
    m_break_targets.push_back(end_block); m_continue_targets.push_back(increment_block);
    stmt.body->accept(*this);
    m_continue_targets.pop_back(); m_break_targets.pop_back();
    if (!isTerminated()) emitBranch(increment_block, stmt.token);
    bool increment_reachable = false;
    for (const auto& block : currentFunction().blocks) for (const auto& instruction : block.instructions)
        for (const auto target : instruction.targets) if (target.value == increment_block.value) increment_reachable = true;
    m_current_block = increment_block;
    if (increment_reachable) {
        if (stmt.increment) lowerExpression(*stmt.increment);
        emitBranch(condition_block, stmt.token);
    } else {
        IRInstruction unreachable; unreachable.opcode = IROpcode::Unreachable; unreachable.source = stmt.token;
        emitInstruction(std::move(unreachable));
    }
    m_current_block = end_block;
}
void IRLowerer::visit(ContinueStmt& stmt) {
    if (m_continue_targets.empty()) throw CompilerError("IR lowering: continue outside loop.", stmt.token);
    emitBranch(m_continue_targets.back(), stmt.token);
}
void IRLowerer::visit(FallthroughStmt&) {}

void IRLowerer::visit(WhileStmt& stmt) {
    requireFunction(stmt.token);
    const auto condition_block = createBlock("while.cond");
    const auto body_block = createBlock("while.body");
    const auto end_block = createBlock("while.end");
    emitBranch(condition_block, stmt.token);

    m_current_block = condition_block;
    if (stmt.is_cached) { IRInstruction cache; cache.opcode = IROpcode::Cache; cache.source = stmt.token; emitInstruction(std::move(cache)); }
    const auto condition = lowerExpression(*stmt.condition);
    emitConditionalBranch(condition, body_block, end_block, stmt.token);

    m_break_targets.push_back(end_block);
    m_continue_targets.push_back(condition_block);
    m_current_block = body_block;
    stmt.body->accept(*this);
    if (!isTerminated()) {
        emitBranch(condition_block, stmt.token);
    }
    m_break_targets.pop_back();
    m_continue_targets.pop_back();
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
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(PlotBlockStmt& stmt) {
    m_plot_context = true;
    PlotBeginStmt begin;
    begin.token = stmt.token;
    visit(begin);
    stmt.body->accept(*this);
    m_plot_context = false;
    if (!isTerminated()) {
        PlotEndStmt end;
        end.token = stmt.token;
        visit(end);
    }
}

void IRLowerer::visit(PlotBeginStmt& stmt) {
    requireFunction(stmt.token);
    m_plot_context = true;
    IRInstruction instruction;
    instruction.opcode = IROpcode::PlotBegin;
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(PlotEndStmt& stmt) {
    requireFunction(stmt.token);
    m_plot_context = false;
    IRInstruction instruction;
    instruction.opcode = IROpcode::PlotEnd;
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(SetColorStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::SetColor;
    const auto& value = *stmt.color_value;
    const bool memory_read = dynamic_cast<const SubscriptExpr*>(&value) || dynamic_cast<const DereferenceExpr*>(&value) || dynamic_cast<const MemberAccessExpr*>(&value) || dynamic_cast<const VariableExpr*>(&value);
    if (memory_read && !value.is_constant && value.result_type.base == BaseType::BYTE && value.result_type.pointer_level == 0 &&
        value.address_type.pointer_level > 0 && value.address_type.space == AddressSpace::ROM && !value.result_type.is_volatile) {
        instruction.operation = "rom.byte";
        instruction.operands = {lowerAddress(*stmt.color_value)};
    } else instruction.operands = {lowerExpression(*stmt.color_value)};
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(CmodeStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::CMode;
    instruction.immediate = stmt.options_value->constant_value;
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(RpixStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::Rpix;
    instruction.type = Type{BaseType::VOID, "", 0, false};
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
}

void IRLowerer::visit(HardwareLoopStmt& stmt) {
    requireFunction(stmt.token);
    IRInstruction instruction;
    instruction.opcode = IROpcode::HardwareLoop;
    const auto loop_id = ++m_next_hardware_loop;
    instruction.loop_id = loop_id;
    instruction.operands = {lowerExpression(*stmt.count)};
    instruction.source = stmt.token;
    emitInstruction(std::move(instruction));
    stmt.body->accept(*this);
    if (isTerminated()) {
        throw CompilerError("IR lowering: hardware loop body cannot terminate with return or branch.",
                            stmt.token);
    }
    IRInstruction end;
    end.opcode = IROpcode::HardwareLoopEnd;
    end.loop_id = loop_id;
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
                                    case_stmt->token);
            }
            arms.push_back({createBlock("switch.case"), index, false,
                            DiscoNumeric::parse(literal->token.lexeme, nullptr, 0)});
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
    // When every arm returns, nothing branches to the end block. Mark it so
    // statements after the switch are skipped like those after if/else.
    bool end_reachable = false;
    for (const auto& block : currentFunction().blocks) {
        for (const auto& instruction : block.instructions) {
            for (const auto target : instruction.targets) {
                end_reachable = end_reachable || target.value == end_block.value;
            }
        }
    }
    if (!end_reachable) {
        IRInstruction instruction;
        instruction.opcode = IROpcode::Unreachable;
        instruction.source = stmt.token;
        emitInstruction(std::move(instruction));
    }
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
                            stmt.token);
    }
    emitBranch(m_break_targets.back(), stmt.token);
}

std::string dumpIR(const IRModule& module) {
    // Bitmap metadata is independent of instruction-level plot state.
    std::ostringstream output;
    if (module.bitmap.enabled) output << "bitmap.config scbr=" << static_cast<unsigned>(module.bitmap.scbr())
        << " scmr=" << static_cast<unsigned>(module.bitmap.scmr()) << " bytes=" << module.bitmap.sizeBytes() << '\n';
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
                if (instruction.memory_volatile) output << " volatile";
                if (instruction.is_live_range_split) output << " live.split";
                if (instruction.compiler_generated_loop) output << " automatic";
                if (!instruction.initialization_values.empty()) {
                    output << " [";
                    for (std::size_t n = 0; n < instruction.initialization_values.size(); ++n) {
                        if (n) output << ",";
                        output << instruction.initialization_values[n];
                    }
                    output << "]";
                }
                if (!instruction.operation.empty()) {
                    output << " " << instruction.operation;
                }
                if (!instruction.symbol.empty()) {
                    output << " @" << instruction.symbol;
                    if (instruction.symbol_id.isValid()) {
                        output << "#" << instruction.symbol_id.value;
                    }
                }
                if (instruction.opcode == IROpcode::Rpix && !instruction.result.isValid()) output << " discard";
                if (instruction.opcode == IROpcode::Constant || instruction.opcode == IROpcode::BitExtract || instruction.opcode == IROpcode::CMode ||
                    instruction.opcode == IROpcode::PlotCoordinateRead || instruction.opcode == IROpcode::PlotCoordinateWrite) {
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
