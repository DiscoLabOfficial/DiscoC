#include "IRValueOptimizer.hpp"
#include "IRControlFlow.hpp"
#include "ConstantEvaluator.hpp"
#include "LinearScanAllocator.hpp"
#include "GSUCostModel.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>

namespace {
bool scalar(const Type& t) {
    return !t.pointer_level && !t.array_size &&
        (t.base == BaseType::WORD || t.base == BaseType::BYTE || t.base == BaseType::BOOL);
}
bool same(const Type& a, const Type& b) {
    return scalar(a) && scalar(b) && a.base == b.base && a.sizeInBytes == b.sizeInBytes &&
        a.is_unsigned == b.is_unsigned && a.enum_name == b.enum_name;
}
using Definitions = std::vector<IRInstruction>;
Definitions definitions(const IRFunction& f) {
    Definitions result(static_cast<std::size_t>(f.value_count) + 1);
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.result.isValid()) result.at(i.result.value) = i;
    return result;
}
bool pure(const IRInstruction& i, const Definitions& defs) {
    if (!scalar(i.type) || i.memory_volatile || i.is_live_range_split) return false;
    for (const auto v : i.operands) if (!scalar(defs.at(v.value).type)) return false;
    if (i.opcode == IROpcode::Constant || i.opcode == IROpcode::Cast || i.opcode == IROpcode::BitExtract) return true;
    if (i.opcode == IROpcode::Unary) return i.operation == "-" || i.operation == "~" || i.operation == "!";
    if (i.opcode != IROpcode::Binary || i.operation == "/" || i.operation == "%") return false;
    if (i.operation == "<<" || i.operation == ">>") {
        const auto& count = defs.at(i.operands.at(1).value);
        return count.opcode == IROpcode::Constant && count.immediate >= 0 && count.immediate < 16;
    }
    return true;
}
IRValueId resolve(IRValueId v, std::vector<IRValueId>& aliases) {
    auto root = v;
    while (aliases.at(root.value).isValid()) root = aliases.at(root.value);
    while (aliases.at(v.value).isValid()) {
        const auto next = aliases.at(v.value); aliases[v.value] = root; v = next;
    }
    return root;
}
void rewrite(IRFunction& f, std::vector<IRValueId>& aliases, const std::set<std::uint32_t>& erased) {
    std::vector<IRValueId> ids(static_cast<std::size_t>(f.value_count) + 1);
    std::uint32_t next = 0;
    for (auto& b : f.blocks) {
        b.instructions.erase(std::remove_if(b.instructions.begin(), b.instructions.end(),
            [&](const IRInstruction& i) { return i.result.isValid() && erased.count(i.result.value); }), b.instructions.end());
        for (const auto& i : b.instructions) if (i.result.isValid()) ids.at(i.result.value) = IRValueId{++next};
    }
    for (auto& b : f.blocks) for (auto& i : b.instructions) {
        for (auto& v : i.operands) v = ids.at(resolve(v, aliases).value);
        if (i.result.isValid()) i.result = ids.at(i.result.value);
    }
    f.value_count = next;
}
IRValueId fresh(IRFunction& f) {
    if (f.value_count >= 100000) throw CompilerError("O2 scalar value growth limit exceeded.", 1, 1);
    return IRValueId{++f.value_count};
}

