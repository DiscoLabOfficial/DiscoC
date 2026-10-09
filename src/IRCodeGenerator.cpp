#include "IRCodeGenerator.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>

#include "CompilerError.hpp"
#include "ABI.hpp"
#include "Opcodes.hpp"
#include "IRLocalOptimizer.hpp"
#include "IRGlobalOptimizer.hpp"
#include "IRControlFlow.hpp"
#include "GSUMachineScheduler.hpp"
#include "GSUCostModel.hpp"

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
    m_accumulator_value = IRValueId{};
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
    emitRegisterLiteral(0, static_cast<std::uint16_t>(value));
}

void IRCodeGenerator::emitMove(std::uint8_t destination, std::uint8_t source) {
    m_spill_cache.clobber(destination);
    const auto cached = m_accumulator_value;
    if (destination == GSUAbi::StackPointerRegister) m_stack_check_credit.reset();
    // TO alone only selects a destination. WITH is required even for R0
    // to form an immediate register copy and clear the selection afterwards.
    emitByte(static_cast<std::uint8_t>(0x20 | source));
    emitByte(static_cast<std::uint8_t>(0x10 | destination));
    // A completed WITH/TO copy clears the selectors but changes only its
    // destination. Retain an R0 snapshot across copies out of R0, not across
    // arbitrary bytes (which might instead be operands or prefixes).
    if (globallyOptimized() && destination != 0) m_accumulator_value = cached;
}

void IRCodeGenerator::emitStackGuard(std::size_t required, const Token& source) {
    if (required > 65535u) fail("Stack requirement exceeds one bank.", source);
    if (globallyOptimized() && m_stack_check_credit.covers(required)) return;
    const auto patch = m_object_file.code_section.size();
    emitWordLiteral(3, 0); // The linker patches an IWT, never a shortened IBT.
    addRelocation(std::string(GSUAbi::StackLimitPrefix) + std::to_string(required), patch, RelocationType::ADDR16_RAM);
    emitByte(0xba); emitByte(0x3f); emitByte(0x63);
    emitGuard(13, 2);
    if (globallyOptimized()) m_stack_check_credit.checked(required);
}

void IRCodeGenerator::emitPush(std::uint8_t reg) {
    m_last_ram_word_address = IRValueId{};
    emitStackGuard(2, Token(TokenType::UNKNOWN, "", 0, 0));
    if (reg != 0) {
        emitByte(static_cast<std::uint8_t>(0x20 | reg));
    }
    emitByte(0x3A);
    emitByte(0xEA);
    emitByte(0xEA);
    if (globallyOptimized()) m_stack_check_credit.decrease(2);
}

void IRCodeGenerator::emitPop(std::uint8_t reg) {
    m_spill_cache.clobber(reg);
    m_last_ram_word_address = IRValueId{};
    emitByte(0xDA);
    emitByte(0xDA);
    if (reg != 0) {
        emitByte(static_cast<std::uint8_t>(0x20 | reg));
    }
    emitByte(0x4A);
    if (globallyOptimized()) m_stack_check_credit.increase(2);
}

void IRCodeGenerator::emitStore(std::uint8_t address_reg, std::uint8_t value_reg, bool byte) {
    m_last_ram_word_address = IRValueId{};
    if (value_reg != 0) {
        emitByte(static_cast<std::uint8_t>(0x20 | value_reg));
    }
    if (byte) {
        emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
    }
    emitByte(static_cast<std::uint8_t>(0x30 | address_reg));
}

bool IRCodeGenerator::matchesLastRamWordAddress(IRValueId address) const {
    if (!m_last_ram_word_address.isValid()) return false;
    if (address.value == m_last_ram_word_address.value) return true;
    const auto& previous = producer(m_last_ram_word_address, Token(TokenType::UNKNOWN, "", 0, 0));
    const auto& current = producer(address, Token(TokenType::UNKNOWN, "", 0, 0));
    // Fresh Address nodes for the same declaration are equivalent. Never
    // equate distinct pointer loads: an alias may have changed the pointer.
    return previous.opcode == IROpcode::Address && current.opcode == IROpcode::Address &&
        previous.operation.empty() && current.operation.empty() &&
        previous.symbol_id.isValid() && previous.symbol_id == current.symbol_id;
}

void IRCodeGenerator::emitImmediateArithmetic(std::uint8_t op_base,
                                               std::int64_t value,
                                               const Token& source) {
    if (value < 0) {
        fail("IR codegen: immediate arithmetic value cannot be negative.", source);
    }
    auto remaining = static_cast<std::uint64_t>(value);
    if (optimized() && remaining > 30 && remaining <= 65535) {
        emitRegisterLiteral(3, static_cast<std::uint16_t>(remaining));
        emitByte(static_cast<std::uint8_t>(op_base | 3));
        return;
    }
    while (remaining > 0) {
        const auto chunk = static_cast<std::uint8_t>(std::min<std::uint64_t>(remaining, 15));
        emitByte(static_cast<std::uint8_t>(OpCode::ALT2));
        emitByte(static_cast<std::uint8_t>(op_base | chunk));
        remaining -= chunk;
    }
}

