#include "IRCodeGenerator.hpp"

#include "Opcodes.hpp"

namespace {
bool powerOfTwo(std::uint16_t value, unsigned& shift) {
    if (value == 0 || (value & (value - 1u)) != 0) return false;
    shift = 0;
    while (value > 1) { value >>= 1; ++shift; }
    return true;
}
}

void IRCodeGenerator::emitConstantShift(bool left, unsigned count, bool unsigned_right) {
    if (count >= 8) {
        emitByte(static_cast<std::uint8_t>(left ? OpCode::LOB : OpCode::HIB));
        if (left) emitByte(static_cast<std::uint8_t>(OpCode::SWAP));
        else if (!unsigned_right) emitByte(0x95); // HIB is zero-extended; restore signedness.
        count -= 8;
    }
    for (unsigned bit = 0; bit < count; ++bit)
        emitByte(left ? 0x50 : unsigned_right ? 0x03 : 0x96);
}

void IRCodeGenerator::emitWordMask(std::uint16_t mask) {
    if (mask > 0 && mask <= 15) {
        emitByte(0x3e); emitByte(static_cast<std::uint8_t>(0x70 | mask));
    } else {
        emitRegisterLiteral(3, mask); emitByte(0x73);
    }
}

void IRCodeGenerator::emitBitExtract(const IRInstruction& instruction) {
    materialize(instruction.operands.at(0));
    // The final low bit is identical under logical/arithmetic right shift.
    // Fusion removes the wide mask and the intermediate SSA copy/spill.
    emitConstantShift(false, static_cast<unsigned>(instruction.immediate), true);
    emitWordMask(1);
}

bool IRCodeGenerator::emitAllocatedOperation(const IRInstruction& instruction, std::uint8_t destination) {
    if (globallyOptimized() && instruction.opcode == IROpcode::Constant && !isFarPointer(instruction.type)) {
        emitRegisterLiteral(destination, static_cast<std::uint16_t>(instruction.immediate));
        return true;
    }
    if (globallyOptimized() && instruction.opcode == IROpcode::LoadIndirect && instruction.type.base != BaseType::BOOL &&
        instruction.type.pointer_level == 0) {
        const auto id = instruction.operands[0];
        const auto& pointer = producer(id, instruction.source).type;
        const auto* address = m_register_allocator.find(id);
        if (pointer.space == AddressSpace::RAM && !isFarPointer(pointer) && address && address->has_register &&
            provenAddress(id, pointer, instruction.type.sizeInBytes)) {
            m_spill_cache.clobber(destination);
            emitByte(static_cast<std::uint8_t>(0x10 | destination));
            if (usesByteStorage(instruction.type)) emitByte(0x3d);
            emitByte(static_cast<std::uint8_t>(0x40 | address->physical_register));
            m_last_ram_word_address = usesByteStorage(instruction.type) ? IRValueId{} : id;
            return true;
        }
    }
    const auto& op = instruction.operation;
    const bool binary = instruction.opcode == IROpcode::Binary &&
        (op == "+" || op == "-" || op == "&" || op == "|" || op == "^");
    const bool unary = instruction.opcode == IROpcode::Unary && (op == "~" || op == "!" || op == "-");
    if (!binary && !unary) return false;
    const auto* source = m_register_allocator.find(instruction.operands[0]);
    if (!source || !source->has_register || (!globallyOptimized() && !m_materialized_values.count(instruction.operands[0].value))) return false;
    std::int64_t literal = 0;
    const bool constant = binary ? constantValue(instruction.operands[1], literal) : op == "!";
    if (unary && op == "!") literal = 1;
    const auto* right = binary && !constant ? m_register_allocator.find(instruction.operands[1]) : nullptr;
    if (binary && !constant && (!right || !right->has_register || (!globallyOptimized() && !m_materialized_values.count(instruction.operands[1].value)))) return false;
    const bool immediate = constant && literal >= 0 && literal <= 15 &&
        ((op != "&" && op != "|" && op != "^") || literal != 0);
    // If R0 already holds the input, computing there also leaves a free
    // result snapshot for the next consumer. A direct immediate/unary
    // destination saves no moves and can force an extra Rn -> R0 reload.
    if ((constant || unary) && m_accumulator_value.value == instruction.operands[0].value) return false;
    if (constant && !immediate) emitRegisterLiteral(scratchRegister(), static_cast<std::uint16_t>(literal));
    m_spill_cache.clobber(destination);
    if (source->physical_register == destination) emitByte(static_cast<std::uint8_t>(0x20 | destination));
    else { emitByte(static_cast<std::uint8_t>(0x10 | destination)); emitByte(static_cast<std::uint8_t>(0xb0 | source->physical_register)); }
    if (unary && op != "!") {
        emitByte(0x4f);
        if (op == "-") emitByte(static_cast<std::uint8_t>(0xd0 | destination));
        if (usesByteStorage(instruction.type)) {
            emitByte(static_cast<std::uint8_t>(0x20 | destination)); emitByte(0x9e);
            if (!instruction.type.is_unsigned) { emitByte(static_cast<std::uint8_t>(0x20 | destination)); emitByte(0x95); }
        }
    } else {
        const auto operand = static_cast<std::uint8_t>(constant ? immediate ? literal : scratchRegister() : right->physical_register);
        if (op == "^" || (unary && op == "!")) emitByte(immediate ? 0x3f : 0x3d);
        else if (immediate) emitByte(0x3e);
        const auto base = static_cast<std::uint8_t>(op == "+" ? 0x50 : op == "-" ? 0x60 : op == "&" ? 0x70 : 0xc0);
        emitByte(static_cast<std::uint8_t>(base | operand));
    }
    return true;
}