// Available expressions are scoped by dominance, not physical emission order.
// Captured SSA operands remain valid across calls/volatile writes; their reads
// are never coalesced. Sibling expressions are popped before the next sibling.
void numberValues(IRFunction& f) {
    const auto defs = definitions(f);
    IRControlFlow cfg(f);
    using Key = std::tuple<int, std::string, int, std::string, std::uint32_t, std::uint32_t, std::int64_t>;
    std::map<Key, IRValueId> available;
    std::vector<IRValueId> aliases(defs.size());
    std::set<std::uint32_t> erased;
    struct Visit { std::uint32_t block; bool leave; std::vector<Key> inserted; };
    std::vector<Visit> work{{f.entry.value, false, {}}};
    std::size_t entries = 0;
    while (!work.empty()) {
        auto visit = std::move(work.back()); work.pop_back();
        if (visit.leave) { for (const auto& key : visit.inserted) available.erase(key); continue; }
        for (auto& i : f.blocks.at(visit.block).instructions) {
            for (auto& v : i.operands) v = resolve(v, aliases);
            if (!pure(i, defs)) continue;
            auto a = i.operands.empty() ? 0 : i.operands[0].value;
            auto b = i.operands.size() < 2 ? 0 : i.operands[1].value;
            if (i.opcode == IROpcode::Binary && (i.operation == "+" || i.operation == "*" || i.operation == "&" ||
                i.operation == "|" || i.operation == "^" || i.operation == "==" || i.operation == "!="))
                if (a > b) std::swap(a, b);
            const auto literal = i.opcode == IROpcode::Constant ? ConstantEvaluator::convert(i.immediate, i.type) : i.immediate;
            const Key key{static_cast<int>(i.opcode), to_string(i.type), i.type.sizeInBytes,
                i.type.enum_name + ":" + i.operation, a, b, literal};
            const auto previous = available.find(key);
            if (previous != available.end()) {
                aliases.at(i.result.value) = previous->second; erased.insert(i.result.value);
            } else if (entries < 8192) {
                available.emplace(key, i.result); visit.inserted.push_back(key); ++entries;
            }
        }
        const auto block = visit.block;
        visit.leave = true; work.push_back(std::move(visit));
        for (auto child = cfg.children[block].rbegin(); child != cfg.children[block].rend(); ++child)
            work.push_back({*child, false, {}});
    }
    rewrite(f, aliases, erased);
}

