#include "IRCodeGenerator.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>

#include "CompilerError.hpp"
#include "ABI.hpp"
#include "Opcodes.hpp"

namespace {

struct NeedsLongBranch {};

std::string internalBlockSymbol(const std::string& function, IRBlockId block) {
    return std::string(1, '\x01') + function + "#" + std::to_string(block.value);
}

} // namespace

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
    throw CompilerError(message, source);
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
    // TO alone only selects a destination. WITH is required even for R0
    // to form an immediate register copy and clear the selection afterwards.
    emitByte(static_cast<std::uint8_t>(0x20 | source));
    emitByte(static_cast<std::uint8_t>(0x10 | destination));
}

void IRCodeGenerator::emitStackGuard(std::size_t required, const Token& source) {
    if (required > 65535u) fail("Stack requirement exceeds one bank.", source);
    const auto patch = m_object_file.code_section.size();
    emitRegisterLiteral(3, 0);
    addRelocation(std::string(GSUAbi::StackLimitPrefix) + std::to_string(required), patch, RelocationType::ADDR16_RAM);
    emitByte(0xba); emitByte(0x3f); emitByte(0x63);
    emitGuard(13, 2);
}

void IRCodeGenerator::emitPush(std::uint8_t reg) {
    emitStackGuard(2, Token(TokenType::UNKNOWN, "", 0, 0));
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
        // A scalar-only entry may call a checked callee from another unit.
        // Normal STOP always clears its volatile scratch/fault category.
        emitRegisterLiteral(GSUAbi::AddressFaultRegister, 0);
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
    m_register_allocator = LinearScanAllocator{};
    if (!m_checked_pointer_mode) m_register_allocator.run(function, {5, 7, 8});
}

