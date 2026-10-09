#include "IRCodeGenerator.hpp"

#include "ABI.hpp"
#include "Opcodes.hpp"
#include "GSUCostModel.hpp"

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
    m_spill_cache.clobber(reg);
    // IBT sign-extends its byte. $0080..$00FF need IWT; $FF80..$FFFF
    // do not. Never shorten PC loads/relocations or assume an unsigned IBT.
    if (optimized() && reg != 15 && (value <= 127 || value >= 0xff80)) {
        emitByte(static_cast<std::uint8_t>(0xa0 | reg));
        emitByte(static_cast<std::uint8_t>(value));
        return;
    }
    emitWordLiteral(reg, value);
}

void IRCodeGenerator::emitWordLiteral(std::uint8_t reg, std::uint16_t value) {
    m_spill_cache.clobber(reg);
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
    m_spill_cache.clear();
    if (space == AddressSpace::RAM) m_last_ram_word_address = IRValueId{};
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
    const auto previous = m_fault_offsets.find(fault);
    if (globallyOptimized() && previous != m_fault_offsets.end() && success_branch >= 6 && success_branch <= 15) {
        const auto distance = static_cast<std::int64_t>(previous->second) -
            static_cast<std::int64_t>(m_object_file.code_section.size()) - 2;
        if (distance >= -128 && distance <= 127) {
            // Reuse a terminal island only while the real signed byte branch
            // fits. The one-byte slot clears selectors before either path.
            emitByte(static_cast<std::uint8_t>(success_branch ^ 1u));
            emitByte(static_cast<std::uint8_t>(distance & 255)); emitByte(1);
            return;
        }
    }
    const bool short_fault = optimized() && (fault <= 127 || fault >= 0xff80);
    emitByte(success_branch); emitByte(short_fault ? 5 : 6); emitByte(1);
    emitAddressFault(fault);
}

void IRCodeGenerator::emitAddressFault(std::uint16_t fault) {
    // No credit reset: this is a terminal failure entry, not a successful
    // control-flow join. Its incoming registers/ROM/RAM state are irrelevant.
    if (globallyOptimized()) m_fault_offsets[fault] = m_object_file.code_section.size();
    emitRegisterLiteral(GSUAbi::AddressFaultRegister, fault);
    emitByte(0); emitByte(1); // Fail-stop. Resuming this STOP is not supported.
}

std::string IRCodeGenerator::localLabel() {
    return std::string(1, '\x01') + m_current_function->name + "@ptr" +
           std::to_string(m_local_label_serial++);
}

void IRCodeGenerator::bindLabel(const std::string& name) {
    m_spill_cache.clear();
    m_accumulator_value = IRValueId{};
    m_stack_check_credit.reset();
    m_object_file.symbol_table.push_back({name, SymbolSection::CODE,
        static_cast<std::uint32_t>(m_object_file.code_section.size())});
}

void IRCodeGenerator::emitLocalJump(const std::string& name, std::uint8_t condition) {
    if (optimized()) {
        const auto serial = m_branch_serial++;
        if (!longBranch(serial)) {
            emitByte(condition);
            const auto patch = m_object_file.code_section.size();
            emitByte(0); emitByte(1); // One pipeline delay-slot opcode.
            m_branch_fixups.push_back({patch, {}, Token(TokenType::UNKNOWN, name, 0, 0), false, name, serial});
            return;
        }
    }
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
        // Verified type layouts have a power-of-two alignment <= 128.
        emitRegisterLiteral(3, static_cast<std::uint16_t>(alignment - 1)); emitByte(0xb0); emitByte(0x13); emitByte(0x73); // R3 = R0 & (alignment - 1)
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

void IRCodeGenerator::emitWordSpillStore(IRValueId value, std::uint8_t source) {
    const auto cached = m_accumulator_value;
    m_last_ram_word_address = IRValueId{};
    // Explicitly target R3: neither R0 nor the allocated source is changed.
    // In particular R6 remains available for a parallel-copy cycle.
    emitRegisterLiteral(3, static_cast<std::uint16_t>(m_spill_offsets.at(value.value)));
    emitByte(0xb9); emitByte(0x13); emitByte(0x53);
    emitStore(3, source, false);
    if (globallyOptimized()) m_accumulator_value = cached;
}

void IRCodeGenerator::ensureAddressChecked(IRValueId value, int width) {
    const auto& pointer = producer(value, Token(TokenType::UNKNOWN, "", 0, 0)).type;
    if (provenAddress(value, pointer, width)) return;
    const bool near_ram = globallyOptimized() && pointer.space == AddressSpace::RAM && !isFarPointer(pointer);
    const auto previous = m_checked_addresses.find(value.value);
    if (near_ram && previous != m_checked_addresses.end() && previous->second >= width) return;
    emitAddressCheck(pointer, width);
    if (near_ram && m_checked_addresses.size() < 1024) m_checked_addresses[value.value] = width;
}

void IRCodeGenerator::emitSpill(IRValueId value, bool load) {
    const auto cached = m_accumulator_value;
    m_last_ram_word_address = IRValueId{};
    const auto& type = producer(value, Token(TokenType::UNKNOWN, "", 0, 0)).type;
    // Preserve the baseline and far-pair encodings, whose bank word also
    // participates in address selection. Optimized word stores keep R0.
    if (!load && optimized() && !isFarPointer(type)) { emitWordSpillStore(value, 0); return; }
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
        // Scalar stores preserve the exact R0 bits. Loads, calls, labels
        // and bank-sensitive operations still invalidate the snapshot.
        if (globallyOptimized() && !isFarPointer(type)) m_accumulator_value = cached;
    }
}

