#include "IRLocalOptimizer.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::size_t count(const IRModule& ir, IROpcode opcode) {
    std::size_t result = 0;
    for (const auto& function : ir.functions) for (const auto& block : function.blocks)
        for (const auto& instruction : block.instructions) result += instruction.opcode == opcode;
    return result;
}

template<class Check> void check(const std::string& source, Check test) {
    Lexer lexer(source);
    const auto tokens = lexer.scanTokens();
    Parser parser(tokens);
    auto program = parser.parseProgram();
    DataSegmentManager data;
    Analyzer analyzer(data);
    analyzer.analyze(program);
    IRLowerer lowerer;
    const auto original = lowerer.lower(program);
    auto optimized = original;
    IRLocalOptimizer::run(optimized, analyzer.getAllLocalSymbols());
    test(original, optimized);
    // Re-running must remain valid and deterministic; all IDs are compact.
    const auto first = dumpIR(optimized);
    IRLocalOptimizer::run(optimized, analyzer.getAllLocalSymbols());
    require(first == dumpIR(optimized), "Local optimization is not idempotent");
}
}

int main() {
    try {
        for (const auto input : {0, 127, 128, 255, 256, 32768, 65535}) {
            const auto low = input & 255;
            const auto expected = low < 128 ? low : low + 65280;
            check("unsigned word f() { return (unsigned word)(word)(byte)" + std::to_string(input) + "; }",
                [expected](const IRModule&, const IRModule& result) {
                    const auto& block = result.functions.at(0).blocks.at(0);
                    const auto value = block.instructions.back().operands.at(0);
                    bool found = false;
                    for (const auto& instruction : block.instructions)
                        if (instruction.result.value == value.value)
                            found = instruction.opcode == IROpcode::Constant && instruction.immediate == expected;
                    require(found, "Nested narrowing/sign extension/reinterpretation changed constant bits");
                });
        }
        check("word f() { return (word)(byte)255; }", [](const IRModule&, const IRModule& result) {
            require(count(result, IROpcode::Cast) == 0, "Nested scalar casts were not folded");
            const auto& block = result.functions.at(0).blocks.at(0);
            const auto value = block.instructions.back().operands.at(0);
            bool found = false;
            for (const auto& instruction : block.instructions)
                if (instruction.result.value == value.value) found = instruction.opcode == IROpcode::Constant && instruction.immediate == -1;
            require(found, "Signed byte widening must sign-extend");
        });
        check("word f(word x) { word a = x; a = a; return (word)a + a; }",
            [](const IRModule& original, const IRModule& result) {
                require(count(result, IROpcode::LoadIndirect) == 1 && count(original, IROpcode::LoadIndirect) > 1,
                        "Unescaped scalar loads were not forwarded");
                require(count(result, IROpcode::StoreIndirect) == 1, "Identical local store was not removed");
                require(count(result, IROpcode::Cast) == 0, "Representation-identical cast was retained");
            });
        check("word f(word x) { word a = x; word* p = &a; *p = 2; return a + a; }",
            [](const IRModule& original, const IRModule& result) {
                require(count(result, IROpcode::LoadIndirect) == count(original, IROpcode::LoadIndirect), "Escaped local was forwarded");
            });
        check("word f(word x) { word a = x; *(volatile word*)0x100 = 1; return a; }",
            [](const IRModule&, const IRModule& result) {
                require(count(result, IROpcode::LoadIndirect) == 2, "Volatile access must fence memory forwarding");
            });
        check("void tick(); word f(word x) { word a = x; tick(); return a; }",
            [](const IRModule&, const IRModule& result) {
                require(count(result, IROpcode::LoadIndirect) == 2 && count(result, IROpcode::Call) == 1,
                        "Call must fence memory forwarding and remain observable");
            });
        check("word f(word x) { word a = x; if (x > 0) a = 2; return a; }",
            [](const IRModule&, const IRModule& result) {
                require(count(result, IROpcode::LoadIndirect) >= 2, "A memory fact crossed a CFG join");
            });
        check("word f(volatile word* p) { return *p + *p; }",
            [](const IRModule&, const IRModule& result) {
                std::size_t reads = 0;
                for (const auto& block : result.functions.at(0).blocks) for (const auto& instruction : block.instructions)
                    reads += instruction.opcode == IROpcode::LoadIndirect && instruction.memory_volatile;
                require(reads == 2, "Volatile reads must not be coalesced");
            });
        check("word f(word x) { word a = x; plot { color a; a = a + 1; pixel; } return a; }",
            [](const IRModule&, const IRModule& result) {
                require(count(result, IROpcode::LoadIndirect) == 2, "COLOR should preserve local RAM facts; PLOT must fence them");
                require(count(result, IROpcode::SetColor) == 1 && count(result, IROpcode::Plot) == 1,
                        "Graphics effects must remain observable");
            });
        check("word f(word a, word b) { word x = a + b; word y = a + b; return x + y; }",
            [](const IRModule& original, const IRModule& result) {
                require(count(original, IROpcode::Binary) == 3 && count(result, IROpcode::Binary) == 2,
                        "Repeated pure expression was not reused");
            });
        check("word f(word x) { return ((x & 512) >> 9) + ((x >> 9) & 1); }",
            [](const IRModule&, const IRModule& result) {
                require(count(result, IROpcode::BitExtract) == 1, "Bit patterns were not fused/reused");
                require(count(result, IROpcode::Binary) == 1, "Unused mask/shift producers were not removed");
            });
        check("word f(word x) { return (x & (word)32768) >> 15; }",
            [](const IRModule&, const IRModule& result) {
                require(count(result, IROpcode::BitExtract) == 0, "Signed sign-bit mask must still produce -1, not 1");
            });
        check("word f() { word x = 3; x = 4; return x; }",
            [](const IRModule& original, const IRModule& result) {
                require(count(original, IROpcode::StoreIndirect) == 2 && count(result, IROpcode::StoreIndirect) == 1,
                        "A locally overwritten store was not removed");
            });
        check("word f() { word x = 3; *(volatile word*)0x100 = 7; x = 4; return x; }",
            [](const IRModule&, const IRModule& result) {
                require(count(result, IROpcode::StoreIndirect) == 3, "DSE crossed an observable store");
            });
        check("word f(word n) { word x = 3; word ignored = 5 / n; x = 4; return x; }",
            [](const IRModule&, const IRModule& result) {
                require(count(result, IROpcode::StoreIndirect) == 3 && count(result, IROpcode::Binary) == 1,
                        "DSE/DCE moved or erased a possible arithmetic fail-stop");
            });
        check("struct S { word a; word b; }; word f(struct S* p) { word x = 3; &p->b; x = 4; return x; }",
            [](const IRModule&, const IRModule& result) {
                require(count(result, IROpcode::StoreIndirect) == 2,
                        "DSE crossed a potentially faulting member address");
            });
        check("word f(word x, word n) { word a = x / n; word b = x / n; return a + b; }",
            [](const IRModule&, const IRModule& result) {
                require(count(result, IROpcode::Binary) == 3, "Local CSE must not coalesce potentially failing division");
            });
        std::cout << "IR local optimizer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