bool IRCodeGenerator::emitConstantArithmetic(const IRInstruction& instruction) {
    const auto& op = instruction.operation;
    if ((op != "*" && op != "/" && op != "%") || instruction.type.base != BaseType::WORD || instruction.type.pointer_level != 0) return false;
    auto input = instruction.operands[0];
    std::int64_t literal = 0;
    if (!constantValue(instruction.operands[1], literal)) {
        if (op != "*" || !constantValue(input, literal)) return false;
        input = instruction.operands[1];
    }
    const auto raw = static_cast<std::uint16_t>(literal);
    unsigned count = 0;
    bool negate = false;
    if (op == "*") {
        if (!powerOfTwo(raw, count)) {
            if (!powerOfTwo(static_cast<std::uint16_t>(0u - raw), count)) return false;
            negate = true;
        }
    } else {
        negate = !instruction.type.is_unsigned && (raw & 0x8000u) != 0;
        if (!powerOfTwo(negate ? static_cast<std::uint16_t>(0u - raw) : raw, count)) return false;
    }
    materialize(input); // Never discard observable/faulting evaluation, even for %1.
    if (op == "*") {
        emitConstantShift(true, count, false);
        if (negate) { emitByte(0x4f); emitByte(0xd0); }
        return true;
    }
    const auto mask = static_cast<std::uint16_t>((std::uint32_t{1} << count) - 1u);
    if (op == "%" && count == 0) { emitLiteral(0); return true; }
    if (instruction.type.is_unsigned) {
        if (op == "/") emitConstantShift(false, count, true);
        else emitWordMask(mask);
        return true;
    }
    if (op == "/") {
        if (count != 0) {
            const auto positive = localLabel();
            emitCompare(0, 0); emitLocalJump(positive, 10);
            // Bias a negative dividend before ASR to truncate toward zero,
            // not toward minus infinity. This sum cannot overflow a word.
            emitImmediateArithmetic(0x50, mask, instruction.source);
            bindLabel(positive);
            emitConstantShift(false, count, false);
        }
        if (negate) { emitByte(0x4f); emitByte(0xd0); }
    } else {
        const auto positive = localLabel(), done = localLabel();
        emitCompare(0, 0); emitLocalJump(positive, 10);
        // The remainder follows the dividend's sign, even for a negative
        // divisor; the unsigned magnitude also represents abs(-32768).
        emitByte(0x4f); emitByte(0xd0); emitWordMask(mask);
        emitByte(0x4f); emitByte(0xd0); emitLocalJump(done);
        bindLabel(positive); emitWordMask(mask); bindLabel(done);
    }
    return true;
}