struct Fact { std::uint32_t zero = 0, one = 0; std::int64_t low = 0, high = 0; };
unsigned width(const Type& t) { return t.base == BaseType::WORD ? 16u : 8u; }
std::uint32_t mask(const Type& t) { return (std::uint32_t{1} << width(t)) - 1; }
Fact unknown(const Type& t) {
    if (t.base == BaseType::BOOL) return {254u, 0, 0, 1};
    const auto sign = std::int64_t{1} << (width(t) - 1);
    return {0, 0, t.is_unsigned ? 0 : -sign, t.is_unsigned ? sign * 2 - 1 : sign - 1};
}
Fact literal(std::int64_t value, const Type& t) {
    value = ConstantEvaluator::convert(value, t);
    const auto raw = static_cast<std::uint32_t>(value) & mask(t);
    return {mask(t) ^ raw, raw, value, value};
}
void bitBounds(Fact& f, const Type& t) {
    const auto m = mask(t), sign = std::uint32_t{1} << (width(t) - 1);
    f.zero &= m; f.one &= m;
    std::int64_t low = f.one, high = m & ~f.zero;
    if (!t.is_unsigned && t.base != BaseType::BOOL) {
        if (f.one & sign) { low -= m + 1; high -= m + 1; }
        else if (!(f.zero & sign)) { low = (f.one | sign) - static_cast<std::int64_t>(m + 1); high &= sign - 1; }
    }
    f.low = std::max(f.low, low); f.high = std::min(f.high, high);
}
Fact derive(const IRInstruction& i, const Definitions& defs, const std::vector<Fact>& facts) {
    auto result = unknown(i.type);
    if (i.opcode == IROpcode::Constant) return literal(i.immediate, i.type);
    if (i.opcode == IROpcode::Phi) {
        bool first = true;
        for (const auto v : i.operands) {
            const auto& f = facts.at(v.value);
            if (first) { result = f; first = false; }
            else { result.zero &= f.zero; result.one &= f.one; result.low = std::min(result.low, f.low); result.high = std::max(result.high, f.high); }
        }
        return result;
    }
    if (i.operands.empty()) return result;
    // A bool result does not make its pointer/address operands integer facts.
    // Hardware/memory producers retain their full result range as well.
    for (const auto v : i.operands) if (!scalar(defs.at(v.value).type)) return result;
    const auto& a = facts.at(i.operands[0].value);
    const auto m = mask(i.type);
    if (i.opcode == IROpcode::Cast) {
        const auto& source = defs.at(i.operands[0].value).type;
        if (!scalar(source)) return result;
        if (i.type.base == BaseType::BOOL) {
            if (a.low > 0 || a.high < 0 || a.one) return literal(1, i.type);
            if (a.low == 0 && a.high == 0) return literal(0, i.type);
        } else {
            result.zero = a.zero & m; result.one = a.one & m;
            if (width(i.type) > width(source)) {
                const auto extension = m ^ mask(source), sign = std::uint32_t{1} << (width(source) - 1);
                if (source.is_unsigned || source.base == BaseType::BOOL || (a.zero & sign)) result.zero |= extension;
                else if (a.one & sign) result.one |= extension;
            }
            if (a.low >= result.low && a.high <= result.high) { result.low = a.low; result.high = a.high; }
        }
    } else if (i.opcode == IROpcode::BitExtract) {
        const auto bit = std::uint32_t{1} << static_cast<unsigned>(i.immediate);
        if (a.zero & bit) return literal(0, i.type);
        if (a.one & bit) return literal(1, i.type);
        result.zero = m ^ 1u; result.low = 0; result.high = 1;
    } else if (i.opcode == IROpcode::Unary && i.operation == "~") {
        result.zero = a.one; result.one = a.zero;
    } else if (i.opcode == IROpcode::Binary) {
        const auto& b = facts.at(i.operands[1].value);
        const auto& op = i.operation;
        if (op == "&") { result.zero = a.zero | b.zero; result.one = a.one & b.one; }
        else if (op == "|") { result.zero = a.zero & b.zero; result.one = a.one | b.one; }
        else if (op == "^") { result.zero = (a.zero & b.zero) | (a.one & b.one); result.one = (a.zero & b.one) | (a.one & b.zero); }
        else if (op == "+" || op == "-" || op == "*") {
            std::int64_t low = 0, high = 0;
            if (op == "+") { low = a.low + b.low; high = a.high + b.high; }
            else if (op == "-") { low = a.low - b.high; high = a.high - b.low; }
            else {
                const std::array<std::int64_t, 4> products{{a.low*b.low, a.low*b.high, a.high*b.low, a.high*b.high}};
                low = *std::min_element(products.begin(), products.end()); high = *std::max_element(products.begin(), products.end());
            }
            // An interval crossing wrap is not a small interval at the other end.
            if (low >= result.low && high <= result.high) { result.low = low; result.high = high; }
        } else if ((op == "<<" || op == ">>") && b.low == b.high && b.low >= 0 && b.low < 16 && i.type.base == BaseType::WORD) {
            const auto count = static_cast<unsigned>(b.low);
            const auto fill = (std::uint32_t{1} << count) - 1;
            if (op == "<<") { result.zero = ((a.zero << count) | fill) & m; result.one = (a.one << count) & m; }
            else {
                result.zero = a.zero >> count; result.one = a.one >> count;
                const auto extension = m ^ (m >> count);
                if (i.type.is_unsigned || (a.zero & 0x8000u)) result.zero |= extension;
                else if (a.one & 0x8000u) result.one |= extension;
            }
        } else if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            const bool identical = i.operands[0].value == i.operands[1].value;
            const bool unequal = a.high < b.low || b.high < a.low || (a.one & b.zero) || (b.one & a.zero);
            const bool equal = identical || (a.low == a.high && b.low == b.high && a.low == b.low);
            int answer = -1;
            if (op == "==" || op == "!=") {
                if (equal || unequal) answer = static_cast<int>(op == "==" ? equal : unequal);
            } else if (op == "<") { if (a.high < b.low) answer = 1; else if (a.low >= b.high || identical) answer = 0; }
            else if (op == "<=") { if (a.high <= b.low || identical) answer = 1; else if (a.low > b.high) answer = 0; }
            else if (op == ">") { if (a.low > b.high) answer = 1; else if (a.high <= b.low || identical) answer = 0; }
            else if (op == ">=") { if (a.low >= b.high || identical) answer = 1; else if (a.high < b.low) answer = 0; }
            if (answer >= 0) return literal(answer, i.type);
        }
    }
    bitBounds(result, i.type);
    return result;
}
void simplifyFacts(IRFunction& f) {
    const auto defs = definitions(f);
    std::vector<Fact> facts(defs.size());
    for (std::size_t v = 1; v < defs.size(); ++v) if (scalar(defs[v].type)) facts[v] = unknown(defs[v].type);
    // Start at top, never an invented bottom/undef PHI. Every intermediate
    // approximation is safe even when a cyclic graph exhausts the work budget.
    for (unsigned round = 0; round < 24; ++round) {
        bool changed = false;
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
            if (!i.result.isValid() || !scalar(i.type)) continue;
            const auto next = derive(i, defs, facts);
            auto& old = facts.at(i.result.value);
            if (std::tie(old.zero, old.one, old.low, old.high) != std::tie(next.zero, next.one, next.low, next.high)) { old = next; changed = true; }
        }
        if (!changed) break;
    }
    std::vector<IRValueId> aliases(defs.size()); std::set<std::uint32_t> erased;
    for (auto& b : f.blocks) for (auto& i : b.instructions) {
        if (!i.result.isValid() || !pure(i, defs)) continue;
        const auto& fact = facts.at(i.result.value);
        if (fact.low == fact.high || ((fact.zero | fact.one) == mask(i.type))) {
            const auto value = fact.low == fact.high ? fact.low : ConstantEvaluator::convert(fact.one, i.type);
            i.opcode = IROpcode::Constant; i.immediate = value; i.operands.clear(); i.operation.clear();
        } else if (i.opcode == IROpcode::Binary && (i.operation == "&" || i.operation == "|")) {
            for (unsigned side = 0; side < 2; ++side) {
                const auto other = i.operands[side], c = i.operands[1 - side];
                if (!same(i.type, defs.at(other.value).type) || defs.at(c.value).opcode != IROpcode::Constant) continue;
                const auto bits = static_cast<std::uint32_t>(defs[c.value].immediate) & mask(i.type);
                const auto& input = facts.at(other.value);
                if ((i.operation == "&" && !(mask(i.type) & ~input.zero & ~bits)) ||
                    (i.operation == "|" && !(bits & ~input.one))) {
                    aliases.at(i.result.value) = other; erased.insert(i.result.value); break;
                }
            }
        }
    }
    rewrite(f, aliases, erased);
}

