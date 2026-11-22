#include "SPC700CodeGenerator.hpp"

#include "CompilerError.hpp"
#include "SPC700Target.hpp"

// Integer semantics follow the language specification: values are extended to
// 16 bits by their own type, arithmetic wraps modulo 2^16 and the result is
// truncated to the result type's width. Signedness of `/`, `%` and `>>` comes
// from the result type and that of comparisons from the left operand, exactly
// as in the GSU backend.

namespace {
constexpr unsigned T0 = 0x06, T1 = 0x08, T2 = 0x0A, T3 = 0x0C;
}

void SPC700CodeGenerator::normalizeBool(unsigned address) {
    const auto zero = newLabel();
    emit("mov a," + dp(address));
    emit("beq " + zero);
    emit("mov " + dp(address) + ",#$01");
    bind(zero);
    emit("mov " + dp(address + 1) + ",#$00");
}

void SPC700CodeGenerator::emitBinary(const IRInstruction& instruction) {
    if (instruction.operands.size() != 2) internal("binary instruction without two operands", instruction.source);
    const auto& op = instruction.operation;
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
        emitComparison(instruction);
        return;
    }
    if (op == "<<" || op == ">>") {
        emitShift(instruction);
        return;
    }
    if (op == "/" || op == "%") {
        emitDivision(instruction);
        return;
    }
    loadWord(instruction.operands[0], T0, instruction.source);
    loadWord(instruction.operands[1], T1, instruction.source);
    if (op == "+" || op == "-") {
        emit("movw ya," + dp(T0));
        emit(std::string(op == "+" ? "addw" : "subw") + " ya," + dp(T1));
        emit("movw " + dp(T2) + ",ya");
    } else if (op == "&" || op == "|" || op == "^") {
        const std::string mnemonic = op == "&" ? "and" : op == "|" ? "or" : "eor";
        for (unsigned i = 0; i < 2; ++i) {
            emit("mov a," + dp(T0 + i));
            emit(mnemonic + " a," + dp(T1 + i));
            emit("mov " + dp(T2 + i) + ",a");
        }
    } else if (op == "*") {
        emitMultiply();
    } else {
        unsupported("binary operator '" + op + "'", instruction.source);
    }
    storeValue(instruction, T2);
}

void SPC700CodeGenerator::emitMultiply() {
    // Low 16 bits of T0 * T1, the modular product for either signedness:
    // lo*lo + ((lo0*hi1 + hi0*lo1) << 8).
    emit("mov a," + dp(T0));
    emit("mov y," + dp(T1));
    emit("mul ya");
    emit("movw " + dp(T2) + ",ya");
    emit("mov a," + dp(T0));
    emit("mov y," + dp(T1 + 1));
    emit("mul ya");
    emit("clrc");
    emit("adc a," + dp(T2 + 1));
    emit("mov " + dp(T2 + 1) + ",a");
    emit("mov a," + dp(T0 + 1));
    emit("mov y," + dp(T1));
    emit("mul ya");
    emit("clrc");
    emit("adc a," + dp(T2 + 1));
    emit("mov " + dp(T2 + 1) + ",a");
}

void SPC700CodeGenerator::emitDivision(const IRInstruction& instruction) {
    // Both operands are evaluated before the divisor is tested (fault 6).
    loadWord(instruction.operands[0], T0, instruction.source);
    loadWord(instruction.operands[1], T1, instruction.source);
    const bool is_unsigned = instruction.type.is_unsigned;
    (is_unsigned ? m_uses_unsigned_division : m_uses_signed_division) = true;
    m_uses_unsigned_division = true; // The signed helper calls the unsigned core.
    emit(std::string("call !") + (is_unsigned ? "__disco_spc_udiv" : "__disco_spc_sdiv"));
    const auto result = instruction.operation == "/" ? T0 : T3;
    storeValue(instruction, result);
}

void SPC700CodeGenerator::emitShift(const IRInstruction& instruction) {
    loadWord(instruction.operands[0], T0, instruction.source);
    loadWord(instruction.operands[1], T1, instruction.source);
    // Counts must be 0..15; they are checked, never masked.
    const auto ok = newLabel();
    emit("mov a," + dp(T1 + 1));
    emit("bne " + ok + "_fault");
    emit("mov a," + dp(T1));
    emit("cmp a,#$10");
    emit("bcc " + ok);
    bind(ok + "_fault");
    emitFault(SPC700Target::Fault::ShiftCount);
    bind(ok);
    const auto loop = newLabel(), done = newLabel();
    emit("mov x," + dp(T1));
    emit("beq " + done);
    bind(loop);
    if (instruction.operation == "<<") {
        emit("asl " + dp(T0));
        emit("rol " + dp(T0 + 1));
    } else if (instruction.type.is_unsigned) {
        emit("lsr " + dp(T0 + 1));
        emit("ror " + dp(T0));
    } else {
        // Arithmetic shift: carry = sign bit, rotated back into bit 15.
        emit("mov a," + dp(T0 + 1));
        emit("cmp a,#$80");
        emit("ror " + dp(T0 + 1));
        emit("ror " + dp(T0));
    }
    emit("dec x");
    emit("bne " + loop);
    bind(done);
    storeValue(instruction, T0);
}