void IRCodeGenerator::emitAdjustStack(std::size_t bytes, bool add, const Token& source) {
    if (globallyOptimized()) {
        if (add) m_stack_check_credit.increase(bytes);
        else m_stack_check_credit.decrease(bytes);
    }
    const auto op_base = static_cast<std::uint8_t>(add ? 0x50 : 0x60);
    if (optimized() && bytes > 15 && bytes <= 65535) {
        emitRegisterLiteral(3, static_cast<std::uint16_t>(bytes));
        emitByte(0x2a);
        emitByte(static_cast<std::uint8_t>(op_base | 3));
        return;
    }
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
    if (globallyOptimized()) {
        if (!m_epilogue_label.empty()) { emitLocalJump(m_epilogue_label); return; }
        m_epilogue_label = localLabel();
        bindLabel(m_epilogue_label);
    }
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
    if (globallyOptimized()) { m_register_allocator.runGlobal(function, {5, 7, 8}, m_allocation_policy); return; }
    // R0 is the expression accumulator, R1/R3 are backend temporaries, R9 is
    // the frame pointer, and R10-R15 have ABI or hardware roles. R5, R7, and
    // R8 are general-purpose registers for the current backend operations.
    m_register_allocator = LinearScanAllocator{};
    if (optimized()) m_register_allocator.runEager(function, {5, 7, 8});
    else if (!m_checked_pointer_mode) {
        for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
            if (instruction.opcode == IROpcode::HardwareLoop) return;
        m_register_allocator.run(function, {5, 7, 8});
    }
}

bool IRCodeGenerator::usesRegisterAllocation(IRValueId value) const {
    if (m_spill_offsets.count(value.value)) return false;
    const auto location = m_register_allocator.find(value);
    const auto uses = m_use_counts.find(value.value);
    return location != nullptr && location->has_register &&
           uses != m_use_counts.end() && (optimized() || uses->second > 1);
}

