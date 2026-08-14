#include "IRCodeGenerator.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>

#include "CompilerError.hpp"
#include "ABI.hpp"
#include "Opcodes.hpp"

IRCodeGenerator::IRCodeGenerator(
    const std::map<std::string, Analyzer::LocalSymbolTable>& all_local_symbols,
    const std::map<std::string, FunctionSymbol>& global_function_symbols,
    const DataSegmentManager& data_manager,
    const CompilerConfig& config)
    : m_all_local_symbols(all_local_symbols),
      m_global_function_symbols(global_function_symbols),
      m_data_manager(data_manager),
      m_config(config) {}

void IRCodeGenerator::fail(const std::string& message, const Token& source) const {
    throw CompilerError(message, source.line_number, source.col_number);
}

void IRCodeGenerator::emitByte(std::uint8_t byte) {
    m_object_file.code_section.push_back(byte);
}

void IRCodeGenerator::emitWord(std::uint16_t word) {
    emitByte(static_cast<std::uint8_t>(word & 0xff));
    emitByte(static_cast<std::uint8_t>((word >> 8) & 0xff));
}

void IRCodeGenerator::emitLiteral(std::int64_t value) {
    if (value < std::numeric_limits<std::int16_t>::min() ||
        value > std::numeric_limits<std::uint16_t>::max()) {
        fail("IR codegen: literal is outside the 16-bit GSU range.",
             Token(TokenType::UNKNOWN, "", 0, 0));
    }
    emitByte(static_cast<std::uint8_t>(OpCode::IWT));
    emitWord(static_cast<std::uint16_t>(value));
}

void IRCodeGenerator::emitMove(std::uint8_t destination, std::uint8_t source) {
    if (source != 0) {
        emitByte(static_cast<std::uint8_t>(0x20 | source));
    }
    emitByte(static_cast<std::uint8_t>(0x10 | destination));
}

void IRCodeGenerator::emitPush(std::uint8_t reg) {
    if (reg != 0) {
        emitByte(static_cast<std::uint8_t>(0x20 | reg));
    }
    emitByte(0x3A);
    emitByte(0xEA);
    emitByte(0xEA);
}

void IRCodeGenerator::emitPop(std::uint8_t reg) {
    emitByte(0xDA);
    emitByte(0xDA);
    if (reg != 0) {
        emitByte(static_cast<std::uint8_t>(0x20 | reg));
    }
    emitByte(0x4A);
}

void IRCodeGenerator::emitStore(std::uint8_t address_reg, std::uint8_t value_reg, bool byte) {
    if (value_reg != 0) {
        emitByte(static_cast<std::uint8_t>(0x20 | value_reg));
    }
    if (byte) {
        emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
    }
    emitByte(static_cast<std::uint8_t>(0x30 | address_reg));
}

void IRCodeGenerator::emitImmediateArithmetic(std::uint8_t op_base,
                                               std::int64_t value,
                                               const Token& source) {
    if (value < 0) {
        fail("IR codegen: immediate arithmetic value cannot be negative.", source);
    }
    auto remaining = static_cast<std::uint64_t>(value);
    while (remaining > 0) {
        const auto chunk = static_cast<std::uint8_t>(std::min<std::uint64_t>(remaining, 15));
        emitByte(static_cast<std::uint8_t>(OpCode::ALT2));
        emitByte(static_cast<std::uint8_t>(op_base | chunk));
        remaining -= chunk;
    }
}

void IRCodeGenerator::emitAdjustStack(std::size_t bytes, bool add, const Token& source) {
    const auto op_base = static_cast<std::uint8_t>(add ? 0x50 : 0x60);
    while (bytes > 0) {
        const auto chunk = static_cast<std::uint8_t>(std::min<std::size_t>(bytes, 15));
        emitByte(0x2A);
        emitByte(static_cast<std::uint8_t>(OpCode::ALT2));
        emitByte(static_cast<std::uint8_t>(op_base | chunk));
        bytes -= chunk;
    }
    (void)source;
}

void IRCodeGenerator::emitFunctionEpilogue() {
    emitMove(GSUAbi::StackPointerRegister, GSUAbi::FramePointerRegister);
    if (m_current_function->name == "main") {
        emitByte(static_cast<std::uint8_t>(OpCode::STOP));
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        return;
    }
    emitPop(GSUAbi::FramePointerRegister);
    emitPop(GSUAbi::LinkRegister);
    emitByte(static_cast<std::uint8_t>(0x90 | GSUAbi::LinkRegister));
    emitByte(static_cast<std::uint8_t>(OpCode::NOP));
}