void IRCodeGenerator::emitPointerCompare(const IRInstruction& instruction) {
    const auto equal = localLabel(), unequal_bank = localLabel(), end = localLabel();
    const bool far = isFarPointer(producer(instruction.operands.front(), instruction.source).type);
    materialize(instruction.operands.front());
    emitPush(0);
    if (far) emitPush(4);
    materialize(instruction.operands.back());
    emitMove(3, 0);
    if (far) {
        emitMove(6, 4); emitPop(4);
        emitByte(0xb4); emitByte(0x3f); emitByte(0x66);
        emitLocalJump(unequal_bank, 8);
    }
    emitPop(0);
    emitByte(0xb0); emitByte(0x3f); emitByte(0x63);
    emitLocalJump(equal, 9);
    emitLiteral(instruction.operation == "!=" ? 1 : 0); emitLocalJump(end);
    if (far) {
        bindLabel(unequal_bank); emitPop(0);
        emitLiteral(instruction.operation == "!=" ? 1 : 0); emitLocalJump(end);
    }
    bindLabel(equal); emitLiteral(instruction.operation == "==" ? 1 : 0);
    bindLabel(end);
}

void IRCodeGenerator::emitIntegerOperation(const IRInstruction& instruction) {
    const auto& operation = instruction.operation;
    const auto scratch = scratchRegister();
    if (operation == "&" || operation == "|" || operation == "^") {
        if (operation == "^") emitByte(0x3d);
        emitByte(static_cast<std::uint8_t>((operation == "&" ? 0x70 : 0xc0) | scratch));
        return;
    }
    if (operation == "<<" || operation == ">>") {
        // Counts are checked, never silently masked by the target ISA.
        emitMove(6, scratch);
        emitCompare(6, 16);
        emitGuard(12, 5);
        const auto loop = localLabel(), end = localLabel();
        emitCompare(6, 0);
        emitLocalJump(end, 9);
        bindLabel(loop);
        if (operation == "<<") emitByte(0x50); // ADD R0: modular left shift.
        else emitByte(instruction.type.is_unsigned ? 0x03 : 0x96);
        emitByte(0xe6);
        emitLocalJump(loop, 8);
        bindLabel(end);
        return;
    }
    if (operation != "/" && operation != "%")
        fail("IR codegen: unknown integer operation.", instruction.source);

    if (sizeOptimized() && m_share_division && m_division_candidates.count(instruction.type.is_unsigned)) {
        emitSharedDivision(instruction);
        return;
    }

    // Fixed sixteen-step restoring division. R1 carries dividend/quotient,
    // R2 the remainder, R3 the divisor, R4 the count. R12/R13 are untouched
    // so this is safe inside hardware loops. Preserve plotting coordinates.
    emitMove(4, scratch); // Stack guards use R3; retain the divisor across pushes.
    if (m_isInPlottingContext) { emitPush(1); emitPush(2); }
    emitMove(3, 4);
    emitMove(1, 0);
    emitRegisterLiteral(6, 0);
    emitByte(0xb3); emitByte(0x3f); emitByte(0x66);
    emitGuard(8, 6);
    const bool signed_operation = !instruction.type.is_unsigned;
    if (signed_operation) {
        emitMove(4, 3);
        emitRegisterLiteral(6, 0x8000);
        emitMove(0, 1); emitByte(0x3d); emitByte(0xc3); emitByte(0x76);
        emitPush(0); // Quotient sign: dividend XOR divisor.
        emitMove(3, 4);
        emitMove(0, 1); emitByte(0x76); emitPush(0); // Remainder sign: dividend.
        emitMove(3, 4);
        const auto make_positive = [&](std::uint8_t reg) {
            const auto positive = localLabel();
            emitByte(static_cast<std::uint8_t>(0xb0 | reg));
            emitByte(static_cast<std::uint8_t>(0x10 | reg));
            emitByte(0x3e); emitByte(0x50); // ADD #0 sets sign, leaves operand intact.
            emitLocalJump(positive, 10);
            emitMove(0, reg); emitByte(0x4f); emitByte(0xd0); emitMove(reg, 0);
            bindLabel(positive);
        };
        make_positive(1);
        make_positive(3);
    }
    emitRegisterLiteral(2, 0);
    emitRegisterLiteral(4, 16);
    const auto loop = localLabel(), subtract = localLabel(), keep = localLabel();
    // Sixteen iterations amortize the cold fetch of this bounded kernel.
    // Do not rebase an explicitly cached region in this function, and let
    // Os retain its byte-oriented policy. Calls may still rebase a caller's
    // cache window; there is no cache-base preservation ABI. The backedge
    // skips CACHE itself.
    if (globallyOptimized() && !sizeOptimized() && !m_manual_cache)
        emitByte(static_cast<std::uint8_t>(OpCode::CACHE));
    bindLabel(loop);
    emitByte(0xb1); emitByte(0x11); emitByte(0x51); // Shift quotient/input into carry.
    emitByte(0xb2); emitByte(0x12); emitByte(0x3d); emitByte(0x52); // ADC R2.
    emitLocalJump(subtract, 13); // The seventeenth remainder bit implies >= divisor.
    emitByte(0xb2); emitByte(0x3f); emitByte(0x63);
    emitLocalJump(keep, 12);
    bindLabel(subtract);
    emitByte(0xb2); emitByte(0x12); emitByte(0x63);
    emitByte(0xd1); // Set quotient low bit, previously zero after the shift.
    bindLabel(keep);
    emitByte(0xe4);
    emitLocalJump(loop, 8);
    const bool combined = instruction.opcode == IROpcode::DivMod;
    if (!combined) emitMove(0, operation == "/" ? 1 : 2);
    if (signed_operation) {
        emitPop(6); emitPop(3);
        emitRegisterLiteral(4, 0);
        if (combined) {
            const auto restore_sign = [&](std::uint8_t value, std::uint8_t sign) {
                const auto positive = localLabel();
                emitByte(static_cast<std::uint8_t>(0xb0 | sign)); emitByte(0x3f); emitByte(0x64);
                emitLocalJump(positive, 9);
                emitMove(0, value); emitByte(0x4f); emitByte(0xd0); emitMove(value, 0);
                bindLabel(positive);
            };
            restore_sign(1, 3); restore_sign(2, 6);
        } else {
            emitByte(operation == "/" ? 0xb3 : 0xb6);
            emitByte(0x3f); emitByte(0x64);
            const auto positive = localLabel();
            emitLocalJump(positive, 9);
            emitByte(0x4f); emitByte(0xd0);
            bindLabel(positive);
        }
    }
    if (combined) {
        // Save the unused component before plotting restores R1/R2. A later
        // projection cannot assume those physical helper registers survive.
        emitRegisterLiteral(3, static_cast<std::uint16_t>(m_divmod_offsets.at(instruction.result.value)));
        emitByte(0xb9); emitByte(0x13); emitByte(0x53);
        emitStore(3, operation == "/" ? 2 : 1, false);
        emitMove(0, operation == "/" ? 1 : 2);
    }
    if (m_isInPlottingContext) { emitPop(2); emitPop(1); }
}