struct Ramp { IRInstruction phi; IRValueId initial; std::uint16_t step; };
std::map<std::uint32_t, Ramp> ramps(const IRFunction& f, const Definitions& defs, std::uint32_t header,
                                  std::uint32_t preheader, std::uint32_t latch) {
    std::map<std::uint32_t, Ramp> result;
    for (const auto& phi : f.blocks[header].instructions) {
        if (phi.opcode != IROpcode::Phi) break;
        if (phi.operands.size() != 2 || !scalar(phi.type) || phi.type.base != BaseType::WORD || !phi.type.enum_name.empty()) continue;
        IRValueId initial, next;
        for (unsigned e = 0; e < 2; ++e) {
            if (phi.targets[e].value == preheader) initial = phi.operands[e];
            if (phi.targets[e].value == latch) next = phi.operands[e];
        }
        if (!initial.isValid() || !next.isValid()) continue;
        const auto& update = defs.at(next.value);
        if (update.opcode != IROpcode::Binary || (update.operation != "+" && update.operation != "-") ||
            update.operands[0].value != phi.result.value || !same(update.type, phi.type)) continue;
        const auto& delta = defs.at(update.operands[1].value);
        if (delta.opcode != IROpcode::Constant) continue;
        const auto raw = static_cast<std::uint16_t>(delta.immediate);
        result.emplace(phi.result.value, Ramp{phi, initial, update.operation == "+" ? raw : static_cast<std::uint16_t>(0u - raw)});
    }
    return result;
}
void recurrences(IRFunction& f) {
    IRControlFlow cfg(f);
    std::sort(cfg.loops.begin(), cfg.loops.end(), [](const IRControlFlow::Loop& a, const IRControlFlow::Loop& b) { return a.blocks.size() < b.blocks.size(); });
    for (const auto& loop : cfg.loops) {
        if (loop.latches.size() != 1) continue;
        // Do not extend an outer recurrence through a hotter nested loop:
        // its new PHI can displace that loop's cursor/induction temporaries.
        // Inner loops and non-nested loops are the initial profitable scope.
        bool nested = false;
        for (const auto b : loop.blocks) nested = nested || cfg.loop_depth[b] > cfg.loop_depth[loop.header];
        if (nested) continue;
        std::vector<std::uint32_t> outside;
        for (const auto p : cfg.predecessors[loop.header]) if (!loop.blocks.count(p)) outside.push_back(p);
        if (outside.size() != 1 || cfg.successors[outside[0]].size() != 1) continue;
        const auto preheader = outside[0], latch = *loop.latches.begin();
        const auto defs = definitions(f);
        const auto induction = ramps(f, defs, loop.header, preheader, latch);
        std::vector<std::uint32_t> owner(defs.size(), IRBlockId::Invalid);
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) if (i.result.isValid()) owner[i.result.value] = b.id.value;
        std::vector<IRInstruction> candidates;
        for (const auto b : loop.blocks) for (const auto& i : f.blocks[b].instructions)
            if (i.opcode == IROpcode::Binary && i.operation == "*" && i.type.base == BaseType::WORD && scalar(i.type)) candidates.push_back(i);
        std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint16_t>, IRValueId> carried;
        std::vector<IRValueId> aliases(static_cast<std::size_t>(f.value_count) + 1); std::set<std::uint32_t> erased;
        for (const auto& multiply : candidates) for (unsigned side = 0; side < 2; ++side) {
            auto input = multiply.operands[side]; std::uint16_t offset = 0;
            const auto& expression = defs.at(input.value);
            if (expression.opcode == IROpcode::Binary && (expression.operation == "+" || expression.operation == "-") &&
                defs.at(expression.operands[1].value).opcode == IROpcode::Constant && same(expression.type, multiply.type)) {
                const auto raw = static_cast<std::uint16_t>(defs[expression.operands[1].value].immediate);
                offset = expression.operation == "+" ? raw : static_cast<std::uint16_t>(0u - raw); input = expression.operands[0];
            }
            const auto ramp = induction.find(input.value);
            const auto factor = multiply.operands[1 - side];
            if (ramp == induction.end() || !same(ramp->second.phi.type, multiply.type) || !same(defs[factor.value].type, multiply.type) ||
                loop.blocks.count(owner.at(factor.value)) || !cfg.dominates(owner.at(factor.value), preheader)) continue;
            if (defs[factor.value].opcode == IROpcode::Constant) {
                const auto magnitude = static_cast<std::uint16_t>(defs[factor.value].immediate);
                if (!magnitude || !(magnitude & (magnitude - 1))) continue; // Existing shifts are cheaper than another PHI.
            }
            const auto key = std::make_tuple(input.value, factor.value, offset);
            auto carried_value = carried.find(key);
            if (carried_value == carried.end()) {
                if (carried.size() >= 2 || f.value_count > 99992) continue;
                const auto& ramp_value = ramp->second;
                std::vector<IRInstruction> setup;
                auto constant = [&](std::uint16_t raw) {
                    IRInstruction i; i.opcode = IROpcode::Constant; i.type = multiply.type; i.result = fresh(f);
                    i.immediate = ConstantEvaluator::convert(raw, i.type); i.source = multiply.source; setup.push_back(i); return i.result;
                };
                auto binary = [&](const char* op, IRValueId a, IRValueId b) {
                    auto i = multiply; i.operation = op; i.operands = {a, b}; i.result = fresh(f); return i;
                };
                auto initial_input = ramp_value.initial;
                if (offset) { auto add = binary("+", initial_input, constant(offset)); initial_input = add.result; setup.push_back(add); }
                auto initial = binary("*", initial_input, factor); setup.push_back(initial);
                auto delta = binary("*", constant(ramp_value.step), factor); setup.push_back(delta);
                auto phi = ramp_value.phi; phi.result = fresh(f); phi.symbol_id = SymbolId{};
                auto next = binary("+", phi.result, delta.result);
                for (std::size_t e = 0; e < phi.targets.size(); ++e) phi.operands[e] = phi.targets[e].value == preheader ? initial.result : next.result;
                auto& pre = f.blocks[preheader].instructions; pre.insert(pre.end() - 1, setup.begin(), setup.end());
                auto& back = f.blocks[latch].instructions; back.insert(back.end() - 1, next);
                f.blocks[loop.header].instructions.insert(f.blocks[loop.header].instructions.begin(), phi);
                carried_value = carried.emplace(key, phi.result).first;
            }
            aliases.resize(static_cast<std::size_t>(f.value_count) + 1);
            aliases.at(multiply.result.value) = carried_value->second; erased.insert(multiply.result.value); break;
        }
        rewrite(f, aliases, erased);
    }
}