std::uint8_t IRCodeGenerator::scratchRegister() const {
    return m_isInPlottingContext ? 3 : 1;
}

void IRCodeGenerator::buildRegisterAllocation(const IRFunction& function) {
    // R0 is the expression accumulator, R1/R3 are backend temporaries, R9 is
    // the frame pointer, and R10-R15 have ABI or hardware roles. R5, R7, and
    // R8 are general-purpose registers for the current backend operations.
    m_register_allocator.run(function, {5, 7, 8});
}

bool IRCodeGenerator::usesRegisterAllocation(IRValueId value) const {
    const auto location = m_register_allocator.find(value);
    const auto uses = m_use_counts.find(value.value);
    return location != nullptr && location->has_register &&
           uses != m_use_counts.end() && uses->second > 1;
}

void IRCodeGenerator::saveLiveRegistersForCall(
    const IRInstruction& instruction,
    std::vector<std::uint8_t>& saved_registers) {
    std::set<std::uint8_t> unique_registers;
    for (const auto& pair : m_register_allocator.locations()) {
        const auto& location = pair.second;
        const auto uses = m_use_counts.find(pair.first);
        if (!location.has_register || uses == m_use_counts.end() || uses->second <= 1 ||
            location.start > m_current_instruction_position ||
            location.end < m_current_instruction_position ||
            (instruction.result.isValid() && pair.first == instruction.result.value)) {
            continue;
        }
        unique_registers.insert(location.physical_register);
    }
    saved_registers.assign(unique_registers.begin(), unique_registers.end());
    for (const auto reg : saved_registers) emitPush(reg);
}

void IRCodeGenerator::restoreRegistersAfterCall(
    const std::vector<std::uint8_t>& saved_registers) {
    for (auto it = saved_registers.rbegin(); it != saved_registers.rend(); ++it) {
        emitPop(*it);
    }
}

const IRInstruction& IRCodeGenerator::producer(IRValueId value, const Token& source) const {
    if (!value.isValid()) {
        fail("IR codegen: invalid value reference.", source);
    }
    const auto found = m_values.find(value.value);
    if (found == m_values.end() || found->second == nullptr) {
        fail("IR codegen: value has no defining instruction.", source);
    }
    return *found->second;
}

bool IRCodeGenerator::constantValue(IRValueId value, std::int64_t& result) const {
    const auto& instruction = producer(value, Token(TokenType::UNKNOWN, "", 0, 0));
    if (instruction.opcode != IROpcode::Constant) {
        return false;
    }
    result = instruction.immediate;
    return true;
}

void IRCodeGenerator::addRelocation(const std::string& symbol, std::size_t patch_offset,
                                    RelocationType type) {
    m_object_file.relocation_table.push_back({
        symbol,
        SymbolSection::CODE,
        static_cast<std::uint32_t>(patch_offset),
        type
    });
}

void IRCodeGenerator::emitAddress(const IRInstruction& instruction) {
    if (instruction.operation == "index") {
        if (instruction.operands.size() != 2) {
            fail("IR codegen: indexed address must have base and index operands.", instruction.source);
        }
        materialize(instruction.operands[1]);
        if (instruction.immediate == 2) {
            emitByte(static_cast<std::uint8_t>(OpCode::ALT2));
            emitByte(static_cast<std::uint8_t>(OpCode::ADD_R));
            emitByte(static_cast<std::uint8_t>(OpCode::ROL));
        }
        emitPush(0);
        materialize(instruction.operands[0]);
        emitPop(scratchRegister());
        emitByte(static_cast<std::uint8_t>(0x60 | scratchRegister()));
        return;
    }

    if (instruction.operation == "member") {
        if (instruction.operands.size() != 1) {
            fail("IR codegen: member address must have an object operand.", instruction.source);
        }
        materialize(instruction.operands[0]);
        if (instruction.immediate > 0) {
            emitMove(scratchRegister(), 0);
            emitLiteral(instruction.immediate);
            emitByte(static_cast<std::uint8_t>(0x50 | scratchRegister()));
        }
        return;
    }

    if (instruction.symbol == "plot_x") {
        emitMove(0, 1);
        return;
    }
    if (instruction.symbol == "plot_y") {
        emitMove(0, 2);
        return;
    }

    const auto function_symbols = m_all_local_symbols.find(m_current_function->name);
    if (function_symbols != m_all_local_symbols.end() && instruction.symbol_id.isValid()) {
        const auto symbol = function_symbols->second.find(instruction.symbol_id);
        if (symbol != function_symbols->second.end()) {
            emitMove(0, 9);
            if (symbol->second.stackOffset > 0) {
                emitImmediateArithmetic(0x50, symbol->second.stackOffset, instruction.source);
            } else if (symbol->second.stackOffset < 0) {
                emitImmediateArithmetic(0x60, std::abs(symbol->second.stackOffset), instruction.source);
            }
            return;
        }
        fail("IR codegen: local symbol ID is not present in the current function.",
             instruction.source);
    }

    if (m_data_manager.hasSymbol(instruction.symbol) ||
        m_global_function_symbols.count(instruction.symbol) != 0) {
        const auto patch_offset = m_object_file.code_section.size();
        emitByte(static_cast<std::uint8_t>(OpCode::IWT));
        emitWord(0);
        addRelocation(instruction.symbol, patch_offset, RelocationType::ADDR16_IWT);
        return;
    }

    fail("IR codegen: unknown address symbol '" + instruction.symbol + "'.",
         instruction.source);
}