void SPC700CodeGenerator::emitComparison(const IRInstruction& instruction) {
    loadWord(instruction.operands[0], T0, instruction.source);
    loadWord(instruction.operands[1], T1, instruction.source);
    const auto& op = instruction.operation;
    const bool is_unsigned = typeOf(instruction.operands[0], instruction.source).is_unsigned ||
        typeOf(instruction.operands[0], instruction.source).base == BaseType::BOOL;
    const auto yes = newLabel(), no = newLabel(), done = newLabel();
    // > and <= compare with the operands swapped.
    const bool swap = op == ">" || op == "<=";
    const auto left = swap ? T1 : T0, right = swap ? T0 : T1;
    emit("movw ya," + dp(left));
    if (op == "==" || op == "!=") {
        emit("cmpw ya," + dp(right));
        emit(std::string(op == "==" ? "beq " : "bne ") + yes);
        emit("bra " + no);
    } else {
        // `less` holds left < right; < and > branch on it, >= and <= on its negation.
        const bool want_less = op == "<" || op == ">";
        if (is_unsigned) {
            emit("cmpw ya," + dp(right)); // C clear: left < right.
            emit(std::string(want_less ? "bcc " : "bcs ") + yes);
            emit("bra " + no);
        } else {
            emit("subw ya," + dp(right)); // Signed less: N != V.
            const auto overflow = newLabel();
            emit("bvs " + overflow);
            emit("bmi " + (want_less ? yes : no));
            emit("bra " + (want_less ? no : yes));
            bind(overflow);
            emit("bmi " + (want_less ? no : yes));
            emit("bra " + (want_less ? yes : no));
        }
    }
    bind(no);
    setWord(T2, 0);
    emit("bra " + done);
    bind(yes);
    setWord(T2, 1);
    bind(done);
    storeValue(instruction, T2);
}

void SPC700CodeGenerator::emitUnary(const IRInstruction& instruction) {
    loadWord(instruction.operands.at(0), T0, instruction.source);
    const auto& op = instruction.operation;
    if (op == "-") {
        emit("movw ya," + dp(T0));
        emit("movw " + dp(T2) + ",ya");
        negateWord(T2);
    } else if (op == "~") {
        for (unsigned i = 0; i < 2; ++i) {
            emit("mov a," + dp(T0 + i));
            emit("eor a,#$FF");
            emit("mov " + dp(T2 + i) + ",a");
        }
    } else if (op == "!") {
        const auto zero = newLabel(), done = newLabel();
        emit("mov a," + dp(T0));
        emit("or a," + dp(T0 + 1));
        emit("beq " + zero);
        setWord(T2, 0);
        emit("bra " + done);
        bind(zero);
        setWord(T2, 1);
        bind(done);
    } else {
        unsupported("unary operator '" + op + "'", instruction.source);
    }
    storeValue(instruction, T2);
}

void SPC700CodeGenerator::emitCast(const IRInstruction& instruction) {
    const auto operand = instruction.operands.at(0);
    const auto& source = producer(operand, instruction.source);
    loadWord(operand, T2, instruction.source);
    if (instruction.type.pointer_level > 0) {
        if (source.operation == "null" && source.opcode == IROpcode::Constant) {
            storeValue(instruction, T2);
            return;
        }
        // A converted pointer must be null or a valid address for its pointee.
        const auto done = newLabel();
        emit("mov a," + dp(T2));
        emit("or a," + dp(T2 + 1));
        emit("beq " + done);
        const auto element = pointeeType(instruction.type);
        emitAddressCheck(instruction.type, element.sizeInBytes > 0 ? element.sizeInBytes : 1, T2);
        bind(done);
    } else if (instruction.type.base == BaseType::BOOL) {
        const auto zero = newLabel();
        emit("mov a," + dp(T2));
        emit("or a," + dp(T2 + 1));
        emit("beq " + zero);
        emit("mov " + dp(T2) + ",#$01");
        bind(zero);
        emit("mov " + dp(T2 + 1) + ",#$00");
    }
    // Narrowing keeps the low byte; widening was done by loadWord.
    storeValue(instruction, T2);
}
