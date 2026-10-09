#include "IRCompactOptimizer.hpp"
#include "IRControlFlow.hpp"
#include "ConstantEvaluator.hpp"
#include <algorithm>
#include <set>

namespace {
using Definitions = std::vector<IRInstruction>;
Definitions definitions(const IRFunction& f) {
    Definitions d(static_cast<std::size_t>(f.value_count) + 1);
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.result.isValid()) d.at(i.result.value) = i;
    return d;
}
bool integer(const Type& t) {
    return !t.pointer_level && !t.array_size && (t.base == BaseType::WORD || t.base == BaseType::BYTE || t.base == BaseType::BOOL);
}

void initializers(IRFunction& f, const Analyzer::LocalSymbolTable& locals) {
    const auto d = definitions(f);
    std::set<std::uint32_t> disposable;
    std::size_t work = 0;
    for (auto& block : f.blocks) {
        auto& code = block.instructions;
        for (std::size_t first = 0; first < code.size(); ++first) {
            if (++work > 1000000) break;
            const auto& start = code[first];
            if (start.opcode != IROpcode::StoreIndirect || start.operation != "declare" || start.memory_volatile || !integer(start.type)) continue;
            const auto& address = d.at(start.operands[0].value);
            if (address.opcode != IROpcode::Address || address.operation != "member" || address.immediate != 0) continue;
            const auto root_id = address.operands[0];
            const auto& root = d.at(root_id.value);
            if (root.opcode != IROpcode::Address || !root.operation.empty() || root.type.pointer_level != 1 ||
                root.type.space != AddressSpace::RAM || isFarPointer(root.type)) continue;
            const auto local = locals.find(root.symbol_id);
            if (local == locals.end() || local->second.stackOffset >= 0 || local->second.type.is_volatile ||
                local->second.type.pointer_level || local->second.type.array_size < 4 ||
                local->second.type.base != start.type.base || local->second.type.alignment > 2) continue;
            const auto width = start.type.sizeInBytes;
            const auto extent = static_cast<std::int64_t>(local->second.type.array_size) * width;
            if (width < 1 || width > 2 || extent > f.total_local_alloc_size) continue;
            std::vector<std::uint16_t> values;
            std::vector<std::size_t> stores;
            for (std::size_t p = first; p < code.size() && p - first < 2048 && values.size() < 256; ++p) {
                if (++work > 1000000) break;
                const auto& i = code[p];
                if (i.opcode == IROpcode::StoreIndirect) {
                    const auto& a = d.at(i.operands[0].value);
                    const auto& v = d.at(i.operands[1].value);
                    if (i.operation != "declare" || i.memory_volatile || i.type.base != start.type.base ||
                        i.type.is_unsigned != start.type.is_unsigned || i.type.enum_name != start.type.enum_name ||
                        a.opcode != IROpcode::Address || a.operation != "member" || a.operands[0].value != root_id.value ||
                        a.immediate != static_cast<std::int64_t>(values.size()) * width ||
                        v.opcode != IROpcode::Constant || !integer(v.type) || a.immediate + width > extent) break;
                    values.push_back(static_cast<std::uint16_t>(ConstantEvaluator::convert(v.immediate, i.type)) &
                        static_cast<std::uint16_t>(width == 1 ? 255 : 65535));
                    stores.push_back(p);
                } else if (i.opcode == IROpcode::Constant && integer(i.type)) {
                    // Only side-effect-free value definitions may be crossed.
                } else if (i.opcode == IROpcode::Address && i.operation == "member" && i.operands[0].value == root_id.value &&
                    i.immediate >= 0 && i.immediate < extent) {
                } else break;
            }
            if (values.size() < 4) continue;
            IRInstruction batch = start;
            batch.opcode = IROpcode::MemoryInitialize; batch.operands = {root_id};
            batch.immediate = extent; batch.initialization_values = std::move(values);
            for (const auto p : stores) disposable.insert(code[p].operands[0].value);
            code[first] = std::move(batch);
            for (auto p = stores.rbegin(); p != stores.rend(); ++p) if (*p != first) code.erase(code.begin() + *p);
        }
    }
    // Erase only addresses whose in-object member checks were proven above.
    // Unknown pointer calculations still execute even when their result dies.
    std::vector<unsigned> uses(d.size());
    for (const auto& b : f.blocks) for (const auto& i : b.instructions) for (const auto v : i.operands) ++uses.at(v.value);
    for (auto& b : f.blocks) b.instructions.erase(std::remove_if(b.instructions.begin(), b.instructions.end(),
        [&](const IRInstruction& i) { return disposable.count(i.result.value) && !uses.at(i.result.value); }), b.instructions.end());
    std::vector<IRValueId> ids(d.size()); std::uint32_t next = 0;
    for (const auto& b : f.blocks) for (const auto& i : b.instructions) if (i.result.isValid()) ids.at(i.result.value) = IRValueId{++next};
    for (auto& b : f.blocks) for (auto& i : b.instructions) {
        for (auto& v : i.operands) v = ids.at(v.value);
        if (i.result.isValid()) i.result = ids.at(i.result.value);
    }
    f.value_count = next;
}