void IRCodeGenerator::emitLoadIndirect(const IRInstruction& instruction) {
    if (instruction.operands.size() != 1) {
        fail("IR codegen: indirect load must have one address operand.", instruction.source);
    }
    const auto& address = producer(instruction.operands[0], instruction.source);
    materialize(instruction.operands[0]);
    if (address.opcode == IROpcode::Address &&
        (address.symbol == "plot_x" || address.symbol == "plot_y")) {
        return;
    }
    if (instruction.operation == "far") {
        // A far pointer is laid out as bank byte at +0 and word offset at
        // +2.  Keep the bank and offset in target registers while R0 is
        // reused to address the pointer fields, matching the GSU ABI.
        const std::uint8_t bank_reg = m_isInPlottingContext ? 3 : 1;
        const std::uint8_t offset_reg = m_isInPlottingContext ? 4 : 2;
        const std::uint8_t pointer_reg = m_isInPlottingContext ? 6 : 3;

        emitByte(static_cast<std::uint8_t>(0x20 | bank_reg));
        emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
        emitByte(0x40);
        emitMove(pointer_reg, 0);

        emitLiteral(2);
        emitByte(static_cast<std::uint8_t>(0x50 | pointer_reg));
        emitByte(static_cast<std::uint8_t>(0x20 | offset_reg));
        emitByte(0x40);

        emitByte(static_cast<std::uint8_t>(instruction.type.space == AddressSpace::ROM
                                                 ? OpCode::ALT3
                                                 : OpCode::ALT2));
        emitByte(static_cast<std::uint8_t>(0x20 | bank_reg));
        emitByte(static_cast<std::uint8_t>(OpCode::GETC));

        if (instruction.type.space == AddressSpace::ROM) {
            emitMove(14, offset_reg);
            if (instruction.type.base == BaseType::BYTE) {
                emitByte(static_cast<std::uint8_t>(OpCode::GETB));
            } else {
                emitByte(static_cast<std::uint8_t>(OpCode::GETB));
                emitByte(0xDE);
                emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
                emitByte(static_cast<std::uint8_t>(OpCode::GETB));
            }
        } else if (instruction.type.base == BaseType::BYTE) {
            emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
            emitByte(static_cast<std::uint8_t>(0x40 | offset_reg));
        } else {
            emitByte(static_cast<std::uint8_t>(0x40 | offset_reg));
        }
        return;
    }
    if (instruction.type.space == AddressSpace::ROM) {
        emitMove(14, 0);
        if (instruction.type.base == BaseType::BYTE) {
            emitByte(static_cast<std::uint8_t>(OpCode::GETB));
        } else {
            emitByte(static_cast<std::uint8_t>(OpCode::GETB));
            emitByte(0xDE);
            emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
            emitByte(static_cast<std::uint8_t>(OpCode::GETB));
        }
        return;
    }
    if (instruction.type.base == BaseType::BYTE) {
        emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
    }
    emitByte(0x40);
}

