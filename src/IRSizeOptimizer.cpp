#include "IRSizeOptimizer.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace {
bool sameType(const Type& a, const Type& b) {
    return a.base == b.base && a.structName == b.structName && a.sizeInBytes == b.sizeInBytes &&
        a.is_unsigned == b.is_unsigned && a.pointer_level == b.pointer_level && a.is_far == b.is_far &&
        a.array_size == b.array_size && a.space == b.space && a.pointer_reach == b.pointer_reach &&
        a.pointer_spaces == b.pointer_spaces && a.is_const == b.is_const && a.is_volatile == b.is_volatile &&
        a.pointee_qualifiers == b.pointee_qualifiers && a.alignment == b.alignment &&
        a.aggregate_size == b.aggregate_size && a.enum_name == b.enum_name;
}
bool scalarConstant(const IRInstruction& i) {
    return i.opcode == IROpcode::Constant && !i.type.pointer_level && !i.type.array_size &&
        (i.type.base == BaseType::BYTE || i.type.base == BaseType::WORD || i.type.base == BaseType::BOOL);
}
bool eligible(const IRInstruction& i) {
    if (scalarConstant(i)) return true;
    if (i.producesValue()) return false;
    switch (i.opcode) {
        case IROpcode::Call: case IROpcode::Store: case IROpcode::StoreIndirect:
        case IROpcode::PlotCoordinateWrite: case IROpcode::Plot: case IROpcode::SetColor:
        case IROpcode::CMode: case IROpcode::Rpix: return true;
        default: return false;
    }
}
void share(IRFunction& f) {
    if (f.blocks.size() > 256 || f.value_count > 16000) return;
    // Hardware scope markers carry save/restore lifetime information. Do not
    // change that graph until a scope-aware suffix proof exists.
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.opcode == IROpcode::HardwareLoop || i.opcode == IROpcode::HardwareLoopEnd ||
            i.opcode == IROpcode::HardwareLoopLeave) return;
    std::size_t work = 0, growth = 0;
    for (unsigned round = 0; round < 16; ++round) {
        std::vector<IRInstruction> defs(static_cast<std::size_t>(f.value_count) + 1);
        for (const auto& b : f.blocks) for (const auto& i : b.instructions)
            if (i.result.isValid()) defs.at(i.result.value) = i;
        const auto equal = [&](const IRInstruction& a, const IRInstruction& b) {
            if (a.opcode != b.opcode || !sameType(a.type, b.type) || a.targets.size() != b.targets.size() ||
                a.loop_target.value != b.loop_target.value || a.loop_id != b.loop_id || a.case_values != b.case_values ||
                a.has_default_target != b.has_default_target || a.immediate != b.immediate ||
                a.operation != b.operation || a.symbol != b.symbol || a.symbol_id.value != b.symbol_id.value ||
                a.memory_volatile != b.memory_volatile || a.in_plot_context != b.in_plot_context ||
                a.is_live_range_split != b.is_live_range_split || a.operands.size() != b.operands.size()) return false;
            for (std::size_t n = 0; n < a.targets.size(); ++n) if (a.targets[n].value != b.targets[n].value) return false;
            for (std::size_t n = 0; n < a.operands.size(); ++n) {
                if (a.operands[n].value == b.operands[n].value) continue;
                const auto& x = defs.at(a.operands[n].value); const auto& y = defs.at(b.operands[n].value);
                if (!scalarConstant(x) || !scalarConstant(y) || x.immediate != y.immediate || !sameType(x.type, y.type)) return false;
            }
            return true;
        };
        bool changed = false;
        for (std::size_t a = 0; a < f.blocks.size() && !changed; ++a) {
            const auto& first = f.blocks[a].instructions;
            const auto& terminal = first.back();
            if (terminal.opcode != IROpcode::Return && terminal.opcode != IROpcode::ReturnVoid && terminal.opcode != IROpcode::Branch) continue;
            if (terminal.opcode == IROpcode::Branch &&
                f.blocks.at(terminal.targets.at(0).value).instructions.front().opcode == IROpcode::Phi) continue;
            for (std::size_t b = a + 1; b < f.blocks.size(); ++b) {
                if (++work > 1000000 || growth >= 256) return;
                const auto& second = f.blocks[b].instructions;
                if (!equal(terminal, second.back())) continue;
                std::size_t count = 0;
                bool effect = false;
                while (count < 16 && count + 1 < first.size() && count + 1 < second.size()) {
                    if (++work > 1000000) return;
                    const auto& x = first[first.size() - 2 - count];
                    const auto& y = second[second.size() - 2 - count];
                    if (!eligible(x) || !eligible(y) || !equal(x, y)) break;
                    effect = effect || !scalarConstant(x); ++count;
                }
                if (!effect) continue;
                const auto start_a = first.size() - 1 - count, start_b = second.size() - 1 - count;
                std::set<std::uint32_t> moved;
                for (std::size_t n = start_a; n < first.size(); ++n) if (first[n].result.isValid()) moved.insert(first[n].result.value);
                for (std::size_t n = start_b; n < second.size(); ++n) if (second[n].result.isValid()) moved.insert(second[n].result.value);
                bool escaped = false;
                for (const auto& block : f.blocks) for (std::size_t n = 0; n < block.instructions.size(); ++n) {
                    if (++work > 1000000) return;
                    if ((block.id.value == a && n >= start_a) || (block.id.value == b && n >= start_b)) continue;
                    for (const auto v : block.instructions[n].operands) escaped = escaped || moved.count(v.value) != 0;
                }
                if (escaped || f.value_count + 64 >= 100000 || growth + count + 32 > 256) continue;
                const IRBlockId join{static_cast<std::uint32_t>(f.blocks.size())};
                std::vector<IRInstruction> tail;
                std::map<std::uint32_t, IRValueId> aliases;
                const auto constant = [&](IRValueId old) {
                    const auto found = aliases.find(old.value);
                    if (found != aliases.end()) return found->second;
                    auto copy = defs.at(old.value); copy.result = IRValueId{++f.value_count};
                    copy.in_plot_context = terminal.in_plot_context;
                    aliases.emplace(old.value, copy.result); tail.push_back(copy); return copy.result;
                };
                for (std::size_t n = start_a; n < first.size(); ++n) {
                    auto copy = first[n];
                    if (scalarConstant(copy)) { constant(copy.result); continue; }
                    for (auto& operand : copy.operands) if (scalarConstant(defs.at(operand.value))) operand = constant(operand);
                    tail.push_back(std::move(copy));
                }
                IRInstruction branch; branch.opcode = IROpcode::Branch; branch.targets = {join};
                branch.source = terminal.source; branch.in_plot_context = terminal.in_plot_context;
                growth += tail.size();
                f.blocks[a].instructions.resize(start_a); f.blocks[a].instructions.push_back(branch);
                f.blocks[b].instructions.resize(start_b); f.blocks[b].instructions.push_back(branch);
                f.blocks.push_back({join, "size.shared.tail", std::move(tail)});
                changed = true; break;
            }
        }
        if (!changed) return;
    }
}
}
void IRSizeOptimizer::run(IRModule& module) {
    IRVerifier::verify(module);
    for (auto& f : module.functions) {
        share(f);
        // Moved constant definitions leave holes. Canonicalize IDs before any
        // public pass boundary; no borrowed definition table survives sharing.
        std::vector<IRValueId> ids(static_cast<std::size_t>(f.value_count) + 1);
        std::uint32_t count = 0;
        for (auto& b : f.blocks) for (auto& i : b.instructions) if (i.result.isValid()) {
            const auto old = i.result.value; i.result = IRValueId{++count}; ids.at(old) = i.result;
        }
        for (auto& b : f.blocks) for (auto& i : b.instructions)
            for (auto& v : i.operands) v = ids.at(v.value);
        f.value_count = count;
    }
    IRVerifier::verify(module);
}
