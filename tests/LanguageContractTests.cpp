#include "Analyzer.hpp"
#include "CompilerError.hpp"
#include "DataSegment.hpp"
#include "IR.hpp"
#include "GsuPointer.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Program = std::vector<std::unique_ptr<Stmt>>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Node>
const Node& node(const Stmt& statement) {
    const auto* result = dynamic_cast<const Node*>(&statement);
    require(result != nullptr, "Unexpected statement shape in language fixture");
    return *result;
}

template <typename Check>
void analyze(const std::string& source, Check check) {
    // All compiler owners stay alive until the check finishes; callbacks must
    // not retain borrowed AST/IR/symbol references beyond this invocation.
    Lexer lexer(source);
    const auto tokens = lexer.scanTokens();
    Parser parser(tokens);
    auto program = parser.parseProgram();
    DataSegmentManager data;
    Analyzer analyzer(data);
    analyzer.analyze(program);
    IRLowerer lowerer;
    const auto ir = lowerer.lower(program);
    IRVerifier::verify(ir);
    check(program, analyzer, data, ir);
}

void expectDiagnostic(const std::string& source, const std::string& expected) {
    try {
        analyze(source, [](const Program&, const Analyzer&,
                           const DataSegmentManager&, const IRModule&) {});
    } catch (const CompilerError& error) {
        require(error.getMessage().find(expected) != std::string::npos,
                "Unexpected diagnostic: " + error.getMessage());
        require(error.getLine() > 0 && error.getCol() > 0,
                "Language diagnostic must identify a source location");
        return;
    }
    throw std::runtime_error("Invalid language fixture was accepted: " + source);
}

void checkWidthsAndWidening() {
    analyze("word widen(byte value) { return value; }\n"
            "void widths(byte a, unsigned byte b, word c, unsigned word d, "
            "byte* p, word* q, byte** r) { return; }\n",
        [](const Program& program, const Analyzer&,
           const DataSegmentManager&, const IRModule& ir) {
            const auto& widen = node<FunctionDeclStmt>(*program.at(0));
            const auto& returned = node<ReturnStmt>(*widen.body.at(0));
            const auto* conversion = dynamic_cast<const CastExpr*>(returned.value.get());
            require(conversion != nullptr && conversion->implicit_conversion,
                    "Byte-to-word return must contain an explicit IR-boundary cast");
            require(conversion->expression->result_type.sizeInBytes == 1 &&
                    conversion->result_type.sizeInBytes == 2 &&
                    !conversion->result_type.is_unsigned,
                    "Signed byte widening must retain signedness and value width");
            const auto& widths = node<FunctionDeclStmt>(*program.at(1));
            const int expected[] = {1, 1, 2, 2, 2, 2, 2};
            require(widths.params.size() == 7, "Incorrect parameter count");
            for (std::size_t index = 0; index < widths.params.size(); ++index) {
                require(widths.params.at(index).type.sizeInBytes == expected[index],
                        "Scalar/near-pointer storage width does not match the contract");
            }
            require(!widths.params.at(0).type.is_unsigned &&
                    widths.params.at(1).type.is_unsigned &&
                    !widths.params.at(2).type.is_unsigned &&
                    widths.params.at(3).type.is_unsigned,
                    "Integer signedness does not match the contract");
            require(ir.functions.size() == 2, "Function lowering lost a definition");
        });
}

void checkAggregateLayout() {
    analyze("struct Packet { byte tag; word value; byte flags; };\n"
            "word read() { struct Packet packets[2]; return packets[1].value; }\n",
        [](const Program& program, const Analyzer&,
           const DataSegmentManager&, const IRModule&) {
            const auto& read = node<FunctionDeclStmt>(*program.at(1));
            const auto& array = node<VarDeclStmt>(*read.body.at(0));
            require(array.type.sizeInBytes == 6 && array.type.array_size == 2,
                    "Structure array must use six-byte Packet elements");
            const auto& returned = node<ReturnStmt>(*read.body.at(1));
            const auto* member = dynamic_cast<const MemberAccessExpr*>(returned.value.get());
            require(member != nullptr && member->member_offset == 2,
                    "Packet.value must be aligned at offset two");
            const auto* indexed = dynamic_cast<const SubscriptExpr*>(member->object.get());
            require(indexed != nullptr && indexed->element_size == 6,
                    "Structure indexing must use the aggregate size, not pointer size");
        });
    analyze("struct Packet { byte tag; word value; byte flags; };\n"
            "void main() { struct Packet p; p.tag = 1; p.value = 42; p.flags = 0; }\n",
        [](const Program& program, const Analyzer&,
           const DataSegmentManager&, const IRModule&) {
            const auto& main = node<FunctionDeclStmt>(*program.at(1));
            const int expected[] = {0, 2, 4};
            for (std::size_t index = 0; index < 3; ++index) {
                const auto& statement = node<ExpressionStmt>(*main.body.at(index + 1));
                const auto* assignment = dynamic_cast<const AssignExpr*>(statement.expression.get());
                require(assignment != nullptr, "Expected a member assignment");
                const auto* member = dynamic_cast<const MemberAccessExpr*>(assignment->name.get());
                require(member != nullptr && member->member_offset == expected[index],
                        "Structure member offset does not match declaration order/alignment");
            }
        });
}

