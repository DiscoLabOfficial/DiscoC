#include "IRCodeGenerator.hpp"

#include "ABI.hpp"
#include "Opcodes.hpp"

void IRCodeGenerator::emitPointerValueCheck(const Type& type, int width) {
    const auto validate = localLabel(), done = localLabel();
    emitCompare(0, 0);
    emitLocalJump(validate, 8);
    if (isFarPointer(type)) {
        emitCompare(4, 0);
        emitLocalJump(validate, 8);
    }
    emitLocalJump(done); // Null is a storable/comparable value, not an address.
    bindLabel(validate); emitAddressCheck(type, width); bindLabel(done);
}

bool IRCodeGenerator::needsPointerSpill(const IRInstruction& instruction) const {
    if (instruction.opcode == IROpcode::Binary && (instruction.operation == "/" || instruction.operation == "%" ||
        instruction.operation == "<<" || instruction.operation == ">>")) return true;
    if (instruction.opcode == IROpcode::Constant || instruction.opcode == IROpcode::Binary ||
        instruction.opcode == IROpcode::Unary) return false;
    if (instruction.opcode == IROpcode::Address && instruction.operation.empty()) return false;
    if (instruction.opcode == IROpcode::Cast) {
        if (instruction.type.pointer_level == 0) return false;
        const auto& source = producer(instruction.operands.front(), instruction.source);
        // The frontend encodes explicit literal construction as a typed
        // pointer constant. The verifier checks its canonical representation.
        // Near ROM still depends on link-time bank context and needs a check.
        if (source.opcode == IROpcode::Constant && source.type.pointer_level > 0 &&
            (isFarPointer(instruction.type) || instruction.type.space == AddressSpace::RAM) &&
            samePointerLayers(source.type, instruction.type) && source.type.base == instruction.type.base)
            return false;
    }
    return true;
}

void IRCodeGenerator::emitRegisterLiteral(std::uint8_t reg, std::uint16_t value) {
    emitByte(static_cast<std::uint8_t>(0xf0 | reg));
    emitWord(value);
}

void IRCodeGenerator::emitNearBank(std::uint8_t reg, AddressSpace space) {
    const auto patch = m_object_file.code_section.size();
    emitByte(static_cast<std::uint8_t>(0xa0 | reg));
    emitByte(0);
    addRelocation(space == AddressSpace::ROM ? GSUAbi::NearRomBankSymbol : GSUAbi::NearRamBankSymbol,
                  patch, RelocationType::ADDR24_BANK);
}

void IRCodeGenerator::emitSelectBank(std::uint8_t reg, AddressSpace space) {
    emitByte(static_cast<std::uint8_t>(0xb0 | reg)); // FROM does not alter the operand.
    emitByte(space == AddressSpace::ROM ? 0x3f : 0x3e);
    emitByte(0xdf);
}

void IRCodeGenerator::emitCompare(std::uint8_t left, std::uint16_t right) {
    emitRegisterLiteral(3, right);
    emitByte(static_cast<std::uint8_t>(0xb0 | left));
    emitByte(0x3f); emitByte(0x63); // CMP R3: unsigned ordering uses C, equality uses Z.
}

void IRCodeGenerator::emitGuard(std::uint8_t success_branch, std::uint16_t fault) {
    emitByte(success_branch); emitByte(6); emitByte(1);
    emitAddressFault(fault);
}

void IRCodeGenerator::emitAddressFault(std::uint16_t fault) {
    emitRegisterLiteral(GSUAbi::AddressFaultRegister, fault);
    emitByte(0); emitByte(1); // Fail-stop. Resuming this STOP is not supported.
}

std::string IRCodeGenerator::localLabel() {
    return std::string(1, '\x01') + m_current_function->name + "@ptr" +
           std::to_string(m_local_label_serial++);
}