void IRCodeGenerator::emitDivMod(const IRInstruction& instruction) {
    if (!globallyOptimized()) fail("IR codegen: divmod requires the O2 backend.", instruction.source);
    emitBinaryOperands(instruction.operands.at(0), instruction.operands.at(1));
    emitIntegerOperation(instruction);
}

void IRCodeGenerator::emitDivModResult(const IRInstruction& instruction) {
    if (!globallyOptimized()) fail("IR codegen: divmod.result requires the O2 backend.", instruction.source);
    m_last_ram_word_address = IRValueId{};
    emitRegisterLiteral(3, static_cast<std::uint16_t>(m_divmod_offsets.at(instruction.operands.at(0).value)));
    emitByte(0xb9); emitByte(0x13); emitByte(0x53); emitByte(0x43);
}

void IRCodeGenerator::emitSharedDivision(const IRInstruction& instruction) {
    // Private kernel ABI: input R1/R3, quotient R1, remainder R2; clobbers
    // R0/R3/R4/R6/R11/flags. R5/R7/R8/R9/R10/R12/R13/R14 and banks survive.
    // The enclosing function already owns its saved R11. This helper neither
    // nests calls nor adds a return-address word to the stack.
    emitMove(4, scratchRegister());
    if (m_isInPlottingContext) { emitPush(1); emitPush(2); }
    emitMove(3, 4); emitMove(1, 0);
    const bool is_unsigned = instruction.type.is_unsigned;
    const std::string label = std::string(1, '\x01') + "__disco_os_divmod_" + (is_unsigned ? "u" : "s");
    m_division_helpers.insert(is_unsigned);
    m_spill_cache.clear(); m_last_ram_word_address = IRValueId{};
    emitByte(0x94);
    const auto patch = m_object_file.code_section.size();
    emitWordLiteral(15, 0); addRelocation(label, patch, RelocationType::ADDR16_IWT); emitByte(1);
    // Balanced kernel sign pushes still require their own runtime checks.
    // Never carry caller-only stack credit into an outlined implementation.
    m_stack_check_credit.reset();
    if (instruction.opcode == IROpcode::DivMod) {
        emitRegisterLiteral(3, static_cast<std::uint16_t>(m_divmod_offsets.at(instruction.result.value)));
        emitByte(0xb9); emitByte(0x13); emitByte(0x53);
        emitStore(3, instruction.operation == "/" ? 2 : 1, false);
    }
    emitMove(0, instruction.operation == "/" ? 1 : 2);
    if (m_isInPlottingContext) { emitPop(2); emitPop(1); }
}