void checkScopesAndPrototypes() {
    analyze("word scopes() { word x = 30; { word x = 12; } return x; }\n",
        [](const Program& program, const Analyzer& analyzer,
           const DataSegmentManager&, const IRModule&) {
            const auto& function = node<FunctionDeclStmt>(*program.at(0));
            const auto& outer = node<VarDeclStmt>(*function.body.at(0));
            const auto& block = node<BlockStmt>(*function.body.at(1));
            const auto& inner = node<VarDeclStmt>(*block.statements.at(0));
            const auto& returned = node<ReturnStmt>(*function.body.at(2));
            const auto* reference = dynamic_cast<const VariableExpr*>(returned.value.get());
            require(outer.symbol_id.isValid() && inner.symbol_id.isValid() &&
                    outer.symbol_id != inner.symbol_id && reference != nullptr &&
                    reference->symbol_id == outer.symbol_id,
                    "Shadowing must preserve declaration identity after the inner block");
            const auto& locals = analyzer.getAllLocalSymbols().at("scopes");
            require(locals.size() == 2 &&
                    locals.at(outer.symbol_id).stackOffset != locals.at(inner.symbol_id).stackOffset,
                    "Shadowed declarations must retain separate stack storage");
        });
    analyze("word add(word a, word b); word add(word x, word y);\n"
            "word caller() { return add(30, 12); }\n"
            "word add(word a, word b) { return a + b; }\n",
        [](const Program&, const Analyzer&, const DataSegmentManager&, const IRModule& ir) {
            require(ir.functions.size() == 2, "Prototypes must not generate function bodies");
            require(ir.functions.at(0).name == "caller" && ir.functions.at(1).name == "add",
                    "Definitions must retain deterministic source order");
        });
}

void checkLiteralsAndRomData() {
    analyze("rom const unsigned byte palette[] = {0, 15, 255};\n"
            "void main() { byte a = 127; unsigned byte b = 255; "
            "word c = 32767; unsigned word d = 65535; "
            "word hex = 0x0C; word octal = 014; }\n",
        [](const Program& program, const Analyzer&,
           const DataSegmentManager& data, const IRModule&) {
            const auto& bytes = data.getEntries().at("palette").bytes;
            require(bytes == std::vector<std::uint8_t>({0, 15, 255}),
                    "ROM byte literals must preserve their exact values");
            const auto& main = node<FunctionDeclStmt>(*program.at(1));
            const auto& hex = node<VarDeclStmt>(*main.body.at(4));
            const auto& octal = node<VarDeclStmt>(*main.body.at(5));
            require(hex.initializer->result_type.sizeInBytes == 2 &&
                    octal.initializer->result_type.sizeInBytes == 2,
                    "Contextual literal types must use the declared word width");
        });
}