void IRCodeGenerator::emitBinary(const IRInstruction& instruction) {
    if (instruction.operands.size() != 2) {
        fail("IR codegen: binary instruction must have two operands.", instruction.source);
    }
    const auto left = instruction.operands[0];
    const auto right = instruction.operands[1];
    std::int64_t immediate = 0;
    const bool right_immediate = constantValue(right, immediate) && immediate >= 0 && immediate <= 15;
    const bool left_immediate = constantValue(left, immediate) && immediate >= 0 && immediate <= 15;

    std::uint8_t op_base = 0;
    if (instruction.operation == "+") op_base = 0x50;
    else if (instruction.operation == "-") op_base = 0x60;
    else if (instruction.operation == "*") op_base = 0x80;

    if (op_base != 0 && right_immediate) {
        materialize(left);
        emitByte(static_cast<std::uint8_t>(OpCode::ALT2));
        emitByte(static_cast<std::uint8_t>(op_base | immediate));
        return;
    }
    if (op_base != 0 && left_immediate &&
        (instruction.operation == "+" || instruction.operation == "*")) {
        materialize(right);
        emitByte(static_cast<std::uint8_t>(OpCode::ALT2));
        emitByte(static_cast<std::uint8_t>(op_base | immediate));
        return;
    }

    materialize(right);
    emitPush(0);
    materialize(left);
    emitPop(scratchRegister());

    if (instruction.operation == ">" || instruction.operation == "<" ||
        instruction.operation == ">=" || instruction.operation == "<=" ||
        instruction.operation == "==" || instruction.operation == "!=") {
        emitByte(static_cast<std::uint8_t>(OpCode::ALT3));
        emitByte(static_cast<std::uint8_t>(0x60 | scratchRegister()));

        std::vector<std::size_t> labels;
        std::vector<LocalBranchFixup> fixups;
        const auto new_label = [&labels]() {
            labels.push_back(0);
            return labels.size() - 1;
        };
        const auto true_label = new_label();
        const auto end_label = new_label();
        const auto emit_local_branch = [&](std::uint8_t opcode, std::size_t label) {
            emitByte(opcode);
            const auto patch_offset = m_object_file.code_section.size();
            emitByte(0);
            emitByte(static_cast<std::uint8_t>(OpCode::NOP));
            fixups.push_back({patch_offset, label, instruction.source});
        };

        if (instruction.operation == "==") {
            emit_local_branch(static_cast<std::uint8_t>(OpCode::BEQ), true_label);
        } else if (instruction.operation == "!=") {
            emit_local_branch(static_cast<std::uint8_t>(OpCode::BNE), true_label);
        } else if (instruction.operation == ">") {
            emit_local_branch(static_cast<std::uint8_t>(OpCode::BPL), true_label);
        } else if (instruction.operation == "<") {
            emit_local_branch(static_cast<std::uint8_t>(OpCode::BMI), true_label);
        } else if (instruction.operation == ">=") {
            emit_local_branch(static_cast<std::uint8_t>(OpCode::BEQ), true_label);
            emit_local_branch(static_cast<std::uint8_t>(OpCode::BPL), true_label);
        } else if (instruction.operation == "<=") {
            emit_local_branch(static_cast<std::uint8_t>(OpCode::BEQ), true_label);
            emit_local_branch(static_cast<std::uint8_t>(OpCode::BMI), true_label);
        }
        emitLiteral(0);
        emit_local_branch(static_cast<std::uint8_t>(OpCode::BRA), end_label);
        labels[true_label] = m_object_file.code_section.size();
        emitLiteral(1);
        labels[end_label] = m_object_file.code_section.size();
        for (auto& fixup : fixups) {
            fixup.target_offset = labels[fixup.target_offset];
        }
        patchLocalBranches(fixups);
        return;
    }
    if (op_base != 0) {
        emitByte(static_cast<std::uint8_t>(op_base | scratchRegister()));
        return;
    }
    fail("IR codegen: unsupported binary operation '" + instruction.operation + "'.",
         instruction.source);
}

void IRCodeGenerator::emitCast(const IRInstruction& instruction) {
    if (instruction.operands.size() != 1) {
        fail("IR codegen: cast must have one operand.", instruction.source);
    }
    const auto& source = producer(instruction.operands[0], instruction.source);
    materialize(instruction.operands[0]);
    if (instruction.type.sizeInBytes == 2 && source.type.sizeInBytes == 1 &&
        !source.type.is_unsigned) {
        emitByte(static_cast<std::uint8_t>(OpCode::SEX));
    }
}