void IRCodeGenerator::bindLabel(const std::string& name) {
    m_object_file.symbol_table.push_back({name, SymbolSection::CODE,
        static_cast<std::uint32_t>(m_object_file.code_section.size())});
}

void IRCodeGenerator::emitLocalJump(const std::string& name, std::uint8_t condition) {
    if (condition != 5) {
        emitByte(static_cast<std::uint8_t>(condition ^ 1u)); emitByte(5); emitByte(1);
    }
    const auto patch = m_object_file.code_section.size();
    emitRegisterLiteral(15, 0);
    addRelocation(name, patch, RelocationType::ADDR16_IWT);
    emitByte(1);
}

void IRCodeGenerator::emitAddressCheck(const Type& pointer, int width) {
    // R0 is the offset, R4 the bank for a far value. Checks leave both intact.
    // No bank is switched until every check succeeds, so failures cannot send
    // a subsequent stack access into the foreign data bank.
    if (!isFarPointer(pointer)) { emitCompare(0, 0); emitGuard(8, 2); }
    if (width > 1) {
        const auto alignment = storageAlignment(pointeeType(pointer));
        if (alignment > 1) {
        emitRegisterLiteral(3, alignment - 1); emitByte(0xb0); emitByte(0x13); emitByte(0x73); // R3 = R0 & 1
        emitGuard(9, 1);
        }
        if (width > 2) {
            emitCompare(0, static_cast<std::uint16_t>(65537 - width));
            emitGuard(12, 2);
        }
    }
    if (pointer.space == AddressSpace::ROM) {
        if (!isFarPointer(pointer)) emitNearBank(4, AddressSpace::ROM);
        emitCompare(4, 0x60); emitGuard(12, 2);
        const auto full_bank = localLabel();
        emitCompare(4, 0x40); emitLocalJump(full_bank, 13);
        emitCompare(0, 0x8000); emitGuard(13, 2);
        bindLabel(full_bank);
    } else if (isFarPointer(pointer)) {
        emitCompare(4, 0x70); emitGuard(13, 2);
        emitCompare(4, 0x72); emitGuard(12, 2);
    }
}

void IRCodeGenerator::emitSpill(IRValueId value, bool load) {
    const auto& type = producer(value, Token(TokenType::UNKNOWN, "", 0, 0)).type;
    if (!load) emitMove(6, 0);
    emitRegisterLiteral(3, static_cast<std::uint16_t>(m_spill_offsets.at(value.value)));
    emitByte(0xb9); emitByte(0x13); emitByte(0x53); // R3 = FP + signed frame displacement.
    if (isFarPointer(type)) {
        if (load) { emitByte(0x14); emitByte(0x43); } // bank word -> R4
        else emitStore(3, 4, false);
        emitByte(0xd3); emitByte(0xd3);
    }
    if (load) emitByte(0x43); // offset/scalar -> R0
    else {
        emitStore(3, 6, false);
        emitMove(0, 6);
    }
}