void IRCodeGenerator::saveLiveRegistersForCall(
    const IRInstruction& instruction,
    std::vector<std::uint8_t>& saved_registers) {
    std::set<std::uint8_t> unique_registers;
    for (const auto& pair : m_register_allocator.locations()) {
        const auto& location = pair.second;
        const auto uses = m_use_counts.find(pair.first);
        if (m_spill_offsets.count(pair.first) || !location.has_register || uses == m_use_counts.end() || (!optimized() && uses->second <= 1) ||
            (globallyOptimized() ? !m_register_allocator.liveAcrossCall(IRValueId{pair.first}, m_current_instruction_position) :
             (location.start > m_current_instruction_position ||
              (optimized() ? location.end <= m_current_instruction_position : location.end < m_current_instruction_position))) ||
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
            if (m_checked_pointer_mode && !(provenAddress(instruction.result, instruction.type, 1) &&
                provenAddress(instruction.operands[0], producer(instruction.operands[0], instruction.source).type, 1))) {
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
        emitWordLiteral(0, 0);
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
    m_last_ram_word_address = IRValueId{};
    if (instruction.operands.size() != 1) {
        fail("IR codegen: indirect load must have one address operand.", instruction.source);
    }
    const auto& address = producer(instruction.operands[0], instruction.source);
    materialize(instruction.operands[0]);
    m_last_ram_word_address = IRValueId{};
    if (instruction.type.base == BaseType::STRUCT && instruction.type.pointer_level == 0)
        fail("IR codegen: aggregate by-value loads are not supported.", instruction.source);
    const bool far_address = isFarPointer(address.type);
    if (m_checked_pointer_mode && !(address.opcode == IROpcode::Address &&
        address.operation.empty() && address.symbol_id.isValid()) &&
        !provenAddress(instruction.operands[0], address.type, instruction.type.sizeInBytes))
        emitAddressCheck(address.type, instruction.type.sizeInBytes);
    if (far_address) emitSelectBank(4, address.type.space);
    else if (address.type.space == AddressSpace::ROM) {
        emitNearBank(3, AddressSpace::ROM);
        emitSelectBank(3, AddressSpace::ROM);
    }
    if (optimized() && address.type.space == AddressSpace::RAM && !far_address && !isFarPointer(instruction.type)) {
        // LOAD samples its address register before writing the destination.
        // R0 can therefore carry both without copying it through R3.
        if (usesByteStorage(instruction.type)) emitByte(0x3d);
        emitByte(0x40);
        if (instruction.type.pointer_level > 0) {
            const auto element = pointeeType(instruction.type);
            emitPointerValueCheck(instruction.type, element.sizeInBytes > 0 ? element.sizeInBytes : 1);
        } else if (instruction.type.base == BaseType::BOOL) emitBoolNormalization();
        if (!usesByteStorage(instruction.type) && instruction.type.sizeInBytes == 2)
            m_last_ram_word_address = instruction.operands[0];
        return;
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

void IRCodeGenerator::emitBinaryOperands(IRValueId left, IRValueId right) {
    std::int64_t immediate = 0;
    if (optimized() && constantValue(right, immediate) &&
        producer(right, Token(TokenType::UNKNOWN, "", 0, 0)).type.pointer_level == 0) {
        materialize(left);
        emitRegisterLiteral(scratchRegister(), static_cast<std::uint16_t>(immediate));
        return;
    }
    const auto* right_location = m_register_allocator.find(right);
    if (optimized() && right_location && right_location->has_register) {
        materialize(left);
        emitMove(scratchRegister(), right_location->physical_register);
    } else if (optimized()) {
        materialize(right); emitMove(6, 0);
        materialize(left); emitMove(scratchRegister(), 6);
    } else {
        materialize(right); emitPush(0);
        materialize(left); emitPop(scratchRegister());
    }
}

void IRCodeGenerator::emitBinary(const IRInstruction& instruction) {
    if (instruction.operands.size() != 2) {
        fail("IR codegen: binary instruction must have two operands.", instruction.source);
    }
    const auto left = instruction.operands[0];
    const auto right = instruction.operands[1];
    if (optimized() && emitConstantArithmetic(instruction)) return;
    std::int64_t immediate = 0;
    const bool right_immediate = constantValue(right, immediate) && immediate >= 0 && immediate <= 15;
    const auto right_literal = immediate;
    const bool left_immediate = constantValue(left, immediate) && immediate >= 0 && immediate <= 15;

    std::uint8_t op_base = 0;
    if (instruction.operation == "+") op_base = 0x50;
    else if (instruction.operation == "-") op_base = 0x60;

    if (op_base != 0 && right_immediate) {
        materialize(left);
        if (optimized() && right_literal == 1) {
            emitByte(op_base == 0x50 ? 0xd0 : 0xe0);
            return;
        }
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

    if (optimized() && constantValue(right, immediate)) {
        if ((instruction.operation == "&" || instruction.operation == "|" || instruction.operation == "^") &&
            immediate >= 1 && immediate <= 15) {
            materialize(left);
            emitByte(instruction.operation == "^" ? 0x3f : 0x3e);
            emitByte(static_cast<std::uint8_t>((instruction.operation == "&" ? 0x70 : 0xc0) | immediate));
            return;
        }
        if ((instruction.operation == "<<" || instruction.operation == ">>") && immediate >= 0 && immediate < 16) {
            materialize(left);
            bool nonnegative = instruction.type.is_unsigned;
            const auto& input = producer(left, instruction.source);
            if (input.opcode == IROpcode::Binary && input.operation == "&") {
                std::int64_t mask = 0;
                for (const auto operand : input.operands)
                    if (constantValue(operand, mask) && (static_cast<std::uint64_t>(mask) & 0x8000u) == 0)
                        nonnegative = true;
            }
            emitConstantShift(instruction.operation == "<<", static_cast<unsigned>(immediate), nonnegative);
            return;
        }
    }

    emitBinaryOperands(left, right);

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
        const auto element = pointeeType(instruction.type);
        if (provenAddress(instruction.result, instruction.type, element.sizeInBytes > 0 ? element.sizeInBytes : 1)) return;
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
    m_spill_cache.clear();
    m_last_ram_word_address = IRValueId{};
    std::vector<std::uint8_t> saved_registers;
    saveLiveRegistersForCall(instruction, saved_registers);
    if (globallyOptimized() && m_has_hardware_loop) { emitPush(12); emitPush(13); }
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
    // Even a no-argument call can perform arbitrary RAM accesses in its callee.
    m_last_ram_word_address = IRValueId{};
    m_stack_check_credit.reset();
    if (instruction.operands.size() >
        std::numeric_limits<std::size_t>::max() / GSUAbi::ParameterSlotSize) {
        fail("IR codegen: call argument area is too large.", instruction.source);
    }
    emitAdjustStack(argument_bytes, true, instruction.source);
    if (m_isInPlottingContext) { emitPop(2); emitPop(1); }
    if (globallyOptimized() && m_has_hardware_loop) { emitPop(13); emitPop(12); }
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
    if (optimized() || m_checked_pointer_mode) {
        if (target.type.space == AddressSpace::ROM)
            fail("IR codegen: cannot store through a ROM address.", instruction.source);
        if (instruction.type.base == BaseType::STRUCT && instruction.type.pointer_level == 0)
            fail("IR codegen: aggregate by-value stores are not supported.", instruction.source);
    }
    if (optimized() && !isFarPointer(target.type) && !isFarPointer(instruction.type)) {
        const auto* location = m_register_allocator.find(value);
        const auto value_reg = static_cast<std::uint8_t>(location && location->has_register ? location->physical_register : 0);
        if (value_reg == 0) materialize(value);
        // Test after materializing the value: a spill/reload may have replaced
        // the address latch. SBK is always 16-bit, including after byte loads.
        const bool word = !byte && instruction.type.sizeInBytes == 2;
        if (word && matchesLastRamWordAddress(address)) {
            if (value_reg != 0) emitByte(static_cast<std::uint8_t>(0x20 | value_reg));
            emitByte(0x90); // SBK: same checked address/bank, no address reload.
            return;
        }
        const auto* address_location = m_register_allocator.find(address);
        if (globallyOptimized() && address_location && address_location->has_register &&
            provenAddress(address, target.type, instruction.type.sizeInBytes)) {
            // The address is already live in an allocatable RAM-address
            // register. STW/STB reads it before consuming the selected value.
            emitStore(address_location->physical_register, value_reg, byte);
            m_last_ram_word_address = word ? address : IRValueId{};
            return;
        }
        const auto reg = static_cast<std::uint8_t>(value_reg == 0 ? 6 : value_reg);
        if (value_reg == 0) emitMove(6, 0);
        materialize(address);
        if (!(target.opcode == IROpcode::Address && target.operation.empty() && target.symbol_id.isValid()) &&
            !provenAddress(address, target.type, instruction.type.sizeInBytes))
            emitAddressCheck(target.type, instruction.type.sizeInBytes);
        emitStore(0, reg, byte);
        if (word) m_last_ram_word_address = address;
        return;
    }
    if (m_checked_pointer_mode) {
        const auto& pointer = producer(address, instruction.source).type;
        materialize(value);
        emitPush(0);
        if (isFarPointer(instruction.type)) emitPush(4);
        materialize(address);
        if (!(target.opcode == IROpcode::Address && target.operation.empty() && target.symbol_id.isValid()) &&
            !provenAddress(address, pointer, instruction.type.sizeInBytes))
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
    m_last_ram_word_address = IRValueId{};
    if (instruction.operands.size() != 1) {
        fail("IR codegen: hardware loop must have a count operand.", instruction.source);
    }
    // The body has an implicit backedge: straight-line state before its first
    // iteration cannot justify eliding COLOR/CMODE on subsequent iterations.
    m_known_color = m_known_plot_options = -1;
    if (!instruction.targets.empty()) { emitPush(12); emitPush(13); }
    std::int64_t count = 0;
    if (constantValue(instruction.operands[0], count)) {
        emitRegisterLiteral(12, static_cast<std::uint16_t>(count));
    } else {
        materialize(instruction.operands[0]);
        emitMove(12, 0);
    }
    // TO executes with R15 at the prefetched first body byte. Copy PC to R13,
    // never R13 to PC; LOOP executes its prefetched NOP before returning here.
    if (instruction.targets.empty()) emitMove(13, 15);
    else {
        const auto patch = m_object_file.code_section.size();
        emitWordLiteral(13, 0);
        addRelocation(internalBlockSymbol(m_current_function->name, instruction.loop_target), patch, RelocationType::ADDR16_IWT);
        if (instruction.compiler_generated_loop && !m_manual_cache && count >= 8) {
            m_auto_caches.push_back({m_object_file.code_section.size(), instruction.targets.front(), instruction.loop_id,
                static_cast<std::uint32_t>(count)});
            emitByte(1); // Same selector reset as CACHE; decide after layout.
        }
        emitBranch(instruction.targets.front(), instruction.source);
    }
    // The next emitted byte is also the hardware backedge target. Its first
    // iteration cannot lend an address-latch assumption to later iterations.
    m_last_ram_word_address = IRValueId{};
    m_stack_check_credit.reset();
}

void IRCodeGenerator::materialize(IRValueId value) {
    const auto& instruction = producer(value, Token(TokenType::UNKNOWN, "", 0, 0));
    const bool scalar_snapshot = optimized() && instruction.type.pointer_level == 0 &&
        (instruction.type.base == BaseType::BYTE || instruction.type.base == BaseType::WORD || instruction.type.base == BaseType::BOOL);
    if (scalar_snapshot && value.value != m_emitting_value.value && m_accumulator_value.value == value.value) return;
    if (m_spill_offsets.count(value.value) && value.value != m_emitting_value.value) {
        const auto copy = globallyOptimized() && scalar_snapshot ? m_spill_cache.find(value) : -1;
        if (copy >= 0) emitMove(0, static_cast<std::uint8_t>(copy));
        else { emitSpill(value, true); retainSpillCopy(value, true); }
        if (scalar_snapshot) m_accumulator_value = value;
        return;
    }
    const auto* location = m_register_allocator.find(value);
    if (scalar_snapshot && value.value == m_emitting_value.value && usesRegisterAllocation(value) &&
        emitAllocatedOperation(instruction, location->physical_register)) {
        // Eager emission has no R0-result contract: later consumers load the
        // allocated register. Do not claim an accumulator snapshot here.
        m_materialized_values.insert(value.value);
        return;
    }
    if (optimized() && value.value != m_emitting_value.value && location && location->has_register) {
        emitMove(0, location->physical_register);
        if (scalar_snapshot) m_accumulator_value = value;
        return;
    }
    if (!optimized() && location != nullptr && !location->has_register && !location->rematerializable) {
        fail("IR codegen: value with observable effects requires a spill slot.",
             instruction.source);
    }
    if (usesRegisterAllocation(value) &&
        m_materialized_values.count(value.value) != 0) {
        emitMove(0, location->physical_register);
        if (scalar_snapshot) m_accumulator_value = value;
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
            m_last_ram_word_address = IRValueId{};
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
        case IROpcode::DivMod:
            emitDivMod(instruction);
            break;
        case IROpcode::DivModResult:
            emitDivModResult(instruction);
            break;
        case IROpcode::BitExtract:
            emitBitExtract(instruction);
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
    if (m_spill_offsets.count(value.value)) { emitSpill(value, false); retainSpillCopy(value, false); }
    m_active_values.erase(value.value);
    if (scalar_snapshot) m_accumulator_value = value;
}

void IRCodeGenerator::emitPhiCopies(IRBlockId target, const Token& source) {
    m_spill_cache.clear();
    struct Move { IRValueId destination, source; bool saved = false; };
    std::vector<Move> moves;
    const auto same = [&](IRValueId a, IRValueId b) {
        if (a.value == b.value) return true;
        const auto* x = m_register_allocator.find(a);
        const auto* y = m_register_allocator.find(b);
        if (x && y && x->has_register && y->has_register) return x->physical_register == y->physical_register;
        const auto sx = m_spill_offsets.find(a.value), sy = m_spill_offsets.find(b.value);
        return sx != m_spill_offsets.end() && sy != m_spill_offsets.end() && sx->second == sy->second;
    };
    for (const auto& phi : m_current_function->blocks.at(target.value).instructions) {
        if (phi.opcode != IROpcode::Phi) break;
        if (!m_use_counts[phi.result.value]) continue;
        for (std::size_t e = 0; e < phi.targets.size(); ++e)
            if (phi.targets[e].value == m_current_block_index && !same(phi.result, phi.operands[e]))
                moves.push_back({phi.result, phi.operands[e], false});
    }
    const bool register_only = std::all_of(moves.begin(), moves.end(), [&](const Move& move) {
        const auto* destination = m_register_allocator.find(move.destination);
        const auto* input = m_register_allocator.find(move.source);
        return destination && input && destination->has_register && input->has_register;
    });
    while (!moves.empty()) {
        std::size_t ready = moves.size();
        for (std::size_t m = 0; m < moves.size(); ++m) {
            bool needed = false;
            for (std::size_t other = 0; other < moves.size(); ++other)
                if (m != other && !moves[other].saved && same(moves[m].destination, moves[other].source)) needed = true;
            if (!needed) { ready = m; break; }
        }
        if (ready == moves.size()) {
            // R6 is volatile edge scratch, never an allocated value/cursor.
            // A register-only cycle cannot clobber it while copying its other
            // edges. Mixed memory/rematerialization cycles keep checked pushes.
            const auto saved = moves.front().destination;
            if (register_only) emitMove(6, m_register_allocator.find(saved)->physical_register);
            else materialize(saved);
            for (auto& move : moves) if (!move.saved && same(saved, move.source)) {
                if (!register_only) emitPush(0);
                move.saved = true;
            }
            m_accumulator_value = IRValueId{};
            continue;
        }
        const auto move = moves[ready];
        const auto* destination = m_register_allocator.find(move.destination);
        const auto* input = m_register_allocator.find(move.source);
        if (destination && destination->has_register && ((move.saved && register_only) ||
            (!move.saved && input && input->has_register))) {
            emitMove(destination->physical_register, move.saved ? 6 : input->physical_register);
        } else if (optimized() && !move.saved && input && input->has_register &&
                   m_spill_offsets.count(move.destination.value) &&
                   !isFarPointer(producer(move.destination, source).type)) {
            emitWordSpillStore(move.destination, input->physical_register);
        } else {
            if (move.saved) emitPop(0); else materialize(move.source);
            if (destination && destination->has_register) emitMove(destination->physical_register, 0);
            else if (m_spill_offsets.count(move.destination.value)) emitSpill(move.destination, false);
            else fail("IR codegen: live phi has no register or spill slot.", source);
        }
        m_accumulator_value = IRValueId{}; m_last_ram_word_address = IRValueId{};
        moves.erase(moves.begin() + static_cast<std::ptrdiff_t>(ready));
    }
}

void IRCodeGenerator::emitMemoryInitialize(const IRInstruction& instruction) {
    const auto& root = producer(instruction.operands.at(0), instruction.source);
    const auto symbols = m_all_local_symbols.find(m_current_function->name);
    if (symbols == m_all_local_symbols.end()) fail("IR codegen: initializer has no local table.", instruction.source);
    const auto found = symbols->second.find(root.symbol_id);
    if (found == symbols->second.end() || found->second.stackOffset >= 0 || found->second.type.is_volatile ||
        found->second.type.pointer_level || found->second.type.array_size < 4 ||
        static_cast<std::int64_t>(found->second.type.array_size) * found->second.type.sizeInBytes != instruction.immediate ||
        found->second.type.base != instruction.type.base)
        fail("IR codegen: initializer is not a proven local array.", instruction.source);
    const auto& values = instruction.initialization_values;
    const bool uniform = values.size() >= 32 && std::all_of(values.begin(), values.end(),
        [&](std::uint16_t v) { return v == values[0]; });
    m_spill_cache.clear(); m_last_ram_word_address = IRValueId{};
    // Preserve the ABI loop registers before calculating R3: a stack guard
    // can itself use R3. No saved SSA value or plotting cursor is overwritten.
    if (uniform) { emitPush(12); emitPush(13); }
    emitAddress(root); emitMove(3, 0);
    const bool byte = usesByteStorage(instruction.type);
    if (uniform) {
        emitRegisterLiteral(12, static_cast<std::uint16_t>(values.size()));
        const auto label = localLabel();
        const auto patch = m_object_file.code_section.size();
        emitWordLiteral(13, 0); addRelocation(label, patch, RelocationType::ADDR16_IWT);
        emitLiteral(values[0]);
        // Small, completely known emitted body. Fetch savings repay all cold
        // cache lines even at the worst final 16-byte alignment.
        if (!m_manual_cache) emitByte(static_cast<std::uint8_t>(OpCode::CACHE));
        bindLabel(label);
        emitStore(3, 0, byte); emitByte(0xd3); if (!byte) emitByte(0xd3);
        emitByte(0x3c); emitByte(1);
        emitPop(13); emitPop(12);
    } else {
        for (std::size_t n = 0; n < values.size(); ++n) {
            if (n == 0 || values[n] != values[n - 1]) emitLiteral(values[n]);
            emitStore(3, 0, byte);
            if (n + 1 < values.size()) { emitByte(0xd3); if (!byte) emitByte(0xd3); }
        }
    }
    m_accumulator_value = m_last_ram_word_address = IRValueId{};
}

void IRCodeGenerator::selectAutomaticCaches() {
    if (m_auto_caches.empty()) return;
    const IRControlFlow cfg(*m_current_function);
    for (const auto& candidate : m_auto_caches) {
        for (const auto& loop : cfg.loops) if (loop.header == candidate.header.value) {
            std::size_t end = candidate.offset;
            bool ordered = true;
            for (const auto block : loop.blocks) {
                const auto start = m_block_addresses.at(block);
                ordered = ordered && start > candidate.offset;
                const auto next = static_cast<std::size_t>(block) + 1;
                end = std::max(end, next < m_current_function->blocks.size() ? m_block_addresses.at(static_cast<std::uint32_t>(next)) :
                    m_object_file.code_section.size());
            }
            const auto bytes = end - candidate.offset;
            // The linker can place this object at any byte alignment. Reserve
            // 15 bytes, not just the current object's observed low PC bits.
            const auto cold = 80u * ((bytes + 30u) / 16u);
            if (ordered && bytes <= 496 && static_cast<std::uint64_t>(candidate.trips - 1) * bytes * 4 > cold + 80)
                m_object_file.code_section.at(candidate.offset) = static_cast<std::uint8_t>(OpCode::CACHE);
        }
    }
}

void IRCodeGenerator::emitBranch(IRBlockId target, const Token& source) {
    if (globallyOptimized()) emitPhiCopies(target, source);
    if (target.isValid() && target.value == m_current_block_index + 1) {
        return;
    }
    const auto serial = m_branch_serial++;
    if (longBranch(serial)) {
        emitByte(static_cast<std::uint8_t>(OpCode::IWT) | GSUAbi::ProgramCounterRegister);
        const auto patch_offset = m_object_file.code_section.size();
        emitWord(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({patch_offset, target, source, true, {}, serial});
        return;
    }
    emitByte(static_cast<std::uint8_t>(OpCode::BRA));
    const auto patch_offset = m_object_file.code_section.size();
    emitByte(0);
    emitByte(static_cast<std::uint8_t>(OpCode::NOP));
    m_branch_fixups.push_back({patch_offset, target, source, false, {}, serial});
}

bool IRCodeGenerator::longBranch(std::size_t serial) const {
    const auto found = m_long_branches.find(m_current_function->name);
    return m_force_long_branches || (found != m_long_branches.end() && found->second.count(serial));
}

void IRCodeGenerator::emitBlockBranch(std::uint8_t opcode, IRBlockId target,
                                      const Token& source) {
    if (opcode == static_cast<std::uint8_t>(OpCode::BRA)) {
        emitBranch(target, source);
        return;
    }
    const auto serial = m_branch_serial++;
    if (!longBranch(serial)) {
        emitByte(opcode);
        const auto patch_offset = m_object_file.code_section.size();
        emitByte(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({patch_offset, target, source, false, {}, serial});
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
    m_branch_fixups.push_back({patch_offset, target, source, true, {}, serial});
}

void IRCodeGenerator::emitConditionalBranch(const IRInstruction& instruction) {
    if (instruction.operands.size() != 1 || instruction.targets.size() != 2) {
        fail("IR codegen: conditional branch shape is invalid.", instruction.source);
    }
    if (optimized() && m_branch_comparisons.count(instruction.operands.front().value)) {
        const auto& comparison = producer(instruction.operands.front(), instruction.source);
        emitBinaryOperands(comparison.operands[0], comparison.operands[1]);
        emitByte(0x3f); emitByte(static_cast<std::uint8_t>(0x60 | scratchRegister()));
        const bool is_unsigned = producer(comparison.operands[0], instruction.source).type.is_unsigned;
        const std::uint8_t less = is_unsigned ? 12 : 7;
        const std::uint8_t ge = is_unsigned ? 13 : 6;
        const auto yes = instruction.targets[0], no = instruction.targets[1];
        const auto& op = comparison.operation;
        if (op == "<=") {
            emitBlockBranch(less, yes, instruction.source);
            emitBlockBranch(9, yes, instruction.source);
            emitBranch(no, instruction.source);
        } else if (op == ">") {
            emitBlockBranch(less, no, instruction.source);
            emitBlockBranch(9, no, instruction.source);
            emitBranch(yes, instruction.source);
        } else {
            const std::uint8_t false_opcode = op == "<" ? ge : op == ">=" ? less : op == "==" ? 8 : 9;
            if (no.value == m_current_block_index + 1) {
                emitBlockBranch(static_cast<std::uint8_t>(false_opcode ^ 1u), yes, instruction.source);
            } else {
                emitBlockBranch(false_opcode, no, instruction.source);
                emitBranch(yes, instruction.source);
            }
        }
        return;
    }
    // Materialize every condition as 0 or 1. This keeps comparison semantics
    // in one place and also makes nested comparisons ordinary values.
    materialize(instruction.operands.front());
    if (optimized()) {
        emitRegisterLiteral(scratchRegister(), 0);
        emitByte(0x3f); emitByte(static_cast<std::uint8_t>(0x60 | scratchRegister()));
        const auto yes = instruction.targets[0], no = instruction.targets[1];
        if (no.value == m_current_block_index + 1) emitBlockBranch(8, yes, instruction.source);
        else { emitBlockBranch(9, no, instruction.source); emitBranch(yes, instruction.source); }
        return;
    }
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
        m_branch_fixups.push_back({false_patch, instruction.targets[1], instruction.source, true, {}, 0});
    } else {
        emitByte(static_cast<std::uint8_t>(OpCode::BEQ));
        const auto false_patch = m_object_file.code_section.size();
        emitByte(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({false_patch, instruction.targets[1], instruction.source, false, {}, 0});
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
        m_stack_check_credit.reset(); // Each search node is a control-flow entry.
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
    m_stack_check_credit.reset(); // Manual comparison/switch joins also fence credit.
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
        case IROpcode::MemoryInitialize:
            emitMemoryInitialize(instruction);
            break;
        case IROpcode::PlotBegin:
        case IROpcode::PlotEnd:
            // Context belongs to each IR instruction, not the order in which
            // blocks happen to be emitted (return/break can skip plot.end).
            break;
        case IROpcode::Plot:
            m_last_ram_word_address = IRValueId{};
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
                m_known_color = -1;
            } else {
                std::int64_t color = 0;
                const bool constant = constantValue(instruction.operands.front(), color);
                const auto byte = static_cast<int>(static_cast<std::uint16_t>(color) & 255u);
                if (!optimized() || !constant || m_known_color != byte)
                    emitByte(static_cast<std::uint8_t>(OpCode::COLOR_R));
                m_known_color = constant ? byte : -1;
            }
            break;
        case IROpcode::CMode:
            if (m_known_plot_options != instruction.immediate) {
                emitLiteral(instruction.immediate);
                emitByte(static_cast<std::uint8_t>(OpCode::ALT1)); emitByte(0x4E);
                m_known_plot_options = static_cast<int>(instruction.immediate);
                m_known_color = -1;
            }
            break;
        case IROpcode::Cache:
            emitByte(static_cast<std::uint8_t>(OpCode::CACHE));
            break;
        case IROpcode::Rpix:
            emitByte(0x10);
            m_last_ram_word_address = IRValueId{};
            emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
            emitByte(0x4C);
            break;
        case IROpcode::HardwareLoop:
            emitHardwareLoop(instruction);
            break;
        case IROpcode::HardwareLoopEnd:
            m_last_ram_word_address = IRValueId{};
            emitByte(0x3C);
            emitByte(static_cast<std::uint8_t>(OpCode::NOP));
            if (!instruction.targets.empty()) emitBranch(instruction.targets[1], instruction.source);
            m_known_color = m_known_plot_options = -1;
            break;
        case IROpcode::HardwareLoopLeave:
            emitPop(13); emitPop(12);
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
    m_spill_cache.clear();
    m_block_remaining_uses.clear();
    for (const auto& i : block.instructions) if (i.opcode != IROpcode::Phi)
        for (const auto v : i.operands) ++m_block_remaining_uses[v.value];
    m_stack_check_credit.reset();
    m_accumulator_value = IRValueId{};
    m_last_ram_word_address = IRValueId{};
    // POR is unknown at a CFG join/backedge. Only elide straight-line repeats.
    m_known_plot_options = -1;
    m_known_color = -1;
    m_materialized_values.clear();
    for (const auto& instruction : block.instructions) {
        m_current_instruction_position = m_emission_position++;
        if (globallyOptimized()) m_spill_cache.enter(m_register_allocator.spareRegisters(m_current_instruction_position));
        if (instruction.opcode != IROpcode::Phi) for (const auto v : instruction.operands) {
            auto& remaining = m_block_remaining_uses.at(v.value);
            if (remaining) --remaining;
        }
        m_isInPlottingContext = instruction.in_plot_context;
        if (instruction.opcode == IROpcode::Call) { m_known_plot_options = -1; m_known_color = -1; }
        // A void call has no SSA result, but it is still a side effect that
        // must be emitted when it appears as an expression statement.
        if (instruction.opcode == IROpcode::Call &&
            isVoidValue(instruction.type)) {
            emitCall(instruction);
            continue;
        }
        if (instruction.producesValue()) {
            if (instruction.opcode == IROpcode::Phi) continue;
            if (optimized()) {
                if (m_branch_comparisons.count(instruction.result.value)) continue;
                const auto* location = m_register_allocator.find(instruction.result);
                if (location && location->rematerializable) continue;
                m_emitting_value = instruction.result;
                materialize(instruction.result);
                m_emitting_value = IRValueId{};
                continue;
            }
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
    std::map<std::string, std::size_t> labels;
    if (optimized()) for (const auto& symbol : m_object_file.symbol_table)
        if (symbol.section == SymbolSection::CODE) labels.emplace(symbol.name, symbol.offset);
    bool widen = false;
    for (const auto& fixup : m_branch_fixups) {
        const auto target = m_block_addresses.find(fixup.target.value);
        const auto local = labels.find(fixup.local_target);
        if (fixup.local_target.empty() ? target == m_block_addresses.end() : local == labels.end()) {
            fail("IR codegen: branch target was not emitted.", fixup.source);
        }
        if (fixup.long_form) {
            // Branch fixups point at the operand, but ADDR16_IWT relocations
            // point at the opcode and patch its following two bytes.
            addRelocation(fixup.local_target.empty() ? internalBlockSymbol(m_current_function->name, fixup.target) : fixup.local_target,
                          fixup.patch_offset - 1, RelocationType::ADDR16_IWT);
            continue;
        }
        const auto next_instruction = static_cast<std::int64_t>(fixup.patch_offset) + 1;
        const auto distance = static_cast<std::int64_t>(fixup.local_target.empty() ? target->second : local->second) - next_instruction;
        if (distance < -128 || distance > 127) {
            if (!optimized()) throw NeedsLongBranch{};
            m_long_branches[m_current_function->name].insert(fixup.serial);
            widen = true;
            continue;
        }
        m_object_file.code_section.at(fixup.patch_offset) =
            static_cast<std::uint8_t>(static_cast<std::int8_t>(distance));
    }
    if (widen) throw NeedsLongBranch{};
}

ObjectFile IRCodeGenerator::generateInternal(const IRModule& module) {
    m_object_file = ObjectFile();
    m_division_helpers.clear();
    m_object_file.config = m_config;
    m_object_file.config.bitmap = module.bitmap;

    for (const auto& function : module.functions) {
        m_current_function = &function;
        m_values.clear();
        m_use_counts.clear();
        m_active_values.clear();
        m_materialized_values.clear();
        m_branch_comparisons.clear();
        m_block_addresses.clear();
        m_branch_fixups.clear();
        m_branch_serial = 0;
        m_current_block_index = 0;
        m_current_instruction_position = 0;
        m_emission_position = 0;
        m_isInPlottingContext = false;
        m_spill_offsets.clear();
        m_spill_cache.clear();
        m_block_remaining_uses.clear();
        m_divmod_offsets.clear();
        m_fault_offsets.clear();
        m_epilogue_label.clear();
        m_emitting_value = IRValueId{};
        m_accumulator_value = IRValueId{};
        m_last_ram_word_address = IRValueId{};
        m_local_label_serial = 0;
        m_stack_check_credit.reset();
        m_address_proof = GSUAddressProof{};
        m_checked_pointer_mode = function.return_type.pointer_level > 0;
        m_has_hardware_loop = false;
        m_auto_caches.clear(); m_manual_cache = function.is_cached;
        for (const auto& parameter : function.parameters)
            m_checked_pointer_mode = m_checked_pointer_mode || parameter.type.pointer_level > 0;

        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                m_has_hardware_loop = m_has_hardware_loop || instruction.opcode == IROpcode::HardwareLoop;
                m_manual_cache = m_manual_cache || instruction.opcode == IROpcode::Cache;
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
        if (optimized()) buildRegisterAllocation(function);
        if (optimized()) {
            for (const auto& block : function.blocks) {
                if (block.instructions.size() < 2) continue;
                const auto& branch = block.instructions.back();
                const auto& comparison = block.instructions[block.instructions.size() - 2];
                const auto& op = comparison.operation;
                if (branch.opcode == IROpcode::CondBranch && comparison.opcode == IROpcode::Binary &&
                    branch.operands.front().value == comparison.result.value && m_use_counts[comparison.result.value] == 1 &&
                    (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!="))
                    m_branch_comparisons.insert(comparison.result.value);
            }
        }
        int frame_bytes = function.total_local_alloc_size;
        {
            std::map<int, int> shared_spills;
            for (const auto& value : m_values) {
                if (optimized()) {
                    const auto* location = m_register_allocator.find(IRValueId{value.first});
                    if (m_branch_comparisons.count(value.first) || !m_use_counts[value.first] ||
                        (location && (location->has_register || location->rematerializable))) continue;
                } else {
                    const auto opcode = value.second->opcode;
                    const bool observable = opcode == IROpcode::PlotCoordinateRead || opcode == IROpcode::LoadIndirect || opcode == IROpcode::Load || opcode == IROpcode::Call || value.second->hardwareEffects().observable();
                    const bool arithmetic_check = opcode == IROpcode::Binary &&
                        (value.second->operation == "/" || value.second->operation == "%" || value.second->operation == "<<" || value.second->operation == ">>");
                    if (!observable && !arithmetic_check && !(m_checked_pointer_mode && needsPointerSpill(*value.second))) continue;
                }
                const auto width = isFarPointer(value.second->type) ? 4 : 2;
                const auto* location = globallyOptimized() ? m_register_allocator.find(IRValueId{value.first}) : nullptr;
                if (location && location->spill_slot >= 0) {
                    const auto existing = shared_spills.find(location->spill_slot);
                    if (existing != shared_spills.end()) { m_spill_offsets.emplace(value.first, existing->second); continue; }
                }
                if (frame_bytes > 65526 - width)
                    fail("IR codegen: checked pointer frame exceeds one RAM bank.", value.second->source);
                frame_bytes += width;
                m_spill_offsets.emplace(value.first, -frame_bytes);
                if (location && location->spill_slot >= 0) shared_spills.emplace(location->spill_slot, -frame_bytes);
            }
            if (!m_spill_offsets.empty()) frame_bytes += 2; // Empty word below the last spill.
        }
        if (globallyOptimized()) {
            for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
                if (instruction.opcode != IROpcode::DivMod) continue;
                if (frame_bytes > 65524) fail("IR codegen: divmod frame exceeds one RAM bank.", instruction.source);
                frame_bytes += 2;
                m_divmod_offsets.emplace(instruction.result.value, -frame_bytes);
            }
            if (!m_divmod_offsets.empty()) frame_bytes += 2; // The last pair word is never the empty/null bottom.
        }
        if (!optimized()) buildRegisterAllocation(function);
        if (globallyOptimized()) {
            const auto locals = m_all_local_symbols.find(function.name);
            const Analyzer::LocalSymbolTable empty;
            m_address_proof.run(function, locals == m_all_local_symbols.end() ? empty : locals->second,
                                static_cast<std::size_t>(frame_bytes), m_checked_pointer_mode);
        }

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
        selectAutomaticCaches();
    }

    emitDivisionHelpers();

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
    if (globallyOptimized()) GSUMachineScheduler::run(m_object_file);
    return m_object_file;
}

ObjectFile IRCodeGenerator::generate(const IRModule& module) {
    if (sizeOptimized()) {
        // Eight deterministic, bounded candidates. Compare emitted CODE plus
        // DATA alignment, not IR node counts or frequency-weighted timing.
        // The O2-policy candidate retains speed transformations only when they
        // actually shrink bytes; it uses O2 allocation but no CACHE padding.
        const auto bytes = [](const ObjectFile& object) {
            return object.code_section.size() + object.data_section.size() +
                (object.data_section.empty() ? 0u : static_cast<unsigned>(object.code_section.size() & 1u));
        };
        m_share_division = false;
        auto best = generateOptimized(module, OptimizationLevel::O2);
        std::size_t instructions = 0;
        for (const auto& f : module.functions) for (const auto& b : f.blocks) instructions += b.instructions.size();
        if (module.functions.size() > 64 || instructions > 16000) return best;
        for (const auto policy : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            for (const bool shared : {false, true}) {
                if (policy == OptimizationLevel::O2 && !shared) continue;
                m_share_division = shared;
                auto candidate = generateOptimized(module, policy);
                if (bytes(candidate) < bytes(best)) best = std::move(candidate);
            }
        }
        // LOOP's ABI saves and pointer SSA can be faster yet larger. Retain
        // conservative IR alternatives rather than charging size mode for a
        // speed win. Machine selection remains byte-exact in every candidate.
        for (const auto policy : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            for (const bool shared : {false, true}) {
                m_share_division = shared;
                auto candidate = generateOptimized(module, policy, IRCompactionPolicy::Disabled);
                if (bytes(candidate) < bytes(best)) best = std::move(candidate);
            }
        }
        return best;
    }
    return generateOptimized(module, m_config.optimization);
}

ObjectFile IRCodeGenerator::generateOptimized(const IRModule& module, OptimizationLevel policy, IRCompactionPolicy compaction) {
    m_allocation_policy = policy;
    m_division_candidates.clear();
    m_force_long_branches = false;
    m_long_branches.clear();
    if (optimized()) {
        auto optimized_module = module;
        if (globallyOptimized()) IRGlobalOptimizer::run(optimized_module, m_all_local_symbols, policy, compaction);
        else IRLocalOptimizer::run(optimized_module, m_all_local_symbols);
        if (sizeOptimized() && m_share_division) {
            std::map<bool, unsigned> counts;
            for (const auto& f : optimized_module.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::DivMod || (i.opcode == IROpcode::Binary && (i.operation == "/" || i.operation == "%")))
                    ++counts[i.type.is_unsigned];
            for (const auto& count : counts) if (count.second >= 2) m_division_candidates.insert(count.first);
        }
        // Monotonic widening, bounded independently of source size. If a
        // pathological cascade needs more rounds, the checked long form wins.
        for (unsigned pass = 0; pass < 8; ++pass) {
            try { return generateInternal(optimized_module); }
            catch (const NeedsLongBranch&) {}
        }
        m_force_long_branches = true;
        return generateInternal(optimized_module);
    }
    try {
        return generateInternal(module);
    } catch (const NeedsLongBranch&) {
        m_force_long_branches = true;
        return generateInternal(module);
    }
}
