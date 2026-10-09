#include "GSUCostModel.hpp"
#include "GSUSpillCache.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "IRGlobalOptimizer.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void allocationPoints(const IRFunction& f) {
    LinearScanAllocator allocator;
    allocator.runGlobal(f, {5,7,8});
    const auto first = allocator.locations();
    allocator.runGlobal(f, {5,7,8});
    for (const auto& e : first) {
        const auto* next = allocator.find(IRValueId{e.first});
        require(next && next->has_register == e.second.has_register && next->physical_register == e.second.physical_register && next->spill_slot == e.second.spill_slot, "Allocation is nondeterministic");
    }
    IRControlFlow cfg(f);
    std::size_t end = 0;
    for (const auto& b : f.blocks) {
        end += b.instructions.size();
        auto live = cfg.live_out.at(b.id.value); auto point = end;
        for (auto i = b.instructions.rbegin(); i != b.instructions.rend(); ++i) {
            --point;
            if (i->opcode != IROpcode::Phi) for (const auto v : i->operands) live.insert(v.value);
            if (i->result.isValid()) live.insert(i->result.value);
            const auto spare = allocator.spareRegisters(point);
            for (const auto reg : spare) {
                require(reg == 5 || reg == 7 || reg == 8, "Special GSU register was loaned");
                for (const auto v : live) {
                    const auto* location = allocator.find(IRValueId{v});
                    require(!location || !location->has_register || location->physical_register != reg, "Spare register overlaps a live value/result/PHI edge");
                }
            }
            if (i->result.isValid()) live.erase(i->result.value);
        }
    }
    const auto cost = GSUCostModel::allocation(f, allocator, cfg);
    require(cost.pressureScore(5) >= cost.pressureScore(), "Uncached fetch should not be cheaper");
    allocator.runEager(f, {5,7,8});
    require(allocator.spareRegisters(0).empty(), "O1 inherited O2 point liveness");
}
}
int main() {
    try {
        require(GSUCostModel::literalBytes(127)==2 && GSUCostModel::literalBytes(128)==3 &&
                GSUCostModel::literalBytes(0xff80)==2 && GSUCostModel::literalBytes(0xff7f)==3, "IBT signed extension cost is wrong");
        require(GSUCostModel::spillLoad(-128).fetch_bytes==6 && GSUCostModel::spillLoad(-130).fetch_bytes==7, "Spill address materialization boundary is wrong");
        require(GSUCostModel::spillStore(-128).fetch_bytes == 6 &&
                GSUCostModel::spillStore(-130).fetch_bytes == 7 &&
                GSUCostModel::spillStore(-128, true).fetch_bytes == 15,
                "Spill cost does not match direct scalar/far pair emission");
        require(GSUCostModel::spillLoad(-2,true).ram_bytes==4 && GSUCostModel::spillStore(-2).ram_bytes==2, "Scalar/far spill width is wrong");
        require(GSUCostModel::preserveCall().fetch_bytes==8 && GSUCostModel::preserveCall().ram_bytes==4, "Call-preservation traffic is wrong");
        require(GSUCostModel::blockWeight(0)==1 && GSUCostModel::blockWeight(1)==64 && GSUCostModel::blockWeight(5)==GSUCostModel::blockWeight(3), "Loop weights are not bounded");
        GSUCost cost; cost.add({2,4},3);
        require(cost.fetch_bytes==6 && cost.ram_bytes==12 && cost.pressureScore()==66 && cost.pressureScore(5)==90, "Weighted cost lost fetch/RAM traffic");
        cost.add({std::numeric_limits<std::uint64_t>::max(),0},2);
        require(cost.fetch_bytes==std::numeric_limits<std::uint64_t>::max() && cost.pressureScore(5)==std::numeric_limits<std::uint64_t>::max(), "Cost arithmetic wrapped");
        GSUSpillCache cache;
        cache.enter({1,2,5,7}); cache.record(1,IRValueId{1});
        require(cache.find(IRValueId{1})<0, "Cursor register cached a spill");
        cache.record(5,IRValueId{1}); cache.record(7,IRValueId{2});
        require(cache.find(IRValueId{1})==5 && cache.choose({{1,1},{2,1}})<0, "Live copy was evicted");
        require(cache.choose({{1,0},{2,1}})==5, "Dead copy is not reusable");
        cache.enter({7}); require(cache.find(IRValueId{1})<0 && cache.find(IRValueId{2})==7, "Point liveness did not revoke copy");
        cache.clobber(7); require(cache.find(IRValueId{2})<0, "Register write retained a stale copy");
        cache.record(7,IRValueId{3}); cache.clear(); require(cache.find(IRValueId{3})<0, "Call/label fence retained copy");
        const std::string source = "void tick(word x){*(volatile word*)0x180=x;} word f(word a,word b,word c,word d){word sum=0; for(word i=0;i<8;i++){ if(i&1){tick(a);sum+=b+c;}else{tick(b);sum+=c+d;} }return sum+a+b+c+d;} word main(){return f(1,2,3,4);}";
        Lexer lexer(source); const auto tokens=lexer.scanTokens(); Parser parser(tokens); auto ast=parser.parseProgram();
        DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(ast);
        IRLowerer lowerer; auto module=lowerer.lower(ast); IRGlobalOptimizer::run(module,analyzer.getAllLocalSymbols());
        for (const auto& f : module.functions) allocationPoints(f);
        std::cout << "GSU cost, point liveness and spill-copy contracts passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