void checkDiagnostics() {
    expectDiagnostic("void main() { byte value = 128; }", "out of range for signed byte");
    expectDiagnostic("void main() { unsigned byte value = 256; }", "out of range for unsigned byte");
    expectDiagnostic("void main() { word value = 32768; }", "out of range for signed word");
    expectDiagnostic("void main() { unsigned word value = 65536; }", "out of range for unsigned word");
    expectDiagnostic("byte narrow(word value) { return value; }", "Cannot implicitly convert");
    expectDiagnostic("word main() { return 4 / 0; }", "Division by zero");
    expectDiagnostic("void main() { word x; word x; }", "already declared in this scope");
    expectDiagnostic("void main() { break; }", "not within a loop or switch");
    expectDiagnostic("void main() { void value; }", "Void is not a valid variable declaration type.");
    expectDiagnostic("void main() { word values[0]; }", "Array size must be between");
    analyze("void main() { word* values[2]; }", [](const Program& program, const Analyzer&, const DataSegmentManager&, const IRModule&) {
        const auto& declaration = node<VarDeclStmt>(*node<FunctionDeclStmt>(*program.at(0)).body.at(0));
        require(declaration.type.array_size == 2 && declaration.type.sizeInBytes == 2, "Pointer arrays require two word-sized elements");
    });
    expectDiagnostic("word main() { word value = 42; }", "without returning a value");
    expectDiagnostic("word add(word a); byte add(word a);", "Conflicting declaration");
    expectDiagnostic("rom const word answer = 42; void main() { answer = 1; }", "ROM is read-only");
}

void checkPointerContract() {
    analyze("struct Pointers { byte tag; far word* pointer; word tail; };\n"
            "far word* identity(byte a, far word* pointer, word b) { return pointer; }\n"
            "void main() { word x; word* p = &x; far word* q = (far word*)p; word* far* outer = &q; }\n",
        [](const Program& program, const Analyzer& analyzer,
           const DataSegmentManager&, const IRModule&) {
            const auto& identity = node<FunctionDeclStmt>(*program.at(1));
            require(identity.returnType.sizeInBytes == 4 && identity.params.at(1).type.sizeInBytes == 4,
                    "Far arguments and returns must be four-byte values");
            const auto& locals = analyzer.getAllLocalSymbols().at("identity");
            const int offsets[] = {6, 8, 12};
            for (std::size_t index = 0; index < identity.params.size(); ++index)
                require(locals.at(identity.params[index].symbol_id).stackOffset == offsets[index],
                        "Mixed near/far arguments use overlapping parameter slots");
            const auto& main = node<FunctionDeclStmt>(*program.at(2));
            const auto& widened = node<VarDeclStmt>(*main.body.at(2));
            const auto* cast = dynamic_cast<const CastExpr*>(widened.initializer.get());
            require(cast && !cast->implicit_conversion, "Near-to-far widening must be explicit in the analyzed AST");
            const auto& outer = node<VarDeclStmt>(*main.body.at(3));
            require(outer.type.sizeInBytes == 2 && !isFarPointer(outer.type) &&
                    isFarPointer(pointeeType(outer.type)), "&far_pointer must be a near pointer to a four-byte value");
        });
    require(GsuPointer::add(0x00ffff, 1, 1, AddressSpace::ROM, true) == 0x018000, "LoROM carry");
    require(GsuPointer::add(0x018000, -1, 1, AddressSpace::ROM, true) == 0x00ffff, "LoROM borrow");
    require(GsuPointer::add(0x70ffff, 1, 1, AddressSpace::RAM, true) == 0x710000, "RAM carry");
    require(GsuPointer::add(0x710000, -1, 1, AddressSpace::RAM, true) == 0x70ffff, "RAM borrow");
    require(GsuPointer::add(0x018000, 65535, 1, AddressSpace::ROM, true) == 0x02ffff, "Multi-window displacement");
    require(GsuPointer::add(0x40ffff, 1, 1, AddressSpace::ROM, true) == 0x410000, "Full-bank ROM carry");
    require(GsuPointer::add(0x00fffc, 1, 2, AddressSpace::ROM, true) == 0x00fffe, "Aligned in-bank word step");
    expectDiagnostic("void main() { far word* p = (far word*)0x70fffe + 1; }", "bank boundary");
    expectDiagnostic("void main() { word* p = (word*)0x1001; }", "Misaligned 16-bit");
    const std::string deepest = "word" + std::string(MaxPointerDepth, '*');
    analyze(deepest + " identity(" + deepest + " p) { return p; }",
        [](const Program&, const Analyzer&, const DataSegmentManager&, const IRModule&) {});
    expectDiagnostic("void main() { " + deepest + "* p; }", "Pointer depth exceeds");
    expectDiagnostic("void main() { " + deepest + " p; &p; }", "Pointer depth exceeds");
}

