#include "IRCodeGenerator.hpp"

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
    emitMove(0, operation == "/" ? 1 : 2);
    if (signed_operation) {
        emitPop(6); emitPop(3);
        emitRegisterLiteral(4, 0);
        emitByte(operation == "/" ? 0xb3 : 0xb6);
        emitByte(0x3f); emitByte(0x64);
        const auto positive = localLabel();
        emitLocalJump(positive, 9);
        emitByte(0x4f); emitByte(0xd0);
        bindLabel(positive);
    }
    if (m_isInPlottingContext) { emitPop(2); emitPop(1); }
}
