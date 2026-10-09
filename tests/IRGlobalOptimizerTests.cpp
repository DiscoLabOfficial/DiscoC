#include "IRGlobalOptimizer.hpp"
#include "IRControlFlow.hpp"
#include "LinearScanAllocator.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Optimizer.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::size_t count(const IRModule& m, IROpcode op) {
    std::size_t n = 0;
    for (const auto& f : m.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions) n += i.opcode == op;
    return n;
}
std::size_t temporaryCount(const IRModule& m) {
    std::size_t n = 0;
    for (const auto& f : m.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        n += i.opcode == IROpcode::Address && i.operation == "temporary";
    return n;
}
// Observe both the value and the ordered side-effecting calls. Promotion must
// not turn a short-circuit join into eager evaluation of its incoming values.
std::pair<bool, std::vector<std::string>> logicalTrace(const IRFunction& f, const std::map<std::string, bool>& inputs) {
    std::vector<std::int64_t> values(static_cast<std::size_t>(f.value_count) + 1);
    std::map<std::int64_t, std::int64_t> memory;
    std::vector<std::string> calls;
    auto block = f.entry; IRBlockId previous;
    for (std::size_t steps = 0; steps < 1000; ++steps) {
        bool branched = false;
        for (const auto& i : f.blocks.at(block.value).instructions) {
            const auto operand = [&](std::size_t n) { return values.at(i.operands.at(n).value); };
            auto& result = values.at(i.result.value);
            switch (i.opcode) {
                case IROpcode::Constant: result = i.immediate; break;
                case IROpcode::Address:
                    require(i.operation == "temporary", "Unexpected source storage in logical evaluator");
                    result = i.immediate; break;
                case IROpcode::StoreIndirect: memory[operand(0)] = operand(1); break;
                case IROpcode::LoadIndirect: result = memory.at(operand(0)); break;
                case IROpcode::Call: result = inputs.at(i.symbol); calls.push_back(i.symbol); break;
                case IROpcode::Phi: {
                    bool found = false;
                    for (std::size_t p = 0; p < i.targets.size(); ++p) if (i.targets[p].value == previous.value) {
                        result = operand(p); found = true; break;
                    }
                    require(found, "Logical PHI has no incoming predecessor"); break;
                }
                case IROpcode::Branch:
                case IROpcode::CondBranch:
                    previous = block;
                    block = i.targets.at(i.opcode == IROpcode::CondBranch && !operand(0) ? 1 : 0);
                    branched = true; break;
                case IROpcode::Return: return {operand(0) != 0, std::move(calls)};
                default: throw std::runtime_error("Unsupported instruction in logical evaluator");
            }
            if (branched) break;
        }
        require(branched, "Logical test did not return or branch");
    }
    throw std::runtime_error("Logical test exceeded its execution budget");
}
template<class Mutate, class Check> void temporaryCase(Mutate mutate, Check test,
    const std::string& source = "bool first(); bool second(); bool f() { return first() && second(); }") {
    Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto program = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(program);
    IRLowerer lowerer; auto module = lowerer.lower(program);
    mutate(module); IRVerifier::verify(module);
    IRGlobalOptimizer::run(module, analyzer.getAllLocalSymbols()); IRVerifier::verify(module);
    test(module);
}
template<class Check> void check(const std::string& source, Check test, bool hardware = false) {
    Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto program = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(program);
    if (hardware) { Optimizer optimizer; optimizer.optimize(program); }
    IRLowerer lowerer; const auto original = lowerer.lower(program);
    auto optimized = original; IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols());
    test(original, optimized);
    // A new invocation cannot reinterpret existing PHIs as mutable storage.
    const auto first = dumpIR(optimized); IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols());
    require(first == dumpIR(optimized), "Global pipeline is not idempotent");
}
void reject(const IRModule& m, const char* needle) {
    try { IRVerifier::verify(m); }
    catch (const CompilerError& error) {
        require(error.getMessage().find(needle) != std::string::npos, "Unexpected PHI verifier diagnostic"); return;
    }
    throw std::runtime_error("Malformed PHI was accepted");
}
void allocation(const IRFunction& f) {
    IRControlFlow cfg(f); LinearScanAllocator allocator; allocator.runGlobal(f, {5, 7, 8});
    const auto disjoint = [&](const std::set<std::uint32_t>& live) {
        std::set<std::uint8_t> registers; std::set<int> slots;
        for (const auto v : live) {
            const auto* location = allocator.find(IRValueId{v});
            require(location != nullptr, "Live value has no allocation");
            if (location->has_register) require(registers.insert(location->physical_register).second, "Interfering live values share a register");
            if (location->spill_slot >= 0) require(slots.insert(location->spill_slot).second, "Interfering live values share a spill slot");
        }
    };
    for (const auto& b : f.blocks) {
        auto live = cfg.live_out[b.id.value]; disjoint(live);
        for (auto i = b.instructions.rbegin(); i != b.instructions.rend(); ++i) {
            if (i->result.isValid()) live.erase(i->result.value);
            if (i->opcode != IROpcode::Phi) for (const auto v : i->operands) live.insert(v.value);
            disjoint(live);
        }
    }
    bool reserved_rejected = false;
    try { allocator.runGlobal(f, {1, 5}); } catch (const std::runtime_error&) { reserved_rejected = true; }
    require(reserved_rejected, "Global allocator accepted the plotting cursor register");
}
}