void IRCodeGenerator::emitCall(const IRInstruction& instruction) {
    std::vector<std::uint8_t> saved_registers;
    saveLiveRegistersForCall(instruction, saved_registers);
    for (auto argument = instruction.operands.rbegin();
         argument != instruction.operands.rend(); ++argument) {
        materialize(*argument);
        emitPush(0);
    }
    emitByte(0x94);
    const auto patch_offset = m_object_file.code_section.size();
    emitByte(static_cast<std::uint8_t>(OpCode::IWT) | 0x0F);
    emitWord(0);
    addRelocation(instruction.symbol, patch_offset, RelocationType::ADDR16_JAL);
    if (instruction.operands.size() >
        std::numeric_limits<std::size_t>::max() / GSUAbi::ParameterSlotSize) {
        fail("IR codegen: call argument area is too large.", instruction.source);
    }
    emitAdjustStack(instruction.operands.size() * GSUAbi::ParameterSlotSize,
                    true, instruction.source);
    restoreRegistersAfterCall(saved_registers);
}

void IRCodeGenerator::emitStoreIndirect(const IRInstruction& instruction) {
    if (instruction.operands.size() != 2) {
        fail("IR codegen: indirect store must have address and value operands.", instruction.source);
    }
    const auto address = instruction.operands[0];
    const auto value = instruction.operands[1];
    const bool byte = instruction.type.base == BaseType::BYTE;

    if (instruction.operation == "declare") {
        materialize(value);
        emitMove(scratchRegister(), 0);
        materialize(address);
        emitStore(0, scratchRegister(), byte);
        return;
    }

    materialize(address);
    emitPush(0);
    materialize(value);
    emitPop(scratchRegister());
    emitStore(scratchRegister(), 0, byte);
}

void IRCodeGenerator::emitHardwareLoop(const IRInstruction& instruction) {
    if (instruction.operands.size() != 1) {
        fail("IR codegen: hardware loop must have a count operand.", instruction.source);
    }
    std::int64_t count = 0;
    if (constantValue(instruction.operands[0], count)) {
        emitByte(static_cast<std::uint8_t>(OpCode::IWT) | 0x0C);
        emitWord(static_cast<std::uint16_t>(count));
    } else {
        materialize(instruction.operands[0]);
        emitMove(12, 0);
    }
    emitByte(0x2D);
    emitByte(0x1F);
}

void IRCodeGenerator::materialize(IRValueId value) {
    const auto& instruction = producer(value, Token(TokenType::UNKNOWN, "", 0, 0));
    const auto* location = m_register_allocator.find(value);
    if (usesRegisterAllocation(value) &&
        m_materialized_values.count(value.value) != 0) {
        emitMove(0, location->physical_register);
        return;
    }
    if (!m_active_values.insert(value.value).second) {
        fail("IR codegen: cyclic value definition.", instruction.source);
    }

    switch (instruction.opcode) {
        case IROpcode::Constant:
            emitLiteral(instruction.immediate);
            break;
        case IROpcode::Address:
            emitAddress(instruction);
            break;
        case IROpcode::Load:
        case IROpcode::LoadIndirect:
            emitLoadIndirect(instruction);
            break;
        case IROpcode::Binary:
            emitBinary(instruction);
            break;
        case IROpcode::Unary: {
            std::int64_t literal = 0;
            if (instruction.operation == "-" && constantValue(instruction.operands.front(), literal)) {
                emitLiteral(-literal);
            } else {
                materialize(instruction.operands.front());
                emitByte(0x4F);
                emitByte(0xD0);
            }
            break;
        }
        case IROpcode::Cast:
            emitCast(instruction);
            break;
        case IROpcode::Call:
            emitCall(instruction);
            break;
        default:
            fail("IR codegen: instruction does not produce a materializable value.",
                 instruction.source);
    }
    if (usesRegisterAllocation(value)) {
        emitMove(location->physical_register, 0);
        m_materialized_values.insert(value.value);
    }
    m_active_values.erase(value.value);
}

void IRCodeGenerator::emitBranch(IRBlockId target, const Token& source) {
    if (target.isValid() && target.value == m_current_block_index + 1) {
        return;
    }
    emitByte(static_cast<std::uint8_t>(OpCode::BRA));
    const auto patch_offset = m_object_file.code_section.size();
    emitByte(0);
    emitByte(static_cast<std::uint8_t>(OpCode::NOP));
    m_branch_fixups.push_back({patch_offset, target, source});
}