void checkAccessQualifiers() {
    analyze("void main() { const word n = 42; const word* p = &n; volatile word* port = (volatile word*)0x100; *port; *port = n; }",
        [](const Program& program, const Analyzer&, const DataSegmentManager&, const IRModule& ir) {
            const auto& main = node<FunctionDeclStmt>(*program.at(0));
            const auto& n = node<VarDeclStmt>(*main.body.at(0));
            const auto& p = node<VarDeclStmt>(*main.body.at(1));
            require(n.type.is_const && !p.type.is_const && pointeeType(p.type).is_const,
                    "Const object and const pointee must be independent");
            unsigned reads = 0, writes = 0;
            for (const auto& block : ir.functions.at(0).blocks)
                for (const auto& instruction : block.instructions)
                    if (instruction.memory_volatile) {
                        if (instruction.opcode == IROpcode::LoadIndirect) ++reads;
                        if (instruction.opcode == IROpcode::StoreIndirect) ++writes;
                    }
            require(reads == 1 && writes == 1, "Volatile must survive lowering, including unused reads");
        });
    expectDiagnostic("void main() { const word n = 1; n = 2; }", "const-qualified");
    expectDiagnostic("void main() { word n = 1; const word* p = &n; *p = 2; }", "const-qualified");
    expectDiagnostic("void main() { word n = 1; word* const p = &n; p = &n; }", "const-qualified");
    expectDiagnostic("void main() { const word n; }", "requires an initializer");
    expectDiagnostic("void main() { const word n = 1; word* p = &n; }", "Cannot implicitly convert");
    expectDiagnostic("void main() { volatile word* p; word* q = (word*)p; }", "discard const or volatile");
    expectDiagnostic("void main() { const word** p; word** q = (word**)p; }", "discard const or volatile");
    analyze("const rom word answer = 42; void main() { word* p; volatile word* q = p; }",
        [](const Program&, const Analyzer&, const DataSegmentManager&, const IRModule&) {});
}

void checkNumericContract() {
    expectDiagnostic("void main() { 42 / 0; }", "Division by zero");
    expectDiagnostic("void main() { 42 << 16; }", "Shift count");
    expectDiagnostic("void main() { 42 >> -1; }", "Shift count");
    expectDiagnostic("word f(unsigned byte a) { return a; }", "Cannot implicitly convert");
    expectDiagnostic("word f(unsigned word a) { return a; }", "Cannot implicitly convert");
    expectDiagnostic("word f(word a, unsigned word b) { return a + b; }", "Mixed signed/unsigned");
    expectDiagnostic("byte f(word a) { return a; }", "Cannot implicitly convert");
    analyze("bool f(unsigned word a) { return a > 32767; } void main() { byte a = -128; word b = -32768; bool c = false; }",
        [](const Program& program, const Analyzer&, const DataSegmentManager&, const IRModule&) {
            const auto& function = node<FunctionDeclStmt>(*program.at(0));
            const auto& returned = node<ReturnStmt>(*function.body.at(0));
            require(returned.value->result_type.base == BaseType::BOOL && returned.value->result_type.sizeInBytes == 1,
                    "Comparisons must have boolean type and byte storage");
        });
}

void checkAggregateRestrictions() {
    const std::string prefix = "struct S { byte b; word w; }; ";
    expectDiagnostic(prefix + "struct S f();", "Struct return by value");
    expectDiagnostic(prefix + "void f(struct S s);", "Struct parameter by value");
    expectDiagnostic(prefix + "void main() { struct S a; struct S b = a; }", "Struct initializer requires members");
    expectDiagnostic(prefix + "void main() { struct S a; struct S b; a = b; }", "Struct assignment by value");
    expectDiagnostic(prefix + "void main() { struct S a; (struct S)a; }", "Struct casts by value");
    expectDiagnostic(prefix + "void main() { struct S a; a; }", "Struct values reside in memory");
    expectDiagnostic(prefix + "void main() { volatile struct S a; a.w = 42; const struct S* p = &a; (*p).w = 0; }", "Cannot implicitly convert");
}