int main() {
    try {
        check("word f(word x) { word a = x; if (x > 0) a = 2; else a = 3; return a; }", [](const IRModule&, const IRModule& m) {
            require(count(m, IROpcode::Phi) == 1, "Branch join was not promoted to a PHI");
            require(count(m, IROpcode::StoreIndirect) == 0 && count(m, IROpcode::LoadIndirect) == 1, "Promoted scalar still has memory traffic");
            allocation(m.functions[0]);
            auto broken = m;
            IRInstruction* phi = nullptr;
            for (auto& b : broken.functions[0].blocks) for (auto& i : b.instructions) if (i.opcode == IROpcode::Phi) phi = &i;
            require(phi != nullptr, "Missing test PHI");
            phi->targets[1] = phi->targets[0]; reject(broken, "phi");
            broken = m;
            for (auto& b : broken.functions[0].blocks) for (auto& i : b.instructions) if (i.opcode == IROpcode::Phi) {
                i.targets.pop_back(); i.operands.pop_back();
            }
            reject(broken, "cover every predecessor");
            broken = m;
            for (auto& b : broken.functions[0].blocks) for (auto& i : b.instructions) if (i.opcode == IROpcode::Phi) i.type.is_unsigned = true;
            reject(broken, "phi value");
            broken = m;
            for (auto& b : broken.functions[0].blocks) for (auto& i : b.instructions) if (i.opcode == IROpcode::Phi) i.operands[0] = i.result;
            reject(broken, "predecessor edge");
        });
        for (const auto& expression : {"(first() && second()) || third()", "first() && (second() || third())"}) {
            check(std::string("bool first(); bool second(); bool third(); bool f() { return ") + expression + "; }",
                [](const IRModule& original, const IRModule& m) {
                    require(temporaryCount(original) == 2, "Nested logical fixture has no independent temporaries");
                    require(temporaryCount(m) == 0 && count(m, IROpcode::StoreIndirect) == 0 && count(m, IROpcode::LoadIndirect) == 0,
                            "Short-circuit temporaries were not promoted to SSA");
                    require(count(m, IROpcode::Call) == 3, "Short-circuit promotion removed a side-effecting call");
                    require(m.functions[0].total_local_alloc_size == 0, "Promoted anonymous slots still reserve a local frame");
                    for (unsigned bits = 0; bits < 8; ++bits) {
                        const std::map<std::string, bool> inputs{{"first", (bits & 1) != 0}, {"second", (bits & 2) != 0}, {"third", (bits & 4) != 0}};
                        require(logicalTrace(original.functions[0], inputs) == logicalTrace(m.functions[0], inputs),
                                "SSA changed short-circuit value or ordered call effects");
                    }
                    allocation(m.functions[0]);
                });
        }
        check("word floor_div(word value, word divisor) { word q = value / divisor; if (value < 0 && value % divisor != 0) q--; return q; }",
            [](const IRModule&, const IRModule& m) {
                require(temporaryCount(m) == 0 && count(m, IROpcode::StoreIndirect) == 0,
                        "floor_div retained the logical temporary's stores");
                require(count(m, IROpcode::LoadIndirect) == 2 && count(m, IROpcode::DivMod) == 1,
                        "floor_div promotion changed parameter loads or paired division");
            });
        temporaryCase([](IRModule& m) {
            for (auto& b : m.functions[0].blocks) {
                for (auto i = b.instructions.begin(); i != b.instructions.end(); ++i)
                    if (i->opcode == IROpcode::Address && i->operation == "temporary") {
                        IRInstruction call; call.opcode = IROpcode::Call; call.type = Type{BaseType::VOID, "", 0, false};
                        call.symbol = "observe_address"; call.operands = {i->result}; call.source = i->source;
                        b.instructions.insert(i + 1, std::move(call)); return;
                    }
            }
            throw std::runtime_error("No temporary to escape");
        }, [](const IRModule& m) {
            require(temporaryCount(m) == 1 && count(m, IROpcode::StoreIndirect) == 2 && count(m, IROpcode::LoadIndirect) == 1,
                    "Escaped anonymous storage was promoted");
        });
        temporaryCase([](IRModule& m) {
            IRValueId address;
            for (auto& b : m.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::Address && i.operation == "temporary") {
                    auto type = pointeeType(i.type); type.is_volatile = true;
                    i.type = pointerTo(type, AddressSpace::RAM); address = i.result;
                }
            for (auto& b : m.functions[0].blocks) for (auto& i : b.instructions)
                if ((i.opcode == IROpcode::LoadIndirect || i.opcode == IROpcode::StoreIndirect) && i.operands[0].value == address.value) {
                    i.memory_volatile = true; i.type.is_volatile = true;
                }
        }, [](const IRModule& m) {
            require(temporaryCount(m) == 1 && count(m, IROpcode::StoreIndirect) == 2 && count(m, IROpcode::LoadIndirect) == 1,
                    "Observable volatile temporary accesses were promoted");
        });
        temporaryCase([](IRModule& m) {
            for (auto& b : m.functions[0].blocks) {
                const auto store = std::find_if(b.instructions.begin(), b.instructions.end(),
                    [](const IRInstruction& i) { return i.opcode == IROpcode::StoreIndirect; });
                if (store != b.instructions.end()) { b.instructions.erase(store); return; }
            }
            throw std::runtime_error("No temporary store to remove");
        }, [](const IRModule& m) {
            require(temporaryCount(m) == 1 && count(m, IROpcode::StoreIndirect) == 1 && count(m, IROpcode::LoadIndirect) == 1,
                    "Uninitialized logical path acquired an invented SSA value");
        });
        temporaryCase([](IRModule& m) {
            std::int64_t first_offset = 0;
            for (auto& b : m.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::Address && i.operation == "temporary") {
                    if (!first_offset) first_offset = i.immediate;
                    else i.immediate = first_offset;
                }
        }, [](const IRModule& m) {
            require(temporaryCount(m) == 2 && count(m, IROpcode::StoreIndirect) == 4 && count(m, IROpcode::LoadIndirect) == 2,
                    "Aliased anonymous frame offsets became independent SSA cells");
        }, "bool first(); bool second(); bool third(); bool f() { return (first() && second()) || third(); }");
        temporaryCase([](IRModule& m) {
            auto& f = m.functions[0]; f.total_local_alloc_size = std::max(6, f.total_local_alloc_size);
            for (auto& b : f.blocks) for (std::size_t p = 0; p < b.instructions.size(); ++p) {
                const auto original = b.instructions[p];
                if (original.opcode != IROpcode::Address || original.operation != "temporary") continue;
                // A four-byte far-pointer slot beginning at -4 overlaps the
                // byte slot at -2 without identical starts or misalignment.
                b.instructions[p].immediate = -2;
                IRInstruction wide = original;
                wide.type = pointerTo(pointerTo(Type{BaseType::WORD, "", 2, false}, AddressSpace::RAM, true), AddressSpace::RAM);
                wide.result = IRValueId{++f.value_count}; wide.immediate = -4;
                IRInstruction call; call.opcode = IROpcode::Call; call.type = Type{BaseType::VOID, "", 0, false};
                call.symbol = "observe_wide_slot"; call.operands = {wide.result}; call.source = original.source;
                b.instructions.insert(b.instructions.begin() + static_cast<std::ptrdiff_t>(p + 1), std::move(wide));
                b.instructions.insert(b.instructions.begin() + static_cast<std::ptrdiff_t>(p + 2), std::move(call));
                return;
            }
            throw std::runtime_error("No temporary to partially alias");
        }, [](const IRModule& m) {
            require(temporaryCount(m) == 2 && count(m, IROpcode::StoreIndirect) == 2 && count(m, IROpcode::LoadIndirect) == 1,
                    "A partially overlapping anonymous slot was promoted past an escaped wider slot");
        });
        temporaryCase([](IRModule& m) {
            for (auto& b : m.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::Address && i.operation == "temporary") { i.immediate = -2; return; }
        }, [](const IRModule& m) {
            require(temporaryCount(m) == 1 && count(m, IROpcode::StoreIndirect) >= 3 && count(m, IROpcode::LoadIndirect) >= 2,
                    "Temporary overlapping a source local was promoted independently");
        }, "bool first(); bool second(); bool f() { bool sentinel = first(); bool result = first() && second(); return result == sentinel; }");
        check("word f(word n) { word x = 1; word y = 2; for (word i = 0; i < n; i++) { word t = x; x = y; y = t; } return x + y; }",
            [](const IRModule&, const IRModule& m) {
                require(count(m, IROpcode::Phi) >= 3, "Loop-carried values were not promoted"); allocation(m.functions[0]);
                IRControlFlow cfg(m.functions[0]); require(!cfg.loops.empty(), "Loop backedge was omitted");
                for (const auto& loop : cfg.loops)
                    require(*loop.blocks.rbegin() - *loop.blocks.begin() + 1 == loop.blocks.size(),
                            "Loop and its PHI backedge copies were not laid out contiguously");
            });
        check("void escape(word* p); word f(word x) { word a = x; word* p = &a; escape(p); *p = 4; return a; }", [](const IRModule&, const IRModule& m) {
            require(count(m, IROpcode::StoreIndirect) >= 2 && count(m, IROpcode::LoadIndirect) >= 2, "Escaped local was promoted");
        });
        check("word f(word x) { word a=x;word* p=&a;*p=4;return a; }", [](const IRModule&, const IRModule& m) {
            std::size_t pointer_memory = 0;
            for (const auto& b : m.functions[0].blocks) for (const auto& i : b.instructions)
                pointer_memory += i.type.pointer_level > 0 && (i.opcode == IROpcode::LoadIndirect || i.opcode == IROpcode::StoreIndirect);
            require(pointer_memory == 0, "A nonescaping near-pointer local was not promoted");
        });
        check("word f(word x) { volatile word a = x; a = a + 1; return a + a; }", [](const IRModule&, const IRModule& m) {
            std::size_t loads = 0, stores = 0;
            for (const auto& b : m.functions[0].blocks) for (const auto& i : b.instructions) {
                loads += i.opcode == IROpcode::LoadIndirect && i.memory_volatile;
                stores += i.opcode == IROpcode::StoreIndirect && i.memory_volatile;
            }
            require(loads == 3 && stores == 2, "SSA changed observable volatile accesses");
        });
        check("word f(word x) { word a; if (x > 0) a = 2; return a; }", [](const IRModule&, const IRModule& m) {
            require(count(m, IROpcode::StoreIndirect) == 1 && count(m, IROpcode::LoadIndirect) >= 2, "Uninitialized local acquired an invented SSA value");
        });
        check("word add(word a, word b) { return a + b; } word main() { return add(12,30); }", [](const IRModule&, const IRModule& m) {
            require(count(m, IROpcode::Call) == 0, "Tiny scalar leaf was not inlined");
            const auto& body = m.functions.back().blocks[0].instructions;
            const auto result = body.back().operands[0]; bool specialized = false;
            for (const auto& i : body) specialized = specialized || (i.result.value == result.value && i.opcode == IROpcode::Constant && i.immediate == 42);
            require(specialized, "Constant arguments did not specialize the inlined leaf");
        });
        check("word divide(word a, word b) { return a / b; } @cache word main() { return divide(42,2); }", [](const IRModule&, const IRModule& m) {
            require(count(m, IROpcode::Call) == 1, "Large runtime helper was duplicated into CACHE");
        });
        check("word five(word x) { return ((((x + 1) ^ 2) + 3) ^ 4) + 5; } @cache word main() { return five(*(volatile word*)0x100); }",
            [](const IRModule&, const IRModule& m) {
                require(count(m, IROpcode::Call) == 1, "CACHE caller exceeded its scalar inlining cost limit");
            });
        check("word four(word x) { return (((x + 1) ^ 2) + 3) ^ 4; } @cache word main() { return four(*(volatile word*)0x100); }",
            [](const IRModule&, const IRModule& m) {
                require(count(m, IROpcode::Call) == 0, "Eligible CACHE scalar leaf was not inlined");
            });
        check("word f(word n, word k) { word s = 0; for (word i = 0; i < n; i++) s += k * 7 + i; return s; }", [](const IRModule&, const IRModule& m) {
            IRControlFlow cfg(m.functions[0]); require(cfg.loops.size() == 1, "Missing LICM loop");
            bool hoisted = false;
            for (const auto& b : m.functions[0].blocks) for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::Binary && i.operation == "*") hoisted = !cfg.loops[0].blocks.count(b.id.value);
            require(hoisted, "Safe invariant multiply was not hoisted");
        });
        check("word f(word n, word k) { word s = 0; for (word i = 0; i < n; i++) s += 8 / k; return s; }", [](const IRModule&, const IRModule& m) {
            IRControlFlow cfg(m.functions[0]); bool inside = false;
            for (const auto& b : m.functions[0].blocks) for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::Binary && i.operation == "/") inside = cfg.loops[0].blocks.count(b.id.value) != 0;
            require(inside, "LICM speculated a division-by-zero fault");
        });
        check("struct Wide { word a; word b; word c; word d; }; word f() { struct Wide a[3]; word s = 0; for (u16 i = 0; i < 3; i++) { a[i].a = (word)i; s += a[i].a; } return s; }",
            [](const IRModule&, const IRModule& m) {
                bool scaled = false;
                for (const auto& b : m.functions[0].blocks) for (const auto& i : b.instructions) scaled = scaled || i.operation == "scaled+";
                require(scaled, "Repeated address scale was not replaced by an induction PHI"); allocation(m.functions[0]);
            });
        check("void tick(word x); word f(word n) { word s = 0; for (word i = 0; i < n; i++) { tick(i); s += i; } return s; }", [](const IRModule&, const IRModule& m) {
            LinearScanAllocator allocator; allocator.runGlobal(m.functions[0], {5,7,8});
            IRControlFlow cfg(m.functions[0]); bool preserved = false; std::size_t position = 0;
            for (const auto& b : m.functions[0].blocks) for (const auto& i : b.instructions) {
                if (i.opcode == IROpcode::Call)
                    for (const auto& location : allocator.locations()) preserved = preserved || allocator.liveAcrossCall(IRValueId{location.first}, position);
                ++position;
            }
            require(preserved && !cfg.loops.empty(), "Loop-carried call liveness was lost"); allocation(m.functions[0]);
        });
        check("word f() { word x = 1; for (word i = 3; i > 0; i = i - 1) x += 2; return x; }", [](const IRModule&, const IRModule& m) {
            require(count(m, IROpcode::HardwareLoop) == 1 && count(m, IROpcode::HardwareLoopLeave) == 1, "Hardware pair was not exposed as CFG");
            const auto& f = m.functions[0]; IRControlFlow cfg(f); require(cfg.loops.size() == 1, "Hidden hardware backedge remains"); allocation(f);
            auto broken = m;
            for (auto& b : broken.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::HardwareLoopEnd) i.loop_id += 1;
            reject(broken, "unmatched hardware-loop");
            broken = m;
            for (auto& b : broken.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::HardwareLoopLeave) i.operands.push_back(IRValueId{1});
            reject(broken, "invalid hardware-loop leave");
            broken = m;
            for (auto& b : broken.functions[0].blocks) for (auto& i : b.instructions)
                if (i.opcode == IROpcode::HardwareLoop) i.loop_target = i.targets[0];
            // With PHI edge splitting, initial entry and R13 backedge are
            // distinct and cannot be silently substituted for one another.
            reject(broken, "backedge or exit");
        }, true);
        check("word f() { word x = 0; for (word i = 2; i > 0; i = i - 1) x += 3; return x; }", [](const IRModule& original, const IRModule&) {
            bool rejected = false;
            try { IRControlFlow cfg(original.functions[0]); }
            catch (const CompilerError& error) { rejected = error.getMessage().find("exposed hardware-loop") != std::string::npos; }
            require(rejected, "Liveness accepted a hidden hardware-loop backedge");
        }, true);
        std::string oversized = "word main() {";
        for (std::size_t i = 0; i < 1025; ++i) oversized += "word v" + std::to_string(i) + " = 0;";
        oversized += "return 0; }";
        bool bounded = false;
        try { check(oversized, [](const IRModule&, const IRModule&) {}); }
        catch (const CompilerError& error) { bounded = error.getMessage().find("SSA analysis resource limit") != std::string::npos; }
        require(bounded, "SSA candidate scan was not bounded");
        std::cout << "O2 CFG, SSA, liveness, allocation, loop and inlining checks passed\n";
    } catch (const CompilerError& e) { std::cerr << e.getMessage() << '\n'; return 1; }
      catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
