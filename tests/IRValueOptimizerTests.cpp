#include "IRGlobalOptimizer.hpp"
#include "IRValueOptimizer.hpp"
#include "IRControlFlow.hpp"
#include "LinearScanAllocator.hpp"
#include "GSUCostModel.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Optimizer.hpp"
#include "ConstantEvaluator.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::size_t count(const IRModule& m, IROpcode op, const std::string& name = "") {
    std::size_t n = 0;
    for (const auto& f : m.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        n += i.opcode == op && (name.empty() || i.operation == name);
    return n;
}
void allocation(const IRFunction& f) {
    IRControlFlow cfg(f); LinearScanAllocator allocator; allocator.runGlobal(f, {5,7,8});
    const auto disjoint = [&](const std::set<std::uint32_t>& live) {
        std::set<std::uint8_t> registers; std::set<int> slots;
        for (const auto v : live) {
            const auto* place = allocator.find(IRValueId{v});
            require(place != nullptr, "Missing allocation");
            if (place->has_register) require(registers.insert(place->physical_register).second, "Overlapping register lifetimes");
            if (place->spill_slot >= 0) require(slots.insert(place->spill_slot).second, "Overlapping spill lifetimes");
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
}
template<class Check> void check(const std::string& source, Check test, bool hardware = false, bool misalign = false,
                                IRCompactionPolicy compaction = IRCompactionPolicy::Enabled) {
    Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto ast = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast);
    if (hardware) { Optimizer optimizer; optimizer.optimize(ast); }
    IRLowerer lowerer; auto original = lowerer.lower(ast);
    if (misalign) {
        bool changed = false;
        for (auto& f : original.functions) for (auto& b : f.blocks) for (auto& i : b.instructions)
            if (i.opcode == IROpcode::Address && i.operation == "member" && i.immediate == 2) { i.immediate = 1; changed = true; }
        require(changed, "Missing packed-address negative probe"); IRVerifier::verify(original);
    }
    auto optimized = original;
    IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols(), OptimizationLevel::O2, compaction);
    test(original, optimized);
    for (const auto& f : optimized.functions) allocation(f);
    const auto first = dumpIR(optimized); IRGlobalOptimizer::run(optimized, analyzer.getAllLocalSymbols(), OptimizationLevel::O2, compaction);
    if (first != dumpIR(optimized)) throw std::runtime_error("Value/global pipeline not idempotent\n" + first + "\n" + dumpIR(optimized));
}
bool returnedConstant(const IRModule& m, std::int64_t expected) {
    const auto& f = m.functions.back();
    for (const auto& b : f.blocks) if (b.instructions.back().opcode == IROpcode::Return) {
        const auto v = b.instructions.back().operands.front();
        for (const auto& block : f.blocks) for (const auto& i : block.instructions)
            if (i.result.value == v.value && i.opcode == IROpcode::Constant && i.immediate == expected) return true;
    }
    return false;
}
using OrderedStores = std::vector<std::pair<std::int64_t, std::int64_t>>;
void costSafeRefusal(const IRModule& module) {
    const auto& f = module.functions[0]; IRControlFlow cfg(f);
    std::vector<IRInstruction> defs(static_cast<std::size_t>(f.value_count) + 1);
    IRValueId address, value;
    for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
        if (i.result.isValid()) defs.at(i.result.value) = i;
        if (i.opcode == IROpcode::Address && i.symbol == "a") address = i.result;
    }
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.opcode == IROpcode::LoadIndirect && i.operands[0].value == address.value) value = i.result;
    require(value.isValid(), "Missing lifetime input for cost-refusal probe");
    bool exercised = false;
    for (const auto& loop : cfg.loops) {
        unsigned uses = 0;
        for (const auto b : loop.blocks) for (const auto& i : f.blocks[b].instructions)
            for (const auto v : i.operands) uses += v.value == value.value;
        if (uses < 3) continue;
        std::vector<std::uint32_t> outside;
        for (const auto p : cfg.predecessors[loop.header]) if (!loop.blocks.count(p)) outside.push_back(p);
        require(outside.size() == 1 && cfg.successors[outside[0]].size() == 1, "Cost-refusal loop has no unique preheader");
        auto trial = module;
        IRInstruction copy; copy.opcode = IROpcode::Cast; copy.type = defs[value.value].type;
        copy.source = defs[value.value].source; copy.operands = {value}; copy.is_live_range_split = true;
        copy.result = IRValueId{++trial.functions[0].value_count};
        auto& preheader = trial.functions[0].blocks[outside[0]].instructions;
        preheader.insert(preheader.end() - 1, copy);
        for (const auto b : loop.blocks) for (auto& i : trial.functions[0].blocks[b].instructions)
            for (auto& v : i.operands) if (v.value == value.value) v = copy.result;
        IRVerifier::verify(trial);
        LinearScanAllocator before, after;
        before.runGlobal(f, {5,7,8}); after.runGlobal(trial.functions[0], {5,7,8});
        const auto old_cost = GSUCostModel::allocation(f, before, cfg).pressureScore();
        const auto new_cost = GSUCostModel::allocation(trial.functions[0], after, cfg).pressureScore();
        const auto* hot = after.find(copy.result);
        require(hot && (!hot->has_register || new_cost >= old_cost), "Lifetime refusal fixture no longer exercises a losing split");
        for (const auto& b : f.blocks) for (const auto& i : b.instructions)
            require(!(i.is_live_range_split && i.operands[0].value == value.value), "Optimizer retained a losing lifetime split");
        exercised = true;
    }
    require(exercised, "Cost-safe lifetime refusal was not exercised");
}
std::pair<std::int64_t, OrderedStores> lifetimeTrace(const IRFunction& f, const std::map<std::string, std::int64_t>& parameters) {
    std::vector<std::int64_t> values(static_cast<std::size_t>(f.value_count) + 1);
    std::map<std::int64_t, std::int64_t> memory;
    OrderedStores stores;
    struct HardwareCount { std::uint32_t id; std::uint16_t remaining; };
    std::vector<HardwareCount> loops;
    auto block = f.entry; IRBlockId previous;
    for (std::size_t steps = 0; steps < 10000; ++steps) {
        const auto incoming = values; bool branched = false;
        for (const auto& i : f.blocks.at(block.value).instructions) {
            const auto operand = [&](std::size_t n) { return values.at(i.operands.at(n).value); };
            auto& result = values.at(i.result.value);
            switch (i.opcode) {
                case IROpcode::Constant: result = ConstantEvaluator::convert(i.immediate, i.type); break;
                case IROpcode::Address: {
                    require(i.operation.empty(), "Unexpected aggregate address in lifetime trace");
                    result = 0x10000 + static_cast<std::int64_t>(i.symbol_id.value) * 2;
                    const auto parameter = parameters.find(i.symbol);
                    if (parameter != parameters.end()) memory[result] = parameter->second;
                    break;
                }
                case IROpcode::LoadIndirect: result = memory.at(operand(0)); break;
                case IROpcode::StoreIndirect:
                    memory[operand(0)] = operand(1);
                    if (i.memory_volatile) stores.emplace_back(operand(0), operand(1));
                    break;
                case IROpcode::Cast:
                    result = i.type.pointer_level ? static_cast<std::uint16_t>(operand(0)) : ConstantEvaluator::convert(operand(0), i.type);
                    break;
                case IROpcode::Binary:
                    if (i.operation == "+") result = ConstantEvaluator::convert(operand(0) + operand(1), i.type);
                    else if (i.operation == "<") result = operand(0) < operand(1);
                    else throw std::runtime_error("Unexpected arithmetic in lifetime trace");
                    break;
                case IROpcode::Phi: {
                    bool found = false;
                    for (std::size_t p = 0; p < i.targets.size(); ++p) if (i.targets[p].value == previous.value) {
                        result = incoming.at(i.operands[p].value); found = true; break;
                    }
                    require(found, "Lifetime PHI has no incoming predecessor"); break;
                }
                case IROpcode::HardwareLoop:
                    require(operand(0) > 0, "Zero-trip hardware LOOP was entered");
                    loops.push_back({i.loop_id, static_cast<std::uint16_t>(operand(0))});
                    previous = block; block = i.targets.at(0); branched = true; break;
                case IROpcode::HardwareLoopEnd:
                    require(!loops.empty() && loops.back().id == i.loop_id, "Lifetime LOOP scope is mismatched");
                    --loops.back().remaining; previous = block;
                    block = i.targets.at(loops.back().remaining ? 0 : 1); branched = true; break;
                case IROpcode::HardwareLoopLeave:
                    require(!loops.empty() && loops.back().id == i.loop_id, "Lifetime LOOP restore is mismatched");
                    loops.pop_back(); break;
                case IROpcode::Cache: break;
                case IROpcode::Branch:
                case IROpcode::CondBranch:
                    previous = block;
                    block = i.targets.at(i.opcode == IROpcode::CondBranch && !operand(0) ? 1 : 0);
                    branched = true; break;
                case IROpcode::Return:
                    require(loops.empty(), "Lifetime trace returned without restoring LOOP state");
                    return {operand(0), std::move(stores)};
                default: throw std::runtime_error("Unsupported instruction in lifetime trace");
            }
            if (branched) break;
        }
        require(branched, "Lifetime trace did not return or branch");
    }
    throw std::runtime_error("Lifetime trace exceeded its execution budget");
}
void reject(const IRModule& m) {
    try { IRVerifier::verify(m); }
    catch (const CompilerError& e) { require(e.getMessage().find("live-range split") != std::string::npos, "Wrong split diagnostic"); return; }
    throw std::runtime_error("Invalid live-range split accepted");
}
}