void checkStaticStorageAndLinkage() {
    expectDiagnostic("const word n = 1; void main() { n = 0; }", "const-qualified");
    expectDiagnostic("extern word n = 1;", "extern global cannot have an initializer");
    expectDiagnostic("internal word f(); void main() {}", "must be defined");
    expectDiagnostic("internal word f(); word f() { return 1; }", "Conflicting declaration");
    expectDiagnostic("internal void main() {}", "main entry must have external linkage");
    expectDiagnostic("word a[40000]; void main() {}", "Static storage exceeds");
    analyze("word n = 1 + 2; void main() {}", [](const Program&, const Analyzer&, const DataSegmentManager& data, const IRModule&) {
        require(data.getEntries().at("n").bytes == std::vector<std::uint8_t>({3, 0}), "Static constant expressions must initialize the RAM image");
    });
    expectDiagnostic("word read(); word n = read(); void main() {}", "Static initializers");
    expectDiagnostic("extern word n; byte n;", "Conflicting global declaration");
    expectDiagnostic("extern volatile word n; word n;", "Conflicting global declaration");
    expectDiagnostic("word n; word n;", "Duplicate global symbol");
    analyze("extern word n; extern word n; word n = 42; extern word n; void main() { n = 43; }",
        [](const Program&, const Analyzer&, const DataSegmentManager& data, const IRModule&) {
            require(!data.getEntries().at("n").is_extern && data.getEntries().at("n").bytes == std::vector<std::uint8_t>({42, 0}),
                "Repeated compatible extern declarations must preserve the defining initialization image");
        });
    analyze("internal word counter; extern word shared; export void main() { counter = shared; }",
        [](const Program&, const Analyzer&, const DataSegmentManager& data, const IRModule&) {
            require(data.getEntries().at("counter").bytes == std::vector<std::uint8_t>({0, 0}), "Globals must have zero initial images");
            require(data.getEntries().at("shared").is_extern && data.getEntries().at("shared").bytes.empty(), "Extern declarations must not allocate storage");
            require(data.getEntries().at("counter").link_name.front() == '\x01', "Internal symbols must use object-local identity");
        });
}

void checkPlotAndResourceLimits() {
    expectDiagnostic("void main() { plot { plot {} } }", "Cannot nest plotting");
    expectDiagnostic("void main() { plot { plot_end; } }", "Legacy graphics API removed");
    expectDiagnostic("void main() { plot_begin; }", "Legacy graphics API removed");
    expectDiagnostic("void main() { plot { word n = 1; } n = 2; }", "Use of undeclared symbol");
    expectDiagnostic("void main() { plot {} cursor.x = 1; }", "plotting context");
    expectDiagnostic("void main() { plot { word* p = &cursor.x; } }", "do not have memory addresses");
    analyze("word plot_y; word f(word plot_x) { return plot_x; }", [](const Program&, const Analyzer&, const DataSegmentManager&, const IRModule&) {});
    expectDiagnostic("void main() { word n = -true; }", "Unary arithmetic requires");
    expectDiagnostic("void main() { " + std::string(1000, '!') + "true; }", "Parser nesting exceeds");
    expectDiagnostic("void main() { " + std::string(1000, '(') + "1" + std::string(1000, ')') + "; }", "Parser nesting exceeds");
    // Each grouping enters assignment and unary. Exercise both sides of the
    // unchanged 128-entry limit, including the native MSVC Debug build.
    analyze("word f() { return " + std::string(63, '(') + "42" + std::string(63, ')') + "; }",
        [](const Program&, const Analyzer&, const DataSegmentManager&, const IRModule&) {});
    expectDiagnostic("void main() { " + std::string(64, '(') + "1" + std::string(64, ')') + "; }", "Parser nesting exceeds");
    expectDiagnostic("void main() { " + std::string(1000, '{') + std::string(1000, '}') + " }", "Parser nesting exceeds");
    std::string chain = "1";
    for (int index = 0; index < 1000; ++index) chain += " + 1";
    expectDiagnostic("void main() { " + chain + "; }", "Expression depth exceeds");
    analyze("word f() { plot { return 42; } } void main() { if (true) { plot { word n = f(); cursor.x = n; } } }",
        [](const Program&, const Analyzer&, const DataSegmentManager&, const IRModule& ir) {
            bool call_in_plot = false;
            for (const auto& function : ir.functions)
                for (const auto& block : function.blocks)
                    for (const auto& instruction : block.instructions)
                        if (instruction.opcode == IROpcode::Call) call_in_plot = instruction.in_plot_context;
            require(call_in_plot, "Lexical plot context must survive lowering into per-instruction metadata");
        });
}

