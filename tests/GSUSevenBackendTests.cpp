#include "IRCodeGenerator.hpp"
#include "IRGlobalOptimizer.hpp"
#include "IRStorageOptimizer.hpp"
#include "IRControlFlow.hpp"
#include "AssemblyGenerator.hpp"
#include "Assembler.hpp"
#include "GSUInstructionModel.hpp"
#include "ABI.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "LinkerDriver.hpp"
#include "TestTempDirectory.hpp"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Compilation { ObjectFile object; IRModule ir; };
Compilation compile(const std::string& source, OptimizationLevel level, std::uint32_t origin = 0x008000,
                    bool require_no_spills = false) {
    Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer; auto original = lowerer.lower(ast);
    auto ir = original;
    if (isGlobalOptimization(level)) {
        IRGlobalOptimizer::run(ir, analyzer.getAllLocalSymbols(), level);
        IRStorageOptions options;
        if ((origin >> 16) == 0x70 || (origin >> 16) == 0x71)
            options.immutable_arrays = IRImmutableArrayPolicy::CopiedNearRamPayload;
        IRStorageOptimizer::run(ir, analyzer.getAllLocalSymbols(), options);
    }
    CompilerConfig config; config.optimization = level; config.code_start_address = origin;
    config.require_no_spills = require_no_spills;
    IRCodeGenerator backend(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    auto object = backend.generate(original);
    Assembler assembler; const auto roundtrip = assembler.assemble(AssemblyGenerator(object).generate());
    require(roundtrip.code_section == object.code_section && roundtrip.data_section == object.data_section,
        "Seven-point backend assembly roundtrip changed CODE/DATA");
    for (const auto& r : object.relocation_table) if (r.target_symbol_name.compare(0,
        std::string(GSUAbi::ImmutablePoolPrefix).size(), GSUAbi::ImmutablePoolPrefix) == 0) {
        require(std::any_of(roundtrip.relocation_table.begin(), roundtrip.relocation_table.end(), [&](const RelocationEntry& copy) {
            return copy.patch_offset == r.patch_offset && copy.type == RelocationType::ADDR16_RAM &&
                copy.target_symbol_name.compare(0, std::string(GSUAbi::ImmutablePoolPrefix).size(), GSUAbi::ImmutablePoolPrefix) == 0;
        }), "Assembly roundtrip lost the guarded private RAM-pool namespace");
    }
    IRCodeGenerator repeated(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    require(AssemblyGenerator(repeated.generate(original)).generate() == AssemblyGenerator(object).generate(),
        "Seven-point candidate selection differs between fresh backend instances");
    require(AssemblyGenerator(backend.generate(original)).generate() == AssemblyGenerator(object).generate(),
        "Selected-candidate state or spill evidence leaked across reuse of one backend instance");
    return {std::move(object), std::move(ir)};
}

std::vector<std::uint8_t> linked(const ObjectFile& object) {
    constexpr unsigned startup = 10;
    const auto base = object.config.code_start_address;
    auto bytes = object.code_section;
    if (!object.data_section.empty() && (base + startup + bytes.size()) % object.data_alignment)
        bytes.resize(bytes.size() + object.data_alignment - (base + startup + bytes.size()) % object.data_alignment, 0);
    const auto data = bytes.size();
    bytes.insert(bytes.end(), object.data_section.begin(), object.data_section.end());
    std::uint32_t entry = 0;
    for (const auto& s : object.symbol_table) if (s.name == "main") entry = base + startup + s.offset;
    require(entry != 0, "Backend execution fixture has no main");
    for (const auto& r : object.relocation_table) {
        std::uint32_t value = 0; bool found = false;
        const auto limit = std::string(GSUAbi::StackLimitPrefix);
        if (r.target_symbol_name.compare(0, limit.size(), limit) == 0) {
            value = static_cast<std::uint32_t>(std::stoul(r.target_symbol_name.substr(limit.size()))); found = true;
        }
        if (r.target_symbol_name == GSUAbi::NearRamBankSymbol) { value = 0x700000; found = true; }
        if (r.target_symbol_name == GSUAbi::NearRomBankSymbol) { value = base <= 0x5fffff ? base : 0x008000; found = true; }
        for (const auto& s : object.symbol_table) if (s.name == r.target_symbol_name) {
            require(s.section != SymbolSection::RAM, "Unexpected static RAM fixture");
            value = base + startup + (s.section == SymbolSection::DATA ? data : 0) + s.offset; found = true;
        }
        require(found, "Unresolved seven-point execution fixture relocation");
        bytes.at(r.patch_offset + 1) = static_cast<std::uint8_t>(r.type == RelocationType::ADDR24_BANK ? value >> 16 : value);
        if (r.type != RelocationType::ADDR24_BANK) bytes.at(r.patch_offset + 2) = static_cast<std::uint8_t>(value >> 8);
    }
    bytes.insert(bytes.begin(), {0xfc, 0xef, 0xbe, 0xfd, 0xfe, 0xca, 0xff,
        static_cast<std::uint8_t>(entry), static_cast<std::uint8_t>(entry >> 8), 1});
    return bytes;
}

void execution(const std::string& source, const std::vector<std::pair<unsigned, unsigned>>& cases,
               std::uint32_t origin = 0x008000, unsigned expected_reads = 1) {
    for (const auto level : {OptimizationLevel::O1, OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto compiled = compile(source, level, origin);
        for (const auto& input : cases) {
            DiscoGSU::Machine machine(linked(compiled.object), origin);
            machine.seed(0x700100, static_cast<std::uint8_t>(input.first), false);
            machine.seed(0x700101, static_cast<std::uint8_t>(input.first >> 8), false);
            machine.run();
            require(machine.reg(0) == input.second, "Seven-point backend changed the scalar result");
            require(machine.reg(6) == 0 && machine.reg(12) == 0xbeef && machine.reg(13) == 0xcafe,
                "Seven-point backend changed fault/LOOP register restoration");
            require(machine.reg(10) == 0x1ffc && machine.reg(9) == 0x1ffc,
                "Seven-point backend changed main's frame/stack restoration");
            require(machine.accesses(0x700100, false) == expected_reads && machine.accesses(0x700100, true) == 0,
                "Seven-point backend changed actual volatile input access multiplicity");
        }
    }
}

void predicates() {
    const std::string source = "word main(){unsigned word x=*(volatile unsigned word*)0x100;word s=0;"
        "if(x>32)s=1;if(x>32)s+=2;return s;}";
    execution(source, {{0,0},{32,0},{33,3},{65535,3}});
    const auto ir = compile(source, OptimizationLevel::O2).ir;
    unsigned reads = 0;
    for (const auto& f : ir.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        reads += i.opcode == IROpcode::LoadIndirect && i.memory_volatile;
    require(reads == 1, "Conditional-only comparison rematerialization repeated a volatile load");
}

void callsAndLoops() {
    const std::string leaf = "word leaf(word x){if(x==12345)return 4;return x+1;}"
        "word main(){word s=0;for(word i=0;i<32;i++)s+=leaf(i);return s;}";
    execution(leaf, {{0,528}}, 0x008000, 0);
    const std::string transitive = "word inner(word x){word s=x;for(word j=0;j<5;j++)s+=j;return s;}"
        "word middle(word x){return inner(x);}"
        "word main(){word s=0;for(word i=0;i<8;i++)s+=middle(i);return s;}";
    execution(transitive, {{0,108}}, 0x008000, 0);
    const std::string recursive = "word inner(word x){word s=x;for(word j=0;j<5;j++)s+=j;return s;}"
        "word recursive(word n){if(n==0)return inner(0);return recursive(n-1)+1;}"
        "word main(){word s=0;for(word i=0;i<8;i++)s+=recursive(i);return s;}";
    execution(recursive, {{0,108}}, 0x008000, 0);
    execution("word main(){word a=*(volatile word*)0x100;word s=0;for(word i=0;i<64;i++){s+=a;s^=i;}return s;}",
        {{0,0},{1,64},{7,384}});
}

void physicalSpills() {
    std::string source = "word main(){";
    for (unsigned n = 0; n < 8; ++n)
        source += "word a" + std::to_string(n) + "=*(volatile word*)" + std::to_string(256 + n * 2) + ";";
    for (unsigned n = 0; n < 8; ++n) source += "a" + std::to_string(n) + "++;";
    source += "return a0+a1+a2+a3+a4+a5+a6+a7;}";
    execution(source, {{0,8},{32767,32775},{65535,7}});
    const auto object = compile(source, OptimizationLevel::O2).object;
    require(AssemblyGenerator(object).generate().find("sbk") != std::string::npos,
        "Physical reusable frame-word updates did not reuse their SBK address latch");
}

void numericStackAliases() {
    for (const bool write : {false, true}) {
        const auto path = std::filesystem::path(__FILE__).parent_path() / "storage" /
            (write ? "raw_numeric_alias_write.dc" : "raw_numeric_alias_read.dc");
        std::ifstream in(path, std::ios::binary);
        require(static_cast<bool>(in), "Cannot read the registered numeric stack-alias fixture");
        const std::string source(std::istreambuf_iterator<char>(in), {});
        // Check the fixture's raw address against the independently analyzed
        // ORIGINAL frame, never against the storage plan under test.
        Lexer lexer(source); const auto tokens = lexer.scanTokens();
        Parser parser(tokens); auto ast = parser.parseProgram(); DataSegmentManager data; Analyzer analyzer(data);
        analyzer.analyze(ast); IRLowerer lowerer; const auto original = lowerer.lower(ast);
        SymbolId table;
        for (const auto& b : original.functions[0].blocks) for (const auto& i : b.instructions)
            if (i.opcode == IROpcode::Address && i.symbol == "table") table = i.symbol_id;
        require(table.isValid(), "Raw stack-alias fixture lost its original array identity");
        const auto offset = analyzer.getAllLocalSymbols().at("main").at(table).stackOffset;
        const auto address = static_cast<unsigned>(0x1ffc + offset);
        require(address == 0x1ff4, "Numeric alias fixture no longer names its original local-array bytes");
        for (const auto level : {OptimizationLevel::O1, OptimizationLevel::O2, OptimizationLevel::Size}) {
            const auto compiled = compile(source, level, 0x706000);
            if (isGlobalOptimization(level)) require(!compiled.ir.functions[0].storage_layout_valid &&
                compiled.ir.functions[0].immutable_local_arrays.empty(), "Raw numeric stack alias did not freeze the original storage plan");
            for (const auto& r : compiled.object.relocation_table)
                require(r.target_symbol_name.compare(0, std::string(GSUAbi::ImmutablePoolPrefix).size(),
                    GSUAbi::ImmutablePoolPrefix) != 0, "Raw numeric stack alias acquired pooled storage");
            for (const auto index : {0u, 1u, 2u, 3u}) {
                DiscoGSU::Machine machine(linked(compiled.object), 0x706000);
                machine.seed(0x700100, static_cast<std::uint8_t>(index), false);
                machine.seed(0x700180, 0x5a, false); machine.seed(0x710180, 0xa5, false);
                machine.run();
                const auto expected = write ? (index == 0 ? 99u : (index + 1) * 11u) : (index + 2) * 11u;
                require(machine.reg(0) == expected, "Raw numeric pointer did not observe the original local-array bytes");
                const auto reads = (write ? 0u : 1u) + (index == 0 ? 1u : 0u);
                require(machine.accesses(0x700000 + address, false) == reads &&
                    machine.accesses(0x700000 + address, true) == (write ? 2u : 1u) && machine.accesses(0x700100, false) == 1,
                    "Raw stack-alias read/write multiplicity changed");
                require(machine.reg(10) == 0x1ffc && machine.reg(9) == 0x1ffc && machine.reg(6) == 0 &&
                    machine.reg(12) == 0xbeef && machine.reg(13) == 0xcafe && machine.ramBank() == 0 &&
                    machine.byte(0x700180) == 0x5a && machine.byte(0x710180) == 0xa5,
                    "Raw stack-alias fallback damaged stack, fault, bank or sentinel state");
            }
        }
    }
}

std::string immutablePoolSource() {
    // Nonuniform initialization is deliberately large enough to amortize
    // retained pointer checks and DATA. A small eligible pool need not win.
    std::string source = "word f(unsigned word i){word a[64]={";
    for (unsigned n = 0; n < 64; ++n) {
        if (n) source += ',';
        source += std::to_string(n % 4 == 0 ? 3 : n % 4 == 1 ? 5 : n % 4 == 2 ? 7 : 11);
    }
    return source + "};return a[i&63];}word main(){return f(*(volatile unsigned word*)0x100);}";
}

void actualLinkerPools() {
    auto object = compile(immutablePoolSource(), OptimizationLevel::Size, 0x706000).object;
    TestTempDirectory temp("seven-backend-link");
    const auto direct = temp.path() / "direct.o", assembly = temp.path() / "assembly.o";
    object.write(direct.string());
    auto from_assembly = Assembler().assemble(AssemblyGenerator(object).generate()); from_assembly.write(assembly.string());
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary); require(static_cast<bool>(in), "Cannot read linker roundtrip artifact");
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
    };
    const auto link = [&](const std::filesystem::path& input, const std::string& stem, bool runtime) {
        const auto binary = temp.path() / (stem + ".bin"), text = temp.path() / (stem + ".s");
        std::vector<std::string> arguments{"discld", input.string(), "--origin", "0x706000", "--ram-bank", "0",
            "--emit-asm", text.string(), "-o", binary.string()};
        if (runtime) arguments.insert(arguments.end(), {"--init-runtime", "--stack-pointer", "0x4000"});
        struct Capture {
            std::ostringstream text;
            std::streambuf* out = std::cout.rdbuf(text.rdbuf());
            std::streambuf* error = std::cerr.rdbuf(text.rdbuf());
            ~Capture() { std::cout.rdbuf(out); std::cerr.rdbuf(error); }
        } capture;
        const auto result = runLinker(arguments);
        if (result != 0) throw std::runtime_error("Actual pool linker path failed: " + capture.text.str());
        return read(binary);
    };
    const auto direct_bytes = link(direct, "direct", true), assembly_bytes = link(assembly, "assembly", true);
    require(direct_bytes == assembly_bytes, "Actual linker changed payload between compiler object and compiler .s");
    std::ifstream final_text(temp.path() / "direct.s");
    const std::string final_source(std::istreambuf_iterator<char>(final_text), {});
    final_text.close();
    auto final_object = Assembler().assemble(final_source); const auto final_path = temp.path() / "final.o";
    final_object.write(final_path.string());
    require(link(final_path, "final", false) == direct_bytes, "Actual linked .s roundtrip changed payload bytes");
    DiscoGSU::Machine machine(direct_bytes, 0x706000); machine.seed(0x700100, 2, false); machine.run();
    require(machine.reg(0) == 7 && machine.reg(6) == 0 && machine.reg(10) == 0x3ffc && machine.accesses(0x700100, false) == 1,
        "Actual linked immutable-pool payload changed execution/stack/volatile reads");
    temp.cleanup();
}

void immutablePools() {
    const auto source = immutablePoolSource();
    execution(source, {{0,3},{1,5},{2,7},{3,11},{4,3},{65535,11}}, 0x706000);
    for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto object = compile(source, level, 0x706000).object;
        require(std::any_of(object.relocation_table.begin(), object.relocation_table.end(), [](const RelocationEntry& r) {
            return r.type == RelocationType::ADDR16_RAM && r.target_symbol_name.compare(0,
                std::string(GSUAbi::ImmutablePoolPrefix).size(), GSUAbi::ImmutablePoolPrefix) == 0;
        }), "Profitable proven immutable local array was not emitted in copied RAM payload DATA");
    }
}

void legacyMixedIncumbent() {
    // Different functions share one immutable DATA range. Division ownership,
    // CODE/DATA padding and the pointer-clear choice must be costed together,
    // not replaced by independently attractive function-byte estimates.
    const auto original = immutablePoolSource();
    const auto main = original.find("word main()");
    require(main != std::string::npos, "Mixed incumbent fixture lost main");
    const auto table = original.substr(0, main);
    auto second = table; second.replace(second.find("word f("), 7, "word g(");
    const auto source = table + second + "word divide(word a,word b,word c){return a/b+c/b;}"
        "word main(){unsigned word i=*((volatile unsigned word*)0x100);word* p=(word*)0x400;"
        "for(word n=0;n<32;n++){p[n]=0;}return f(i)+g(i+1)+divide((word)i,3,99);}";
    const auto selected = compile(source, OptimizationLevel::Size, 0x706000).object;
    // Frozen legacy complete-search payload: CODE669 + startup10 + pad1 +
    // shared DATA128 =808. This ceiling is self-contained, not tool-dependent.
    require(linked(selected).size() <= 808 && selected.data_section.size() == 128,
        "Pointer-only extension lost the complete legacy mixed CODE/padding/shared-DATA incumbent");
    std::vector<std::uint32_t> offsets;
    for (const auto& symbol : selected.symbol_table) if (symbol.section == SymbolSection::DATA &&
        symbol.name.compare(0, std::string(GSUAbi::ImmutablePoolPrefix).size(), GSUAbi::ImmutablePoolPrefix) == 0)
        offsets.push_back(symbol.offset);
    require(offsets.size() == 2 && offsets[0] == offsets[1], "Mixed candidate lost cross-function DATA deduplication");
    for (const auto input : {0u, 1u, 2u, 37u}) {
        DiscoGSU::Machine machine(linked(selected), 0x706000);
        machine.seed(0x700100, static_cast<std::uint8_t>(input), false);
        for (unsigned p = 0x400; p < 0x440; ++p) machine.seed(0x700000 + p, 0xa5, false);
        machine.run();
        const unsigned values[] = {3,5,7,11};
        require(machine.reg(0) == values[input % 4] + values[(input + 1) % 4] + input / 3 + 33 &&
            machine.reg(6) == 0 && machine.reg(10) == 0x1ffc && machine.reg(12) == 0xbeef && machine.reg(13) == 0xcafe,
            "Mixed incumbent changed pooled loads/division or ABI restoration");
        require(machine.accesses(0x700100, false) == 1 && machine.accesses(0x700100, true) == 0,
            "Mixed incumbent changed volatile input multiplicity");
        for (unsigned p = 0x400; p < 0x440; ++p) require(machine.byte(0x700000 + p) == 0,
            "Mixed incumbent changed bounded clear stores");
    }
}

void strictCandidateReuse() {
    const std::string source = "word main(){word a=*(volatile word*)0x100;word b=*(volatile word*)0x102;"
        "word c=*(volatile word*)0x104;for(word i=0;i<32;i++){a+=b*3;b+=c*3;c+=a*3;}return a^b^c;}"
        "word last_spill_free(){return 42;}";
    const auto ordinary = compile(source, OptimizationLevel::Size).object;
    // compile() also repeats generate on this SAME strict backend instance.
    const auto strict = compile(source, OptimizationLevel::Size, 0x008000, true).object;
    require(strict.code_section.size() > ordinary.code_section.size() &&
        AssemblyGenerator(strict).generate().find("loop ;") != std::string::npos &&
        AssemblyGenerator(ordinary).generate().find("loop ;") == std::string::npos,
        "Strict mixed-function Os lost its exact larger spill-free selected candidate");
    for (const auto* object : {&ordinary, &strict}) for (const auto initial : {0u, 1u, 32768u, 65535u}) {
        DiscoGSU::Machine machine(linked(*object), 0x008000);
        std::uint16_t values[] = {static_cast<std::uint16_t>(initial), 5, 7};
        for (unsigned n = 0; n < 3; ++n) {
            machine.seed(0x700100 + n * 2, static_cast<std::uint8_t>(values[n]), false);
            machine.seed(0x700101 + n * 2, static_cast<std::uint8_t>(values[n] >> 8), false);
        }
        for (unsigned n = 0; n < 32; ++n) {
            values[0] = static_cast<std::uint16_t>(values[0] + values[1] * 3);
            values[1] = static_cast<std::uint16_t>(values[1] + values[2] * 3);
            values[2] = static_cast<std::uint16_t>(values[2] + values[0] * 3);
        }
        machine.run();
        require(machine.reg(0) == (values[0] ^ values[1] ^ values[2]) && machine.reg(6) == 0 &&
            machine.reg(9) == 0x1ffc && machine.reg(10) == 0x1ffc && machine.reg(12) == 0xbeef && machine.reg(13) == 0xcafe,
            "Strict selected-candidate reuse changed execution or ABI restoration");
        for (unsigned n = 0; n < 3; ++n) require(machine.accesses(0x700100 + n * 2, false) == 1 &&
            machine.accesses(0x700100 + n * 2, true) == 0, "Strict selected candidate changed volatile reads");
    }
}

void regionalFarOperandFallback() {
    // Deliberately use the validated IR boundary: the frontend disallows a
    // far-to-word cast, but the verifier/backend support this operand shape.
    // Eight live scalar snapshots force regional invariant pressure. The
    // otherwise identical near-pointer case must really borrow both registers.
    for (const bool far : {false, true}) {
        const Type integer{BaseType::WORD, "", 2, true};
        IRFunction function; function.name = "main"; function.return_type = integer;
        function.entry = IRBlockId{0};
        function.blocks = {{IRBlockId{0}, "entry", {}}, {IRBlockId{1}, "pixels", {}}, {IRBlockId{2}, "exit", {}}};
        const auto value = [&](unsigned block, IROpcode opcode, Type type, std::vector<IRValueId> operands,
                               std::int64_t immediate = 0, const std::string& operation = std::string()) {
            IRInstruction instruction; instruction.opcode = opcode; instruction.type = std::move(type);
            instruction.result = IRValueId{++function.value_count}; instruction.operands = std::move(operands);
            instruction.immediate = immediate; instruction.operation = operation;
            instruction.in_plot_context = block != 0;
            function.blocks.at(block).instructions.push_back(std::move(instruction));
            return IRValueId{function.value_count};
        };
        const auto effect = [&](unsigned block, IROpcode opcode, std::vector<IRValueId> operands = {}) {
            IRInstruction instruction; instruction.opcode = opcode; instruction.operands = std::move(operands);
            if (opcode == IROpcode::Rpix) instruction.type = Type{BaseType::VOID, "", 0, false};
            instruction.in_plot_context = block != 0;
            function.blocks.at(block).instructions.push_back(std::move(instruction));
        };
        std::vector<IRValueId> inputs;
        auto volatile_integer = integer; volatile_integer.is_volatile = true;
        for (unsigned n = 0; n < 8; ++n) {
            const auto address = value(0, IROpcode::Constant, pointerTo(volatile_integer, AddressSpace::RAM), {}, 0x100 + n * 2);
            inputs.push_back(value(0, IROpcode::LoadIndirect, integer, {address}));
            function.blocks[0].instructions.back().memory_volatile = true;
        }
        const auto pointer = value(0, IROpcode::Constant,
            pointerTo(Type{BaseType::BYTE, "", 1, true}, AddressSpace::RAM, far), {}, far ? 0x700201 : 0x0201);
        const auto trips = value(0, IROpcode::Constant, integer, {}, 4);
        IRInstruction setup; setup.opcode = IROpcode::HardwareLoop; setup.operands = {trips};
        setup.targets = {IRBlockId{1}}; setup.loop_target = IRBlockId{1}; setup.loop_id = 1;
        function.blocks[0].instructions.push_back(std::move(setup));
        const auto cast = value(1, IROpcode::Cast, integer, {pointer});
        effect(1, IROpcode::SetColor, {cast}); effect(1, IROpcode::Plot);
        for (const auto input : inputs) { effect(1, IROpcode::SetColor, {input}); effect(1, IROpcode::Plot); }
        IRInstruction latch; latch.opcode = IROpcode::HardwareLoopEnd; latch.loop_id = 1;
        latch.targets = {IRBlockId{1}, IRBlockId{2}}; latch.in_plot_context = true;
        function.blocks[1].instructions.push_back(std::move(latch));
        effect(2, IROpcode::HardwareLoopLeave); function.blocks[2].instructions.back().loop_id = 1;
        effect(2, IROpcode::Rpix); // Flush both pixel caches before checking the independent planar image.
        auto sum = inputs.front();
        for (std::size_t n = 1; n < inputs.size(); ++n) sum = value(2, IROpcode::Binary, integer, {sum, inputs[n]}, 0, "+");
        effect(2, IROpcode::Return, {sum});
        IRModule module; module.functions.push_back(std::move(function)); IRVerifier::verify(module);
        const std::map<std::string, Analyzer::LocalSymbolTable> locals{{"main", {}}};
        const std::map<std::string, FunctionSymbol> functions;
        DataSegmentManager data;
        auto optimized = module; IRGlobalOptimizer::run(optimized, locals); IRVerifier::verify(optimized);
        std::map<std::uint32_t, Type> definitions;
        for (const auto& b : optimized.functions[0].blocks) for (const auto& i : b.instructions)
            if (i.result.isValid()) definitions.emplace(i.result.value, i.type);
        const IRControlFlow flow(optimized.functions[0]);
        unsigned loops = 0, far_casts = 0;
        for (const auto& b : optimized.functions[0].blocks) for (const auto& i : b.instructions) {
            loops += i.opcode == IROpcode::HardwareLoop;
            if (i.opcode == IROpcode::Cast && isFarPointer(definitions.at(i.operands.at(0).value)))
                far_casts += std::any_of(flow.loops.begin(), flow.loops.end(),
                    [&](const IRControlFlow::Loop& loop) { return loop.blocks.count(b.id.value) != 0; });
        }
        require(loops == 1 && far_casts == (far ? 1u : 0u), "Regional far-operand fixture lost its verified LOOP/Cast boundary");
        for (const auto level : {OptimizationLevel::O1, OptimizationLevel::O2, OptimizationLevel::Size}) {
            CompilerConfig config; config.optimization = level; config.code_start_address = 0x008000;
            IRCodeGenerator generator(locals, functions, data, config); const auto object = generator.generate(module);
            const auto assembly = AssemblyGenerator(object).generate();
            require(Assembler().assemble(assembly).code_section == object.code_section,
                "Regional far-operand fixture changed bytes in assembly roundtrip");
            unsigned borrowed = 0, used = 0, setups = 0;
            if (isGlobalOptimization(level)) {
                // Canonical assembly already separates opcodes from operand
                // bytes. Never treat a literal $14/$43 as TO R4 / LDW (R3).
                struct Emitted { std::size_t offset; std::string text; };
                std::vector<Emitted> instructions;
                std::istringstream lines(assembly); std::string line;
                const std::string marker = " ; CODE+$";
                while (std::getline(lines, line)) {
                    const auto comment = line.find(marker), start = line.find_first_not_of(' ');
                    if (comment != std::string::npos && start < comment)
                        instructions.push_back({static_cast<std::size_t>(std::stoul(line.substr(comment + marker.size()), nullptr, 16)),
                            line.substr(start, comment - start)});
                }
                for (const auto& relocation : object.relocation_table) {
                    const auto end = static_cast<std::size_t>(relocation.patch_offset);
                    if (relocation.section_to_patch != SymbolSection::CODE || relocation.type != RelocationType::ADDR16_IWT ||
                        object.code_section.at(end) != 0xfd) continue;
                    ++setups;
                    const auto target = std::find_if(object.symbol_table.begin(), object.symbol_table.end(),
                        [&](const SymbolEntry& symbol) { return symbol.name == relocation.target_symbol_name; });
                    require(target != object.symbol_table.end() && target->section == SymbolSection::CODE,
                        "Regional fixture has no resolved LOOP body label");
                    const auto latch = std::find_if(instructions.begin(), instructions.end(), [&](const Emitted& i) {
                        return i.offset >= target->offset && i.text == "loop";
                    });
                    require(latch != instructions.end(), "Regional fixture has no decoded LOOP latch");
                    for (std::size_t n = 0; n + 1 < instructions.size(); ++n) {
                        const auto& i = instructions[n];
                        if (i.offset < end && end - i.offset <= 24 && instructions[n + 1].text == "ldw (r3)") {
                            if (i.text == "to r4") borrowed |= 1u << 4;
                            if (i.text == "to r6") borrowed |= 1u << 6;
                        }
                        if (i.offset >= target->offset && i.offset < latch->offset) {
                            if (i.text == "from r4" || i.text == "with r4") used |= 1u << 4;
                            if (i.text == "from r6" || i.text == "with r6") used |= 1u << 6;
                        }
                    }
                }
            }
            for (const auto first : {1u, 0x7ff8u, 0xfff8u}) {
                DiscoGSU::Machine machine(linked(object), 0x008000); machine.configureScreen(0x21, 0x18);
                unsigned expected_sum = 0;
                for (unsigned n = 0; n < 8; ++n) {
                    const auto input = static_cast<std::uint16_t>(first + n); expected_sum += input;
                    machine.seed(0x700100 + n * 2, static_cast<std::uint8_t>(input), false);
                    machine.seed(0x700101 + n * 2, static_cast<std::uint8_t>(input >> 8), false);
                }
                machine.seed(0x700180, 0x5a, false); machine.seed(0x710180, 0xa5, false); machine.run();
                require(machine.reg(0) == static_cast<std::uint16_t>(expected_sum) && machine.reg(6) == 0 &&
                    machine.reg(9) == 0x1ffc && machine.reg(10) == 0x1ffc && machine.reg(11) == 0x5555 &&
                    machine.reg(12) == 0xbeef && machine.reg(13) == 0xcafe && machine.ramBank() == 0 &&
                    machine.reg(1) == 36 && machine.reg(2) == 0 && machine.graphicsState("--plots") == 36 &&
                    machine.byte(0x700180) == 0x5a && machine.byte(0x710180) == 0xa5,
                    "Regional far fallback changed scalar/cursor/stack/LOOP/fault/bank/sentinel state");
                for (unsigned n = 0; n < 8; ++n) require(machine.accesses(0x700100 + n * 2, false) == 1 &&
                    machine.accesses(0x700100 + n * 2, true) == 0, "Regional far fallback changed volatile snapshot multiplicity");
                std::vector<std::uint8_t> pixels(0x6000, 0);
                for (unsigned x = 0; x < 36; ++x) {
                    const auto color = x % 9 == 0 ? 1u : (first + x % 9 - 1) & 15u;
                    // Independent 256x192 4bpp row-zero planar expectation.
                    for (unsigned plane = 0; plane < 4; ++plane) if (color & (1u << plane))
                        pixels.at((x / 8) * 768 + (plane / 2) * 16 + plane % 2) |= static_cast<std::uint8_t>(1u << (7 - x % 8));
                }
                for (std::size_t n = 0; n < pixels.size(); ++n) {
                    const auto actual = machine.byte(0x706000 + static_cast<std::uint32_t>(n));
                    if (actual != pixels[n]) throw std::runtime_error("Regional plotted-byte mismatch: far=" + std::to_string(far) +
                        " level=" + std::to_string(static_cast<unsigned>(level)) + " input=" + std::to_string(first) +
                        " offset=" + std::to_string(n) + " expected=" + std::to_string(pixels[n]) +
                        " actual=" + std::to_string(actual));
                }
            }
            if (isGlobalOptimization(level) && !(setups == 1 && borrowed == (far ? 0u : (1u << 4) | (1u << 6)) &&
                (far || used == borrowed)))
                throw std::runtime_error("Regional path proof failed: far=" + std::to_string(far) +
                    " level=" + std::to_string(static_cast<unsigned>(level)) + " setups=" + std::to_string(setups) +
                    " borrowed=" + std::to_string(borrowed) + " used=" + std::to_string(used));
        }
    }
}
}

int main() {
    try { predicates(); callsAndLoops(); physicalSpills(); numericStackAliases(); immutablePools(); actualLinkerPools(); legacyMixedIncumbent(); strictCandidateReuse(); regionalFarOperandFallback(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
