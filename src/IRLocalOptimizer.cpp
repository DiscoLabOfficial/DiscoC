#include "IRLocalOptimizer.hpp"

#include <algorithm>
#include <set>
#include <tuple>

#include "IRScalarFold.hpp"

namespace {

bool scalar(const Type& type) {
    return type.pointer_level == 0 && type.array_size == 0 &&
        (type.base == BaseType::BYTE || type.base == BaseType::WORD || type.base == BaseType::BOOL);
}

bool sameScalar(const Type& a, const Type& b) {
    return scalar(a) && scalar(b) && a.base == b.base &&
        a.sizeInBytes == b.sizeInBytes && a.is_unsigned == b.is_unsigned && a.enum_name == b.enum_name;
}

bool fold(const IRInstruction& instruction,
          const std::vector<const IRInstruction*>& definitions, std::int64_t& result) {
    std::array<IRScalarFold::Operand, 2> values{};
    if (instruction.operands.size() > 2) return false;
    for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
        const auto& operand = *definitions.at(instruction.operands[i].value);
        if (operand.opcode != IROpcode::Constant || !scalar(operand.type)) return false;
        values[i] = {&operand.type, operand.immediate};
    }
    return IRScalarFold::evaluate(instruction, values, result);
}

bool pureScalar(const IRInstruction& instruction,
                const std::vector<const IRInstruction*>& definitions) {
    if (!scalar(instruction.type) || instruction.memory_volatile || instruction.is_live_range_split) return false;
    for (const auto operand : instruction.operands)
        if (!scalar(definitions.at(operand.value)->type)) return false;
    if (instruction.opcode == IROpcode::Phi || instruction.opcode == IROpcode::Constant || instruction.opcode == IROpcode::Cast ||
        instruction.opcode == IROpcode::BitExtract) return true;
    if (instruction.opcode == IROpcode::Unary)
        return instruction.operation == "-" || instruction.operation == "~" || instruction.operation == "!";
    if (instruction.opcode != IROpcode::Binary) return false;
    if (instruction.operation == "/" || instruction.operation == "%") return false;
    if (instruction.operation == "<<" || instruction.operation == ">>") {
        const auto& count = *definitions.at(instruction.operands.at(1).value);
        const auto value = ConstantEvaluator::convert(count.immediate, count.type);
        return count.opcode == IROpcode::Constant && value >= 0 && value < 16;
    }
    return true;
}

void fuseBitExtract(IRInstruction& instruction,
                    const std::vector<const IRInstruction*>& definitions) {
    if (instruction.opcode != IROpcode::Binary || instruction.type.base != BaseType::WORD || !scalar(instruction.type)) return;
    const auto& left = *definitions.at(instruction.operands[0].value);
    const auto& right = *definitions.at(instruction.operands[1].value);
    if (right.opcode != IROpcode::Constant || left.opcode != IROpcode::Binary) return;
    IRValueId input;
    std::int64_t bit = -1;
    if (instruction.operation == ">>" && left.operation == "&") {
        bit = ConstantEvaluator::convert(right.immediate, right.type);
        if (bit < 0 || bit > 15 || (bit == 15 && !instruction.type.is_unsigned)) return;
        for (std::size_t index = 0; index < 2; ++index) {
            const auto& mask = *definitions.at(left.operands[index].value);
            if (mask.opcode == IROpcode::Constant &&
                (static_cast<std::uint64_t>(mask.immediate) & 0xffffu) == (std::uint64_t{1} << static_cast<unsigned>(bit)))
                input = left.operands[1 - index];
        }
    } else if (instruction.operation == "&" && right.immediate == 1 && left.operation == ">>") {
        const auto& count = *definitions.at(left.operands[1].value);
        if (count.opcode != IROpcode::Constant) return;
        bit = ConstantEvaluator::convert(count.immediate, count.type);
        if (bit < 0 || bit > 15) return;
        input = left.operands[0];
    }
    if (!input.isValid() || !sameScalar(instruction.type, definitions.at(input.value)->type)) return;
    instruction.opcode = IROpcode::BitExtract;
    instruction.operands = {input};
    instruction.operation.clear();
    instruction.immediate = bit;
}