void IRCodeGenerator::emitPointerOffset(const IRInstruction& instruction) {
    const auto& pointer = producer(instruction.operands[0], instruction.source).type;
    const auto& index_type = producer(instruction.operands[1], instruction.source).type;
    const auto stride = static_cast<std::uint16_t>(instruction.immediate);
    materialize(instruction.operands[1]);
    if (index_type.sizeInBytes == 1 && !index_type.is_unsigned) emitByte(0x95);
    emitMove(6, 0); // Signed displacement/count, retained across pointer materialization.
    materialize(instruction.operands[0]);
    // Reloading a spill uses R3, not R6. All checked-address values are eager
    // frame values, so there can be no intervening call/rematerialized load.
    emitAddressCheck(pointer, stride);
    const auto positive = localLabel(), negative = localLabel(), done = localLabel();
    if (!index_type.is_unsigned) {
        emitCompare(6, 0); emitLocalJump(instruction.operation == "+" ? negative : positive, 11);
    }
    emitLocalJump(instruction.operation == "+" ? positive : negative);

    const auto emit_direction = [&](const std::string& start, bool subtract) {
        bindLabel(start);
        const auto loop = localLabel();
        // A negative signed index uses an incrementing count towards zero;
        // unsigned indices and the positive path use a decrementing count.
        const bool count_negative = !index_type.is_unsigned &&
            ((instruction.operation == "+" && subtract) || (instruction.operation == "-" && !subtract));
        bindLabel(loop);
        emitCompare(6, 0); emitLocalJump(done, 9);
        if (stride > 1 || !isFarPointer(pointer)) {
            // Check BEFORE arithmetic. This rejects even aligned word carries
            // and cannot transiently wrap an address used by typed memory.
            if (subtract) {
                if (pointer.space == AddressSpace::ROM) {
                    if (!isFarPointer(pointer)) emitNearBank(4, AddressSpace::ROM);
                    const auto full = localLabel(), checked = localLabel();
                    emitCompare(4, 0x40); emitLocalJump(full, 13);
                    // A whole 32 KiB LoROM object has no preceding same-bank
                    // slot. Do not narrow the unrepresentable $10000 limit.
                    if (stride >= 0x8000u) emitAddressFault(3);
                    else {
                        emitCompare(0, static_cast<std::uint16_t>(0x8000u + stride));
                        emitGuard(13, 3);
                    }
                    emitLocalJump(checked);
                    bindLabel(full); emitCompare(0, stride); emitGuard(13, 3);
                    bindLabel(checked);
                } else { emitCompare(0, stride); emitGuard(13, 3); }
            } else {
                emitCompare(0, static_cast<std::uint16_t>(65536u - stride)); emitGuard(12, 3);
            }
            emitRegisterLiteral(3, stride);
            emitByte(static_cast<std::uint8_t>(subtract ? 0x63 : 0x53));
        } else {
            // Far byte stepping follows one canonical window at a time. The
            // bounded 16-bit count also handles negative and multi-bank steps.
            const auto simple = localLabel(), stepped = localLabel();
            if (subtract) {
                if (pointer.space == AddressSpace::ROM) {
                    const auto full = localLabel();
                    emitCompare(4, 0x40); emitLocalJump(full, 13);
                    emitCompare(0, 0x8000); emitLocalJump(simple, 8);
                    emitCompare(4, 0); emitGuard(8, 3);
                    emitByte(0xe4); emitRegisterLiteral(0, 0xffff);
                    emitLocalJump(stepped);
                    bindLabel(full);
                }
                emitCompare(0, 0); emitLocalJump(simple, 8);
                emitCompare(4, pointer.space == AddressSpace::ROM ? 0x40 : 0x70); emitGuard(8, 3);
                emitByte(0xe4); emitRegisterLiteral(0, 0xffff);
            } else {
                emitCompare(0, 0xffff); emitLocalJump(simple, 8);
                if (pointer.space == AddressSpace::ROM) {
                    const auto full = localLabel();
                    emitCompare(4, 0x40); emitLocalJump(full, 13);
                    emitCompare(4, 0x3f); emitGuard(8, 3);
                    emitByte(0xd4); emitRegisterLiteral(0, 0x8000);
                    emitLocalJump(stepped);
                    bindLabel(full);
                }
                emitCompare(4, pointer.space == AddressSpace::ROM ? 0x5f : 0x71); emitGuard(8, 3);
                emitByte(0xd4); emitRegisterLiteral(0, 0);
            }
            emitLocalJump(stepped);
            bindLabel(simple); emitByte(subtract ? 0xe0 : 0xd0);
            bindLabel(stepped);
        }
        emitByte(count_negative ? 0xd6 : 0xe6);
        emitLocalJump(loop);
    };
    emit_direction(positive, false);
    emitLocalJump(done);
    emit_direction(negative, true);
    bindLabel(done);
    emitAddressCheck(pointer, stride);
}
