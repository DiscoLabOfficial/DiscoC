#include "IRCodeGenerator.hpp"
#include "IRGlobalOptimizer.hpp"
#include "AssemblyGenerator.hpp"
#include "Assembler.hpp"
#include "GSUInstructionModel.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool b, const char* text) { if (!b) throw std::runtime_error(text); }
struct Compilation { ObjectFile object; IRModule ir; };
Compilation compile(const std::string& source, OptimizationLevel level, std::uint32_t origin) {
    Lexer lexer(source); auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast); IRLowerer lowerer;
    const auto ir = lowerer.lower(ast); auto optimized = ir;
    if (isGlobalOptimization(level)) {
        IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols(), level);
        const auto first = dumpIR(optimized); IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols(), level);
        if (first != dumpIR(optimized)) throw std::runtime_error("Compaction/global pipeline is not idempotent\n" + source +
            "\nFirst:\n" + first + "Second:\n" + dumpIR(optimized));
    }
    CompilerConfig config; config.optimization = level; config.code_start_address = origin;
    IRCodeGenerator backend(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    auto object = backend.generate(ir);
    const auto assembly = AssemblyGenerator(object).generate(); Assembler assembler;
    const auto round_trip = assembler.assemble(assembly);
    require(round_trip.code_section == object.code_section, "Compact assembly round trip changed bytes");
    IRCodeGenerator again(analyzer.getAllLocalSymbols(), analyzer.getFunctionSymbols(), data, config);
    require(assembly == AssemblyGenerator(again.generate(ir)).generate(), "Compaction is nondeterministic");
    return {std::move(object), std::move(optimized)};
}
std::vector<std::uint8_t> linked(const ObjectFile& object) {
    auto bytes = object.code_section;
    for (const auto& relocation : object.relocation_table) {
        bool found = false;
        const std::string prefix = "__disco_stack_limit_";
        if (relocation.target_symbol_name.compare(0, prefix.size(), prefix) == 0) {
            const auto required = static_cast<unsigned>(std::stoul(relocation.target_symbol_name.substr(prefix.size())));
            require(relocation.type == RelocationType::ADDR16_RAM && required <= 65535, "Invalid stack-limit fixture");
            bytes.at(relocation.patch_offset + 1) = static_cast<std::uint8_t>(required);
            bytes.at(relocation.patch_offset + 2) = static_cast<std::uint8_t>(required >> 8); found = true;
        }
        for (const auto& symbol : object.symbol_table) if (symbol.name == relocation.target_symbol_name) {
            require(symbol.section == SymbolSection::CODE && relocation.type == RelocationType::ADDR16_IWT,
                "Unexpected compaction fixture relocation");
            const auto value = object.config.code_start_address + symbol.offset;
            bytes.at(relocation.patch_offset + 1) = static_cast<std::uint8_t>(value);
            bytes.at(relocation.patch_offset + 2) = static_cast<std::uint8_t>(value >> 8); found = true;
        }
        if (!found) throw std::runtime_error("Missing compact loop relocation symbol: " + relocation.target_symbol_name);
    }
    return bytes;
}
std::size_t count(const IRModule& m, IROpcode op) {
    std::size_t n = 0; for (const auto& f : m.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions) n += i.opcode == op;
    return n;
}
void execution(const std::string& source, std::uint16_t result, bool cache = false, bool compact = false) {
    for (const auto origin : {0x008000u, 0x706007u}) for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto optimized = compile(source, level, origin), baseline = compile(source, OptimizationLevel::O1, origin);
        for (unsigned seed : {0u, 1u, 3u}) {
            DiscoGSU::Machine a(linked(optimized.object), origin), b(linked(baseline.object), origin);
            a.seed(0x700100, static_cast<std::uint8_t>(seed), false); b.seed(0x700100, static_cast<std::uint8_t>(seed), false);
            a.run(); b.run();
            require(a.reg(0) == result && a.reg(0) == b.reg(0), "Compact program changed its return value");
            require(a.reg(6) == 0 && a.reg(10) == b.reg(10), "Compaction damaged stack/fault state");
            require(a.reg(12) == b.reg(12) && a.reg(13) == b.reg(13), "Generated LOOP did not restore R12/R13");
            for (unsigned p = 0; p < 1024; ++p)
                require(a.byte(0x700000 + p) == b.byte(0x700000 + p), "Compaction changed externally visible RAM");
            if (cache && (level == OptimizationLevel::O2 || compact))
                if (a.cacheRequests() == 0) throw std::runtime_error("Profitable small generated loop has no automatic CACHE: " + source +
                    "\n" + dumpIR(optimized.ir) + "\n" + AssemblyGenerator(optimized.object).generate());
        }
        if (compact) require(optimized.object.code_section.size() < baseline.object.code_section.size(), "Initializer compaction did not save bytes");
    }
}
void reject(IRModule m, const char* expected) {
    try { IRVerifier::verify(m); } catch (const CompilerError& e) {
        require(e.getMessage().find(expected) != std::string::npos, "Wrong compact IR diagnostic"); return;
    }
    throw std::runtime_error("Malformed compact IR was accepted");
}
void dynamicLoops() {
    const std::string span = "word main(){word n=*((volatile word*)0x100);word r=10;plot{at(4,3);color 2;"
        "@cache for(word i=0;i<n;i++){r++;pixel;}flush;return r+cursor.x;}}";
    for (const auto origin : {0x008000u, 0x706007u}) for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
        const auto optimized = compile(span, level, origin), baseline = compile(span, OptimizationLevel::O1, origin);
        require(count(optimized.ir, IROpcode::HardwareLoop) == 1, "Dynamic cached span was not converted to LOOP");
        const bool emitted_loop = AssemblyGenerator(optimized.object).generate().find("\n    loop ;") != std::string::npos;
        require(level != OptimizationLevel::O2 || emitted_loop, "O2 did not emit the converted dynamic LOOP");
        for (const auto n : std::array<std::uint16_t, 7>{{0, 1, 3, 112, 127, 0xffff, 0x8000}}) {
            DiscoGSU::Machine a(linked(optimized.object), origin), b(linked(baseline.object), origin);
            for (auto* machine : {&a, &b}) {
                machine->seed(0x700100, static_cast<std::uint8_t>(n), false);
                machine->seed(0x700101, static_cast<std::uint8_t>(n >> 8), false);
                machine->run();
            }
            const unsigned iterations = n < 0x8000 ? n : 0;
            require(a.reg(0) == 14 + 2 * iterations && a.reg(0) == b.reg(0), "Dynamic LOOP mishandled zero/signed count or live-out value");
            require(a.reg(6) == 0 && a.reg(10) == b.reg(10), "Dynamic LOOP damaged stack/fault state");
            require(a.reg(12) == b.reg(12) && a.reg(13) == b.reg(13), "Dynamic LOOP failed to restore scoped loop registers");
            for (unsigned p = 0; p < 1024; ++p)
                require(a.byte(0x700000 + p) == b.byte(0x700000 + p), "Dynamic LOOP changed pixel/cache output");
            // Os may select the smaller original software loop. Its final
            // failed condition executes CACHE once more; hardware LOOP does
            // not revisit that condition, including on the zero-count path.
            require(a.cacheRequests() == iterations + (emitted_loop ? 0u : 1u),
                "Dynamic LOOP lost the explicit CACHE hint or entered a zero count");
        }
    }
    // The final induction value is also a live-out. A negative start and a
    // positive end exercise unsigned count construction for signed words.
    execution("word main(){word n=(word)*((volatile u8*)0x100);word i=-3;word r=0;"
        "for(;i<n;i++){r++;}return r-i;}", 3);
    const std::string signed_distance = "word main(){word i=*((volatile word*)0x100);word end=*((volatile word*)0x102);"
        "word count=0;word sum=5;for(;i<end;i++){count++;sum+=i;}"
        "*((volatile word*)0x104)=count;*((volatile word*)0x106)=i;return sum;}";
    const auto signed_loop = compile(signed_distance, OptimizationLevel::O2, 0x008000);
    const auto signed_baseline = compile(signed_distance, OptimizationLevel::O1, 0x008000);
    require(count(signed_loop.ir, IROpcode::HardwareLoop) == 1, "Runtime signed start/end were not converted");
    for (const auto& range : {std::make_pair(0x8000u, 0x7fffu), std::make_pair(0xfffdu, 3u),
                            std::make_pair(3u, 0xfffdu), std::make_pair(0x8000u, 0x8000u)}) {
        DiscoGSU::Machine a(linked(signed_loop.object), 0x008000), b(linked(signed_baseline.object), 0x008000);
        for (auto* machine : {&a, &b}) {
            machine->seed(0x700100, static_cast<std::uint8_t>(range.first), false);
            machine->seed(0x700101, static_cast<std::uint8_t>(range.first >> 8), false);
            machine->seed(0x700102, static_cast<std::uint8_t>(range.second), false);
            machine->seed(0x700103, static_cast<std::uint8_t>(range.second >> 8), false);
            machine->run();
        }
        require(a.reg(0) == b.reg(0) && a.reg(10) == b.reg(10) && a.reg(12) == b.reg(12) && a.reg(13) == b.reg(13),
            "Signed crossing LOOP changed multiple carried live-outs or register restoration");
        for (unsigned p = 0; p < 8; ++p)
            require(a.byte(0x700100 + p) == b.byte(0x700100 + p), "Signed crossing LOOP changed induction/count observability");
    }
    execution("word main(){word n=(word)*((volatile u8*)0x100);word r=0;"
        "for(word i=0;i<=7;i++){r+=n+1;}return r/(n+1);}", 8);
    for (const auto source : {
        "word main(){word n=*((volatile word*)0x100);word r=0;for(word i=0;i<=n;i++){r++;}return r;}",
        "word main(){word r=0;for(word i=32760;i<=32767;i++){r++;}return r;}",
        "word main(){unsigned word r=0;for(unsigned word i=65528;i<=65535;i++){r++;}return (word)r;}"})
        require(count(compile(source, OptimizationLevel::O2, 0x008000).ir, IROpcode::HardwareLoop) == 0,
            "Potentially wrapping inclusive loop was converted to a finite count");
    const auto unsigned_span = compile("word main(){unsigned word n=*((volatile unsigned word*)0x100);unsigned word r=0;"
        "for(unsigned word i=0;i<n;i++){r++;}return (word)r;}", OptimizationLevel::O2, 0x008006);
    require(count(unsigned_span.ir, IROpcode::HardwareLoop) == 1, "Unsigned strict-bound loop was not converted");
    auto seeded_code = linked(unsigned_span.object);
    // Start before the function with live caller LOOP registers. Relocations
    // were linked at $8006, so this literal prelude changes no payload address.
    seeded_code.insert(seeded_code.begin(), {0xfc, 0xef, 0xbe, 0xfd, 0xfe, 0xca});
    for (const auto n : std::array<std::uint16_t, 4>{{0, 1, 0x8000, 0xffff}}) {
        DiscoGSU::Machine a(seeded_code, 0x008000);
        a.seed(0x700100, static_cast<std::uint8_t>(n), false); a.seed(0x700101, static_cast<std::uint8_t>(n >> 8), false); a.run();
        require(a.reg(0) == n && a.reg(6) == 0 && a.reg(10) == 0x1ffc && a.reg(12) == 0xbeef && a.reg(13) == 0xcafe,
            "Unsigned LOOP truncated its 16-bit count or failed to exit cleanly");
    }
}
}
int main() {
    try {
        const std::string table = "word main(){word a[17]={0,6,12,19,24,30,36,41,45,49,53,56,59,61,63,64,64};"
            "word i=(word)*((volatile u8*)0x100)&3; word total=0; for(word n=0;n<17;n++) total+=a[n]; return total+(i-i);}";
        // All entries, including signed materialization, are observed dynamically.
        execution(table, 682, false, true);
        auto good = compile(table, OptimizationLevel::O2, 0x008000).ir;
        require(count(good, IROpcode::MemoryInitialize) == 1, "Sequential local initializer was not compacted");
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
            auto invalid = good;
            for (auto& b : invalid.functions[0].blocks) for (auto& i : b.instructions) if (i.opcode == IROpcode::MemoryInitialize) {
                if (mutation == 0) i.initialization_values.resize(257);
                if (mutation == 1) i.immediate = 1;
                if (mutation == 2) i.operands.clear();
                if (mutation == 3) i.memory_volatile = true;
            }
            reject(invalid, mutation == 3 ? "volatile metadata" : "memory initializer");
        }
        std::string uniform = "word main(){word a[32]={";
        for (unsigned n = 0; n < 32; ++n) { if (n) uniform += ','; uniform += "-3"; }
        uniform += "}; word total=0;for(word i=0;i<32;i++)total+=a[i];return total;}";
        execution(uniform, 65440, true, true);
        const std::string span = "word main(){plot{at(4,3); color 2; for(word i=0;i<128;i++){pixel;} flush; return cursor.x;}}";
        execution(span, 132, true);
        const auto explicit_cache = compile("@cache " + span, OptimizationLevel::O2, 0x706007);
        DiscoGSU::Machine manual(linked(explicit_cache.object), 0x706007); manual.run();
        require(manual.reg(0) == 132 && manual.cacheRequests() == 1, "Automatic CACHE interfered with an explicit cache window");
        std::string large = "word main(){for(word i=0;i<8;i++){";
        for (unsigned n = 0; n < 160; ++n) large += "*((volatile byte*)0x200)=(byte)3;";
        large += "}return 7;}";
        const auto large_loop = compile(large, OptimizationLevel::O2, 0x706007);
        require(count(large_loop.ir, IROpcode::HardwareLoop) == 1, "Oversized-cache fallback fixture lost its LOOP");
        DiscoGSU::Machine cold(linked(large_loop.object), 0x706007); cold.run();
        require(cold.reg(0) == 7 && cold.cacheRequests() == 0 && cold.accesses(0x700200, true) == 1280,
            "Automatic CACHE crossed its window or batched observable stores");
        require(count(compile(span, OptimizationLevel::O2, 0x008000).ir, IROpcode::HardwareLoop) == 1,
            "Ordinary constant-trip loop was not converted to LOOP");
        require(compile(span, OptimizationLevel::Size, 0x008000).object.code_section.size() <
            compile(span, OptimizationLevel::O2, 0x008000).object.code_section.size(),
            "Os charged a larger LOOP setup instead of selecting conservative emitted bytes");
        const auto early = compile("word main(){word r=0;for(word i=0;i<16;i++){if(i==3)break;r++;}return r;}", OptimizationLevel::O2, 0x008000);
        require(count(early.ir, IROpcode::HardwareLoop) == 0, "Early-exit loop was unsafely converted");
        execution("word main(){word r=0;for(word i=0;i<0;i++){r++;}return r;}", 0);
        const auto live_out = compile("word main(){word r=0;for(word i=0;i<16;i++){r+=i;}return r;}", OptimizationLevel::O2, 0x008000);
        require(count(live_out.ir, IROpcode::HardwareLoop) == 1, "Loop with repaired live-out SSA value was not converted");
        execution("word main(){word r=0;for(word i=0;i<16;i++){r+=i;}return r;}", 120);
        dynamicLoops();
        execution("word main(){word a=9;word b=9;word* p=&a;if(*((volatile u8*)0x100))p=&b;return *p;}", 9);
        const auto volatile_table = compile("word main(){volatile word a[4]={1,2,3,4};return a[0];}", OptimizationLevel::O2, 0x008000);
        require(count(volatile_table.ir, IROpcode::MemoryInitialize) == 0, "Volatile stores were batched");
        for (const auto level : {OptimizationLevel::O2, OptimizationLevel::Size}) {
            const auto checked = compile("word f(volatile word* p){*((volatile byte*)0x100)=(byte)1;return *p;}", level, 0x008000);
            DiscoGSU::Machine fault(linked(checked.object), 0x008000);
            fault.seed(0x702002, 1, false); fault.seed(0x702003, 3, false); fault.run();
            require(fault.reg(6) == 1 && fault.byte(0x700100) == 1 && fault.accesses(0x700100, true) == 1,
                "Pointer-parameter SSA hoisted a failing check before the volatile witness");
        }
        std::cout << "GSU register, initializer, LOOP and automatic CACHE checks passed\n";
        return 0;
    } catch (const CompilerError& e) { std::cerr << e.getMessage() << '\n'; }
      catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
    return 1;
}