int main() {
    try {
        check("word f(word x,word y){word a=x*y;*(volatile word*)0x120=a; if(x)return y*x+a; return x*y-a;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::Binary,"*")==1,"GVN missed dominating/commutative expression"); });
        check("word f(word x,word y){if(x)return y*3;return y*3;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::Binary,"*")==2,"GVN reused sibling/non-dominating value"); });
        check("word f(word x,word y){word a=x/y;if(x)return x/y+a;return a;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::Binary,"/")==2,"GVN speculated/coalesced division"); });
        check("word f(word x){word a=*(volatile word*)0x120; if(x)a+=*(volatile word*)0x120; return a;}",
            [](const IRModule&, const IRModule& m) {
                unsigned reads=0; for(const auto& b:m.functions[0].blocks)for(const auto& i:b.instructions)reads+=i.opcode==IROpcode::LoadIndirect&&i.memory_volatile;
                require(reads==2,"GVN merged volatile reads");
            });
        check("word f(word* p,word* q){bool equal=p==q;if((word)equal==1)return 42;return 9;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::CondBranch)==1,"Pointer comparison invented scalar facts"); });
        check("struct Pair {word a;word b;}; word f(word x){struct Pair p={x,x+1};if(x){p.a+=3;p.b+=5;}return p.a+p.b;}",
            [](const IRModule&, const IRModule& m) {
                require(count(m,IROpcode::StoreIndirect)==0&&count(m,IROpcode::LoadIndirect)==1,"Struct cells retained local memory");
                require(count(m,IROpcode::Phi)==2,"Struct fields shared a PHI/rename stack");
                require(m.functions[0].total_local_alloc_size==0,"Dead aggregate frame retained");
            });
        check("word f(word x){word a[3]={x,x+1,x+2};if(x)a[1]+=5;return a[0]+a[1]+a[2];}",
            [](const IRModule&, const IRModule& m) {
                require(count(m,IROpcode::StoreIndirect)==0&&count(m,IROpcode::LoadIndirect)==1,"Constant array cells not scalarized");
                require(count(m,IROpcode::PointerOffset)==0,"Dead proven array path retained");
            });
        check("word f(word x){word i=1;word a[2]={x,x+2};return a[i]+a[(word)0];}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::StoreIndirect)==0&&count(m,IROpcode::LoadIndirect)==1,"Constant index exposed by SSA was not scalarized"); });
        check("word f(word x){word a[2]={0,1};word b[2]={x,x+2};word c[2]={x,x+3};return c[b[a[1]]&0]+b[0];}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::StoreIndirect)==0&&count(m,IROpcode::LoadIndirect)==1,"Nested scalar-cell constant chain did not converge"); });
        check("struct P{byte a;word b;}; word f(word x){struct P a[2]={{1,x},{2,x+3}};return a[0].b+a[1].b;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::StoreIndirect)==0,"Nested static subobjects not scalarized"); });
        check("word f(word x){word a[3]={1,2,3};return a[x&1];}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::LoadIndirect)>=2,"Dynamic aggregate was scalarized"); });
        check("void escape(word* p);word f(word x){word a[2]={x,2};word* p=&a[0];escape(p);*p+=4;return a[0];}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::StoreIndirect)>=3,"Escaped subobject was scalarized"); });
        check("word f(word x){word a[2];a[0]=x;return a[0]+a[1];}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::LoadIndirect)>=2,"Uninitialized array element invented"); });
        check("@packed struct P{word a;word b;};word f(word x){struct P p={1,x};return p.b;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::StoreIndirect)==2,"Misaligned packed word was scalarized"); }, false, true);
        check("struct P{volatile word a;word b;};word f(word x){struct P p={x,2};return p.a+p.b;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::StoreIndirect)==2,"Volatile aggregate root was scalarized"); });

        check("u16 f(u16 x){u16 y=x&(u16)63; if(y>(u16)63)return (u16)99;return (y&(u16)255);}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::CondBranch)==0&&count(m,IROpcode::Binary,"&")==1,"Mask/range proof missed"); });
        check("u16 f(u16 x){u16 y=(x&(u16)63)|(u16)256;return y&(u16)256;}",
            [](const IRModule&, const IRModule& m) { require(returnedConstant(m,256),"Known-one bit not folded"); });
        check("bool f(word x){word y=x&63;return y>=0 && y<64;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::CondBranch)==0 && count(m,IROpcode::Binary,">=")==0 && count(m,IROpcode::Binary,"<")==0,"Signed nonnegative interval not proved"); });
        check("u16 f(u16 x){u16 y=x+(u16)1;if(y>x)return y;return x;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::CondBranch)==1,"Unsigned wrap interval was treated as monotonic"); });
        check("bool f(word x){word y=x+1;return y>x;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::Binary,">")==1,"Signed wrap interval was treated as monotonic"); });
        check("word f(word x){byte y=(byte)x;return (word)y;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::Cast)>=1,"Narrowed unknown signed bits were invented"); });
        check("bool f(word x){u8 y=(u8)x;return (u16)y <= (u16)255;}",
            [](const IRModule&, const IRModule& m) { require(returnedConstant(m,1),"Unsigned byte zero-extension range lost"); });
        check("bool f(word x){word y=x | (word)0x8000;return (y>>3)<0;}",
            [](const IRModule&, const IRModule& m) { require(returnedConstant(m,1),"Arithmetic shift lost known sign bits"); });

        check("word f(word start,word n,word k){word sum=0;for(word i=start;i!=n;i++)sum+=(i+3)*k;return sum;}",
            [](const IRModule&, const IRModule& m) {
                IRControlFlow cfg(m.functions[0]);
                for(const auto& loop:cfg.loops)for(const auto b:loop.blocks)for(const auto& i:m.functions[0].blocks[b].instructions)
                    require(!(i.opcode==IROpcode::Binary&&i.operation=="*"),"Numeric recurrence retained loop multiplication");
                require(count(m,IROpcode::Phi)>=3,"Derived modular recurrence missing");
            });
        check("u16 f(u16 start,u16 n,u16 k){u16 sum=(u16)0;for(u16 i=start;i!=n;i-=(u16)2)sum+=i*k;return sum;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::Phi)>=3,"Unsigned subtracting recurrence missing"); });

        const std::string lifetime_source = "word f(word a,word b,word c,word d,word n){word sum=0;for(word j=0;j<n;j++){for(word i=0;i<n;i++){*(volatile word*)0x120=b;*(volatile word*)0x122=c;*(volatile word*)0x124=d;*(volatile word*)0x126=b+c+d;}}for(word k=0;k<n;k++){*(volatile word*)0x128=a;*(volatile word*)0x12a=a;sum+=a;}return sum+a+b+c+d;}";
        // Test profitable lifetime splitting independently of LOOP compaction:
        // removing induction values can legitimately make this split costlier.
        // Keep the split and invalid-split assertions, plus allocation and
        // idempotence checks, on the uncompressed pressure-sensitive fixture.
        check(lifetime_source,
            [](const IRModule&, const IRModule& m) {
                const IRInstruction* split=nullptr;
                for(const auto& b:m.functions[0].blocks)for(const auto& i:b.instructions)if(i.is_live_range_split)split=&i;
                require(split!=nullptr,"Hot/cold lifetime was not split");
                auto bad=m;
                for(auto& b:bad.functions[0].blocks)for(auto& i:b.instructions)if(i.is_live_range_split){i.type.is_unsigned=!i.type.is_unsigned;break;}
                reject(bad);
            }, false, false, IRCompactionPolicy::Disabled);
        check(lifetime_source, [](const IRModule& original, const IRModule& m) {
            require(count(m, IROpcode::HardwareLoop) >= 2, "Lifetime integration missed dynamic LOOP compaction");
            costSafeRefusal(m);
            for (const auto n : {-1, 0, 1, 3}) {
                const std::map<std::string, std::int64_t> inputs{{"a",32760},{"b",-10},{"c",100},{"d",-32750},{"n",n}};
                require(lifetimeTrace(original.functions[0], inputs) == lifetimeTrace(m.functions[0], inputs),
                        "Combined LOOP/lifetime optimization changed wrap results or ordered volatile stores");
            }
        });
        check("bitmap s{mode bitmap;size 256x128;depth 2bpp;base 0x4000;}word main(){use bitmap s;plot{at(0,0);for(word i=3;i>0;i--){color cursor.x;pixel;}flush;}return 42;}",
            [](const IRModule&, const IRModule& m) { require(count(m,IROpcode::Plot)==1&&count(m,IROpcode::Rpix)==1,"Hardware state operations were erased"); }, true);
        std::cout << "GVN, scalar cells, modular recurrences, known bits/ranges and hot lifetimes passed.\n";
        return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