bool IRCodeGenerator::usesRegisterAllocation(IRValueId value) const {
    if (m_spill_offsets.count(value.value)) return false;
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
        if (m_spill_offsets.count(pair.first) || !location.has_register || uses == m_use_counts.end() || uses->second <= 1 ||
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
    if (instruction.operation == "temporary") {
        emitMove(0, 9);
        emitImmediateArithmetic(0x60, -instruction.immediate, instruction.source);
        return;
    }
    // Indexing is the typed PointerOffset opcode, never an untyped legacy
    // string operation with its own competing stride/direction semantics.
    if (!instruction.operation.empty() && instruction.operation != "member")
        fail("IR codegen: unsupported address operation.", instruction.source);

    if (instruction.operation == "member") {
        if (instruction.operands.size() != 1) {
            fail("IR codegen: member address must have an object operand.", instruction.source);
        }
        materialize(instruction.operands[0]);
        if (instruction.immediate > 0) {
            if (m_checked_pointer_mode) {
                if (instruction.immediate > 65535) fail("IR codegen: member offset exceeds one bank.", instruction.source);
                emitCompare(0, static_cast<std::uint16_t>(65536 - instruction.immediate));
                emitGuard(12, 3);
            }
            emitMove(scratchRegister(), 0);
            emitLiteral(instruction.immediate);
            emitByte(static_cast<std::uint8_t>(0x50 | scratchRegister()));
        }
        return;
    }

    if (m_data_manager.hasSymbol(instruction.symbol) &&
        m_data_manager.getEntries().at(instruction.symbol).storage == AddressSpace::RAM) {
        const auto patch = m_object_file.code_section.size();
        emitRegisterLiteral(0, 0);
        addRelocation(m_data_manager.getEntries().at(instruction.symbol).link_name, patch, RelocationType::ADDR16_RAM);
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
            if (symbol->second.type.alignment > 2) {
                const auto alignment = symbol->second.type.alignment;
                emitImmediateArithmetic(0x50, alignment - 1, instruction.source);
                emitRegisterLiteral(3, static_cast<std::uint16_t>(~(alignment - 1)));
                emitByte(0x73); // AND R3 rounds this allocation's padded base upward.
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
        const auto name = m_data_manager.hasSymbol(instruction.symbol) ? m_data_manager.getEntries().at(instruction.symbol).link_name :
            m_global_function_symbols.at(instruction.symbol).link_name;
        addRelocation(name, patch_offset, RelocationType::ADDR16_IWT);
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
    if (instruction.type.base == BaseType::STRUCT && instruction.type.pointer_level == 0)
        fail("IR codegen: aggregate by-value loads are not supported.", instruction.source);
    const bool far_address = isFarPointer(address.type);
    if (m_checked_pointer_mode && !(address.opcode == IROpcode::Address &&
        address.operation.empty() && address.symbol_id.isValid()))
        emitAddressCheck(address.type, instruction.type.sizeInBytes);
    if (far_address) emitSelectBank(4, address.type.space);
    else if (address.type.space == AddressSpace::ROM) {
        emitNearBank(3, AddressSpace::ROM);
        emitSelectBank(3, AddressSpace::ROM);
    }
    emitMove(3, 0);
    const auto load_word = [&](std::uint8_t destination, bool byte) {
        if (address.type.space == AddressSpace::ROM) {
            emitMove(14, 3);
            emitByte(static_cast<std::uint8_t>(0x10 | destination));
            emitByte(0xef);
            if (!byte) {
                emitByte(0xde);
                // GETBH merges the high byte with the destination's low byte.
                emitByte(static_cast<std::uint8_t>(0xb0 | destination));
                emitByte(static_cast<std::uint8_t>(0x10 | destination));
                emitByte(0x3d); emitByte(0xef);
            }
        } else {
            emitByte(static_cast<std::uint8_t>(0x10 | destination));
            if (byte) emitByte(0x3d);
            emitByte(0x43);
        }
    };
    if (isFarPointer(instruction.type)) {
        load_word(4, false);
        emitByte(0xd3); emitByte(0xd3);
    }
    load_word(0, usesByteStorage(instruction.type));
    if (far_address) {
        emitNearBank(3, address.type.space);
        emitSelectBank(3, address.type.space);
    }
    if (instruction.type.pointer_level > 0) {
        const auto element = pointeeType(instruction.type);
        emitPointerValueCheck(instruction.type, element.sizeInBytes > 0 ? element.sizeInBytes : 1);
    } else if (instruction.type.base == BaseType::BOOL) emitBoolNormalization();
}

void IRCodeGenerator::emitBinary(const IRInstruction& instruction) {
    if (instruction.operands.size() != 2) {
        fail("IR codegen: binary instruction must have two operands.", instruction.source);
    }
    const auto left = instruction.operands[0];
    const auto right = instruction.operands[1];
    std::int64_t immediate = 0;
    const bool right_immediate = constantValue(right, immediate) && immediate >= 0 && immediate <= 15;
    const auto right_literal = immediate;
    const bool left_immediate = constantValue(left, immediate) && immediate >= 0 && immediate <= 15;

    std::uint8_t op_base = 0;
    if (instruction.operation == "+") op_base = 0x50;
    else if (instruction.operation == "-") op_base = 0x60;

    if (op_base != 0 && right_immediate) {
        materialize(left);
        emitByte(static_cast<std::uint8_t>(OpCode::ALT2));
        emitByte(static_cast<std::uint8_t>(op_base | right_literal));
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

    if (instruction.operation == "&" || instruction.operation == "|" || instruction.operation == "^" ||
        instruction.operation == "<<" || instruction.operation == ">>" || instruction.operation == "/" || instruction.operation == "%") {
        emitIntegerOperation(instruction);
        return;
    }
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

        const bool unsigned_comparison = producer(left, instruction.source).type.is_unsigned;
        const auto equal = static_cast<std::uint8_t>(OpCode::BEQ);
        const auto not_equal = static_cast<std::uint8_t>(OpCode::BNE);
        const auto less = static_cast<std::uint8_t>(
            unsigned_comparison ? OpCode::BCC : OpCode::BLT);
        const auto greater_equal = static_cast<std::uint8_t>(
            unsigned_comparison ? OpCode::BCS : OpCode::BGE);
        const auto false_label = new_label();

        if (instruction.operation == "==") {
            emit_local_branch(equal, true_label);
        } else if (instruction.operation == "!=") {
            emit_local_branch(not_equal, true_label);
        } else if (instruction.operation == "<") {
            emit_local_branch(less, true_label);
        } else if (instruction.operation == "<=") {
            emit_local_branch(less, true_label);
            emit_local_branch(equal, true_label);
        } else if (instruction.operation == ">") {
            emit_local_branch(equal, false_label);
            emit_local_branch(greater_equal, true_label);
        } else if (instruction.operation == ">=") {
            emit_local_branch(less, false_label);
            emit_local_branch(greater_equal, true_label);
        }
        labels[false_label] = m_object_file.code_section.size();
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
    if (instruction.operation == "*") {
        // MULT consumes only eight bits. LMULT's low word is the full
        // modular product for either signedness.
        emitMove(6, scratchRegister());
        emitByte(0x3d); emitByte(0x9f);
        emitMove(0, 4);
        return;
    }
    fail("IR codegen: unsupported binary operation '" + instruction.operation + "'.",
         instruction.source);
}

void IRCodeGenerator::emitBoolNormalization() {
    const auto zero = localLabel(), end = localLabel();
    emitCompare(0, 0);
    emitLocalJump(zero, 9);
    emitLiteral(1); emitLocalJump(end);
    bindLabel(zero); emitLiteral(0); bindLabel(end);
}

void IRCodeGenerator::emitCast(const IRInstruction& instruction) {
    if (instruction.operands.size() != 1) {
        fail("IR codegen: cast must have one operand.", instruction.source);
    }
    const auto& source = producer(instruction.operands[0], instruction.source);
    materialize(instruction.operands[0]);
    if (instruction.type.pointer_level > 0) {
        if (source.operation == "null") return;
        if (!needsPointerSpill(instruction)) return;
        const auto nonnull = localLabel(), done = localLabel();
        emitCompare(0, 0); emitLocalJump(nonnull, 8);
        if (isFarPointer(source.type)) { emitCompare(4, 0); emitLocalJump(nonnull, 8); }
        if (isFarPointer(instruction.type)) emitRegisterLiteral(4, 0);
        emitLocalJump(done);
        bindLabel(nonnull);
        if (isFarPointer(source.type) && !isFarPointer(instruction.type)) {
            emitNearBank(3, instruction.type.space);
            emitByte(0xb4); emitByte(0x3f); emitByte(0x63);
            emitGuard(9, 4);
        } else if (!isFarPointer(source.type) && isFarPointer(instruction.type)) {
            emitNearBank(4, instruction.type.space);
        }
        const auto element = pointeeType(instruction.type);
        emitAddressCheck(instruction.type, element.sizeInBytes > 0 ? element.sizeInBytes : 1);
        bindLabel(done);
        return;
    }
    if (instruction.type.base == BaseType::BOOL) {
        emitBoolNormalization();
    } else if (instruction.type.sizeInBytes == 1) {
        emitByte(0x9e); // Explicit narrowing discards the high bits.
        if (!instruction.type.is_unsigned) emitByte(0x95);
    } else if (instruction.type.sizeInBytes == 2 && source.type.sizeInBytes == 1 &&
        !source.type.is_unsigned) {
        emitByte(static_cast<std::uint8_t>(OpCode::SEX));
    }
}

void IRCodeGenerator::emitCall(const IRInstruction& instruction) {
    std::vector<std::uint8_t> saved_registers;
    saveLiveRegistersForCall(instruction, saved_registers);
    if (m_isInPlottingContext) { emitPush(1); emitPush(2); }
    std::size_t argument_bytes = 0;
    for (auto argument = instruction.operands.rbegin();
         argument != instruction.operands.rend(); ++argument) {
        materialize(*argument);
        emitPush(0);
        const bool far = isFarPointer(producer(*argument, instruction.source).type);
        if (far) emitPush(4); // bank is first in the callee's ascending parameter layout.
        argument_bytes += far ? 4 : 2;
    }
    emitByte(0x94);
    const auto patch_offset = m_object_file.code_section.size();
    emitByte(static_cast<std::uint8_t>(OpCode::IWT) | 0x0F);
    emitWord(0);
    addRelocation(instruction.symbol, patch_offset, RelocationType::ADDR16_JAL);
    // IWT R15 leaves the next opcode in the pipeline. LINK #4 returns after
    // this delay slot, so caller cleanup must begin after the NOP.
    emitByte(static_cast<std::uint8_t>(OpCode::NOP));
    if (instruction.operands.size() >
        std::numeric_limits<std::size_t>::max() / GSUAbi::ParameterSlotSize) {
        fail("IR codegen: call argument area is too large.", instruction.source);
    }
    emitAdjustStack(argument_bytes, true, instruction.source);
    if (m_isInPlottingContext) { emitPop(2); emitPop(1); }
    restoreRegistersAfterCall(saved_registers);
}

void IRCodeGenerator::emitStoreIndirect(const IRInstruction& instruction) {
    if (instruction.operands.size() != 2) {
        fail("IR codegen: indirect store must have address and value operands.", instruction.source);
    }
    const auto address = instruction.operands[0];
    const auto value = instruction.operands[1];
    const auto& target = producer(address, instruction.source);
    const bool byte = usesByteStorage(instruction.type);
    if (m_checked_pointer_mode) {
        const auto& pointer = producer(address, instruction.source).type;
        if (pointer.space == AddressSpace::ROM)
            fail("IR codegen: cannot store through a ROM address.", instruction.source);
        if (instruction.type.base == BaseType::STRUCT && instruction.type.pointer_level == 0)
            fail("IR codegen: aggregate by-value stores are not supported.", instruction.source);
        materialize(value);
        emitPush(0);
        if (isFarPointer(instruction.type)) emitPush(4);
        materialize(address);
        if (!(target.opcode == IROpcode::Address && target.operation.empty() && target.symbol_id.isValid()))
            emitAddressCheck(pointer, instruction.type.sizeInBytes);
        emitMove(3, 0);
        if (isFarPointer(pointer)) emitMove(6, 4);
        if (isFarPointer(instruction.type)) emitPop(4);
        emitPop(0);
        if (isFarPointer(pointer)) emitSelectBank(6, AddressSpace::RAM);
        if (isFarPointer(instruction.type)) {
            emitStore(3, 4, false);
            emitByte(0xd3); emitByte(0xd3);
        }
        emitStore(3, 0, byte);
        if (isFarPointer(pointer)) {
            emitNearBank(3, AddressSpace::RAM);
            emitSelectBank(3, AddressSpace::RAM);
        }
        return;
    }

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
    if (m_spill_offsets.count(value.value) && value.value != m_emitting_value.value) {
        emitSpill(value, true);
        return;
    }
    const auto* location = m_register_allocator.find(value);
    if (location != nullptr && !location->has_register && !location->rematerializable) {
        fail("IR codegen: value with observable effects requires a spill slot.",
             instruction.source);
    }
    if (usesRegisterAllocation(value) &&
        m_materialized_values.count(value.value) != 0) {
        emitMove(0, location->physical_register);
        return;
    }
    if (!m_active_values.insert(value.value).second) {
        fail("IR codegen: cyclic value definition.", instruction.source);
    }

    switch (instruction.opcode) {
        case IROpcode::PlotCoordinateRead:
            emitMove(0, instruction.immediate == 0 ? 1 : 2);
            break;
        case IROpcode::Rpix:
            emitByte(0x10); // Explicitly direct the read result to R0, never R1/R2.
            emitByte(0x3d); emitByte(0x4c);
            break;
        case IROpcode::Constant:
            if (isFarPointer(instruction.type)) {
                emitLiteral(instruction.immediate & 0xffff);
                emitRegisterLiteral(4, static_cast<std::uint16_t>(instruction.immediate >> 16));
            } else emitLiteral(instruction.immediate);
            break;
        case IROpcode::Address:
            emitAddress(instruction);
            break;
        case IROpcode::PointerCompare:
            emitPointerCompare(instruction);
            break;
        case IROpcode::PointerOffset:
            emitPointerOffset(instruction);
            break;
        case IROpcode::Load:
        case IROpcode::LoadIndirect:
            emitLoadIndirect(instruction);
            break;
        case IROpcode::Binary:
            emitBinary(instruction);
            break;
        case IROpcode::Unary: {
            if (instruction.operation == "!") {
                materialize(instruction.operands.front());
                emitByte(0x3f); emitByte(0xc1); // XOR #1, canonical bool.
                break;
            }
            if (instruction.operation == "~") {
                materialize(instruction.operands.front());
                emitByte(0x4f);
                if (usesByteStorage(instruction.type)) {
                    emitByte(0x9e);
                    if (!instruction.type.is_unsigned) emitByte(0x95);
                }
                break;
            }
            std::int64_t literal = 0;
            if (instruction.operation == "-" && constantValue(instruction.operands.front(), literal)) {
                emitLiteral(-literal);
            } else {
                materialize(instruction.operands.front());
                emitByte(0x4F);
                emitByte(0xD0);
            }
            if (usesByteStorage(instruction.type)) {
                emitByte(0x9e);
                if (!instruction.type.is_unsigned) emitByte(0x95);
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
    if (m_spill_offsets.count(value.value)) emitSpill(value, false);
    m_active_values.erase(value.value);
}

void IRCodeGenerator::emitBranch(IRBlockId target, const Token& source) {
    if (target.isValid() && target.value == m_current_block_index + 1) {
        return;
    }
    if (m_force_long_branches) {
        emitByte(static_cast<std::uint8_t>(OpCode::IWT) | GSUAbi::ProgramCounterRegister);
        const auto patch_offset = m_object_file.code_section.size();
        emitWord(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({patch_offset, target, source, true});
        return;
    }
    emitByte(static_cast<std::uint8_t>(OpCode::BRA));
    const auto patch_offset = m_object_file.code_section.size();
    emitByte(0);
    emitByte(static_cast<std::uint8_t>(OpCode::NOP));
    m_branch_fixups.push_back({patch_offset, target, source});
}

void IRCodeGenerator::emitBlockBranch(std::uint8_t opcode, IRBlockId target,
                                      const Token& source) {
    if (opcode == static_cast<std::uint8_t>(OpCode::BRA)) {
        emitBranch(target, source);
        return;
    }
    if (!m_force_long_branches) {
        emitByte(opcode);
        const auto patch_offset = m_object_file.code_section.size();
        emitByte(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({patch_offset, target, source});
        return;
    }

    static const std::map<std::uint8_t, std::uint8_t> inverse = {
        {static_cast<std::uint8_t>(OpCode::BEQ), static_cast<std::uint8_t>(OpCode::BNE)},
        {static_cast<std::uint8_t>(OpCode::BNE), static_cast<std::uint8_t>(OpCode::BEQ)},
        {static_cast<std::uint8_t>(OpCode::BGE), static_cast<std::uint8_t>(OpCode::BLT)},
        {static_cast<std::uint8_t>(OpCode::BLT), static_cast<std::uint8_t>(OpCode::BGE)},
        {static_cast<std::uint8_t>(OpCode::BPL), static_cast<std::uint8_t>(OpCode::BMI)},
        {static_cast<std::uint8_t>(OpCode::BMI), static_cast<std::uint8_t>(OpCode::BPL)},
        {static_cast<std::uint8_t>(OpCode::BCC), static_cast<std::uint8_t>(OpCode::BCS)},
        {static_cast<std::uint8_t>(OpCode::BCS), static_cast<std::uint8_t>(OpCode::BCC)}};
    const auto inverse_opcode = inverse.find(opcode);
    if (inverse_opcode == inverse.end()) {
        fail("IR codegen: unsupported long conditional branch.", source);
    }
    emitByte(inverse_opcode->second);
    emitByte(5);
    emitByte(static_cast<std::uint8_t>(OpCode::NOP));
    emitByte(static_cast<std::uint8_t>(OpCode::IWT) | GSUAbi::ProgramCounterRegister);
    const auto patch_offset = m_object_file.code_section.size();
    emitWord(0);
    emitByte(static_cast<std::uint8_t>(OpCode::NOP));
    m_branch_fixups.push_back({patch_offset, target, source, true});
}

void IRCodeGenerator::emitConditionalBranch(const IRInstruction& instruction) {
    if (instruction.operands.size() != 1 || instruction.targets.size() != 2) {
        fail("IR codegen: conditional branch shape is invalid.", instruction.source);
    }
    // Materialize every condition as 0 or 1. This keeps comparison semantics
    // in one place and also makes nested comparisons ordinary values.
    materialize(instruction.operands.front());
    emitByte(static_cast<std::uint8_t>(OpCode::IWT) | scratchRegister());
    emitWord(0);
    emitByte(static_cast<std::uint8_t>(OpCode::ALT3));
    emitByte(static_cast<std::uint8_t>(0x60 | scratchRegister()));
    if (m_force_long_branches) {
        emitByte(static_cast<std::uint8_t>(OpCode::BNE));
        emitByte(5);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        emitByte(static_cast<std::uint8_t>(OpCode::IWT) | GSUAbi::ProgramCounterRegister);
        const auto false_patch = m_object_file.code_section.size();
        emitWord(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({false_patch, instruction.targets[1], instruction.source, true});
    } else {
        emitByte(static_cast<std::uint8_t>(OpCode::BEQ));
        const auto false_patch = m_object_file.code_section.size();
        emitByte(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({false_patch, instruction.targets[1], instruction.source});
    }
    emitBranch(instruction.targets[0], instruction.source);
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
            emitBlockBranch(static_cast<std::uint8_t>(OpCode::BEQ),
                            instruction.targets[case_index], instruction.source);
        }
        if (default_index < instruction.targets.size()) {
            emitBlockBranch(static_cast<std::uint8_t>(OpCode::BRA),
                            instruction.targets[default_index], instruction.source);
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
        emitBlockBranch(opcode, target, instruction.source);
    };

    std::function<void(std::size_t, std::size_t, std::size_t)> emit_search;
    emit_search = [&](std::size_t begin, std::size_t end, std::size_t label) {
        labels[label] = m_object_file.code_section.size();
        const auto middle = begin + (end - begin) / 2;
        const auto case_index = order[middle];
        emitLiteral(instruction.case_values[case_index]);
        emitByte(static_cast<std::uint8_t>(OpCode::ALT3));
        emitByte(static_cast<std::uint8_t>(0x60 | scratchRegister()));
        emitBlockBranch(static_cast<std::uint8_t>(OpCode::BEQ),
                        instruction.targets[case_index], instruction.source);

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
        case IROpcode::PlotCoordinateWrite:
            materialize(instruction.operands.front());
            emitMove(instruction.immediate == 0 ? 1 : 2, 0);
            break;
        case IROpcode::StoreIndirect:
            emitStoreIndirect(instruction);
            break;
        case IROpcode::PlotBegin:
        case IROpcode::PlotEnd:
            // Context belongs to each IR instruction, not the order in which
            // blocks happen to be emitted (return/break can skip plot.end).
            break;
        case IROpcode::Plot:
            // PLOT reads the persistent cursor and increments R1 in hardware.
            emitByte(0x4C);
            break;
        case IROpcode::SetColor:
            materialize(instruction.operands.front());
            if (instruction.operation == "rom.byte") {
                const auto& address = producer(instruction.operands.front(), instruction.source).type;
                emitAddressCheck(address, 1);
                if (isFarPointer(address)) emitSelectBank(4, AddressSpace::ROM);
                else { emitNearBank(3, AddressSpace::ROM); emitSelectBank(3, AddressSpace::ROM); }
                emitMove(14, 0);
                // Writing R14 starts the ROM-buffer fetch. GETC synchronizes
                // with that buffer in hardware, just like GETB; no address operand.
                emitByte(0xdf);
                if (isFarPointer(address)) { emitNearBank(3, AddressSpace::ROM); emitSelectBank(3, AddressSpace::ROM); }
            } else emitByte(static_cast<std::uint8_t>(OpCode::COLOR_R));
            break;
        case IROpcode::CMode:
            if (m_known_plot_options != instruction.immediate) {
                emitLiteral(instruction.immediate);
                emitByte(static_cast<std::uint8_t>(OpCode::ALT1)); emitByte(0x4E);
                m_known_plot_options = static_cast<int>(instruction.immediate);
            }
            break;
        case IROpcode::Cache:
            emitByte(static_cast<std::uint8_t>(OpCode::CACHE));
            break;
        case IROpcode::Rpix:
            emitByte(0x10);
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
    // POR is unknown at a CFG join/backedge. Only elide straight-line repeats.
    m_known_plot_options = -1;
    m_materialized_values.clear();
    for (const auto& instruction : block.instructions) {
        m_current_instruction_position = m_emission_position++;
        m_isInPlottingContext = instruction.in_plot_context;
        if (instruction.opcode == IROpcode::Call) m_known_plot_options = -1;
        // A void call has no SSA result, but it is still a side effect that
        // must be emitted when it appears as an expression statement.
        if (instruction.opcode == IROpcode::Call &&
            isVoidValue(instruction.type)) {
            emitCall(instruction);
            continue;
        }
        if (instruction.producesValue()) {
            if (m_spill_offsets.count(instruction.result.value)) {
                m_emitting_value = instruction.result;
                materialize(instruction.result);
                m_emitting_value = IRValueId{};
                continue;
            }
            if (m_checked_pointer_mode) continue;
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
        if (fixup.long_form) {
            // Branch fixups point at the operand, but ADDR16_IWT relocations
            // point at the opcode and patch its following two bytes.
            addRelocation(internalBlockSymbol(m_current_function->name, fixup.target),
                          fixup.patch_offset - 1, RelocationType::ADDR16_IWT);
            continue;
        }
        const auto next_instruction = static_cast<std::int64_t>(fixup.patch_offset) + 1;
        const auto distance = static_cast<std::int64_t>(target->second) - next_instruction;
        if (distance < -128 || distance > 127) {
            throw NeedsLongBranch{};
        }
        m_object_file.code_section.at(fixup.patch_offset) =
            static_cast<std::uint8_t>(static_cast<std::int8_t>(distance));
    }
}

ObjectFile IRCodeGenerator::generateInternal(const IRModule& module) {
    m_object_file = ObjectFile();
    m_object_file.config = m_config;
    m_object_file.config.bitmap = module.bitmap;

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
        m_spill_offsets.clear();
        m_emitting_value = IRValueId{};
        m_local_label_serial = 0;
        m_checked_pointer_mode = function.return_type.pointer_level > 0;
        for (const auto& parameter : function.parameters)
            m_checked_pointer_mode = m_checked_pointer_mode || parameter.type.pointer_level > 0;

        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.memory_volatile || instruction.in_plot_context) m_checked_pointer_mode = true;
                if (instruction.result.isValid()) {
                    m_values[instruction.result.value] = &instruction;
                    if (instruction.opcode == IROpcode::PointerOffset ||
                        (instruction.type.pointer_level > 0 && instruction.opcode != IROpcode::Address) ||
                        instruction.type.pointer_level > 1 || isFarPointer(instruction.type))
                        m_checked_pointer_mode = true;
                }
                for (const auto operand : instruction.operands) {
                    ++m_use_counts[operand.value];
                }
            }
        }
        int frame_bytes = function.total_local_alloc_size;
        {
            for (const auto& value : m_values) {
                const auto opcode = value.second->opcode;
                const bool observable = opcode == IROpcode::PlotCoordinateRead || opcode == IROpcode::LoadIndirect || opcode == IROpcode::Load || opcode == IROpcode::Call || value.second->hardwareEffects().observable();
                const bool arithmetic_check = opcode == IROpcode::Binary &&
                    (value.second->operation == "/" || value.second->operation == "%" || value.second->operation == "<<" || value.second->operation == ">>");
                if (!observable && !arithmetic_check && !(m_checked_pointer_mode && needsPointerSpill(*value.second))) continue;
                const auto width = isFarPointer(value.second->type) ? 4 : 2;
                if (frame_bytes > 65526 - width)
                    fail("IR codegen: checked pointer frame exceeds one RAM bank.", value.second->source);
                frame_bytes += width;
                m_spill_offsets.emplace(value.first, -frame_bytes);
            }
            if (!m_spill_offsets.empty()) frame_bytes += 2; // Empty word below the last spill.
        }
        buildRegisterAllocation(function);

        const auto function_offset = static_cast<std::uint32_t>(m_object_file.code_section.size());
        m_object_file.symbol_table.push_back({function.link_name.empty() ? function.name : function.link_name, SymbolSection::CODE, function_offset});
        if (function.is_cached) emitByte(static_cast<std::uint8_t>(OpCode::CACHE));
        emitStackGuard(static_cast<std::size_t>(frame_bytes) + 4, Token(TokenType::UNKNOWN, function.name, 1, 1));
        if (m_checked_pointer_mode) {
            emitMove(0, 10);
            emitByte(0x3e); emitByte(0x71);
            emitGuard(9, 1);
            std::size_t parameters = 0;
            for (const auto& parameter : function.parameters)
                parameters += isFarPointer(parameter.type) ? 4 : 2;
            emitCompare(10, static_cast<std::uint16_t>(65535u - parameters));
            emitGuard(12, 2);
        }

        emitPush(GSUAbi::LinkRegister);
        emitPush(GSUAbi::FramePointerRegister);
        emitMove(GSUAbi::FramePointerRegister, GSUAbi::StackPointerRegister);
        if (frame_bytes > 0) {
            emitAdjustStack(static_cast<std::size_t>(frame_bytes), false,
                            Token(TokenType::UNKNOWN, "", 0, 0));
        }

        for (std::size_t block_index = 0; block_index < function.blocks.size(); ++block_index) {
            m_current_block_index = block_index;
            m_block_addresses[function.blocks[block_index].id.value] =
                m_object_file.code_section.size();
            m_object_file.symbol_table.push_back({
                internalBlockSymbol(function.name, function.blocks[block_index].id),
                SymbolSection::CODE,
                static_cast<std::uint32_t>(m_object_file.code_section.size())});
            emitBlock(function.blocks[block_index]);
        }
        patchBranches();
    }

    for (const auto& pair : m_data_manager.getEntries()) {
        const auto& entry = pair.second;
        if (entry.is_extern) continue;
        if (entry.storage == AddressSpace::RAM) {
            const auto alignment = static_cast<std::uint8_t>(std::max(2, entry.type.alignment));
            m_object_file.ram_alignment = std::max(m_object_file.ram_alignment, alignment);
            while (m_object_file.ram_section.size() % alignment) m_object_file.ram_section.push_back(0);
            if (entry.bytes.size() > 65536u - m_object_file.ram_section.size())
                fail("Static storage exceeds one RAM bank.", Token(TokenType::UNKNOWN, entry.label, 1, 1));
            m_object_file.symbol_table.push_back({entry.link_name, SymbolSection::RAM,
                static_cast<std::uint32_t>(m_object_file.ram_section.size())});
            m_object_file.ram_section.insert(m_object_file.ram_section.end(), entry.bytes.begin(), entry.bytes.end());
            continue;
        }
        m_object_file.data_alignment = 2;
        // Every compiler-emitted data object starts on a word boundary.
        // The linker also aligns concatenated DATA sections to two bytes.
        if (m_object_file.data_section.size() & 1u) m_object_file.data_section.push_back(0);
        const auto offset = static_cast<std::uint32_t>(m_object_file.data_section.size());
        m_object_file.symbol_table.push_back({entry.link_name, SymbolSection::DATA, offset});
        m_object_file.data_section.insert(m_object_file.data_section.end(),
                                          entry.bytes.begin(), entry.bytes.end());
    }
    return m_object_file;
}

ObjectFile IRCodeGenerator::generate(const IRModule& module) {
    m_force_long_branches = false;
    try {
        return generateInternal(module);
    } catch (const NeedsLongBranch&) {
        m_force_long_branches = true;
        return generateInternal(module);
    }
}