void IRCodeGenerator::emitDivisionHelpers() {
    struct FunctionObserverGuard {
        const IRFunction*& observer;
        const IRFunction* previous;
        ~FunctionObserverGuard() { observer = previous; }
    };
    for (const bool is_unsigned : m_division_helpers) {
        IRFunction helper; helper.name = "__disco_os.divmod." + std::string(is_unsigned ? "u" : "s");
        // Restore this borrow before the temporary descriptor is destroyed,
        // including branch-relaxation retries that unwind through this scope.
        const FunctionObserverGuard restore{m_current_function, m_current_function};
        m_current_function = &helper;
        m_branch_fixups.clear(); m_block_addresses.clear(); m_branch_serial = 0; m_local_label_serial = 0;
        m_fault_offsets.clear(); m_spill_cache.clear(); m_last_ram_word_address = IRValueId{};
        m_isInPlottingContext = false; m_stack_check_credit.reset();
        bindLabel(std::string(1, '\x01') + "__disco_os_divmod_" + (is_unsigned ? "u" : "s"));
        emitRegisterLiteral(6, 0); emitByte(0xb3); emitByte(0x3f); emitByte(0x66); emitGuard(8, 6);
        if (!is_unsigned) {
            emitMove(4, 3); emitRegisterLiteral(6, 0x8000);
            emitMove(0, 1); emitByte(0x3d); emitByte(0xc3); emitByte(0x76); emitPush(0);
            emitMove(3, 4);
            emitMove(0, 1); emitByte(0x76); emitPush(0); emitMove(3, 4);
            for (const std::uint8_t reg : {std::uint8_t{1}, std::uint8_t{3}}) {
                const auto positive = localLabel();
                emitByte(static_cast<std::uint8_t>(0xb0 | reg)); emitByte(static_cast<std::uint8_t>(0x10 | reg));
                emitByte(0x3e); emitByte(0x50); emitLocalJump(positive, 10);
                emitMove(0, reg); emitByte(0x4f); emitByte(0xd0); emitMove(reg, 0); bindLabel(positive);
            }
        }
        emitRegisterLiteral(2, 0); emitRegisterLiteral(4, 16);
        const auto loop = localLabel(), subtract = localLabel(), keep = localLabel();
        bindLabel(loop);
        emitByte(0xb1); emitByte(0x11); emitByte(0x51);
        emitByte(0xb2); emitByte(0x12); emitByte(0x3d); emitByte(0x52);
        emitLocalJump(subtract, 13);
        emitByte(0xb2); emitByte(0x3f); emitByte(0x63); emitLocalJump(keep, 12);
        bindLabel(subtract); emitByte(0xb2); emitByte(0x12); emitByte(0x63); emitByte(0xd1);
        bindLabel(keep); emitByte(0xe4); emitLocalJump(loop, 8);
        if (!is_unsigned) {
            emitPop(6); emitPop(3); emitRegisterLiteral(4, 0);
            for (const auto& pair : {std::pair<std::uint8_t, std::uint8_t>{std::uint8_t{1}, std::uint8_t{3}},
                                   {std::uint8_t{2}, std::uint8_t{6}}}) {
                const auto positive = localLabel();
                emitByte(static_cast<std::uint8_t>(0xb0 | pair.second)); emitByte(0x3f); emitByte(0x64);
                emitLocalJump(positive, 9);
                emitMove(0, pair.first); emitByte(0x4f); emitByte(0xd0); emitMove(pair.first, 0); bindLabel(positive);
            }
        }
        emitByte(0x9b); emitByte(1);
        patchBranches();
    }
}
