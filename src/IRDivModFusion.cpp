#include "IRDivModFusion.hpp"

#include "ConstantEvaluator.hpp"
#include "IRControlFlow.hpp"

#include <algorithm>
#include <map>
#include <tuple>

namespace {
bool word(const Type& type) {
    return type.base == BaseType::WORD && type.pointer_level == 0 && type.array_size == 0 && type.sizeInBytes == 2;
}
void fuse(IRFunction& function) {
    IRControlFlow cfg(function);
    struct Position { std::uint32_t block; std::size_t index; };
    std::vector<Position> definitions(static_cast<std::size_t>(function.value_count) + 1);
    for (const auto& b : function.blocks) for (std::size_t n = 0; n < b.instructions.size(); ++n)
        if (b.instructions[n].result.isValid()) definitions.at(b.instructions[n].result.value) = {b.id.value, n};
    const auto definition = [&](IRValueId id) -> const IRInstruction& {
        const auto& p = definitions.at(id.value);
        return function.blocks.at(p.block).instructions.at(p.index);
    };
    // Equal constants need not share an SSA ID. This canonical identity is
    // used only for the matching key, never to rewrite a dominance relation.
    using ConstantKey = std::tuple<bool, std::string, std::int64_t>;
    std::map<ConstantKey, std::uint32_t> constants;
    std::vector<std::uint32_t> identity(definitions.size());
    for (std::uint32_t id = 1; id <= function.value_count; ++id) {
        identity[id] = id;
        const auto& i = definition(IRValueId{id});
        if (i.opcode == IROpcode::Constant && word(i.type))
            identity[id] = constants.emplace(ConstantKey{i.type.is_unsigned, i.type.enum_name,
                ConstantEvaluator::convert(i.immediate, i.type)}, id).first->second;
    }
    using Key = std::tuple<std::uint32_t, std::uint32_t, bool, std::string>;
    std::map<Key, std::vector<Position>> available;
    // Traverse the dominator tree, not physical block order. Loop/branch
    // projections may use a pair established by an earlier dominating block.
    std::vector<std::uint32_t> blocks{function.entry.value};
    std::size_t work = 0, pairs = 0;
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
        pairs += instruction.opcode == IROpcode::DivMod;
    while (!blocks.empty()) {
        const auto b = blocks.back(); blocks.pop_back();
        auto& body = function.blocks.at(b).instructions;
        for (std::size_t n = 0; n < body.size(); ++n) {
            auto& i = body[n];
            if (i.opcode != IROpcode::Binary || (i.operation != "/" && i.operation != "%") ||
                !word(i.type) || i.operands.size() != 2) continue;
            const auto& a = definition(i.operands[0]);
            const auto& d = definition(i.operands[1]);
            if (!word(a.type) || !word(d.type) || a.type.is_unsigned != i.type.is_unsigned ||
                d.type.is_unsigned != i.type.is_unsigned || a.type.enum_name != i.type.enum_name ||
                d.type.enum_name != i.type.enum_name) continue;
            // Do not replace the existing cheap shift/mask selection with a
            // sixteen-step helper or allocate a needless pair for literal zero.
            if (d.opcode == IROpcode::Constant) {
                auto value = ConstantEvaluator::convert(d.immediate, d.type);
                const auto magnitude = static_cast<std::uint64_t>(value < 0 ? -value : value);
                if (magnitude == 0 || !(magnitude & (magnitude - 1))) continue;
            }
            const Key key{identity.at(i.operands[0].value), identity.at(i.operands[1].value), i.type.is_unsigned, i.type.enum_name};
            auto& candidates = available[key];
            bool matched = false;
            for (auto p = candidates.rbegin(); p != candidates.rend(); ++p) {
                if (++work > 25000000) return; // Conservative fallback, no new invalid IR.
                auto& first = function.blocks.at(p->block).instructions.at(p->index);
                if (first.operation == i.operation || !cfg.dominates(p->block, b)) continue;
                if (first.opcode == IROpcode::Binary && pairs >= 256) continue;
                if (first.opcode == IROpcode::Binary) { first.opcode = IROpcode::DivMod; ++pairs; }
                i.opcode = IROpcode::DivModResult;
                i.operands = {first.result};
                matched = true; break;
            }
            if (!matched) candidates.push_back({b, n});
        }
        for (auto child = cfg.children[b].rbegin(); child != cfg.children[b].rend(); ++child) blocks.push_back(*child);
    }
}
}

void IRDivModFusion::run(IRModule& module) {
    IRVerifier::verify(module);
    for (auto& function : module.functions) fuse(function);
    IRVerifier::verify(module);
}