void splitLifetimes(IRFunction& f) {
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.is_live_range_split) return; // A partition is planned once, not repeatedly expanded.
    IRControlFlow cfg(f);
    if (cfg.loops.size() > 32 || f.value_count > 2048) return;
    std::sort(cfg.loops.begin(), cfg.loops.end(), [](const IRControlFlow::Loop& a, const IRControlFlow::Loop& b) { return a.blocks.size() < b.blocks.size(); });
    std::size_t growth = 0;
    unsigned trials = 0;
    for (const auto& loop : cfg.loops) {
        if (growth >= 12) break;
        std::vector<std::uint32_t> outside;
        for (const auto p : cfg.predecessors[loop.header]) if (!loop.blocks.count(p)) outside.push_back(p);
        if (outside.size() != 1 || cfg.successors[outside[0]].size() != 1) continue;
        const auto preheader = outside[0];
        const auto defs = definitions(f);
        std::set<std::uint32_t> inside_defs, already_split;
        std::map<std::uint32_t, unsigned> hot_uses, cold_uses;
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
            if (loop.blocks.count(b.id.value) && i.result.isValid()) inside_defs.insert(i.result.value);
            if (b.id.value == preheader && i.is_live_range_split) already_split.insert(i.operands.at(0).value);
            for (const auto v : i.operands) ++(loop.blocks.count(b.id.value) ? hot_uses : cold_uses)[v.value];
        }
        std::vector<std::uint32_t> choices;
        for (const auto& use : hot_uses) {
            const auto& producer = defs.at(use.first);
            if (use.second < 2 || !cold_uses.count(use.first) || inside_defs.count(use.first) || already_split.count(use.first) ||
                producer.is_live_range_split || !scalar(producer.type) || producer.opcode == IROpcode::Constant ||
                !cfg.live_in.at(loop.header).count(use.first)) continue;
            choices.push_back(use.first);
        }
        std::sort(choices.begin(), choices.end(), [&](std::uint32_t a, std::uint32_t b) {
            return hot_uses[a] != hot_uses[b] ? hot_uses[a] > hot_uses[b] : a < b;
        });
        if (choices.size() > 4) choices.resize(4);
        unsigned accepted = 0;
        for (const auto value : choices) {
            if (growth >= 12 || accepted >= 2 || trials >= 16 || f.value_count >= 99999) break;
            ++trials;
            LinearScanAllocator before; before.runGlobal(f, {5,7,8});
            const auto* original = before.find(IRValueId{value});
            if (!original || original->rematerializable) continue;
            const auto score = [&](const IRFunction& function, const LinearScanAllocator& allocator) {
                return GSUCostModel::allocation(function, allocator, cfg).pressureScore();
            };
            const auto old_cost = score(f, before);
            auto trial = f;
            IRInstruction copy; copy.opcode = IROpcode::Cast; copy.type = defs.at(value).type;
            copy.source = defs.at(value).source; copy.operands = {IRValueId{value}};
            copy.result = fresh(trial); copy.is_live_range_split = true;
            auto& pre = trial.blocks[preheader].instructions;
            copy.in_plot_context = pre.back().in_plot_context;
            pre.insert(pre.end() - 1, copy);
            for (const auto b : loop.blocks) for (auto& i : trial.blocks[b].instructions)
                for (auto& v : i.operands) if (v.value == value) v = copy.result;
            LinearScanAllocator after; after.runGlobal(trial, {5,7,8});
            const auto* hot = after.find(copy.result);
            // A hot copy can both remove repeated reloads and free a register
            // held by cold live-through state elsewhere. Require exact trial
            // reallocation to reduce weighted memory traffic and copy cost.
            if (!hot || !hot->has_register || score(trial, after) >= old_cost) continue;
            f = std::move(trial);
            ++growth; ++accepted;
        }
    }
    std::vector<IRValueId> aliases(static_cast<std::size_t>(f.value_count) + 1);
    rewrite(f, aliases, {});
}
}

void IRValueOptimizer::run(IRModule& module) {
    IRVerifier::verify(module);
    for (auto& f : module.functions) {
        // Optional analysis remains bounded even at the verifier's maximum IR size.
        if (f.value_count > 16000) continue;
        simplifyFacts(f); numberValues(f);
    }
    IRVerifier::verify(module);
}
void IRValueOptimizer::reduceRecurrences(IRModule& module) {
    IRVerifier::verify(module);
    for (auto& f : module.functions) if (f.value_count <= 16000) recurrences(f);
    IRVerifier::verify(module);
}
void IRValueOptimizer::splitLoopLifetimes(IRModule& module) {
    IRVerifier::verify(module);
    for (auto& f : module.functions) if (f.value_count <= 16000) splitLifetimes(f);
    IRVerifier::verify(module);
}