void optimizeFunction(IRFunction& function, const Analyzer::LocalSymbolTable& locals) {
    // Dense IDs have a verifier-enforced bound. Instruction addresses remain
    // stable until all rewrites are complete; only then compact the vectors.
    std::vector<const IRInstruction*> definitions(static_cast<std::size_t>(function.value_count) + 1, nullptr);
    std::vector<IRValueId> aliases(definitions.size());
    std::vector<bool> removed(definitions.size(), false);
    std::set<const IRInstruction*> erased_stores;
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
        if (instruction.result.isValid()) definitions.at(instruction.result.value) = &instruction;

    const auto resolve = [&aliases](IRValueId value) {
        auto root = value;
        while (aliases.at(root.value).isValid()) root = aliases.at(root.value);
        while (aliases.at(value.value).isValid()) {
            const auto next = aliases.at(value.value);
            aliases.at(value.value) = root;
            value = next;
        }
        return root;
    };
    const auto localAddress = [&locals, &definitions](IRValueId value) {
        const auto& address = *definitions.at(value.value);
        if (address.opcode != IROpcode::Address || !address.operation.empty() ||
            !address.symbol_id.isValid() || isFarPointer(address.type) || address.type.space != AddressSpace::RAM)
            return SymbolId{};
        const auto found = locals.find(address.symbol_id);
        if (found == locals.end() || !scalar(found->second.type) || found->second.type.is_volatile)
            return SymbolId{};
        return address.symbol_id;
    };

    std::set<SymbolId> escaped;
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
        for (std::size_t i = 0; i < instruction.operands.size(); ++i) {
            const auto symbol = localAddress(instruction.operands[i]);
            if (symbol.isValid() && !(i == 0 &&
                (instruction.opcode == IROpcode::LoadIndirect || instruction.opcode == IROpcode::StoreIndirect)))
                escaped.insert(symbol);
        }
    }

    for (auto& block : function.blocks) {
        // No fact flows through a CFG join/backedge, call, unknown write,
        // volatile access, or framebuffer access. Escaping locals,
        // arrays, pointers and aggregates are deliberately outside this proof.
        std::map<SymbolId, IRValueId> memory;
        std::map<SymbolId, const IRInstruction*> pending_stores;
        // Bounded local value numbering. Captured SSA operands, not lexical
        // names, make reuse independent of subsequent changes to memory.
        using Key = std::tuple<int, int, int, bool, std::string, std::string,
                               std::uint32_t, std::uint32_t, std::int64_t>;
        std::map<Key, IRValueId> expressions;
        for (auto& instruction : block.instructions) {
            for (auto& operand : instruction.operands) operand = resolve(operand);
            const auto id = instruction.result;
            const auto eliminate = [&](IRValueId replacement) {
                aliases.at(id.value) = replacement;
                removed.at(id.value) = true;
            };
            if (id.isValid() && !instruction.is_live_range_split) {
                std::int64_t constant = 0;
                if (fold(instruction, definitions, constant)) {
                    instruction.opcode = IROpcode::Constant;
                    instruction.immediate = constant;
                    instruction.operands.clear();
                    instruction.operation.clear();
                } else if (instruction.opcode == IROpcode::Cast &&
                           sameScalar(instruction.type, definitions.at(instruction.operands[0].value)->type)) {
                    eliminate(instruction.operands[0]);
                } else if (instruction.opcode == IROpcode::Binary) {
                    const auto& right = *definitions.at(instruction.operands[1].value);
                    const auto& op = instruction.operation;
                    if (right.opcode == IROpcode::Constant && sameScalar(instruction.type, definitions.at(instruction.operands[0].value)->type) &&
                        ((right.immediate == 0 && (op == "+" || op == "-" || op == "|" || op == "^" || op == "<<" || op == ">>")) ||
                         (right.immediate == 1 && op == "*")))
                        eliminate(instruction.operands[0]);
                }
                if (!removed.at(id.value)) {
                    fuseBitExtract(instruction, definitions);
                    if (pureScalar(instruction, definitions) && instruction.opcode != IROpcode::Constant && instruction.opcode != IROpcode::Phi) {
                        const Key key{static_cast<int>(instruction.opcode), static_cast<int>(instruction.type.base),
                            instruction.type.sizeInBytes, instruction.type.is_unsigned, instruction.type.enum_name,
                            instruction.operation, instruction.operands.empty() ? 0 : instruction.operands[0].value,
                            instruction.operands.size() < 2 ? 0 : instruction.operands[1].value, instruction.immediate};
                        const auto previous = expressions.find(key);
                        if (previous != expressions.end()) eliminate(resolve(previous->second));
                        else {
                            if (expressions.size() == 64) expressions.clear();
                            expressions.emplace(key, id);
                        }
                    }
                }
            }
            const auto effects = instruction.hardwareEffects();
            if (instruction.memory_volatile || instruction.opcode == IROpcode::MemoryInitialize || instruction.opcode == IROpcode::Call ||
                effects.reads_framebuffer || effects.writes_framebuffer || instruction.opcode == IROpcode::HardwareLoop ||
                instruction.opcode == IROpcode::HardwareLoopEnd || instruction.opcode == IROpcode::HardwareLoopLeave) memory.clear();
            // Dead-store elimination must not move an address fault past an
            // observable or potentially failing operation. Its barrier is
            // deliberately stricter than ordinary load forwarding.
            const bool store_barrier = instruction.memory_volatile || instruction.opcode == IROpcode::MemoryInitialize || effects.observable() ||
                instruction.opcode == IROpcode::Call || instruction.opcode == IROpcode::Cache ||
                instruction.opcode == IROpcode::HardwareLoop || instruction.opcode == IROpcode::HardwareLoopEnd ||
                (id.isValid() && !(instruction.opcode == IROpcode::Address && instruction.operation.empty()) &&
                 !pureScalar(instruction, definitions));
            if (store_barrier) pending_stores.clear();
            if (instruction.opcode == IROpcode::Call || instruction.opcode == IROpcode::HardwareLoop ||
                instruction.opcode == IROpcode::HardwareLoopEnd || instruction.opcode == IROpcode::HardwareLoopLeave) expressions.clear();
            if (instruction.memory_volatile) continue;
            if (instruction.opcode != IROpcode::LoadIndirect && instruction.opcode != IROpcode::StoreIndirect) continue;
            const auto symbol = localAddress(instruction.operands[0]);
            if (!symbol.isValid() || escaped.count(symbol)) {
                if (instruction.opcode == IROpcode::StoreIndirect) { memory.clear(); pending_stores.clear(); }
                continue;
            }
            const auto found = memory.find(symbol);
            if (instruction.opcode == IROpcode::LoadIndirect) {
                pending_stores.erase(symbol);
                if (found != memory.end() && sameScalar(instruction.type, definitions.at(resolve(found->second).value)->type))
                    eliminate(resolve(found->second));
                else memory[symbol] = id;
            } else {
                const auto value = instruction.operands[1];
                if (found != memory.end() && resolve(found->second).value == value.value &&
                    sameScalar(instruction.type, definitions.at(value.value)->type)) {
                    // No value result to alias. Erase only this proven identical
                    // nonvolatile write, without moving any surrounding effect.
                    erased_stores.insert(&instruction);
                } else if (sameScalar(instruction.type, definitions.at(value.value)->type)) {
                    const auto previous = pending_stores.find(symbol);
                    if (previous != pending_stores.end() && sameScalar(instruction.type, previous->second->type))
                        erased_stores.insert(previous->second);
                    pending_stores[symbol] = &instruction;
                    memory[symbol] = value;
                }
                else memory.erase(symbol);
            }
        }
    }

    // Cascading DCE removes the now-unused mask/shift chain, never loads,
    // calls, pointer checks or potentially faulting division/shift operations.
    std::vector<std::size_t> uses(definitions.size(), 0);
    for (auto& block : function.blocks) for (auto& instruction : block.instructions) {
        for (auto& operand : instruction.operands) operand = resolve(operand);
        if ((instruction.result.isValid() && removed.at(instruction.result.value)) || erased_stores.count(&instruction)) continue;
        for (const auto operand : instruction.operands) ++uses.at(operand.value);
    }
    std::vector<IRValueId> dead;
    for (std::size_t id = 1; id < definitions.size(); ++id)
        if (!removed[id] && uses[id] == 0 && pureScalar(*definitions[id], definitions)) dead.push_back(IRValueId{static_cast<std::uint32_t>(id)});
    while (!dead.empty()) {
        const auto id = dead.back(); dead.pop_back();
        if (removed.at(id.value)) continue;
        removed.at(id.value) = true;
        for (const auto operand : definitions.at(id.value)->operands) {
            if (--uses.at(operand.value) == 0 && !removed.at(operand.value) && pureScalar(*definitions.at(operand.value), definitions))
                dead.push_back(operand);
        }
    }

    std::vector<IRValueId> renumber(definitions.size());
    std::uint32_t next = 0;
    for (auto& block : function.blocks) {
        block.instructions.erase(std::remove_if(block.instructions.begin(), block.instructions.end(),
            [&removed, &erased_stores](const IRInstruction& instruction) {
                return instruction.result.isValid() ? removed.at(instruction.result.value) : erased_stores.count(&instruction) != 0;
            }), block.instructions.end());
        for (const auto& instruction : block.instructions)
            if (instruction.result.isValid()) renumber.at(instruction.result.value) = IRValueId{++next};
    }
    for (auto& block : function.blocks) for (auto& instruction : block.instructions) {
        for (auto& operand : instruction.operands) operand = renumber.at(resolve(operand).value);
        if (instruction.result.isValid()) instruction.result = renumber.at(instruction.result.value);
    }
    function.value_count = next;
}

} // namespace

void IRLocalOptimizer::run(IRModule& module,
                         const std::map<std::string, Analyzer::LocalSymbolTable>& locals) {
    IRVerifier::verify(module);
    for (auto& function : module.functions) {
        const auto found = locals.find(function.name);
        const Analyzer::LocalSymbolTable empty;
        optimizeFunction(function, found == locals.end() ? empty : found->second);
    }
    IRVerifier::verify(module);
}
