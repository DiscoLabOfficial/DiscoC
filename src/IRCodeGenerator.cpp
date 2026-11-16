#include "IRCodeGenerator.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>

#include "CompilerError.hpp"
#include "ConstantEvaluator.hpp"
#include "ABI.hpp"
#include "Opcodes.hpp"
#include "IRLocalOptimizer.hpp"
#include "IRGlobalOptimizer.hpp"
#include "IRControlFlow.hpp"
#include "GSUMachineScheduler.hpp"
#include "GSUCostModel.hpp"
#include "IRStorageOptimizer.hpp"

namespace {

std::string internalBlockSymbol(const std::string& function, IRBlockId block) {
    return std::string(1, '\x01') + function + "#" + std::to_string(block.value);
}

// Trips assumed for a call-free loop whose count is not a compile-time
// constant. Four already repays a cold fill of every body line; a loop that
// actually runs once loses about one partly used line per entry.
constexpr std::uint32_t AssumedDynamicTrips = 4;

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

[[noreturn]] void IRCodeGenerator::fail(const std::string& message, const Token& source) const {
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

void IRCodeGenerator::emitMove(Register destination_register, std::uint8_t source) {
    const auto destination = static_cast<std::uint8_t>(destination_register);
    m_spill_cache.clobber(destination);
    const auto cached = m_accumulator_value;
    if (destination == GSUAbi::StackPointerRegister) {
        m_stack_check_credit.reset();
        // Only the epilogue writes SP, from FP; any other source is untracked.
        if (source == GSUAbi::FramePointerRegister) m_stack_proof.restoreFrame();
        else if (m_stack_proof.active()) fail("IR codegen: stack proof cannot track an SP copy.", Token(TokenType::UNKNOWN, "", 0, 0));
    }
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
    if (m_stack_proof.active()) { m_stack_proof.guard(required); return; }
    if (globallyOptimized() && m_stack_check_credit.covers(required)) return;
    const auto patch = m_object_file.code_section.size();
    emitWordLiteral(Register::R3, 0); // The linker patches an IWT, never a shortened IBT.
    addRelocation(std::string(GSUAbi::StackLimitPrefix) + std::to_string(required), patch, RelocationType::ADDR16_RAM);
    emitByte(0xba); emitByte(0x3f); emitByte(0x63);
    emitGuard(OpCode::BCS, 2);
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
    m_stack_proof.push(2);
    if (globallyOptimized()) {
        m_stack_check_credit.decrease(2);
    }
}

void IRCodeGenerator::emitPop(std::uint8_t reg) {
    m_spill_cache.clobber(reg);
    m_last_ram_word_address = IRValueId{};
    emitByte(0xDA);
    emitByte(0xDA);
    m_stack_proof.pop(2);
    if (reg != 0) {
        emitByte(static_cast<std::uint8_t>(0x20 | reg));
    }
    emitByte(0x4A);
    if (globallyOptimized()) {
        m_stack_check_credit.increase(2);
    }
}

void IRCodeGenerator::emitStore(Register address, std::uint8_t value_reg, bool byte) {
    auto address_reg = static_cast<std::uint8_t>(address);
    m_last_ram_word_address = IRValueId{};
    // STB/STW encode only (R0)-(R11). A pointer parked in R14 is copied into
    // an encodable address register before the value prefix is selected.
    if (address_reg > 11) {
        const auto temp = value_reg == 3 ? std::uint8_t{0} : std::uint8_t{3};
        emitMove(static_cast<Register>(temp), address_reg);
        if (temp == 0) m_accumulator_value = IRValueId{};
        address_reg = temp;
    }
    if (value_reg != 0) {
        emitByte(static_cast<std::uint8_t>(0x20 | value_reg));
    }
    if (byte) {
        emitByte(static_cast<std::uint8_t>(OpCode::ALT1));
    }
    emitByte(static_cast<std::uint8_t>(0x30 | address_reg));
}

bool IRCodeGenerator::matchesLastRamWordAddress(IRValueId address) const {
    if (!m_last_ram_word_address.source.isValid()) return false;
    if (address.value == m_last_ram_word_address.source.value) return true;
    const auto& previous = producer(m_last_ram_word_address.source, Token(TokenType::UNKNOWN, "", 0, 0));
    const auto& current = producer(address, Token(TokenType::UNKNOWN, "", 0, 0));
    // Fresh Address nodes for the same declaration are equivalent. Never
    // equate distinct pointer loads: an alias may have changed the pointer.
    return previous.opcode == IROpcode::Address && current.opcode == IROpcode::Address &&
        previous.operation.empty() && current.operation.empty() &&
        previous.symbol_id.isValid() && previous.symbol_id == current.symbol_id;
}

void IRCodeGenerator::emitImmediateArithmetic(OpCode operation,
                                               std::int64_t value,
                                               const Token& source) {
    const auto op_base = static_cast<std::uint8_t>(operation);
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
    if (add) m_stack_proof.pop(bytes); else m_stack_proof.push(bytes);
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
    // A frameless body is balanced at every return: calls clean up their
    // arguments and the IR verifier closes every hardware-loop save first.
    if (!m_frameless) emitMove(static_cast<Register>(GSUAbi::StackPointerRegister), GSUAbi::FramePointerRegister);
    if (m_current_function->name == "main") {
        // The entry STOPs with its frame installed instead of popping R9/R11.
        // If the body reused R11, reload the saved host value from FP+4
        // through scratch R3 so R0/R10 and the observable STOP state survive.
        bool r11_allocated = false;
        for (const auto& location : m_register_allocator.locations())
            r11_allocated = r11_allocated || (location.second.has_register && !m_spill_offsets.count(location.first) &&
                location.second.physical_register == GSUAbi::LinkRegister);
        if (r11_allocated) {
            emitRegisterLiteral(3, 4);
            emitByte(0xb9); emitByte(0x13); emitByte(0x53);
            emitByte(static_cast<std::uint8_t>(0x10 | GSUAbi::LinkRegister)); emitByte(0x43);
            m_last_ram_word_address = IRValueId{};
        }
        // A scalar-only entry may call a checked callee from another unit.
        // Normal STOP always clears its volatile scratch/fault category.
        emitRegisterLiteral(GSUAbi::AddressFaultRegister, 0);
        emitByte(static_cast<std::uint8_t>(OpCode::STOP));
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        return;
    }
    if (!m_frameless) emitPop(GSUAbi::FramePointerRegister);
    emitPop(GSUAbi::LinkRegister);
    emitByte(static_cast<std::uint8_t>(0x90 | GSUAbi::LinkRegister));
    emitByte(static_cast<std::uint8_t>(OpCode::NOP));
}

std::uint8_t IRCodeGenerator::scratchRegister() const {
    return m_isInPlottingContext ? 3 : 1;
}

std::vector<std::uint8_t> IRCodeGenerator::allocatableRegisters(const IRFunction& function) const {
    // Calls already save every allocated register that is live across them,
    // so widening this set does not change the callee-preserved ABI. R6 stays
    // the temporary used across materialize; R1 stays the outside-plot scratch.
    // R14 is the ROM pointer: every write starts a ROM byte fetch, so it is
    // not a general register even in a function that never reads ROM.
    std::vector<std::uint8_t> regs{5, 7, 8};
    bool plot = false, division = false, multiply = false, far = false;
    bool hardware_loop = false, wide_pointer = false, near_rom_pointer = false;
    const auto consider = [&](const Type& type) {
        if (isFarPointer(type)) far = true;
        // Checked near-ROM values also load the link-time bank into R4.
        // Inspect every address-producing result, not just a load's scalar
        // result type; parameters/returns can carry the same pointer type.
        if (type.pointer_level > 0 && type.space == AddressSpace::ROM) near_rom_pointer = true;
    };
    for (const auto& parameter : function.parameters) consider(parameter.type);
    consider(function.return_type);
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
        consider(instruction.type);
        if (instruction.in_plot_context) plot = true;
        switch (instruction.opcode) {
            case IROpcode::Plot:
            case IROpcode::PlotBegin:
            case IROpcode::PlotEnd:
            case IROpcode::PlotCoordinateRead:
            case IROpcode::PlotCoordinateWrite:
            case IROpcode::Rpix:
                plot = true;
                break;
            case IROpcode::DivMod:
                division = true;
                break;
            case IROpcode::HardwareLoop:
                hardware_loop = true;
                break;
            case IROpcode::PointerOffset:
                if (instruction.operands.size() == 3) wide_pointer = true;
                break;
            case IROpcode::Binary:
                if (instruction.operation == "*") multiply = true;
                else if (instruction.operation == "/" || instruction.operation == "%") division = true;
                break;
            default:
                break;
        }
    }
    if (!plot && !division) regs.push_back(2);
    if (!far && !near_rom_pointer && !division && !multiply && !hardware_loop && !wide_pointer) regs.push_back(4);
    // Every function saves R11 in its prologue and restores it only for the
    // final JMP, so the body may reuse it. LINK at a call writes R11; the
    // call-clobber envelope always contains R11, so a live value is saved
    // like any caller-saved home. Shared divmod kernels also LINK but do not
    // save allocated homes, so division keeps R11 reserved.
    // R12/R13 stay reserved: they must survive to every STOP, including fault
    // islands shared across the function, which no prologue save can cover.
    if (!division) regs.push_back(GSUAbi::LinkRegister);
    if (framePointerAllocatable(function)) regs.push_back(GSUAbi::FramePointerRegister);
    return regs;
}

void IRCodeGenerator::buildRegisterAllocation(const IRFunction& function) {
    if (globallyOptimized()) {
        m_register_allocator.runGlobal(function, allocatableRegisters(function), m_allocation_policy);
        return;
    }
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
        if (callClobbers(instruction) & (1u << location.physical_register))
            unique_registers.insert(location.physical_register);
    }
    if (globallyOptimized() && !framePointerAllocatable(*m_current_function)) {
        const auto callee = m_call_clobbers.find(instruction.symbol);
        if (callee != m_call_clobbers.end() && (callee->second & (1u << GSUAbi::FramePointerRegister)))
            unique_registers.insert(GSUAbi::FramePointerRegister);
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
        if (!m_static_frame_owner.empty() && instruction.immediate < 0) {
            emitFrameAddress(Register::R0, static_cast<int>(instruction.immediate));
            return;
        }
        requireFramePointer(instruction.source);
        emitMove(Register::R0, 9);
        emitImmediateArithmetic(OpCode::SUB_R, -instruction.immediate, instruction.source);
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
                emitCompare(Register::R0, static_cast<std::uint16_t>(65536 - instruction.immediate));
                emitGuard(OpCode::BCC, 3);
            }
            emitMove(static_cast<Register>(scratchRegister()), 0);
            emitLiteral(instruction.immediate);
            emitByte(static_cast<std::uint8_t>(0x50 | scratchRegister()));
        }
        return;
    }

    if (m_data_manager.hasSymbol(instruction.symbol) &&
        m_data_manager.getEntries().at(instruction.symbol).storage == AddressSpace::RAM) {
        const auto patch = m_object_file.code_section.size();
        emitWordLiteral(Register::R0, 0);
        addRelocation(m_data_manager.getEntries().at(instruction.symbol).link_name, patch, RelocationType::ADDR16_RAM);
        return;
    }
    if (m_current_function->immutable_local_arrays.count(instruction.symbol_id)) {
        const auto patch = m_object_file.code_section.size();
        emitWordLiteral(Register::R0, 0);
        addRelocation(std::string(GSUAbi::ImmutablePoolPrefix) + m_current_function->name + "." +
            std::to_string(instruction.symbol_id.value), patch, RelocationType::ADDR16_RAM);
        return;
    }
    const auto& function_symbols = localSymbols(*m_current_function);
    if (instruction.symbol_id.isValid()) {
        const auto symbol = function_symbols.find(instruction.symbol_id);
        if (symbol != function_symbols.end()) {
            if (!m_static_frame_owner.empty() && symbol->second.stackOffset < 0) {
                // A static frame keeps the FP-relative layout and alignment
                // padding; only its base is a link-time RAM address.
                emitFrameAddress(Register::R0, symbol->second.stackOffset);
            } else {
                requireFramePointer(instruction.source);
                emitMove(Register::R0, 9);
                if (symbol->second.stackOffset > 0) {
                    emitImmediateArithmetic(OpCode::ADD_R, symbol->second.stackOffset, instruction.source);
                } else if (symbol->second.stackOffset < 0) {
                    emitImmediateArithmetic(OpCode::SUB_R, std::abs(symbol->second.stackOffset), instruction.source);
                }
            }
            if (symbol->second.type.alignment > 2) {
                const auto alignment = symbol->second.type.alignment;
                emitImmediateArithmetic(OpCode::ADD_R, alignment - 1, instruction.source);
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
        address.operation.empty() && address.symbol_id.isValid()))
        ensureAddressChecked(instruction.operands[0], instruction.type.sizeInBytes);
    if (far_address) emitSelectBank(4, address.type.space);
    else if (address.type.space == AddressSpace::ROM) {
        emitNearBank(3, AddressSpace::ROM);
        emitSelectBank(3, AddressSpace::ROM);
    }
    if (optimized() && address.type.space == AddressSpace::RAM && !far_address && !isFarPointer(instruction.type)) {
        // LOAD samples its address register before writing the destination.
        // R0 can therefore carry both without copying it through R3.
        const bool plain_scalar = instruction.type.pointer_level == 0 &&
            instruction.type.base != BaseType::BOOL && instruction.type.base != BaseType::STRUCT;
        std::uint8_t direct = 0;
        if (globallyOptimized() && plain_scalar && usesRegisterAllocation(instruction.result)) {
            direct = m_register_allocator.find(instruction.result)->physical_register;
            if (direct != 0) {
                m_spill_cache.clobber(direct);
                emitByte(static_cast<std::uint8_t>(0x10 | direct));
            }
        }
        if (usesByteStorage(instruction.type)) emitByte(0x3d);
        emitByte(0x40);
        if (instruction.type.pointer_level > 0) {
            const auto element = pointeeType(instruction.type);
            emitPointerValueCheck(instruction.type, element.sizeInBytes > 0 ? element.sizeInBytes : 1);
        } else if (instruction.type.base == BaseType::BOOL) emitBoolNormalization();
        if (!usesByteStorage(instruction.type) && instruction.type.sizeInBytes == 2)
            m_last_ram_word_address = instruction.operands[0];
        if (direct) m_direct_register_load = true;
        return;
    }
    emitMove(Register::R3, 0);
    const auto load_word = [&](std::uint8_t destination, bool byte) {
        if (address.type.space == AddressSpace::ROM) {
            emitMove(Register::R14_LR, 3);
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

void IRCodeGenerator::emitBinaryOperands(const IRInstruction& instruction) {
    const auto left = instruction.operands.at(0), right = instruction.operands.at(1);
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
        emitMove(static_cast<Register>(scratchRegister()), right_location->physical_register);
    } else if (optimized()) {
        materialize(right); emitMove(Register::R6_LMULT_HI, 0);
        materialize(left); emitMove(static_cast<Register>(scratchRegister()), 6);
    } else {
        materialize(right); emitPush(0);
        materialize(left); emitPop(scratchRegister());
    }
}

void IRCodeGenerator::emitScalarComparison(const IRInstruction& instruction, bool reverse) {
    const auto left = instruction.operands.at(0), right = instruction.operands.at(1);
    const auto home = [&](IRValueId value) {
        std::int64_t literal = 0;
        return producer(value, instruction.source).type.pointer_level == 0 &&
            (constantValue(value, literal) || usesRegisterAllocation(value) || m_spill_offsets.count(value.value));
    };
    std::uint8_t source = 0, operand = scratchRegister();
    if (globallyOptimized() && home(left) && home(right)) {
        // A regional loop home (R4/R6) is as direct as an allocated one; the
        // other operand's reload uses only R0/R3, never that home.
        const auto direct = [&](IRValueId value) -> int {
            if (usesRegisterAllocation(value)) return m_register_allocator.find(value)->physical_register;
            return regionalRegister(value);
        };
        const int a = direct(left), b = direct(right);
        if (a >= 0 && b >= 0) { source = static_cast<std::uint8_t>(a); operand = static_cast<std::uint8_t>(b); }
        else if (a >= 0) { materialize(right); source = static_cast<std::uint8_t>(a); operand = 0; }
        else if (b >= 0) { materialize(left); operand = static_cast<std::uint8_t>(b); }
        else {
            // Both scalar homes are already evaluated. Retain the existing
            // right-before-left reload order, but compare R6 directly rather
            // than copying it through another backend scratch register.
            materialize(right); emitMove(Register::R6_LMULT_HI, 0);
            materialize(left); operand = 6;
        }
    } else {
        // Ephemeral/effectful operands keep their existing materialization
        // order. Reversing only the final CMP never reorders a checked fault.
        emitBinaryOperands(instruction);
    }
    if (reverse) std::swap(source, operand);
    if (source) emitByte(static_cast<std::uint8_t>(0xb0 | source));
    emitByte(0x3f); emitByte(static_cast<std::uint8_t>(0x60 | operand));
}

bool IRCodeGenerator::emitAllocatedMultiply(const IRInstruction& instruction, std::uint8_t destination) {
    if (!globallyOptimized() || instruction.opcode != IROpcode::Binary || instruction.operation != "*") return false;
    const auto kind = m_address_proof.multiplyKind(instruction);
    if (kind == GSUAddressProof::MultiplyKind::FullWord) return false;
    const auto left = instruction.operands[0], right = instruction.operands[1];
    const auto* source = m_register_allocator.find(left);
    const auto* operand = m_register_allocator.find(right);
    std::int64_t literal = 0;
    const bool immediate = constantValue(right, literal) && literal >= 0 && literal <= 15;
    if (!source || !source->has_register || (!immediate && (!operand || !operand->has_register)) ||
        m_accumulator_value.value == left.value) return false;
    m_spill_cache.clobber(destination);
    if (source->physical_register == destination) emitByte(static_cast<std::uint8_t>(0x20 | destination));
    else { emitByte(static_cast<std::uint8_t>(0x10 | destination)); emitByte(static_cast<std::uint8_t>(0xb0 | source->physical_register)); }
    const bool unsigned_product = kind == GSUAddressProof::MultiplyKind::UnsignedByte;
    if (immediate) emitByte(unsigned_product ? 0x3f : 0x3e);
    else if (unsigned_product) emitByte(0x3d);
    emitByte(static_cast<std::uint8_t>(0x80 | (immediate ? static_cast<unsigned>(literal) : operand->physical_register)));
    return true;
}

void IRCodeGenerator::emitMultiply(const IRInstruction& instruction) {
    const auto kind = globallyOptimized() ? m_address_proof.multiplyKind(instruction) : GSUAddressProof::MultiplyKind::FullWord;
    const bool short_product = kind != GSUAddressProof::MultiplyKind::FullWord;
    const bool unsigned_product = kind == GSUAddressProof::MultiplyKind::UnsignedByte;
    const auto left = instruction.operands[0], right = instruction.operands[1];
    const auto* place = m_register_allocator.find(right);
    std::int64_t literal = 0;
    if (short_product && constantValue(right, literal) && literal >= 0 && literal <= 15) {
        materialize(left); emitByte(unsigned_product ? 0x3f : 0x3e);
        emitByte(static_cast<std::uint8_t>(0x80 | literal));
        return;
    }
    if (short_product && place && place->has_register) {
        materialize(left);
        if (unsigned_product) emitByte(0x3d);
        emitByte(static_cast<std::uint8_t>(0x80 | place->physical_register));
        return;
    }
    // Eager SSA operands are already defined; scalar spill materialization
    // uses R3, so R6 safely retains the right input while loading the left.
    if (place && place->has_register) emitMove(Register::R6_LMULT_HI, place->physical_register);
    else { materialize(right); emitMove(Register::R6_LMULT_HI, 0); }
    materialize(left);
    if (short_product) {
        if (unsigned_product) emitByte(0x3d);
        emitByte(0x86);
    } else {
        emitByte(0x3d); emitByte(0x9f); emitMove(Register::R0, 4);
    }
}

void IRCodeGenerator::emitBinary(const IRInstruction& instruction) {
    if (instruction.operands.size() != 2) {
        fail("IR codegen: binary instruction must have two operands.", instruction.source);
    }
    const auto left = instruction.operands[0];
    const auto right = instruction.operands[1];
    if (optimized() && emitConstantArithmetic(instruction)) return;
    if (optimized() && instruction.operation == "*") { emitMultiply(instruction); return; }
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

    const bool comparison = instruction.operation == ">" || instruction.operation == "<" ||
        instruction.operation == ">=" || instruction.operation == "<=" ||
        instruction.operation == "==" || instruction.operation == "!=";
    const bool reverse = globallyOptimized() && (instruction.operation == "<=" || instruction.operation == ">");
    if (comparison) emitScalarComparison(instruction, reverse);
    else emitBinaryOperands(instruction);

    if (instruction.operation == "&" || instruction.operation == "|" || instruction.operation == "^" ||
        instruction.operation == "<<" || instruction.operation == ">>" || instruction.operation == "/" || instruction.operation == "%") {
        emitIntegerOperation(instruction);
        return;
    }
    if (comparison) {
        const auto operation = reverse ? (instruction.operation == "<=" ? ">=" : "<") : instruction.operation;
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

        if (operation == "==") {
            emit_local_branch(equal, true_label);
        } else if (operation == "!=") {
            emit_local_branch(not_equal, true_label);
        } else if (operation == "<") {
            emit_local_branch(less, true_label);
        } else if (operation == "<=") {
            emit_local_branch(less, true_label);
            emit_local_branch(equal, true_label);
        } else if (operation == ">") {
            emit_local_branch(equal, false_label);
            emit_local_branch(greater_equal, true_label);
        } else if (operation == ">=") {
            if (!globallyOptimized()) emit_local_branch(less, false_label);
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
        emitMove(Register::R6_LMULT_HI, scratchRegister());
        emitByte(0x3d); emitByte(0x9f);
        emitMove(Register::R0, 4);
        return;
    }
    fail("IR codegen: unsupported binary operation '" + instruction.operation + "'.",
         instruction.source);
}

void IRCodeGenerator::emitBoolNormalization() {
    const auto zero = localLabel(), end = localLabel();
    emitZeroTest(Register::R0);
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
        emitZeroTest(Register::R0); emitLocalJump(nonnull, 8);
        if (isFarPointer(source.type)) { emitZeroTest(Register::R4_LMULT_LO); emitLocalJump(nonnull, 8); }
        if (isFarPointer(instruction.type)) emitRegisterLiteral(4, 0);
        emitLocalJump(done);
        bindLabel(nonnull);
        if (isFarPointer(source.type) && !isFarPointer(instruction.type)) {
            emitNearBank(3, instruction.type.space);
            emitByte(0xb4); emitByte(0x3f); emitByte(0x63);
            emitGuard(OpCode::BEQ, 4);
        } else if (!isFarPointer(source.type) && isFarPointer(instruction.type)) {
            emitNearBank(4, instruction.type.space);
        }
        // A far non-null pair can still have offset zero. Only the near
        // source branch proves R0 nonzero when narrowing to a near pointer.
        emitAddressCheck(instruction.type, element.sizeInBytes > 0 ? element.sizeInBytes : 1,
            globallyOptimized() && !isFarPointer(source.type)
                ? AddressComponents::NonNull : AddressComponents::Unknown);
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
    m_checked_addresses.clear();
    m_spill_cache.clear();
    m_last_ram_word_address = IRValueId{};
    std::vector<std::uint8_t> saved_registers;
    saveLiveRegistersForCall(instruction, saved_registers);
    const auto clobbers = callClobbers(instruction);
    const bool save_loop = m_has_hardware_loop && (clobbers & ((1u << 12) | (1u << 13)));
    const bool save_x = m_isInPlottingContext && (clobbers & (1u << 1));
    const bool save_y = m_isInPlottingContext && (clobbers & (1u << 2));
    if (save_loop) { emitPush(12); emitPush(13); }
    if (save_x) emitPush(1);
    if (save_y) emitPush(2);
    std::size_t argument_bytes = 0;
    const bool register_arguments = m_register_arguments.count(instruction.symbol) != 0;
    if (register_arguments) emitRegisterArguments(instruction);
    else for (auto argument = instruction.operands.rbegin();
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
    m_stack_proof.call(instruction.symbol);
    // Argument setup may have parked spill copies in callee-clobbered registers.
    if (register_arguments) m_spill_cache.clear();
    // Even a no-argument call can perform arbitrary RAM accesses in its callee.
    m_last_ram_word_address = IRValueId{};
    m_stack_check_credit.reset();
    if (instruction.operands.size() >
        std::numeric_limits<std::size_t>::max() / GSUAbi::ParameterSlotSize) {
        fail("IR codegen: call argument area is too large.", instruction.source);
    }
    emitAdjustStack(argument_bytes, true, instruction.source);
    if (save_y) emitPop(2);
    if (save_x) emitPop(1);
    if (save_loop) { emitPop(13); emitPop(12); }
    restoreRegistersAfterCall(saved_registers);
}

void IRCodeGenerator::emitParallelCopies(std::vector<std::pair<std::uint8_t, std::uint8_t>> copies) {
    copies.erase(std::remove_if(copies.begin(), copies.end(),
        [](const std::pair<std::uint8_t, std::uint8_t>& copy) { return copy.first == copy.second; }), copies.end());
    for (std::size_t a = 0; a < copies.size(); ++a) {
        bool repeated = !copies[a].first || !copies[a].second;
        for (std::size_t b = a + 1; b < copies.size(); ++b) repeated = repeated || copies[a].first == copies[b].first;
        if (repeated) fail("IR codegen: parallel copy has a repeated destination or uses R0.", Token(TokenType::UNKNOWN, "", 0, 0));
    }
    // Each round emits one copy or retires one cycle; bounded by 2 * copies.
    for (std::size_t round = 0, limit = 2 * copies.size() + 1; !copies.empty(); ++round) {
        if (round > limit) fail("IR codegen: parallel copy did not converge.", Token(TokenType::UNKNOWN, "", 0, 0));
        const auto ready = std::find_if(copies.begin(), copies.end(), [&](const std::pair<std::uint8_t, std::uint8_t>& copy) {
            return std::none_of(copies.begin(), copies.end(),
                [&](const std::pair<std::uint8_t, std::uint8_t>& other) { return other.second == copy.first; });
        });
        if (ready != copies.end()) {
            emitMove(static_cast<Register>(ready->first), ready->second);
            copies.erase(ready);
            continue;
        }
        // Every destination is still read: park one in R0 and redirect.
        const auto parked = copies.front().first;
        emitMove(Register::R0, parked);
        for (auto& copy : copies) if (copy.second == parked) copy.second = 0;
    }
    m_accumulator_value = IRValueId{};
}

void IRCodeGenerator::emitParameterInputs() {
    // Frame homes first: these stores read argument registers and write only
    // R3 and RAM. Then one parallel copy fills the register homes.
    std::vector<std::pair<std::uint8_t, std::uint8_t>> copies;
    for (const auto& input : m_parameter_inputs) {
        const IRValueId value{input.first};
        if (m_spill_offsets.count(input.first)) emitWordSpillStore(value, input.second);
        else if (usesRegisterAllocation(value))
            copies.emplace_back(m_register_allocator.find(value)->physical_register, input.second);
    }
    emitParallelCopies(std::move(copies));
    for (const auto& input : m_parameter_inputs) m_materialized_values.insert(input.first);
    m_spill_cache.clear();
    m_last_ram_word_address = IRValueId{};
}

void IRCodeGenerator::emitRegisterArguments(const IRInstruction& call) {
    if (call.operands.size() > GSUAbi::RegisterArgumentCount)
        fail("IR codegen: register-argument call has too many operands.", call.source);
    struct Argument { IRValueId value; std::uint8_t target; };
    std::vector<Argument> computed, spilled;
    std::vector<std::pair<std::uint8_t, std::uint8_t>> copies;
    std::vector<std::pair<std::uint8_t, std::uint16_t>> literals;
    for (std::size_t n = 0; n < call.operands.size(); ++n) {
        const auto value = call.operands[n];
        const auto target = GSUAbi::RegisterArguments[n];
        std::int64_t literal = 0;
        const bool regional = regionalRegister(value) >= 0;
        if (producer(value, call.source).opcode == IROpcode::Constant && constantValue(value, literal))
            literals.emplace_back(target, static_cast<std::uint16_t>(literal));
        else if (!regional && m_spill_offsets.count(value.value)) spilled.push_back({value, target});
        else if (!regional && usesRegisterAllocation(value) && !m_cursor_elided_values.count(value.value) &&
                 !m_ephemeral_values.count(value.value))
            copies.emplace_back(target, m_register_allocator.find(value)->physical_register);
        else computed.push_back({value, target});
    }
    // Computing a value may use scratch registers and R0, so those are parked
    // on the stack before any argument register is written. Register homes
    // are then read in one parallel copy, before pops, reloads and literals,
    // each of which writes only its own argument register (and R0/R3).
    for (const auto& argument : computed) { materialize(argument.value); emitPush(0); }
    emitParallelCopies(std::move(copies));
    for (auto argument = computed.rbegin(); argument != computed.rend(); ++argument) emitPop(argument->target);
    for (const auto& argument : spilled) {
        // Load the frame word straight into its register (TO before LDW, or
        // LMS); no spill-cache copy, since spare registers may hold arguments.
        const auto offset = m_spill_offsets.at(argument.value.value);
        m_spill_cache.clobber(argument.target);
        if (shortFrameSlot(offset)) { emitShortFrameAccess(true, argument.target, offset); continue; }
        emitFrameAddress(Register::R3, offset);
        emitByte(static_cast<std::uint8_t>(0x10 | argument.target)); emitByte(0x43);
    }
    for (const auto& literal : literals) emitRegisterLiteral(literal.first, literal.second);
    m_accumulator_value = IRValueId{};
    m_last_ram_word_address = IRValueId{};
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
        if (m_ephemeral_values.count(address.value) && m_accumulator_value.value == address.value &&
            provenAddress(address, target.type, instruction.type.sizeInBytes)) {
            std::int64_t literal = 0;
            if (value_reg || constantValue(value, literal)) {
                if (!value_reg) emitRegisterLiteral(6, static_cast<std::uint16_t>(literal));
                emitStore(Register::R0, value_reg ? value_reg : 6, byte);
                if (!byte) m_last_ram_word_address = address;
                return;
            }
        }
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
        // A successful check of this immutable SSA address earlier in the
        // block covers its register home exactly as it covers a reload into
        // R0: ensureAddressChecked would emit nothing for it either.
        const auto credited = m_checked_addresses.find(address.value);
        const bool checked_home = target.type.space == AddressSpace::RAM && credited != m_checked_addresses.end() &&
            credited->second >= instruction.type.sizeInBytes;
        if (globallyOptimized() && address_location && address_location->has_register &&
            address_location->physical_register <= 11 &&
            (checked_home || provenAddress(address, target.type, instruction.type.sizeInBytes))) {
            // The address is already live in an allocatable RAM-address
            // register. STW/STB reads it before consuming the selected value.
            emitStore(static_cast<Register>(address_location->physical_register), value_reg, byte);
            m_last_ram_word_address = word ? address : IRValueId{};
            return;
        }
        const auto reg = static_cast<std::uint8_t>(value_reg == 0 ? 6 : value_reg);
        if (value_reg == 0) emitMove(Register::R6_LMULT_HI, 0);
        materialize(address);
        if (!(target.opcode == IROpcode::Address && target.operation.empty() && target.symbol_id.isValid()))
            ensureAddressChecked(address, instruction.type.sizeInBytes);
        emitStore(Register::R0, reg, byte);
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
        emitMove(Register::R3, 0);
        if (isFarPointer(pointer)) emitMove(Register::R6_LMULT_HI, 4);
        if (isFarPointer(instruction.type)) emitPop(4);
        emitPop(0);
        if (isFarPointer(pointer)) emitSelectBank(6, AddressSpace::RAM);
        if (isFarPointer(instruction.type)) {
            emitStore(Register::R3, 4, false);
            emitByte(0xd3); emitByte(0xd3);
        }
        emitStore(Register::R3, 0, byte);
        if (isFarPointer(pointer)) {
            emitNearBank(3, AddressSpace::RAM);
            emitSelectBank(3, AddressSpace::RAM);
        }
        return;
    }

    if (instruction.operation == "declare") {
        materialize(value);
        emitMove(static_cast<Register>(scratchRegister()), 0);
        materialize(address);
        emitStore(Register::R0, scratchRegister(), byte);
        return;
    }

    materialize(address);
    emitPush(0);
    materialize(value);
    emitPop(scratchRegister());
    emitStore(static_cast<Register>(scratchRegister()), 0, byte);
}

void IRCodeGenerator::planHardwareLoopPreservation() {
    // Match a complete exposed scope, not a source pattern or merely the
    // natural loop. No edge may enter the body/leave without its setup, and
    // no instruction or PHI copy may borrow R4/R6 while they own the saves.
    const auto& blocks = m_current_function->blocks;
    std::vector<std::vector<std::uint32_t>> predecessors(blocks.size());
    for (const auto& block : blocks) for (const auto& instruction : block.instructions) {
        if (instruction.opcode == IROpcode::Phi) continue;
        for (const auto target : instruction.targets) {
            auto& incoming = predecessors.at(target.value);
            if (std::find(incoming.begin(), incoming.end(), block.id.value) == incoming.end())
                incoming.push_back(block.id.value);
        }
    }
    for (const auto& block : blocks) {
        if (block.instructions.empty()) continue;
        const auto& setup = block.instructions.back();
        if (setup.opcode != IROpcode::HardwareLoop || setup.targets.size() != 1 ||
            setup.targets[0].value != setup.loop_target.value) continue;
        std::int64_t count = 0;
        const auto* count_location = m_register_allocator.find(setup.operands.front());
        // Loading an already evaluated scalar home does not touch SP or
        // introduce faults between the capacity decision and the capture.
        if (!constantValue(setup.operands.front(), count) && !m_spill_offsets.count(setup.operands.front().value) &&
            !(count_location && count_location->has_register)) continue;
        const auto body_id = setup.loop_target.value;
        std::set<std::uint32_t> visited;
        auto current = body_id;
        bool plots = false;
        // Compaction can retain an empty condition header before its PLOT
        // latch. Admit only a small single-entry chain, never arbitrary CFG.
        while (visited.size() < 32 && visited.insert(current).second) {
            const auto& body = blocks.at(current).instructions;
            if (body.empty()) break;
            bool closed = true;
            for (auto i = body.begin(); i != body.end() - 1; ++i) {
                plots = plots || i->opcode == IROpcode::Plot;
                closed = closed && (i->opcode == IROpcode::Plot || i->opcode == IROpcode::Cache);
            }
            if (!closed) break;
            const auto& end = body.back();
            if (end.opcode == IROpcode::Branch && end.targets.size() == 1) {
                const auto next = end.targets[0].value;
                if (predecessors.at(next).size() != 1 || predecessors[next][0] != current) break;
                current = next;
                continue;
            }
            if (!plots || end.opcode != IROpcode::HardwareLoopEnd || end.loop_id != setup.loop_id ||
                end.targets.size() != 2 || end.targets[0].value != body_id || predecessors[body_id].size() != 2 ||
                std::find(predecessors[body_id].begin(), predecessors[body_id].end(), block.id.value) == predecessors[body_id].end() ||
                std::find(predecessors[body_id].begin(), predecessors[body_id].end(), current) == predecessors[body_id].end()) break;
            const auto exit_id = end.targets[1].value;
            const auto& exit = blocks.at(exit_id).instructions;
            if (!exit.empty() && predecessors[exit_id].size() == 1 && predecessors[exit_id][0] == current &&
                exit.front().opcode == IROpcode::HardwareLoopLeave && exit.front().loop_id == setup.loop_id)
                m_register_hardware_loops.insert(setup.loop_id);
            break;
        }
    }
}

void IRCodeGenerator::emitBoundaryTransfer(std::uint32_t value, bool load) {
    const auto* location = m_register_allocator.find(IRValueId{value});
    if (!location || !location->boundary_spill || !location->has_register || !m_spill_offsets.count(value)) return;
    if (load) {
        emitSpill(IRValueId{value}, true);
        emitMove(static_cast<Register>(location->physical_register), 0);
        m_accumulator_value = IRValueId{};
        return;
    }
    emitWordSpillStore(IRValueId{value}, location->physical_register);
}

void IRCodeGenerator::emitLoopBoundary(const IRInstruction&, bool reload) {
    if (!globallyOptimized()) return;
    const auto block = static_cast<std::uint32_t>(m_current_block_index);
    if (!m_boundary_stores_emitted.count(block)) {
        const auto& stores = m_register_allocator.boundaryStores(block);
        if (!stores.empty()) m_boundary_stores_emitted.insert(block);
        for (const auto value : stores) emitBoundaryTransfer(value, false);
    }
    if (!reload) return;
    for (const auto value : m_register_allocator.boundaryReloads(block)) emitBoundaryTransfer(value, true);
}

void IRCodeGenerator::emitHardwareLoopCapacityGuard(const Token& source) {
    if (m_stack_proof.active()) { m_stack_proof.guard(4); return; }
    if (m_stack_check_credit.covers(4)) return;
    const auto ready = localLabel();
    const auto patch = m_object_file.code_section.size();
    emitWordLiteral(Register::R3, 0);
    addRelocation(std::string(GSUAbi::StackLimitPrefix) + "4", patch, RelocationType::ADDR16_RAM);
    emitByte(0xba); emitByte(0x3f); emitByte(0x63);
    emitLocalJump(ready, static_cast<std::uint8_t>(OpCode::BCS));
    // Preserve the old failure order, including SP and the first saved word:
    // SP < floor+4 either fails the first checked push, or fails the second
    // check after saving R12 and decrementing SP. The second store cannot run.
    // Every function already requires floor+4 for its frame, so this new
    // relocation introduces no additional link-time representability bound.
    emitPush(12);
    emitStackGuard(2, source);
    bindLabel(ready);
}

void IRCodeGenerator::emitHardwareLoopRestore(std::uint32_t loop_id) {
    if (m_register_hardware_loops.count(loop_id)) {
        emitMove(Register::R13_LOOP_ADDR, 6);
        emitMove(Register::R12_LOOP_COUNT, 4);
    } else {
        emitPop(13); emitPop(12);
    }
}

void IRCodeGenerator::emitHardwareLoop(const IRInstruction& instruction) {
    m_last_ram_word_address = IRValueId{};
    if (instruction.operands.size() != 1) {
        fail("IR codegen: hardware loop must have a count operand.", instruction.source);
    }
    // The body has an implicit backedge: straight-line state before its first
    // iteration cannot justify eliding COLOR/CMODE on subsequent iterations.
    m_known_color = m_known_plot_options = -1;
    const bool register_save = m_register_hardware_loops.count(instruction.loop_id) != 0;
    // Keep all stack-fault decisions before count evaluation, as on the
    // general path. Untaken/zero-trip source edges never enter this setup.
    if (register_save) emitHardwareLoopCapacityGuard(instruction.source);
    else { emitPush(12); emitPush(13); }
    std::int64_t count = 0;
    const bool constant = constantValue(instruction.operands[0], count);
    if (register_save) {
        if (!constant) materialize(instruction.operands[0]);
        // Count materialization can use both the far-bank accumulator R4 and
        // volatile arithmetic scratch R6. Capture only after it is complete;
        // the proven body and its empty PHI edges cannot touch either save.
        emitMove(Register::R4_LMULT_LO, 12);
        emitMove(Register::R6_LMULT_HI, 13);
    }
    if (constant) {
        emitRegisterLiteral(12, static_cast<std::uint16_t>(count));
    } else {
        if (!register_save) materialize(instruction.operands[0]);
        emitMove(Register::R12_LOOP_COUNT, 0);
    }
    emitRegionalLoopValues(instruction.loop_id);
    // TO executes with R15 at the prefetched first body byte. Copy PC to R13,
    // never R13 to PC; LOOP executes its prefetched NOP before returning here.
    if (instruction.targets.empty()) emitMove(Register::R13_LOOP_ADDR, 15);
    else {
        const auto patch = m_object_file.code_section.size();
        emitWordLiteral(Register::R13_LOOP_ADDR, 0);
        addRelocation(loopEntrySymbol(threadedLoopEntry(instruction.loop_target), true), patch, RelocationType::ADDR16_IWT);
        const auto header = m_hardware_headers.find(instruction.loop_id);
        if (header != m_hardware_headers.end() && m_cache_trips.count(header->second.value) && count >= 2) {
            m_auto_caches.push_back({m_object_file.code_section.size(), header->second,
                static_cast<std::uint32_t>(count), false});
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
    m_direct_register_load = false;
    const auto& instruction = producer(value, Token(TokenType::UNKNOWN, "", 0, 0));
    const bool scalar_snapshot = optimized() && ((instruction.type.pointer_level == 0 &&
        (instruction.type.base == BaseType::BYTE || instruction.type.base == BaseType::WORD || instruction.type.base == BaseType::BOOL)) ||
        (globallyOptimized() && instruction.type.pointer_level > 0 &&
         instruction.type.space == AddressSpace::RAM && !isFarPointer(instruction.type)) ||
        m_ephemeral_values.count(value.value));
    if (scalar_snapshot && value.value != m_emitting_value.value && m_accumulator_value.value == value.value) return;
    const auto regional = value.value != m_emitting_value.value ? regionalRegister(value) : -1;
    if (regional >= 0) {
        emitMove(Register::R0, static_cast<std::uint8_t>(regional));
        if (scalar_snapshot) m_accumulator_value = value;
        return;
    }
    if (m_spill_offsets.count(value.value) && value.value != m_emitting_value.value) {
        const auto copy = globallyOptimized() && scalar_snapshot ? m_spill_cache.find(value) : -1;
        if (copy >= 0) emitMove(Register::R0, static_cast<std::uint8_t>(copy));
        else { emitSpill(value, true); retainSpillCopy(value, true); }
        if (scalar_snapshot) m_accumulator_value = value;
        return;
    }
    const auto* location = m_register_allocator.find(value);
    if (scalar_snapshot && value.value == m_emitting_value.value && usesRegisterAllocation(value) &&
        (emitAllocatedMultiply(instruction, location->physical_register) ||
         emitAllocatedOperation(instruction, location->physical_register))) {
        // Eager emission has no R0-result contract: later consumers load the
        // allocated register. Do not claim an accumulator snapshot here.
        m_materialized_values.insert(value.value);
        return;
    }
    if (optimized() && value.value != m_emitting_value.value && location && location->has_register) {
        emitMove(Register::R0, location->physical_register);
        if (scalar_snapshot) m_accumulator_value = value;
        return;
    }
    if (!optimized() && location != nullptr && !location->has_register && !location->rematerializable) {
        fail("IR codegen: value with observable effects requires a spill slot.",
             instruction.source);
    }
    if (usesRegisterAllocation(value) &&
        m_materialized_values.count(value.value) != 0) {
        emitMove(Register::R0, location->physical_register);
        if (scalar_snapshot) m_accumulator_value = value;
        return;
    }
    if (!m_active_values.insert(value.value).second) {
        fail("IR codegen: cyclic value definition.", instruction.source);
    }

    switch (instruction.opcode) {
        case IROpcode::PlotCoordinateRead:
            emitMove(Register::R0, instruction.immediate == 0 ? 1 : 2);
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
        if (!m_direct_register_load)
            emitMove(static_cast<Register>(location->physical_register), 0);
        m_materialized_values.insert(value.value);
    }
    const bool direct_register_load = m_direct_register_load;
    m_direct_register_load = false;
    if (m_spill_offsets.count(value.value)) { emitSpill(value, false); retainSpillCopy(value, false); }
    m_active_values.erase(value.value);
    if (scalar_snapshot && !direct_register_load) m_accumulator_value = value;
}

void IRCodeGenerator::emitPhiCopies(IRBlockId target, const Token& source) {
    m_checked_addresses.clear();
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
            if (register_only) emitMove(Register::R6_LMULT_HI, m_register_allocator.find(saved)->physical_register);
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
        std::int64_t literal = 0;
        if (!move.saved && destination && destination->has_register &&
            !isFarPointer(producer(move.source, source).type) && constantValue(move.source, literal)) {
            emitRegisterLiteral(destination->physical_register, static_cast<std::uint16_t>(literal));
        } else if (destination && destination->has_register && ((move.saved && register_only) ||
            (!move.saved && input && input->has_register))) {
            emitMove(static_cast<Register>(destination->physical_register), move.saved ? 6 : input->physical_register);
        } else if (optimized() && !move.saved && input && input->has_register &&
                   m_spill_offsets.count(move.destination.value) &&
                   !isFarPointer(producer(move.destination, source).type)) {
            emitWordSpillStore(move.destination, input->physical_register);
        } else {
            if (move.saved) emitPop(0); else materialize(move.source);
            if (destination && destination->has_register) emitMove(static_cast<Register>(destination->physical_register), 0);
            else if (m_spill_offsets.count(move.destination.value)) emitSpill(move.destination, false);
            else fail("IR codegen: live phi has no register or spill slot.", source);
        }
        m_accumulator_value = IRValueId{}; m_last_ram_word_address = IRValueId{};
        moves.erase(moves.begin() + static_cast<std::ptrdiff_t>(ready));
    }
}

void IRCodeGenerator::emitMemoryInitialize(const IRInstruction& instruction) {
    const auto& root = producer(instruction.operands.at(0), instruction.source);
    if (m_current_function->immutable_local_arrays.count(root.symbol_id)) return;
    const auto& symbols = localSymbols(*m_current_function);
    const auto found = symbols.find(root.symbol_id);
    if (found == symbols.end() || found->second.stackOffset >= 0 || found->second.type.is_volatile ||
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
    emitAddress(root); emitMove(Register::R3, 0);
    const bool byte = usesByteStorage(instruction.type);
    if (uniform) {
        emitRegisterLiteral(12, static_cast<std::uint16_t>(values.size()));
        const auto label = localLabel();
        const auto patch = m_object_file.code_section.size();
        emitWordLiteral(Register::R13_LOOP_ADDR, 0); addRelocation(label, patch, RelocationType::ADDR16_IWT);
        emitLiteral(values[0]);
        // Small, completely known emitted body. Fetch savings repay all cold
        // cache lines even at the worst final 16-byte alignment.
        if (!m_manual_cache) emitByte(static_cast<std::uint8_t>(OpCode::CACHE));
        bindLabel(label);
        emitStore(Register::R3, 0, byte); emitByte(0xd3); if (!byte) emitByte(0xd3);
        emitByte(0x3c); emitByte(1);
        emitPop(13); emitPop(12);
    } else {
        for (std::size_t n = 0; n < values.size(); ++n) {
            if (n == 0 || values[n] != values[n - 1]) emitLiteral(values[n]);
            emitStore(Register::R3, 0, byte);
            if (n + 1 < values.size()) { emitByte(0xd3); if (!byte) emitByte(0xd3); }
        }
    }
    m_accumulator_value = IRValueId{}; m_last_ram_word_address = IRValueId{};
}

void IRCodeGenerator::planLocalValues() {
    for (const auto& block : m_current_function->blocks) {
        std::map<std::uint32_t, std::size_t> positions;
        for (std::size_t n = 0; n < block.instructions.size(); ++n)
            if (block.instructions[n].result.isValid()) positions.emplace(block.instructions[n].result.value, n);
        for (std::size_t n = 0; n < block.instructions.size(); ++n) {
            const auto& write = block.instructions[n];
            if (write.opcode != IROpcode::PlotCoordinateWrite) continue;
            const auto& update = producer(write.operands[0], write.source);
            if (update.opcode != IROpcode::Binary || update.operands.size() != 2 ||
                (update.operation != "+" && update.operation != "-") || m_use_counts[update.result.value] != 1) continue;
            const auto& read = producer(update.operands[0], write.source);
            std::int64_t step = 0;
            if (read.opcode != IROpcode::PlotCoordinateRead || read.immediate != write.immediate ||
                m_use_counts[read.result.value] != 1 || !constantValue(update.operands[1], step) ||
                (step != 1 && step != -1) || !positions.count(read.result.value) ||
                !positions.count(update.result.value) || positions.at(read.result.value) >= n) continue;
            bool adjacent = true;
            for (auto p = positions.at(read.result.value) + 1; p < n; ++p) {
                const auto& i = block.instructions[p];
                if (i.opcode != IROpcode::Constant && i.result.value != update.result.value) {
                    adjacent = false; break;
                }
            }
            if (!adjacent) continue;
            const bool increment = (step == 1) == (update.operation == "+");
            m_cursor_updates.emplace(update.result.value, static_cast<std::uint8_t>((increment ? 0xd0 : 0xe0) | (write.immediate ? 2 : 1)));
            m_cursor_elided_values.insert(read.result.value); m_cursor_elided_values.insert(update.result.value);
        }
        for (std::size_t n = 0; n + 1 < block.instructions.size(); ++n) {
            const auto& value = block.instructions[n];
            // A register parameter has no memory word to reload at its use.
            if (!value.producesValue() || m_use_counts[value.result.value] != 1 || m_parameter_inputs.count(value.result.value) ||
                m_cursor_elided_values.count(value.result.value) || m_branch_comparisons.count(value.result.value)) continue;
            const auto* location = m_register_allocator.find(value.result);
            if (!location || location->has_register || location->rematerializable) continue;
            auto next = n + 1;
            while (next < block.instructions.size() && block.instructions[next].opcode == IROpcode::Constant &&
                !isFarPointer(block.instructions[next].type)) ++next;
            if (next == block.instructions.size()) continue;
            const auto& consumer = block.instructions[next];
            if (consumer.operands.empty()) continue;
            bool first = consumer.operands[0].value == value.result.value;
            bool direct = first && (consumer.opcode == IROpcode::Cast || consumer.opcode == IROpcode::Unary ||
                consumer.opcode == IROpcode::BitExtract || consumer.opcode == IROpcode::Return ||
                consumer.opcode == IROpcode::PlotCoordinateWrite || consumer.opcode == IROpcode::SetColor ||
                consumer.opcode == IROpcode::LoadIndirect || consumer.opcode == IROpcode::CondBranch);
            if (first && consumer.opcode == IROpcode::Binary && consumer.operands.size() == 2) {
                std::int64_t literal = 0;
                const auto* right = m_register_allocator.find(consumer.operands[1]);
                direct = constantValue(consumer.operands[1], literal) || (right && right->has_register);
            }
            if (consumer.opcode == IROpcode::StoreIndirect) {
                const auto* stored = m_register_allocator.find(consumer.operands[1]);
                std::int64_t literal = 0;
                direct = first && !isFarPointer(consumer.type) && provenAddress(value.result, value.type, consumer.type.sizeInBytes) &&
                    ((stored && stored->has_register) || constantValue(consumer.operands[1], literal));
                // Value-first store materialization consumes R0 before calculating
                // the address; no pointer arithmetic is deferred or reordered.
                direct = direct || consumer.operands[1].value == value.result.value;
            }
            if (!direct || (consumer.opcode == IROpcode::PlotCoordinateWrite &&
                m_cursor_updates.count(consumer.operands[0].value))) continue;
            const auto address_width = first && (consumer.opcode == IROpcode::LoadIndirect || consumer.opcode == IROpcode::StoreIndirect)
                ? consumer.type.sizeInBytes : 1;
            if (value.type.pointer_level && (!provenAddress(value.result, value.type, address_width) || isFarPointer(value.type))) continue;
            if (value.opcode == IROpcode::DivMod || value.opcode == IROpcode::Phi || isFarPointer(value.type)) continue;
            m_ephemeral_values.insert(value.result.value);
            if (value.type.pointer_level) m_ephemeral_address_widths.emplace(value.result.value, address_width);
        }
    }
}

std::string IRCodeGenerator::loopEntrySymbol(IRBlockId target, bool backedge) const {
    auto symbol = internalBlockSymbol(m_current_function->name, target);
    if (backedge && m_cached_headers.count(target.value)) symbol += "@after_cache";
    return symbol;
}

void IRCodeGenerator::planLoopCaches() {
    if (m_current_function->blocks.size() > 256 || m_current_function->value_count > 4096) return;
    const IRControlFlow cfg(*m_current_function);
    std::vector<const std::set<std::uint32_t>*> manual_scopes;
    for (const auto& loop : cfg.loops) {
        const auto& code = m_current_function->blocks[loop.header].instructions;
        auto first = code.begin();
        while (first != code.end() && first->opcode == IROpcode::Phi) ++first;
        if (first != code.end() && first->opcode == IROpcode::Cache) manual_scopes.push_back(&loop.blocks);
    }
    std::size_t work = 0;
    for (const auto& loop : cfg.loops) {
        if (loop.latches.size() != 1 || !cfg.reachable[loop.header]) continue;
        // `fenced`: an operation that may execute a CACHE outside this
        // selector's control (a callee or an initializer kernel). LOOP never
        // writes CBR; an inline division kernel's CACHE is recorded in
        // m_division_caches and demoted when an enclosing window is selected;
        // selectAutomaticCaches claims an outer nest before nested candidates.
        // The explicit-header bypass below stays conservative about all three.
        unsigned caches = 0; bool fenced = false, nested_kernel = false;
        for (const auto b : loop.blocks) for (const auto& i : m_current_function->blocks[b].instructions) {
            if (++work > 1000000) return;
            caches += i.opcode == IROpcode::Cache;
            nested_kernel = nested_kernel || i.opcode == IROpcode::HardwareLoop || i.opcode == IROpcode::DivMod ||
                (i.opcode == IROpcode::Binary && (i.operation == "/" || i.operation == "%"));
            fenced = fenced || i.opcode == IROpcode::Call || i.opcode == IROpcode::MemoryInitialize;
        }
        const bool rebases = fenced || nested_kernel;
        const auto& header = m_current_function->blocks[loop.header];
        auto first = header.instructions.begin();
        while (first != header.instructions.end() && first->opcode == IROpcode::Phi) ++first;
        if (!rebases && caches == 1 && first != header.instructions.end() && first->opcode == IROpcode::Cache) {
            m_cached_headers.insert(loop.header);
            for (const auto b : loop.blocks) if (std::find(cfg.successors[b].begin(), cfg.successors[b].end(), loop.header) != cfg.successors[b].end())
                m_cached_backedges.emplace(b, loop.header);
        }
        const bool manual_scope = std::any_of(manual_scopes.begin(), manual_scopes.end(),
            [&](const std::set<std::uint32_t>* blocks) { return blocks->count(loop.header) != 0; });
        for (const auto& block : m_current_function->blocks) {
            if (loop.blocks.count(block.id.value) || block.instructions.empty()) continue;
            const auto& setup = block.instructions.back();
            if (setup.opcode != IROpcode::HardwareLoop || !setup.loop_target.isValid() ||
                !loop.blocks.count(setup.loop_target.value)) continue;
            bool matches = false;
            for (const auto b : loop.blocks) for (const auto& i : m_current_function->blocks[b].instructions) {
                if (++work > 1000000) return;
                matches = matches || (i.opcode == IROpcode::HardwareLoopEnd && i.loop_id == setup.loop_id);
            }
            if (!matches) continue;
            m_hardware_headers.emplace(setup.loop_id, IRBlockId{loop.header});
            std::int64_t trips = 0;
            if (m_allow_auto_cache && !manual_scope && !m_current_function->is_cached && !fenced && !caches &&
                m_cache_trips.size() < 32 && constantValue(setup.operands[0], trips) && trips >= 2 && trips <= 65535) {
                m_cache_trips.emplace(loop.header, static_cast<std::uint32_t>(trips));
                m_constant_cache_trips.insert(loop.header);
            }
        }
        if (m_cache_trips.count(loop.header)) m_cache_loop_blocks.emplace(loop.header, loop.blocks);
        if (!m_allow_auto_cache || manual_scope || m_current_function->is_cached || fenced || caches || m_cache_trips.count(loop.header) ||
            cfg.predecessors[loop.header].size() != 2 || header.instructions.empty()) continue;
        // A trip count unknown here is assumed small but repeated: one cold
        // fill then cached iterations. Selection still requires the loop to be
        // laid out after its entry and to fit one window; a constant count
        // below replaces this estimate.
        if (m_cache_trips.size() < 32) {
            m_cache_trips.emplace(loop.header, AssumedDynamicTrips);
            m_cache_loop_blocks.emplace(loop.header, loop.blocks);
        }
        const auto& branch = header.instructions.back();
        if (branch.opcode != IROpcode::CondBranch || branch.targets.size() != 2 ||
            !loop.blocks.count(branch.targets[0].value) || loop.blocks.count(branch.targets[1].value)) continue;
        const auto& test = producer(branch.operands[0], branch.source);
        if (test.opcode != IROpcode::Binary || test.operands.size() != 2 || (test.operation != "<" && test.operation != "<=")) continue;
        const auto& phi = producer(test.operands[0], test.source);
        const auto& limit = producer(test.operands[1], test.source);
        if (phi.opcode != IROpcode::Phi || phi.operands.size() != 2 || phi.type.pointer_level || phi.type.base != BaseType::WORD ||
            phi.type.base != limit.type.base || phi.type.is_unsigned != limit.type.is_unsigned) continue;
        IRValueId initial, update;
        for (std::size_t edge = 0; edge < 2; ++edge) {
            if (loop.latches.count(phi.targets[edge].value)) update = phi.operands[edge];
            else if (!loop.blocks.count(phi.targets[edge].value)) initial = phi.operands[edge];
        }
        if (!initial.isValid() || !update.isValid()) continue;
        const auto& step = producer(update, test.source);
        std::int64_t start = 0, bound = 0, stride = 0;
        if (step.opcode != IROpcode::Binary || step.operation != "+" || step.operands.size() != 2 ||
            step.operands[0].value != phi.result.value || !constantValue(step.operands[1], stride) || stride != 1 ||
            !constantValue(initial, start) || !constantValue(limit.result, bound)) continue;
        start = ConstantEvaluator::convert(start, phi.type); bound = ConstantEvaluator::convert(bound, phi.type);
        const auto trips = bound - start + (test.operation == "<=" ? 1 : 0);
        if (trips < 2 || trips > 65535 || start + trips > (phi.type.is_unsigned ? 65535 : 32767)) continue;
        bool exits = false;
        for (const auto b : loop.blocks) if (b != loop.header) for (const auto target : cfg.successors[b])
            exits = exits || !loop.blocks.count(target);
        if (!exits && m_cache_trips.count(loop.header)) {
            m_cache_trips[loop.header] = static_cast<std::uint32_t>(trips);
            m_constant_cache_trips.insert(loop.header);
        }
    }
}

void IRCodeGenerator::selectAutomaticCaches() {
    if (m_auto_caches.empty()) return;
    m_had_cache_probe = true;
    const IRControlFlow cfg(*m_current_function);
    const auto loopOf = [&](IRBlockId header) -> const IRControlFlow::Loop* {
        for (const auto& loop : cfg.loops) if (loop.header == header.value) return &loop;
        return nullptr;
    };
    // Outer loops first: one window over a whole nest stays valid across its
    // outer iterations, and blocks of a selected loop cannot be claimed again
    // by a nested or overlapping candidate. Further entries of a selected
    // loop are converted too, so every way in starts the same window.
    std::vector<std::size_t> order(m_auto_caches.size());
    for (std::size_t n = 0; n < order.size(); ++n) order[n] = n;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const auto* x = loopOf(m_auto_caches[a].header);
        const auto* y = loopOf(m_auto_caches[b].header);
        return (x ? x->blocks.size() : 0) > (y ? y->blocks.size() : 0);
    });
    std::set<std::uint32_t> selected, selected_headers;
    for (const auto index : order) {
        const auto& candidate = m_auto_caches[index];
        const auto* loop = loopOf(candidate.header);
        if (!loop) continue;
        const auto anchor = candidate.entry_slot ? m_block_addresses.at(loop->header) : candidate.offset + 1;
        std::size_t end = anchor;
        bool ordered = true;
        bool overlaps = false;
        for (const auto block : loop->blocks) {
            const auto start = m_block_addresses.at(block);
            ordered = ordered && start >= anchor;
            overlaps = overlaps || selected.count(block) != 0;
            const auto next = static_cast<std::size_t>(block) + 1;
            end = std::max(end, next < m_current_function->blocks.size() ? m_block_addresses.at(static_cast<std::uint32_t>(next)) :
                m_object_file.code_section.size());
        }
        const auto bytes = end - anchor;
        // The linker can place this object at any byte alignment. Reserve
        // 15 bytes, not just the current object's observed low PC bits.
        const auto cold = 80u * ((bytes + 30u) / 16u);
        const bool fits = ordered && bytes <= 496;
        if (selected_headers.count(loop->header) ? fits :
            fits && !overlaps && static_cast<std::uint64_t>(candidate.trips - 1) * bytes * 4 > cold + 80) {
            m_object_file.code_section.at(candidate.offset) = static_cast<std::uint8_t>(OpCode::CACHE);
            m_selected_auto_cache = true;
            // Division kernels inside this window would otherwise anchor CBR
            // on their own line and flush the window on every pass. Slots are
            // offsets into this function's append-only emission; each is
            // demoted once and must still hold the kernel's CACHE.
            const auto inside = [&](std::size_t slot) { return slot >= anchor && slot < end; };
            for (const auto slot : m_division_caches) if (inside(slot)) {
                auto& byte = m_object_file.code_section.at(slot);
                if (byte != static_cast<std::uint8_t>(OpCode::CACHE))
                    fail("Internal error: division CACHE slot was overwritten before window selection.",
                        Token(TokenType::UNKNOWN, m_current_function->name, 1, 1));
                byte = static_cast<std::uint8_t>(OpCode::NOP);
            }
            m_division_caches.erase(std::remove_if(m_division_caches.begin(), m_division_caches.end(), inside),
                m_division_caches.end());
            if (!selected_headers.count(loop->header) && m_constant_cache_trips.count(loop->header)) {
                // Os exchange evidence in candidateSpeed's units (bytes times
                // structural loop weight): a warm cached byte costs 1 clock
                // instead of 5, and each conservatively counted cold line costs
                // 16 uncached bytes per entry into the window. The structural
                // weight assumes 64 trips per loop level; a smaller proved count
                // scales the saving down. Unknown trip counts earn no credit.
                std::uint64_t saving = 0;
                for (const auto block : loop->blocks) {
                    const auto start = m_block_addresses.at(block);
                    const auto next = static_cast<std::size_t>(block) + 1;
                    const auto stop = next < m_current_function->blocks.size() ?
                        m_block_addresses.at(static_cast<std::uint32_t>(next)) : m_object_file.code_section.size();
                    if (stop <= start) continue;
                    saving = GSUCost::saturatedAdd(saving, GSUCost::saturatedMultiply(stop - start,
                        GSUCostModel::blockWeight(cfg.loop_depth.at(block))) / 5 * 4);
                }
                saving = saving / 64 * std::min<std::uint64_t>(candidate.trips, 64);
                const auto depth = cfg.loop_depth.at(loop->header);
                const auto cold_cost = GSUCost::saturatedMultiply(cold / 5,
                    GSUCostModel::blockWeight(depth ? depth - 1u : 0u));
                if (saving > cold_cost) {
                    auto& credit = m_cache_credit[m_current_function->name];
                    credit = GSUCost::saturatedAdd(credit, saving - cold_cost);
                }
            }
            selected.insert(loop->blocks.begin(), loop->blocks.end());
            selected_headers.insert(loop->header);
        }
    }
}

void IRCodeGenerator::emitBranch(IRBlockId target, const Token& source) {
    if (globallyOptimized()) {
        IRInstruction edge; edge.targets = {target}; edge.source = source;
        emitLoopBoundary(edge, true);
        emitPhiCopies(target, source);
        emitSoftwareRegionalValues(static_cast<std::uint32_t>(m_current_block_index), target);
    }
    const bool bypass = m_cached_backedges.count({static_cast<std::uint32_t>(m_current_block_index), target.value}) != 0;
    const auto alias = bypass ? loopEntrySymbol(target, true) : std::string{};
    if (!bypass && target.isValid() && target.value == m_current_block_index + 1) {
        // Falling into a candidate loop from outside: a selector-reset NOP
        // here can become CACHE, anchoring CBR at the header that follows.
        // It costs a byte even when unused, so size mode keeps only the
        // existing free delay-slot candidates.
        const auto loop = m_cache_loop_blocks.find(target.value);
        if (!sizeOptimized() && loop != m_cache_loop_blocks.end() &&
            !loop->second.count(static_cast<std::uint32_t>(m_current_block_index))) {
            m_auto_caches.push_back({m_object_file.code_section.size(), target, m_cache_trips.at(target.value), false});
            emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        }
        return;
    }
    const auto serial = m_branch_serial++;
    if (longBranch(serial)) {
        emitByte(static_cast<std::uint8_t>(OpCode::IWT) | GSUAbi::ProgramCounterRegister);
        const auto patch_offset = m_object_file.code_section.size();
        emitWord(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({patch_offset, target, source, true, alias, serial});
        if (!bypass && m_cache_trips.count(target.value))
            m_auto_caches.push_back({patch_offset + 2, target, m_cache_trips.at(target.value), true});
        return;
    }
    emitByte(static_cast<std::uint8_t>(OpCode::BRA));
    const auto patch_offset = m_object_file.code_section.size();
    emitByte(0);
    emitByte(static_cast<std::uint8_t>(OpCode::NOP));
    m_branch_fixups.push_back({patch_offset, target, source, false, alias, serial});
    if (!bypass && m_cache_trips.count(target.value))
        m_auto_caches.push_back({patch_offset + 1, target, m_cache_trips.at(target.value), true});
}

bool IRCodeGenerator::longBranch(std::size_t serial) const {
    const auto found = m_long_branches.find(m_current_function->name);
    return m_force_long_branches || m_long_branch_functions.count(m_current_function->name) ||
        (found != m_long_branches.end() && found->second.count(serial));
}

void IRCodeGenerator::emitBlockBranch(std::uint8_t opcode, IRBlockId target,
                                      const Token& source) {
    if (opcode == static_cast<std::uint8_t>(OpCode::BRA)) {
        emitBranch(target, source);
        return;
    }
    const auto serial = m_branch_serial++;
    const auto alias = m_cached_backedges.count({static_cast<std::uint32_t>(m_current_block_index), target.value})
        ? loopEntrySymbol(target, true) : std::string{};
    if (!longBranch(serial)) {
        emitByte(opcode);
        const auto patch_offset = m_object_file.code_section.size();
        emitByte(0);
        emitByte(static_cast<std::uint8_t>(OpCode::NOP));
        m_branch_fixups.push_back({patch_offset, target, source, false, alias, serial});
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
    m_branch_fixups.push_back({patch_offset, target, source, true, alias, serial});
}

void IRCodeGenerator::emitConditionalBranch(const IRInstruction& instruction) {
    emitLoopBoundary(instruction, false);
    if (instruction.operands.size() != 1 || instruction.targets.size() != 2) {
        fail("IR codegen: conditional branch shape is invalid.", instruction.source);
    }
    if (optimized() && m_branch_comparisons.count(instruction.operands.front().value)) {
        const auto& comparison = producer(instruction.operands.front(), instruction.source);
        // A just-emitted word +/- 1 already set Z for its register result.
        // Constants emit no bytes here; any other intervening instruction,
        // spill store, byte normalization or different predicate fences this
        // reuse. In particular, loads/calls/graphics effects never qualify.
        bool reuse_zero = false;
        std::int64_t zero = 1;
        const auto& code = m_current_function->blocks.at(m_current_block_index).instructions;
        if (globallyOptimized() && (comparison.operation == "==" || comparison.operation == "!=") &&
            constantValue(comparison.operands[1], zero) && zero == 0 && code.size() >= 3) {
            std::size_t prior = code.size() - 2;
            while (prior && code[prior - 1].opcode == IROpcode::Constant) --prior;
            if (prior) {
                const auto& update = code[prior - 1];
                const auto* place = m_register_allocator.find(update.result);
                const auto* input = update.operands.empty() ? nullptr : m_register_allocator.find(update.operands[0]);
                std::int64_t one = 0;
                reuse_zero = update.opcode == IROpcode::Binary && update.operands.size() == 2 &&
                    update.result.value == comparison.operands[0].value &&
                    update.type.base == BaseType::WORD && update.type.sizeInBytes == 2 && !update.type.pointer_level &&
                    (update.operation == "+" || update.operation == "-") &&
                    place && place->has_register && input && input->has_register &&
                    constantValue(update.operands[1], one) && one == 1;
            }
        }
        const bool reverse = globallyOptimized() && (comparison.operation == "<=" || comparison.operation == ">");
        if (!reuse_zero) emitScalarComparison(comparison, reverse);
        const bool is_unsigned = producer(comparison.operands[0], instruction.source).type.is_unsigned;
        const std::uint8_t less = is_unsigned ? 12 : 7;
        const std::uint8_t ge = is_unsigned ? 13 : 6;
        const auto yes = instruction.targets[0], no = instruction.targets[1];
        const auto op = reverse ? (comparison.operation == "<=" ? ">=" : "<") : comparison.operation;
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
        // MOVES scratch, R0 sets Z like CMP R0,#0; only BEQ/BNE consume it.
        emitByte(static_cast<std::uint8_t>(0x20 | scratchRegister())); emitByte(0xb0);
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

    const auto& selector_type = producer(instruction.operands.front(), instruction.source).type;
    const bool unsigned_selector = selector_type.is_unsigned;
    const bool byte_selector = selector_type.sizeInBytes == 1;
    const std::int64_t minimum = unsigned_selector ? 0 : byte_selector ? -128 : -32768;
    const std::int64_t maximum = unsigned_selector ? (byte_selector ? 255 : 65535) : (byte_selector ? 127 : 32767);
    std::vector<std::size_t> order;
    for (std::size_t index = 0; index < instruction.case_values.size(); ++index) {
        // An out-of-domain label is legal but cannot match. In particular,
        // do not alias case -1 with unsigned 65535, or case 256 with byte 0.
        if (instruction.case_values[index] >= minimum && instruction.case_values[index] <= maximum)
            order.push_back(index);
    }

    std::int64_t constant_selector = 0;
    if (constantValue(instruction.operands.front(), constant_selector)) {
        for (const auto index : order) {
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
    // LDB supplies raw, zero-extended bits. Dispatch compares the byte's
    // signed value in the GSU's 16-bit comparison domain.
    if (byte_selector && !unsigned_selector) emitByte(static_cast<std::uint8_t>(OpCode::SEX));
    emitMove(static_cast<Register>(scratchRegister()), 0);

    // A short switch remains a compact linear chain. For larger switches,
    // compare against the median case and recursively search each half. This
    // reduces comparisons from O(n) to O(log n) without changing case/fall-
    // through semantics or the object-file relocation model.
    if (order.size() < 4) {
        for (const auto case_index : order) {
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
            // CMP computes case - selector. Equality was handled above;
            // signed ordering needs S == V, not just a nonnegative result.
            emit_local_branch(static_cast<std::uint8_t>(unsigned_selector ? OpCode::BCS : OpCode::BGE), left_label);
        } else if (default_index < instruction.targets.size()) {
            emit_block_branch(static_cast<std::uint8_t>(unsigned_selector ? OpCode::BCS : OpCode::BGE),
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
            if (m_cursor_updates.count(instruction.operands.front().value)) {
                emitByte(m_cursor_updates.at(instruction.operands.front().value));
                break;
            }
            if (globallyOptimized() && usesRegisterAllocation(instruction.operands.front())) {
                emitMove(static_cast<Register>(instruction.immediate == 0 ? 1 : 2),
                    m_register_allocator.find(instruction.operands.front())->physical_register);
            } else {
                materialize(instruction.operands.front());
                emitMove(static_cast<Register>(instruction.immediate == 0 ? 1 : 2), 0);
            }
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
                if (!provenAddress(instruction.operands.front(), address, 1)) emitAddressCheck(address, 1);
                if (isFarPointer(address)) emitSelectBank(4, AddressSpace::ROM);
                else { emitNearBank(3, AddressSpace::ROM); emitSelectBank(3, AddressSpace::ROM); }
                emitMove(Register::R14_LR, 0);
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
            else emitHardwareLoopRestore(instruction.loop_id);
            m_known_color = m_known_plot_options = -1;
            break;
        case IROpcode::HardwareLoopLeave:
            emitHardwareLoopRestore(instruction.loop_id);
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
        default:
            break;
    }
}

void IRCodeGenerator::emitBlock(const IRBasicBlock& block) {
    m_spill_cache.clear();
    m_checked_addresses.clear();
    m_block_remaining_uses.clear();
    for (const auto& i : block.instructions) if (i.opcode != IROpcode::Phi)
        for (const auto v : i.operands) ++m_block_remaining_uses[v.value];
    // Emission order does not prove that a predecessor (or an earlier loop
    // trip) ran. Only straight-line stack credit is valid at this entry.
    m_stack_check_credit.reset();
    m_stack_proof.beginBlock();
    m_accumulator_value = IRValueId{};
    m_last_ram_word_address = IRValueId{};
    // POR is unknown at a CFG join/backedge. Only elide straight-line repeats.
    m_known_plot_options = -1;
    m_known_color = -1;
    m_materialized_values.clear();
    if (!m_parameter_inputs.empty() && block.id.value == m_current_function->entry.value) emitParameterInputs();
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
            if (m_cursor_elided_values.count(instruction.result.value)) continue;
            if (m_parameter_inputs.count(instruction.result.value)) continue; // Already in its home.
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
        if (instruction.opcode == IROpcode::Cache && m_cached_headers.count(block.id.value)) {
            m_object_file.symbol_table.push_back({loopEntrySymbol(block.id, true), SymbolSection::CODE,
                static_cast<std::uint32_t>(m_object_file.code_section.size())});
        }
    }
}

bool IRCodeGenerator::patchBranches() {
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
    if (widen) m_relaxation_pending.insert(m_current_function->name);
    return widen;
}

int IRCodeGenerator::planFrameStorage() {
    m_spill_offsets.clear(); m_divmod_offsets.clear();
    int frame_bytes = m_current_function->total_local_alloc_size;
    std::map<int, int> shared_spills;
    for (const auto& value : m_values) {
        if (optimized()) {
            const auto* location = m_register_allocator.find(IRValueId{value.first});
            if (m_branch_comparisons.count(value.first) || m_cursor_elided_values.count(value.first) ||
                m_ephemeral_values.count(value.first) || !m_use_counts[value.first] ||
                (location && (location->rematerializable || (location->has_register && !location->boundary_spill)))) continue;
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
    if (globallyOptimized()) {
        for (const auto& block : m_current_function->blocks) for (const auto& instruction : block.instructions) {
            if (instruction.opcode != IROpcode::DivMod) continue;
            if (frame_bytes > 65524) fail("IR codegen: divmod frame exceeds one RAM bank.", instruction.source);
            frame_bytes += 2;
            m_divmod_offsets.emplace(instruction.result.value, -frame_bytes);
        }
        if (!m_divmod_offsets.empty()) frame_bytes += 2; // Keep the last pair above the empty/null bottom word.
    }
    return frame_bytes;
}

void IRCodeGenerator::recordFinalSpillPlan() {
    if (!m_config.require_no_spills) return;
    const auto record = [&](const std::map<std::uint32_t, int>& homes, SpillCategory category) {
        if (homes.empty()) return;
        std::set<int> words;
        for (const auto& home : homes) {
            words.insert(home.second);
            if (category == SpillCategory::IRValue && isFarPointer(m_values.at(home.first)->type))
                words.insert(home.second + 2);
        }
        SpillIssue issue;
        issue.function = m_current_function->name;
        issue.value = IRValueId{homes.begin()->first};
        issue.source = m_values.at(issue.value.value)->source;
        issue.category = category;
        issue.mapped_values = homes.size(); issue.word_slots = words.size();
        m_spill_issues.push_back(std::move(issue));
    };
    record(m_spill_offsets, SpillCategory::IRValue);
    record(m_divmod_offsets, SpillCategory::DivModAuxiliary);
}

namespace {
std::size_t candidateBytes(const ObjectFile& object) {
    return object.code_section.size() + object.data_section.size() +
        (object.data_section.empty() ? 0u : static_cast<unsigned>(object.code_section.size() & 1u));
}
}

bool IRCodeGenerator::preferCandidate(const GeneratedCandidate& candidate, const GeneratedCandidate& current,
                                      std::size_t smallest) const {
    if (m_config.require_no_spills && candidate.issues.empty() != current.issues.empty())
        return candidate.issues.empty();
    return sizeFirstPreferred({candidateBytes(candidate.object), candidate.inclusive_score, candidate.speed_score},
                              {candidateBytes(current.object), current.inclusive_score, current.speed_score}, smallest);
}

bool IRCodeGenerator::preferUncachedCandidate(const GeneratedCandidate& uncached, const GeneratedCandidate& cached) const {
    // A local smallest enables the existing exchange rule: CACHE may cost at
    // most max(2, smallest/64) bytes, and only for a >=25% lower inclusive
    // estimate. Outside Os inclusive scores are zero, so bytes decide.
    return preferCandidate(uncached, cached, std::min(candidateBytes(uncached.object), candidateBytes(cached.object)));
}

ObjectFile IRCodeGenerator::finishCandidate(GeneratedCandidate candidate) const {
    if (m_config.require_no_spills && !candidate.issues.empty()) {
        const auto& issue = candidate.issues.front();
        const auto category = issue.category == SpillCategory::IRValue ? "IR-value spill homes" : "auxiliary divmod spill homes";
        fail("--require-no-spills: function '" + issue.function + "' requires " + category +
            "; first value %" + std::to_string(issue.value.value) + " (" + std::to_string(issue.mapped_values) +
            " mapped IR value(s), " + std::to_string(issue.word_slots) + " unique 16-bit slot(s)).", issue.source);
    }
    return std::move(candidate.object);
}

void IRCodeGenerator::collectStaticObjects() {
    // Mirror the RAM layout below: only objects this candidate defines, at
    // the alignment it pads them to. Extern objects belong to another
    // object's RAM segment, which this object's marker does not constrain.
    m_static_objects.clear();
    std::uint32_t id = 0;
    for (const auto& pair : m_data_manager.getEntries()) {
        const auto& entry = pair.second;
        if (entry.is_extern || entry.bytes.empty() || entry.bytes.size() > 65536u) continue;
        // Non-RAM entries become word-aligned DATA objects (see the layout
        // below); the linker aligns concatenated DATA to two bytes too.
        if (entry.storage == AddressSpace::RAM)
            m_static_objects.emplace(pair.first, GSUAddressProof::StaticObject{
                id++, static_cast<std::uint32_t>(entry.bytes.size()),
                static_cast<unsigned>(std::max(2, entry.type.alignment)), AddressSpace::RAM});
        else if (entry.storage == AddressSpace::ROM)
            m_static_objects.emplace(pair.first, GSUAddressProof::StaticObject{
                id++, static_cast<std::uint32_t>(entry.bytes.size()), 2u, AddressSpace::ROM});
    }
}

void IRCodeGenerator::runAddressProof(const IRFunction& function, std::size_t frame_bytes) {
    // run() resets the proof; keep whether the previous facts were relied on.
    m_static_ram_proof_used = m_static_ram_proof_used || m_address_proof.usedStaticObjectProof();
    m_address_proof.run(function, localSymbols(function), m_static_objects, frame_bytes, m_checked_pointer_mode);
}

IRCodeGenerator::GeneratedCandidate IRCodeGenerator::generateInternal(const IRModule& module) {
    m_object_file = ObjectFile();
    m_spill_issues.clear();
    m_relaxation_pending.clear();
    m_selected_auto_cache = m_had_cache_probe = false;
    m_cache_credit.clear();
    m_division_helpers.clear();
    m_address_proof = GSUAddressProof{};
    m_static_ram_proof_used = false;
    m_stack_proof.clear();
    if (globallyOptimized() && m_config.static_stack_proof && m_config.target == TargetKind::GSU) m_stack_proof.plan(module);
    planRegisterArguments(module);
    m_static_frame_sizes.clear(); m_static_frame_slots.clear(); m_static_frame_owner.clear();
    m_short_frame_offsets.clear(); m_short_frame_layout.clear(); m_short_frame_sizes.clear(); m_short_ram_admitted = 0;
    planStaticFrames(module);
    collectStaticObjects();
    m_object_file.config = m_config;
    m_object_file.config.bitmap = module.bitmap;
    if (globallyOptimized()) planCallClobbers(module);
    else m_call_clobbers.clear();

    for (const auto& function : module.functions) {
        const auto policy = m_function_policies.find(function.name);
        if (policy != m_function_policies.end()) m_allocation_policy = policy->second;
        const auto division = m_function_division_policies.find(function.name);
        if (division != m_function_division_policies.end()) m_share_division = division->second;
        const auto pool = m_function_pool_policies.find(function.name);
        if (pool != m_function_pool_policies.end()) m_pool_enabled = pool->second;
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
        m_stack_proof.enter(function.link_name.empty() ? function.name : function.link_name);
        m_parameter_inputs = parameterInputs(function);
        m_static_frame_owner.clear(); m_static_frame_bytes = 0; m_frameless = false;
        m_boundary_stores_emitted.clear();
        m_static_ram_proof_used = m_static_ram_proof_used || m_address_proof.usedStaticObjectProof();
        m_address_proof = GSUAddressProof{};
        m_checked_pointer_mode = function.return_type.pointer_level > 0;
        m_has_hardware_loop = false;
        m_register_hardware_loops.clear();
        m_regional_loop_values.clear(); m_regional_software_loops.clear();
        m_auto_caches.clear(); m_division_caches.clear(); m_manual_cache = function.is_cached;
        m_cache_trips.clear(); m_constant_cache_trips.clear(); m_cache_loop_blocks.clear(); m_hardware_headers.clear();
        m_cached_headers.clear(); m_cached_backedges.clear();
        m_cursor_updates.clear(); m_cursor_elided_values.clear(); m_ephemeral_values.clear();
        m_ephemeral_address_widths.clear();
        m_checked_addresses.clear();
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
        if (globallyOptimized()) {
            runAddressProof(function, static_cast<std::size_t>(function.total_local_alloc_size));
            planLocalValues();
            planLoopCaches();
        }
        int frame_bytes = planFrameStorage();
        if (!optimized()) buildRegisterAllocation(function);
        if (globallyOptimized()) {
            runAddressProof(function, static_cast<std::size_t>(frame_bytes));
            const bool lost_proof = std::any_of(m_ephemeral_address_widths.begin(), m_ephemeral_address_widths.end(),
                [&](const std::pair<const std::uint32_t, int>& address) {
                    return !provenAddress(IRValueId{address.first}, m_values.at(address.first)->type, address.second);
                });
            if (lost_proof) {
                // Spill/divmod bottom words can exceed the proof's frame cap.
                // Restore every pointer spill in one bounded retry; no emitter
                // may rematerialize an unchecked address across a saved value.
                for (const auto& address : m_ephemeral_address_widths) m_ephemeral_values.erase(address.first);
                m_ephemeral_address_widths.clear();
                frame_bytes = planFrameStorage();
                runAddressProof(function, static_cast<std::size_t>(frame_bytes));
            }
            planHardwareLoopPreservation();
            planRegionalLoopValues();
        }

        recordFinalSpillPlan();
        const auto frame_owner = function.link_name.empty() ? function.name : function.link_name;
        if (frame_bytes > 0 && m_static_frame_functions.count(frame_owner)) {
            // Even size keeps every word slot at FP-relative parity.
            m_static_frame_owner = frame_owner;
            m_static_frame_bytes = (frame_bytes + 1) & ~1;
            m_static_frame_sizes[frame_owner] = m_static_frame_bytes;
        }
        planShortFrame();
        const bool static_frame = !m_static_frame_owner.empty();
        // Without stack parameters and FP-relative frame slots nothing in the
        // body reads R9, so it is left untouched (hence preserved). `main`
        // keeps its frame: its STOP state includes SP and the FP+4 reload.
        const bool stack_parameters = !function.parameters.empty() && !m_register_arguments.count(frame_owner);
        m_frameless = globallyOptimized() && function.name != "main" && !stack_parameters &&
            (frame_bytes == 0 || static_frame);
        if (framePointerAllocatable(function) && !m_frameless)
            fail("IR codegen: R9-allocating function needs a frame pointer.", function.blocks.front().instructions.front().source);
        const auto function_offset = static_cast<std::uint32_t>(m_object_file.code_section.size());
        m_object_file.symbol_table.push_back({function.link_name.empty() ? function.name : function.link_name, SymbolSection::CODE, function_offset});
        if (function.is_cached) emitByte(static_cast<std::uint8_t>(OpCode::CACHE));
        emitStackGuard(static_cast<std::size_t>(static_frame ? 0 : frame_bytes) + (m_frameless ? 2 : 4),
                       Token(TokenType::UNKNOWN, function.name, 1, 1));
        if (m_checked_pointer_mode) {
            emitMove(Register::R0, 10);
            emitByte(0x3e); emitByte(0x71);
            emitGuard(OpCode::BEQ, 1);
            std::size_t parameters = 0;
            if (!m_register_arguments.count(function.link_name.empty() ? function.name : function.link_name))
                for (const auto& parameter : function.parameters)
                    parameters += isFarPointer(parameter.type) ? 4 : 2;
            // Without a stack argument area the ceiling is $FFFF: only an odd
            // SP reaches it, and the alignment guard above already stopped that.
            if (parameters) {
                emitCompare(Register::R10_SP, static_cast<std::uint16_t>(65535u - parameters));
                emitGuard(OpCode::BCC, 2);
            }
        }

        emitPush(GSUAbi::LinkRegister);
        if (!m_frameless) {
            emitPush(GSUAbi::FramePointerRegister);
            emitMove(static_cast<Register>(GSUAbi::FramePointerRegister), GSUAbi::StackPointerRegister);
        }
        if (frame_bytes > 0 && !static_frame) {
            emitAdjustStack(static_cast<std::size_t>(frame_bytes), false,
                            Token(TokenType::UNKNOWN, "", 0, 0));
        }
        if (m_stack_proof.active()) {
            // Saved R12/R13 words are the only pushes that outlive a block.
            std::size_t saved_loop_bytes = 0;
            for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
                if (instruction.opcode == IROpcode::HardwareLoop && !m_register_hardware_loops.count(instruction.loop_id))
                    saved_loop_bytes += 4;
            m_stack_proof.beginBody(saved_loop_bytes);
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

    // Collect all functions' widening requests before retrying. Stopping at
    // the first function made the pass bound depend on function count and
    // needlessly forced unrelated, already-short branches into long forms.
    if (!m_relaxation_pending.empty()) throw NeedsLongBranch{};

    // Shared helpers keep their own runtime guards and stack frames.
    m_stack_proof.leave();
    m_static_frame_owner.clear(); m_short_frame_offsets.clear();
    emitDivisionHelpers();
    m_static_ram_proof_used = m_static_ram_proof_used || m_address_proof.usedStaticObjectProof();

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
    layoutStaticFrames(module);
    // Static-object proofs assumed a nonzero base; make the linker check it.
    if (m_static_ram_proof_used)
        m_object_file.symbol_table.push_back({GSUAbi::StaticRamProofSymbol, SymbolSection::RAM, 0});
    // Omitted guards assumed this bound at `main`; make the linker prove it.
    if (const auto stack_bound = m_stack_proof.bound()) {
        if (stack_bound > 65535u) fail("Stack requirement exceeds one bank.", Token(TokenType::UNKNOWN, "main", 1, 1));
        m_object_file.symbol_table.push_back({GSUAbi::StaticStackBoundPrefix + std::to_string(stack_bound), SymbolSection::RAM, 0});
    }
    struct PoolEntry { std::vector<std::uint8_t> bytes; unsigned alignment; std::uint32_t offset; };
    std::vector<PoolEntry> pool;
    for (const auto& function : module.functions) for (const auto& array : function.immutable_local_arrays) {
        const auto alignment = static_cast<unsigned>(std::max(2, array.second.type.alignment));
        auto existing = std::find_if(pool.begin(), pool.end(), [&](const PoolEntry& entry) {
            return entry.alignment >= alignment && entry.offset % alignment == 0 && entry.bytes == array.second.bytes;
        });
        std::uint32_t offset = 0;
        if (existing != pool.end()) offset = existing->offset;
        else {
            while (m_object_file.data_section.size() % alignment) m_object_file.data_section.push_back(0);
            if (array.second.bytes.size() > 65536u - std::min<std::size_t>(65536, m_object_file.data_section.size()))
                fail("Immutable local-array payload exceeds one bank.", Token(TokenType::UNKNOWN, function.name, 1, 1));
            offset = static_cast<std::uint32_t>(m_object_file.data_section.size());
            m_object_file.data_section.insert(m_object_file.data_section.end(), array.second.bytes.begin(), array.second.bytes.end());
            pool.push_back({array.second.bytes, alignment, offset});
        }
        m_object_file.data_alignment = std::max(m_object_file.data_alignment, static_cast<std::uint8_t>(alignment));
        m_object_file.symbol_table.push_back({std::string(GSUAbi::ImmutablePoolPrefix) + function.name + "." +
            std::to_string(array.first.value), SymbolSection::DATA, offset});
    }
    if (globallyOptimized()) GSUMachineScheduler::run(m_object_file);
    GeneratedCandidate result;
    result.object = std::move(m_object_file); result.issues = std::move(m_spill_issues);
    if (globallyOptimized()) {
        result.speed_score = candidateSpeed(module, result.object);
        result.cache_credit = std::move(m_cache_credit);
        m_cache_credit.clear();
        if (sizeOptimized()) {
            result.inclusive_score = inclusiveSpeed(module, result.object, nullptr, &result.cache_credit);
            recordCandidateMetrics(module, result);
        }
    }
    return result;
}

ObjectFile IRCodeGenerator::generate(const IRModule& module) {
    struct Cleanup {
        IRCodeGenerator& generator;
        ~Cleanup() { generator.resetCandidateEmissionState(); }
    } cleanup{*this};
    m_function_candidates.clear(); m_function_smallest.clear();
    m_function_policies.clear(); m_function_division_policies.clear();
    m_function_pool_policies.clear(); m_pool_enabled = true;
    m_collect_function_candidates = false;
    std::size_t instructions = 0;
    for (const auto& f : module.functions) for (const auto& b : f.blocks) if (instructions <= 16000)
        instructions += std::min<std::size_t>(b.instructions.size(), 16001 - instructions);
    const bool bounded = module.functions.size() <= 64 && instructions <= 16000;
    const auto bank = m_config.code_start_address >> 16;
    const bool pool_capable = m_config.target == TargetKind::GSU && (bank == 0x70 || bank == 0x71);
    if (sizeOptimized()) {
        // At most fourteen IR/helper policies, each with scheduled cache/no-cache alternatives.
        // Compare emitted CODE plus
        // DATA alignment, not IR node counts or frequency-weighted timing.
        // The O2-policy candidate retains speed transformations only when they
        // actually shrink bytes; it uses O2 allocation but no CACHE padding.
        m_share_division = false;
        m_collect_function_candidates = bounded;
        // Outside the comparison budget keep per-call initialization:
        // eligibility does not prove that DATA plus checks is cheaper.
        auto best = generateOptimized(module, OptimizationLevel::O2, IRCompactionPolicy::Enabled, bounded);
        if (!bounded) return finishCandidate(std::move(best));
        // The exchange budget is fixed by the smallest complete candidate seen.
        std::size_t smallest = candidateBytes(best.object);
        const auto better = [&](const GeneratedCandidate& candidate) {
            smallest = std::min(smallest, candidateBytes(candidate.object));
            return preferCandidate(candidate, best, smallest);
        };
        for (const auto policy : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            for (const bool shared : {false, true}) {
                if (policy == OptimizationLevel::O2 && !shared) continue;
                m_share_division = shared;
                auto candidate = generateOptimized(module, policy);
                if (better(candidate)) best = std::move(candidate);
            }
        }
        // LOOP's ABI saves and pointer SSA can be faster yet larger. Retain
        // conservative IR alternatives rather than charging size mode for a
        // speed win. Machine selection remains byte-exact in every candidate.
        for (const auto policy : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            for (const bool shared : {false, true}) {
                m_share_division = shared;
                auto candidate = generateOptimized(module, policy, IRCompactionPolicy::Disabled);
                if (better(candidate)) best = std::move(candidate);
            }
        }
        if (pool_capable) for (const auto policy : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            // Two extra primary alternatives, not another Cartesian
            // helper/compaction search. Start from original unplanned input.
            m_share_division = false;
            auto candidate = generateOptimized(module, policy, IRCompactionPolicy::Enabled, false);
            if (better(candidate)) best = std::move(candidate);
        }
        m_collect_function_candidates = false;
        auto mixed = generateMixedCandidate(module);
        if (better(mixed)) best = std::move(mixed);
        // Anchor every extension to this complete emitted winner, not the
        // independently greedy function snapshots or a later mutable best.
        const CandidateReference legacy(best);
        // Preserve the complete legacy search, including its re-emitted mixed
        // CODE/DATA sharing, as an owned incumbent before enriching snapshots.
        // Pointer SSA need not also pay for counted-loop compaction. These two
        // primaries reuse its checks/proofs and start from the original input.
        m_function_policies.clear(); m_function_division_policies.clear(); m_function_pool_policies.clear();
        m_share_division = false;
        m_collect_function_candidates = true;
        for (const auto policy : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            auto candidate = generateOptimized(module, policy, IRCompactionPolicy::PointerPromotionOnly, true, &legacy);
            if (admissibleExtension(candidate, legacy) && better(candidate)) best = std::move(candidate);
        }
        m_collect_function_candidates = false;
        auto enriched = generateMixedCandidate(module);
        if (admissibleExtension(enriched, legacy) && better(enriched)) best = std::move(enriched);
        return finishCandidate(std::move(best));
    }
    auto best = generateOptimized(module, m_config.optimization, IRCompactionPolicy::Enabled, bounded);
    if (globallyOptimized() && bounded && pool_capable) {
        auto unpooled = generateOptimized(module, m_config.optimization, IRCompactionPolicy::Enabled, false);
        if (preferCandidate(unpooled, best)) best = std::move(unpooled);
    }
    return finishCandidate(std::move(best));
}

IRCodeGenerator::GeneratedCandidate IRCodeGenerator::generateOptimized(const IRModule& module, OptimizationLevel policy,
    IRCompactionPolicy compaction, bool pool_enabled, const CandidateReference* legacy) {
    m_pool_enabled = pool_enabled;
    m_allocation_policy = policy;
    m_division_candidates.clear();
    m_force_long_branches = false;
    m_long_branches.clear();
    m_long_branch_functions.clear();
    if (optimized()) {
        auto optimized_module = module;
        if (globallyOptimized()) IRGlobalOptimizer::run(optimized_module, m_all_local_symbols, policy, compaction);
        else IRLocalOptimizer::run(optimized_module, m_all_local_symbols);
        if (globallyOptimized()) {
            IRStorageOptions options;
            const auto bank = m_config.code_start_address >> 16;
            if (m_pool_enabled && m_config.target == TargetKind::GSU && (bank == 0x70 || bank == 0x71))
                options.immutable_arrays = IRImmutableArrayPolicy::CopiedNearRamPayload;
            IRStorageOptimizer::run(optimized_module, m_all_local_symbols, options);
        }
        if (sizeOptimized() && m_share_division) {
            std::map<bool, unsigned> counts;
            for (const auto& f : optimized_module.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::DivMod || (i.opcode == IROpcode::Binary && (i.operation == "/" || i.operation == "%")))
                    ++counts[i.type.is_unsigned];
            for (const auto& count : counts) if (count.second >= 2) m_division_candidates.insert(count.first);
        }
        // Monotonic widening, bounded independently of source size. If a
        // pathological cascade needs more rounds, the checked long form wins.
        const auto emit = [&]() {
            for (unsigned pass = 0; pass < 8; ++pass) {
                try { return generateInternal(optimized_module); }
                catch (const NeedsLongBranch&) {
                    // patchBranches recorded additional long forms. Retry
                    // within the same bounded, monotonic relaxation loop.
                    continue;
                }
            }
            // A pathological intra-function cascade loses only that
            // function's short forms, not every branch in the module.
            m_long_branch_functions.insert(m_relaxation_pending.begin(), m_relaxation_pending.end());
            return generateInternal(optimized_module);
        };
        m_allow_auto_cache = true;
        auto result = emit();
        const bool selected = m_selected_auto_cache, probed = m_had_cache_probe;
        if (probed && (sizeOptimized() || !selected)) {
            // Rejected probes must not leave padding. In size mode CACHE can
            // also block useful-slot scheduling: compare actual scheduled bytes,
            // not the pre-scheduler NOP replacement's apparent neutral cost.
            m_allow_auto_cache = false;
            m_force_long_branches = false; m_long_branches.clear(); m_long_branch_functions.clear();
            auto uncached = emit();
            const bool discard_probe = !selected && (!m_config.require_no_spills ||
                uncached.issues.empty() == result.issues.empty());
            if (discard_probe || preferUncachedCandidate(uncached, result)) result = std::move(uncached);
        }
        m_allow_auto_cache = true;
        considerFunctionCandidates(optimized_module, result, legacy);
        return result;
    }
    try {
        return generateInternal(module);
    } catch (const NeedsLongBranch&) {
        m_force_long_branches = true;
        return generateInternal(module);
    }
}