void IRCodeGenerator::emitConditionalBranch(const IRInstruction& instruction) {
    if (instruction.operands.size() != 1 || instruction.targets.size() != 2) {
        fail("IR codegen: conditional branch shape is invalid.", instruction.source);
    }
    const auto& condition = producer(instruction.operands.front(), instruction.source);
    const bool is_comparison = condition.opcode == IROpcode::Binary &&
                               (condition.operation == ">" || condition.operation == ">=" ||
                                condition.operation == "<" || condition.operation == "<=" ||
                                condition.operation == "==" || condition.operation == "!=");
    if (is_comparison) {
        materialize(condition.operands[1]);
        emitPush(0);
        materialize(condition.operands[0]);
        emitPop(scratchRegister());
        emitByte(static_cast<std::uint8_t>(OpCode::ALT3));
        emitByte(static_cast<std::uint8_t>(0x60 | scratchRegister()));
        std::uint8_t false_opcode = static_cast<std::uint8_t>(OpCode::BNE);
        if (condition.operation == ">" || condition.operation == ">=") {
            false_opcode = static_cast<std::uint8_t>(OpCode::BMI);
        } else if (condition.operation == "<" || condition.operation == "<=") {
            false_opcode = static_cast<std::uint8_t>(OpCode::BPL);
        } else if (condition.operation == "!=") {
            false_opcode = static_cast<std::uint8_t>(OpCode::BEQ);
        }
        emitByte(false_opcode);
        const auto false_patch = m_object_file.code_section.size();
        emitByte(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({false_patch, instruction.targets[1], instruction.source});
        emitBranch(instruction.targets[0], instruction.source);
        return;
    }

    // Ordinary integer conditions use truthiness (non-zero is true).
    materialize(instruction.operands.front());
    emitMove(scratchRegister(), 0);
    emitLiteral(0);
    emitByte(static_cast<std::uint8_t>(OpCode::ALT3));
    emitByte(static_cast<std::uint8_t>(0x60 | scratchRegister()));
    emitByte(static_cast<std::uint8_t>(OpCode::BNE));
    const auto true_patch = m_object_file.code_section.size();
    emitByte(0);
    emitByte(static_cast<std::uint8_t>(OpCode::NOP));
    m_branch_fixups.push_back({true_patch, instruction.targets[0], instruction.source});
    emitBranch(instruction.targets[1], instruction.source);
}

void IRCodeGenerator::emitSwitch(const IRInstruction& instruction) {
    if (instruction.operands.size() != 1 || instruction.targets.empty()) {
        fail("IR codegen: switch shape is invalid.", instruction.source);
    }
    const auto default_index = instruction.has_default_target
        ? instruction.targets.size() - 1
        : instruction.targets.size();

    std::int64_t constant_selector = 0;
    if (constantValue(instruction.operands.front(), constant_selector)) {
        for (std::size_t index = 0; index < instruction.case_values.size(); ++index) {
            if (instruction.case_values[index] == constant_selector) {
                emitBranch(instruction.targets[index], instruction.source);
                return;
            }
        }
        if (default_index < instruction.targets.size()) {
            emitBranch(instruction.targets[default_index], instruction.source);
        }
        return;
    }

    materialize(instruction.operands.front());
    emitMove(scratchRegister(), 0);

    // A short switch remains a compact linear chain. For larger switches,
    // compare against the median case and recursively search each half. This
    // reduces comparisons from O(n) to O(log n) without changing case/fall-
    // through semantics or the object-file relocation model.
    if (instruction.case_values.size() < 4) {
        for (std::size_t case_index = 0;
             case_index < instruction.case_values.size(); ++case_index) {
            emitLiteral(instruction.case_values[case_index]);
            emitByte(static_cast<std::uint8_t>(OpCode::ALT3));
            emitByte(static_cast<std::uint8_t>(0x60 | scratchRegister()));
            emitByte(static_cast<std::uint8_t>(OpCode::BEQ));
            const auto patch_offset = m_object_file.code_section.size();
            emitByte(0);
            emitByte(static_cast<std::uint8_t>(OpCode::NOP));
            m_branch_fixups.push_back({patch_offset, instruction.targets[case_index], instruction.source});
        }
        if (default_index < instruction.targets.size()) {
            emitByte(static_cast<std::uint8_t>(OpCode::BRA));
            const auto patch_offset = m_object_file.code_section.size();
            emitByte(0);
            emitByte(static_cast<std::uint8_t>(OpCode::NOP));
            m_branch_fixups.push_back({patch_offset, instruction.targets[default_index], instruction.source});
        }
        return;
    }

    std::vector<std::size_t> order(instruction.case_values.size());
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return instruction.case_values[left] < instruction.case_values[right];
    });

    std::vector<std::size_t> labels;
    std::vector<LocalBranchFixup> local_fixups;
    const auto new_label = [&labels]() {
        labels.push_back(0);
        return labels.size() - 1;
    };
    const auto emit_local_branch = [&](std::uint8_t opcode, std::size_t label) {
        emitByte(opcode);
        const auto patch_offset = m_object_file.code_section.size();
        emitByte(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        local_fixups.push_back({patch_offset, label, instruction.source});
    };
    const auto emit_block_branch = [&](std::uint8_t opcode, IRBlockId target) {
        emitByte(opcode);
        const auto patch_offset = m_object_file.code_section.size();
        emitByte(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({patch_offset, target, instruction.source});
    };

    std::function<void(std::size_t, std::size_t, std::size_t)> emit_search;
    emit_search = [&](std::size_t begin, std::size_t end, std::size_t label) {
        labels[label] = m_object_file.code_section.size();
        const auto middle = begin + (end - begin) / 2;
        const auto case_index = order[middle];
        emitLiteral(instruction.case_values[case_index]);
        emitByte(static_cast<std::uint8_t>(OpCode::ALT3));
        emitByte(static_cast<std::uint8_t>(0x60 | scratchRegister()));
        emitByte(static_cast<std::uint8_t>(OpCode::BEQ));
        const auto equal_patch = m_object_file.code_section.size();
        emitByte(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({equal_patch, instruction.targets[case_index], instruction.source});

        const bool has_left = begin < middle;
        const bool has_right = middle + 1 < end;
        std::size_t left_label = 0;
        std::size_t right_label = 0;
        if (has_left) {
            left_label = new_label();
            // The GSU CMP/branch convention used by the existing relational
            // emitter selects the lower half with BPL after cmp scratch,R0.
            emit_local_branch(static_cast<std::uint8_t>(OpCode::BPL), left_label);
        } else if (default_index < instruction.targets.size()) {
            emit_block_branch(static_cast<std::uint8_t>(OpCode::BPL),
                              instruction.targets[default_index]);
        }
        if (has_right) {
            right_label = new_label();
            emit_local_branch(static_cast<std::uint8_t>(OpCode::BRA), right_label);
        } else if (default_index < instruction.targets.size()) {
            emit_block_branch(static_cast<std::uint8_t>(OpCode::BRA),
                              instruction.targets[default_index]);
        }

        if (has_left) {
            emit_search(begin, middle, left_label);
        }
        if (has_right) {
            emit_search(middle + 1, end, right_label);
        }
    };

    const auto root_label = new_label();
    emit_search(0, order.size(), root_label);
    for (auto& fixup : local_fixups) {
        fixup.target_offset = labels[fixup.target_offset];
    }
    patchLocalBranches(local_fixups);
}

void IRCodeGenerator::patchLocalBranches(const std::vector<LocalBranchFixup>& fixups) {
    for (const auto& fixup : fixups) {
        const auto next_instruction = static_cast<std::int64_t>(fixup.patch_offset) + 1;
        const auto distance = static_cast<std::int64_t>(fixup.target_offset) - next_instruction;
        if (distance < -128 || distance > 127) {
            fail("IR codegen: optimized switch branch is out of range.", fixup.source);
        }
        m_object_file.code_section.at(fixup.patch_offset) =
            static_cast<std::uint8_t>(static_cast<std::int8_t>(distance));
    }
}

void IRCodeGenerator::emitInstruction(const IRInstruction& instruction) {
    switch (instruction.opcode) {
        case IROpcode::StoreIndirect:
            emitStoreIndirect(instruction);
            break;
        case IROpcode::PlotBegin:
            m_isInPlottingContext = true;
            break;
        case IROpcode::PlotEnd:
            m_isInPlottingContext = false;
            break;
        case IROpcode::Plot:
            if (instruction.operands.size() != 2) fail("IR codegen: plot shape is invalid.", instruction.source);
            materialize(instruction.operands[1]);
            emitMove(2, 0);
            materialize(instruction.operands[0]);
            emitMove(1, 0);
            emitByte(0x4C);
            break;
        case IROpcode::SetColor:
            materialize(instruction.operands.front());
            emitByte(static_cast<std::uint8_t>(OpCode::COLOR_R));
            break;
        case IROpcode::CMode:
            materialize(instruction.operands.front());
            emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
            emitByte(0x4E);
            break;
        case IROpcode::Rpix:
            emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
            emitByte(0x4C);
            break;
        case IROpcode::HardwareLoop:
            emitHardwareLoop(instruction);
            break;
        case IROpcode::HardwareLoopEnd:
            emitByte(0x3C);
            emitByte(static_cast<std::uint8_t>(OpCode::NOP));
            break;
        case IROpcode::Branch:
            emitBranch(instruction.targets.front(), instruction.source);
            break;
        case IROpcode::CondBranch:
            emitConditionalBranch(instruction);
            break;
        case IROpcode::Switch:
            emitSwitch(instruction);
            break;
        case IROpcode::Return:
            materialize(instruction.operands.front());
            emitFunctionEpilogue();
            break;
        case IROpcode::ReturnVoid:
            emitFunctionEpilogue();
            break;
        case IROpcode::Unreachable:
            break;
        default:
            break;
    }
}

void IRCodeGenerator::emitBlock(const IRBasicBlock& block) {
    m_materialized_values.clear();
    for (const auto& instruction : block.instructions) {
        m_current_instruction_position = m_emission_position++;
        // A void call has no SSA result, but it is still a side effect that
        // must be emitted when it appears as an expression statement.
        if (instruction.opcode == IROpcode::Call &&
            instruction.type.base == BaseType::VOID) {
            emitCall(instruction);
            continue;
        }
        if (instruction.producesValue()) {
            if (instruction.opcode == IROpcode::Call &&
                instruction.result.isValid() &&
                m_use_counts[instruction.result.value] == 0) {
                materialize(instruction.result);
            }
            continue;
        }
        emitInstruction(instruction);
    }
}

void IRCodeGenerator::patchBranches() {
    for (const auto& fixup : m_branch_fixups) {
        const auto target = m_block_addresses.find(fixup.target.value);
        if (target == m_block_addresses.end()) {
            fail("IR codegen: branch target was not emitted.", fixup.source);
        }
        const auto next_instruction = static_cast<std::int64_t>(fixup.patch_offset) + 1;
        const auto distance = static_cast<std::int64_t>(target->second) - next_instruction;
        if (distance < -128 || distance > 127) {
            fail("IR codegen: branch target is out of range.", fixup.source);
        }
        m_object_file.code_section.at(fixup.patch_offset) =
            static_cast<std::uint8_t>(static_cast<std::int8_t>(distance));
    }
}

ObjectFile IRCodeGenerator::generate(const IRModule& module) {
    m_object_file = ObjectFile();
    m_object_file.config = m_config;

    for (const auto& function : module.functions) {
        m_current_function = &function;
        m_values.clear();
        m_use_counts.clear();
        m_active_values.clear();
        m_materialized_values.clear();
        m_block_addresses.clear();
        m_branch_fixups.clear();
        m_current_block_index = 0;
        m_current_instruction_position = 0;
        m_emission_position = 0;
        m_isInPlottingContext = false;

        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.result.isValid()) {
                    m_values[instruction.result.value] = &instruction;
                }
                for (const auto operand : instruction.operands) {
                    ++m_use_counts[operand.value];
                }
            }
        }
        buildRegisterAllocation(function);

        const auto function_offset = static_cast<std::uint32_t>(m_object_file.code_section.size());
        m_object_file.symbol_table.push_back({function.name, SymbolSection::CODE, function_offset});

        emitPush(GSUAbi::LinkRegister);
        emitPush(GSUAbi::FramePointerRegister);
        emitMove(GSUAbi::FramePointerRegister, GSUAbi::StackPointerRegister);
        if (function.total_local_alloc_size > 0) {
            emitAdjustStack(static_cast<std::size_t>(function.total_local_alloc_size), false,
                            Token(TokenType::UNKNOWN, "", 0, 0));
        }

        for (std::size_t block_index = 0; block_index < function.blocks.size(); ++block_index) {
            m_current_block_index = block_index;
            m_block_addresses[function.blocks[block_index].id.value] =
                m_object_file.code_section.size();
            emitBlock(function.blocks[block_index]);
        }
        patchBranches();
    }

    for (const auto& pair : m_data_manager.getEntries()) {
        const auto& entry = pair.second;
        const auto offset = static_cast<std::uint32_t>(m_object_file.data_section.size());
        m_object_file.symbol_table.push_back({entry.label, SymbolSection::DATA, offset});
        m_object_file.data_section.insert(m_object_file.data_section.end(),
                                          entry.bytes.begin(), entry.bytes.end());
    }
    return m_object_file;
}