void IRCodeGenerator::retainSpillCopy(IRValueId value, bool after_load) {
    if (!globallyOptimized() || !GSUCostModel::scalar(producer(value, Token(TokenType::UNKNOWN, "", 0, 0)).type)) return;
    const auto uses = m_block_remaining_uses.find(value.value);
    // R0 already preserves many adjacent consumers after a definition. Avoid
    // paying for an additional copy unless repeated future uses justify it.
    if (uses == m_block_remaining_uses.end() || uses->second < (after_load ? 1u : 2u)) return;
    if (m_spill_cache.find(value) >= 0) return;
    const auto reg = m_spill_cache.choose(m_block_remaining_uses);
    if (reg < 0) return;
    const auto reload = GSUCostModel::spillLoad(m_spill_offsets.at(value.value)).pressureScore();
    if (reload <= GSUCostModel::copy().pressureScore() * 2) return;
    emitMove(static_cast<std::uint8_t>(reg), 0);
    m_spill_cache.record(static_cast<std::uint8_t>(reg), value);
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
    if (!provenAddress(instruction.operands[0], pointer, stride)) emitAddressCheck(pointer, stride);
    if (globallyOptimized() && m_address_proof.provesOffset(instruction)) {
        // The signed displacement is used as a raw word. Two's-complement
        // ADD/SUB is correct here because the proof excludes address wrap,
        // misalignment, null, and scaled-magnitude overflow on every path.
        for (std::uint16_t scale = stride; scale > 1; scale >>= 1) {
            emitByte(0xb6); emitByte(0x16); emitByte(0x56);
        }
        emitByte(static_cast<std::uint8_t>(instruction.operation == "-" ? 0x66 : 0x56));
        return;
    }
    if (instruction.operands.size() == 3) {
        // The recurrence is modulo 2^16. Never let its wrap mask an illegal
        // scaled displacement: check the original index before using it.
        emitCompare(6, static_cast<std::uint16_t>(65535u / stride + 1)); emitGuard(12, 3);
        emitMove(4, 0);
        materialize(instruction.operands[2]); emitMove(6, 0); emitMove(0, 4);
        const bool subtract = instruction.operation == "scaled-";
        emitByte(subtract ? 0x66 : 0x56); emitGuard(subtract ? 13 : 12, 3);
        if (pointer.space == AddressSpace::ROM && subtract) {
            const auto full = localLabel();
            emitNearBank(4, AddressSpace::ROM); emitCompare(4, 0x40); emitLocalJump(full, 13);
            emitCompare(0, 0x8000); emitGuard(13, 3); bindLabel(full);
        }
        emitAddressCheck(pointer, stride);
        if (globallyOptimized() && pointer.space == AddressSpace::RAM && !isFarPointer(pointer) && m_checked_addresses.size() < 1024)
            m_checked_addresses[instruction.result.value] = stride;
        return;
    }
    if (optimized() && !isFarPointer(pointer) && stride && !(stride & (stride - 1))) {
        // Near stepping cannot legally carry into another bank. Check the
        // scaled magnitude and ADD/SUB carry instead of walking N elements.
        // Far byte window transitions keep their original explicit algorithm.
        const auto negative = localLabel(), done = localLabel();
        if (!index_type.is_unsigned) {
            emitCompare(6, 0); emitLocalJump(negative, 11);
        }
        const auto step = [&](bool subtract) {
            for (std::uint16_t scale = stride; scale > 1; scale >>= 1) {
                emitByte(0xb6); emitByte(0x16); emitByte(0x56);
                emitGuard(12, 3); // Magnitude multiplication must not overflow.
            }
            emitByte(static_cast<std::uint8_t>(subtract ? 0x66 : 0x56));
            emitGuard(subtract ? 13 : 12, 3);
            if (pointer.space == AddressSpace::ROM && subtract) {
                const auto full = localLabel();
                emitNearBank(4, AddressSpace::ROM);
                emitCompare(4, 0x40); emitLocalJump(full, 13);
                emitCompare(0, 0x8000); emitGuard(13, 3);
                bindLabel(full);
            }
        };
        step(instruction.operation == "-");
        if (!index_type.is_unsigned) {
            emitLocalJump(done); bindLabel(negative);
            emitByte(0xb6); emitByte(0x16); emitByte(0x4f); emitByte(0xd6);
            step(instruction.operation == "+");
        }
        bindLabel(done);
        emitAddressCheck(pointer, stride);
        if (globallyOptimized() && pointer.space == AddressSpace::RAM && m_checked_addresses.size() < 1024)
            m_checked_addresses[instruction.result.value] = stride;
        return;
    }
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
    if (globallyOptimized() && pointer.space == AddressSpace::RAM && !isFarPointer(pointer) && m_checked_addresses.size() < 1024)
        m_checked_addresses[instruction.result.value] = stride;
}