void checkTypeAliasesAndSelection() {
    analyze("type Count = u16; type Read = const volatile i16*; "
            "type Far = far i16*; type Slot = Far*; "
            "i16 widths(i8 a, u8 b, i16 c, Count d, Read p, Far f, Slot s) { return c; }",
        [](const Program& program, const Analyzer&, const DataSegmentManager&, const IRModule& ir) {
            const auto& function = node<FunctionDeclStmt>(*program.at(4));
            const int widths[] = {1, 1, 2, 2, 2, 4, 2};
            for (std::size_t index = 0; index < function.params.size(); ++index)
                require(function.params.at(index).type.sizeInBytes == widths[index],
                        "Transparent aliases must preserve canonical storage widths");
            require(!function.params.at(0).type.is_unsigned && function.params.at(1).type.is_unsigned &&
                    function.params.at(3).type.is_unsigned, "Builtin aliases must preserve signedness");
            const auto read = pointeeType(function.params.at(4).type);
            require(read.is_const && read.is_volatile, "Pointer aliases must preserve access qualifiers");
            require(isFarPointer(function.params.at(5).type) && !isFarPointer(function.params.at(6).type) &&
                    isFarPointer(pointeeType(function.params.at(6).type)),
                    "Adding a pointer to a far alias must not widen the new outer pointer");
            require(ir.functions.size() == 1, "Aliases must not allocate code or data");
        });
    analyze("@cfg(spc700) type Choice = u16; @cfg(gsu) type Choice = i16; "
            "@cfg(spc700) i16 selected() { return unknown(); } "
            "@cfg(gsu) i16 selected() { return 149; }",
        [](const Program& program, const Analyzer&, const DataSegmentManager&, const IRModule& ir) {
            require(program.size() == 2 && !node<TypeAliasDeclStmt>(*program.at(0)).resolved_type.is_unsigned,
                    "Inactive declarations must not register aliases or enter the AST");
            require(ir.functions.size() == 1 && ir.functions.at(0).name == "selected",
                    "Inactive function bodies must not be semantically resolved or lowered");
        });
    expectDiagnostic("i16 Counter; type Counter = i16;", "Value name conflicts with a type alias");
    expectDiagnostic("type Count = u16; void f(i16 Count);", "Type alias name cannot be used as a value name");
    expectDiagnostic("enum E { Count }; type Count = u16;", "Enumerator name conflicts with a type alias");
    expectDiagnostic("@cfg(spc700) type Hidden = i16; Hidden value;", "Unknown type alias");
    expectDiagnostic("@cfg(gsu) import \"invalid.txt\";", ".dci interface path");
    expectDiagnostic("@cfg(spc700) import \"invalid.txt\";", ".dci interface path");
    expectDiagnostic("@cfg(gsu void main() {}", "Expect ')' after attribute arguments");

    std::string aliases;
    for (std::size_t index = 4; index < MaxTypeAliases; ++index)
        aliases += "type Alias" + std::to_string(index) + " = u16;\n";
    analyze(aliases, [](const Program& program, const Analyzer&, const DataSegmentManager&, const IRModule&) {
        require(program.size() == MaxTypeAliases - 4, "Alias count limit must include builtin bindings");
    });
    expectDiagnostic(aliases + "type Extra = u16;", "Type alias count exceeds");
    std::string layers = "type P0 = i16;\n";
    for (std::size_t index = 1; index <= MaxPointerDepth; ++index)
        layers += "type P" + std::to_string(index) + " = P" + std::to_string(index - 1) + "*;\n";
    analyze(layers, [](const Program& program, const Analyzer&, const DataSegmentManager&, const IRModule&) {
        require(node<TypeAliasDeclStmt>(*program.back()).resolved_type.pointer_level == MaxPointerDepth,
                    "Alias expansion must retain the maximum valid pointer depth");
    });
    expectDiagnostic(layers + "type Extra = P32*;", "Pointer depth exceeds");
}

} // namespace

int main() {
    try {
        checkWidthsAndWidening();
        checkAggregateLayout();
        checkScopesAndPrototypes();
        checkLiteralsAndRomData();
        checkDiagnostics();
        checkPointerContract();
        checkAccessQualifiers();
        checkNumericContract();
        checkAggregateRestrictions();
        checkStaticStorageAndLinkage();
        checkPlotAndResourceLimits();
        checkTypeAliasesAndSelection();
        std::cout << "Language baseline contract checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