// A strict increasing word loop cannot wrap before reaching its invariant
// bound. The entry comparison is retained: LOOP with R12=0 would execute
// 65536 iterations, not zero. Carried values are merged on zero/completion
// paths rather than exposing the final body's SSA values to the zero path.
bool countedLoop(IRFunction& f, OptimizationLevel policy) {
    const IRControlFlow cfg(f);
    const auto d = definitions(f);
    std::size_t work = 0;
    for (const auto& loop : cfg.loops) {
        if (loop.blocks.size() < 2 || loop.blocks.size() > 3 || loop.latches.size() != 1) continue;
        const auto h = loop.header, latch = *loop.latches.begin();
        const auto& test = f.blocks.at(h).instructions.back();
        if (test.opcode != IROpcode::CondBranch || test.targets.size() != 2 || !loop.blocks.count(test.targets[0].value) ||
            loop.blocks.count(test.targets[1].value)) continue;
        const auto& cmp = d.at(test.operands[0].value);
        if (cmp.opcode != IROpcode::Binary || (cmp.operation != "<" && cmp.operation != "<=") || cmp.operands.size() != 2) continue;
        const auto& phi = d.at(cmp.operands[0].value);
        const auto& bound = d.at(cmp.operands[1].value);
        if (phi.opcode != IROpcode::Phi || phi.type.base != BaseType::WORD || phi.type.pointer_level || phi.operands.size() != 2 ||
            bound.type.base != BaseType::WORD || bound.type.pointer_level || bound.type.is_unsigned != phi.type.is_unsigned) continue;
        std::uint32_t pre = IRBlockId::Invalid; IRValueId initial, increment;
        for (std::size_t e = 0; e < 2; ++e) {
            if (phi.targets[e].value == latch) increment = phi.operands[e];
            else { pre = phi.targets[e].value; initial = phi.operands[e]; }
        }
        if (pre >= f.blocks.size() || loop.blocks.count(pre) || !increment.isValid() || !initial.isValid() ||
            cfg.predecessors.at(h).size() != 2) continue;
        const auto& entry = f.blocks[pre].instructions.back();
        const auto& first = d.at(initial.value); const auto& next = d.at(increment.value);
        if (entry.opcode != IROpcode::Branch || entry.targets[0].value != h ||
            next.opcode != IROpcode::Binary || next.operation != "+" || next.operands[0].value != phi.result.value ||
            d.at(next.operands[1].value).opcode != IROpcode::Constant || d.at(next.operands[1].value).immediate != 1) continue;
        std::vector<std::uint32_t> defining_block(d.size(), IRBlockId::Invalid);
        for (const auto& block : f.blocks) for (const auto& i : block.instructions)
            if (i.result.isValid()) defining_block.at(i.result.value) = block.id.value;
        if (loop.blocks.count(defining_block.at(bound.result.value))) continue;
        const bool inclusive = cmp.operation == "<=";
        const bool constant_count = first.opcode == IROpcode::Constant && bound.opcode == IROpcode::Constant;
        const auto begin = first.opcode == IROpcode::Constant ? ConstantEvaluator::convert(first.immediate, phi.type) : 0;
        const auto end = bound.opcode == IROpcode::Constant ? ConstantEvaluator::convert(bound.immediate, phi.type) : 0;
        const auto trips = end - begin + (inclusive ? 1 : 0);
        // <= at the type maximum is an infinite wrapping source loop. Never
        // silently turn it into a finite hardware count.
        if (inclusive && (bound.opcode != IROpcode::Constant || end >= (phi.type.is_unsigned ? 65535 : 32767))) continue;
        if (constant_count && (trips < 8 || trips > 65535)) continue;
        bool legal = true; std::size_t body_ops = 0;
        std::set<std::uint32_t> live_out;
        for (const auto b : loop.blocks) {
            for (const auto& i : f.blocks[b].instructions) {
                if (++work > 1000000) return false;
                if (i.opcode == IROpcode::Call || i.opcode == IROpcode::HardwareLoop ||
                    i.opcode == IROpcode::HardwareLoopEnd || i.opcode == IROpcode::HardwareLoopLeave) legal = false;
                if (b == h && i.opcode != IROpcode::Phi && i.opcode != IROpcode::Constant && i.opcode != IROpcode::Cache &&
                    i.result.value != cmp.result.value && !i.isTerminator()) legal = false;
                if (i.result.isValid()) for (const auto& outside : f.blocks) if (!loop.blocks.count(outside.id.value))
                    for (const auto& consumer : outside.instructions) for (const auto v : consumer.operands)
                        { if (++work > 1000000) return false; if (v.value == i.result.value) {
                            if (b == h && i.opcode == IROpcode::Phi) live_out.insert(i.result.value);
                            else if (b != h || i.opcode != IROpcode::Constant) legal = false;
                        } }
                if (b != h && !i.isTerminator()) ++body_ops;
            }
            if (b != h && (cfg.successors[b].size() != 1 || !loop.blocks.count(cfg.successors[b][0]))) legal = false;
            if (b != h) for (const auto p : cfg.predecessors[b]) if (!loop.blocks.count(p)) legal = false;
        }
        // Scope saves cost bytes. Size mode rerolls only when removing the
        // induction variable and its compare pays for that fixed overhead.
        bool induction_used = live_out.count(phi.result.value) != 0;
        for (const auto b : loop.blocks) for (const auto& i : f.blocks[b].instructions)
            if (i.result.value != cmp.result.value && i.result.value != next.result.value)
                for (const auto v : i.operands) if (v.value == phi.result.value) induction_used = true;
        if (!legal || (policy == OptimizationLevel::Size && (induction_used || body_ops > 16))) continue;
        std::uint32_t id = 1;
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) id = std::max(id, i.loop_id + 1);
        const auto exit = test.targets[1]; const auto source = test.source;
        const auto setup_id = IRBlockId{static_cast<std::uint32_t>(f.blocks.size())};
        const auto leave_id = IRBlockId{setup_id.value + 1}, merge_id = IRBlockId{setup_id.value + 2};
        if (f.value_count >= 99900 || f.blocks.size() > 8188) return false;
        IRBasicBlock setup_block; setup_block.id = setup_id; setup_block.label = "compact.loop.setup";
        Type count_type; count_type.base = BaseType::WORD; count_type.sizeInBytes = 2; count_type.is_unsigned = true;
        IRInstruction count; count.type = count_type; count.source = source; count.in_plot_context = test.in_plot_context;
        if (constant_count) { count.opcode = IROpcode::Constant; count.immediate = trips; }
        else {
            // Modular subtraction is exact for every taken signed/unsigned
            // strict comparison: its mathematical distance is 1..65535.
            IRInstruction start_cast = count; start_cast.opcode = IROpcode::Cast; start_cast.operands = {initial};
            start_cast.result = IRValueId{++f.value_count}; setup_block.instructions.push_back(start_cast);
            IRInstruction end_cast = start_cast; end_cast.operands = {bound.result};
            end_cast.result = IRValueId{++f.value_count}; setup_block.instructions.push_back(end_cast);
            count.opcode = IROpcode::Binary; count.operation = "-"; count.operands = {end_cast.result, start_cast.result};
        }
        count.result = IRValueId{++f.value_count}; setup_block.instructions.push_back(count);
        if (inclusive && !constant_count) {
            IRInstruction one = count; one.opcode = IROpcode::Constant; one.operation.clear(); one.operands.clear(); one.immediate = 1;
            one.result = IRValueId{++f.value_count}; setup_block.instructions.push_back(one);
            IRInstruction plus = count; plus.operation = "+"; plus.operands = {count.result, one.result};
            plus.result = IRValueId{++f.value_count}; setup_block.instructions.push_back(plus); count = plus;
        }
        IRInstruction setup; setup.opcode = IROpcode::HardwareLoop; setup.operands = {count.result};
        setup.targets = {IRBlockId{h}}; setup.loop_target = IRBlockId{h}; setup.loop_id = id;
        setup.source = source; setup.in_plot_context = test.in_plot_context; setup.compiler_generated_loop = true;
        setup_block.instructions.push_back(setup);
        IRInstruction guard = cmp; guard.operands[0] = initial; guard.result = IRValueId{++f.value_count};
        IRInstruction guard_branch = test; guard_branch.operands = {guard.result}; guard_branch.targets = {setup_id, merge_id};
        auto& pre_code = f.blocks[pre].instructions;
        pre_code.pop_back();
        for (const auto& i : f.blocks[h].instructions) if (i.opcode == IROpcode::Constant) pre_code.push_back(i);
        pre_code.push_back(guard); pre_code.push_back(guard_branch);
        IRInstruction branch; branch.opcode = IROpcode::Branch; branch.targets = {test.targets[0]}; branch.source = source;
        branch.in_plot_context = test.in_plot_context;
        f.blocks[h].instructions.back() = branch;
        for (auto& i : f.blocks[h].instructions) if (i.opcode == IROpcode::Phi)
            for (auto& target : i.targets) if (target.value == pre) target = setup_id;
        IRInstruction finish; finish.opcode = IROpcode::HardwareLoopEnd;
        finish.loop_id = id; finish.targets = {IRBlockId{h}, leave_id}; finish.source = source;
        finish.in_plot_context = test.in_plot_context;
        f.blocks[latch].instructions.back() = finish;
        IRBasicBlock leave; leave.id = leave_id; leave.label = "compact.loop.leave";
        IRInstruction restore; restore.opcode = IROpcode::HardwareLoopLeave; restore.loop_id = id; restore.source = source;
        restore.in_plot_context = test.in_plot_context;
        branch.targets = {merge_id}; leave.instructions = {restore, branch};
        IRBasicBlock merge; merge.id = merge_id; merge.label = "compact.loop.exit";
        std::map<std::uint32_t, IRValueId> replacements;
        for (const auto& carried : f.blocks[h].instructions) if (carried.opcode == IROpcode::Phi && live_out.count(carried.result.value)) {
            IRInstruction joined = carried; joined.targets = {IRBlockId{pre}, leave_id};
            for (std::size_t edge = 0; edge < carried.targets.size(); ++edge)
                joined.operands[carried.targets[edge].value == latch ? 1 : 0] = carried.operands[edge];
            joined.result = IRValueId{++f.value_count}; replacements.emplace(carried.result.value, joined.result);
            merge.instructions.push_back(std::move(joined));
        }
        branch.targets = {exit}; merge.instructions.push_back(branch);
        for (auto& outside : f.blocks) if (!loop.blocks.count(outside.id.value)) for (auto& i : outside.instructions) {
            for (auto& operand : i.operands) {
                const auto found = replacements.find(operand.value);
                if (found != replacements.end()) operand = found->second;
            }
            if (outside.id.value == exit.value && i.opcode == IROpcode::Phi)
                for (auto& target : i.targets) if (target.value == h) target = merge_id;
        }
        const auto induction = phi.result.value, update = next.result.value, condition = cmp.result.value;
        for (const auto b : loop.blocks) {
            auto& code = f.blocks[b].instructions;
            code.erase(std::remove_if(code.begin(), code.end(), [&](const IRInstruction& i) {
                return (b == h && i.opcode == IROpcode::Constant) ||
                    (i.result.isValid() && (i.result.value == condition || (!induction_used &&
                    (i.result.value == induction || i.result.value == update))));
            }), code.end());
        }
        f.blocks.push_back(std::move(setup_block)); f.blocks.push_back(std::move(leave)); f.blocks.push_back(std::move(merge));
        // CACHE at setup must precede the hot body in physical layout. Merely
        // appending setup can place its cache window after the LOOP target.
        std::vector<IRBlockId> block_ids(f.blocks.size());
        std::vector<std::uint32_t> order;
        for (std::uint32_t old = 0; old < setup_id.value; ++old) {
            if (old == h) order.push_back(setup_id.value);
            order.push_back(old);
        }
        order.push_back(leave_id.value); order.push_back(merge_id.value);
        for (std::size_t index = 0; index < order.size(); ++index)
            block_ids.at(order[index]) = IRBlockId{static_cast<std::uint32_t>(index)};
        std::vector<IRBasicBlock> ordered;
        for (const auto old : order) {
            auto block = std::move(f.blocks.at(old)); block.id = block_ids.at(old);
            for (auto& i : block.instructions) {
                for (auto& target : i.targets) target = block_ids.at(target.value);
                if (i.loop_target.isValid()) i.loop_target = block_ids.at(i.loop_target.value);
            }
            ordered.push_back(std::move(block));
        }
        f.entry = block_ids.at(f.entry.value); f.blocks = std::move(ordered);
        // Renumber value IDs before publishing the verified CFG.
        std::vector<IRValueId> ids(static_cast<std::size_t>(f.value_count) + 1); std::uint32_t number = 0;
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) if (i.result.isValid()) ids[i.result.value] = IRValueId{++number};
        for (auto& b : f.blocks) for (auto& i : b.instructions) {
            for (auto& v : i.operands) v = ids.at(v.value);
            if (i.result.isValid()) i.result = ids.at(i.result.value);
        }
        f.value_count = number;
        return true;
    }
    return false;
}
}

void IRCompactOptimizer::run(IRModule& module,
    const std::map<std::string, Analyzer::LocalSymbolTable>& locals, OptimizationLevel policy) {
    IRVerifier::verify(module);
    if (module.target != TargetKind::GSU) return;
    for (auto& f : module.functions) {
        const auto symbols = locals.find(f.name);
        if (symbols != locals.end()) initializers(f, symbols->second);
        if (f.blocks.size() <= 256 && f.value_count <= 4096)
            for (unsigned round = 0; round < 16 && countedLoop(f, policy); ++round) {}
    }
    IRVerifier::verify(module);
}
