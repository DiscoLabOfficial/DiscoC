#include "IRConditionalOptimizer.hpp"
#include "IRScalarFold.hpp"

#include <algorithm>
#include <deque>
#include <iterator>
#include <set>

namespace {
constexpr std::size_t MaxInstructions = 200000, MaxEntries = 1000000, MaxWork = 25000000;
struct Budget {
    std::size_t work = 0;
    void charge(std::size_t count = 1) {
        if (count > MaxWork - work) throw CompilerError("O2 SCCP/CFG work limit exceeded.", 1, 1);
        work += count;
    }
};
enum class Kind { Unknown, Constant, Variable };
struct Fact {
    Kind kind = Kind::Unknown;
    std::int64_t value = 0;
};
Fact meet(Fact a, Fact b) {
    if (a.kind == Kind::Unknown) return b;
    if (b.kind == Kind::Unknown) return a;
    if (a.kind == Kind::Variable || b.kind == Kind::Variable || a.value != b.value) return {Kind::Variable, 0};
    return a;
}
struct Node { std::uint32_t block; std::size_t instruction; };
struct Analysis {
    std::vector<Fact> facts;
    std::vector<bool> reachable;
};

void validateResources(const IRFunction& f) {
    if (f.blocks.empty() || f.blocks.size() > 8192 || f.value_count > 100000)
        throw CompilerError("O2 SCCP resource limit exceeded.", 1, 1);
    std::size_t instructions = 0, entries = 0;
    for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
        if ((i.opcode == IROpcode::HardwareLoop || i.opcode == IROpcode::HardwareLoopEnd) && i.targets.empty())
            throw CompilerError("O2 SCCP requires exposed hardware-loop backedges.", i.source);
        if (++instructions > MaxInstructions || i.operands.size() > MaxEntries - entries)
            throw CompilerError("O2 SCCP instruction/use limit exceeded.", i.source);
        entries += i.operands.size();
        if (i.targets.size() > MaxEntries - entries) throw CompilerError("O2 SCCP edge limit exceeded.", i.source);
        entries += i.targets.size();
    }
}

Analysis analyze(const IRFunction& f, Budget& budget) {
    Analysis a;
    a.facts.resize(static_cast<std::size_t>(f.value_count) + 1);
    a.reachable.assign(f.blocks.size(), false);
    std::vector<Node> nodes;
    std::vector<std::size_t> begin(f.blocks.size()), end(f.blocks.size()), definitions(a.facts.size());
    std::vector<std::vector<std::size_t>> users(a.facts.size());
    for (const auto& b : f.blocks) {
        begin[b.id.value] = nodes.size();
        for (std::size_t n = 0; n < b.instructions.size(); ++n) {
            const auto& i = b.instructions[n];
            if (i.result.isValid()) definitions.at(i.result.value) = nodes.size();
            for (const auto v : i.operands) users.at(v.value).push_back(nodes.size());
            nodes.push_back({b.id.value, n});
        }
        end[b.id.value] = nodes.size();
    }
    // No IR mutation until the fixed point: these borrowed references remain
    // valid even when an executable backedge changes a constant to Variable.
    const auto instruction = [&](std::size_t n) -> const IRInstruction& {
        const auto node = nodes.at(n); return f.blocks.at(node.block).instructions.at(node.instruction);
    };
    std::vector<std::set<std::uint32_t>> executable(f.blocks.size());
    std::deque<std::size_t> pending;
    std::vector<bool> queued(nodes.size(), false);
    const auto enqueue = [&](std::size_t n) {
        budget.charge();
        if (!queued.at(n)) { queued[n] = true; pending.push_back(n); }
    };
    const auto update = [&](IRValueId v, Fact next) {
        auto& old = a.facts.at(v.value); next = meet(old, next);
        if (old.kind == next.kind && (next.kind != Kind::Constant || old.value == next.value)) return;
        old = next;
        for (const auto use : users.at(v.value)) if (a.reachable.at(nodes.at(use).block)) enqueue(use);
    };
    const auto edge = [&](std::uint32_t from, IRBlockId target) {
        budget.charge();
        if (!executable.at(from).insert(target.value).second) return;
        if (!a.reachable.at(target.value)) {
            a.reachable[target.value] = true;
            for (auto n = begin.at(target.value); n < end.at(target.value); ++n) enqueue(n);
        } else {
            for (auto n = begin.at(target.value); n < end.at(target.value) && instruction(n).opcode == IROpcode::Phi; ++n) enqueue(n);
        }
    };
    const auto evaluate = [&](const IRInstruction& i, std::uint32_t block) -> Fact {
        if (!IRScalarFold::scalar(i.type)) return {Kind::Variable, 0};
        if (i.opcode == IROpcode::Constant) return {Kind::Constant, ConstantEvaluator::convert(i.immediate, i.type)};
        if (i.opcode == IROpcode::Phi) {
            Fact result;
            for (std::size_t n = 0; n < i.operands.size(); ++n) {
                budget.charge();
                if (executable.at(i.targets[n].value).count(block)) result = meet(result, a.facts.at(i.operands[n].value));
            }
            return result;
        }
        if (i.opcode != IROpcode::Binary && i.opcode != IROpcode::Unary &&
            i.opcode != IROpcode::Cast && i.opcode != IROpcode::BitExtract) return {Kind::Variable, 0};
        if (i.operands.size() > 2) return {Kind::Variable, 0};
        std::array<IRScalarFold::Operand, 2> values{};
        bool unknown = false;
        for (std::size_t n = 0; n < i.operands.size(); ++n) {
            const auto v = i.operands[n]; const auto fact = a.facts.at(v.value);
            if (fact.kind == Kind::Variable) return {Kind::Variable, 0};
            unknown = unknown || fact.kind == Kind::Unknown;
            values[n] = {&instruction(definitions.at(v.value)).type, fact.value};
        }
        if (unknown) return {};
        std::int64_t value = 0;
        return IRScalarFold::evaluate(i, values, value) ? Fact{Kind::Constant, value} : Fact{Kind::Variable, 0};
    };
    a.reachable.at(f.entry.value) = true;
    for (auto n = begin.at(f.entry.value); n < end.at(f.entry.value); ++n) enqueue(n);
    for (;;) {
        while (!pending.empty()) {
            budget.charge();
            const auto n = pending.front(); pending.pop_front(); queued[n] = false;
            const auto& i = instruction(n); const auto b = nodes[n].block;
            if (i.result.isValid()) update(i.result, evaluate(i, b));
            if (!i.isTerminator()) continue;
            if (i.opcode == IROpcode::CondBranch || i.opcode == IROpcode::Switch) {
                const auto condition = a.facts.at(i.operands[0].value);
                if (condition.kind == Kind::Unknown) continue;
                if (condition.kind == Kind::Constant) {
                    if (i.opcode == IROpcode::CondBranch) { edge(b, i.targets[condition.value != 0 ? 0 : 1]); continue; }
                    std::size_t selected = i.case_values.size();
                    for (std::size_t c = 0; c < i.case_values.size(); ++c) {
                        budget.charge();
                        // Case labels retain their evaluated value. Narrowing
                        // a label would invent matches (byte zero vs case 256).
                        if (i.case_values[c] == condition.value) { selected = c; break; }
                    }
                    if (selected < i.targets.size()) { edge(b, i.targets[selected]); continue; }
                    // No default is a partial dispatch; do not invent its
                    // missing-path semantics or incorrectly delete all edges.
                }
            }
            // In particular, LOOP's implicit R12 state is not an SSA constant.
            // Retain both the taken and final edges, even for count zero.
            for (const auto target : i.targets) edge(b, target);
        }
        bool unresolved = false;
        for (std::size_t v = 1; v < a.facts.size(); ++v) {
            budget.charge();
            if (a.facts[v].kind == Kind::Unknown && a.reachable.at(nodes.at(definitions[v]).block)) {
                update(IRValueId{static_cast<std::uint32_t>(v)}, {Kind::Variable, 0}); unresolved = true;
            }
        }
        if (!unresolved) break;
    }
    return a;
}

struct Graph {
    std::vector<std::set<std::uint32_t>> predecessors;
    std::vector<std::vector<std::uint32_t>> successors;
    std::vector<bool> reachable;
    explicit Graph(const IRFunction& f, Budget& budget) {
        predecessors.resize(f.blocks.size()); successors.resize(f.blocks.size()); reachable.assign(f.blocks.size(), false);
        for (const auto& b : f.blocks) for (const auto target : b.instructions.back().targets) {
            budget.charge();
            if (predecessors.at(target.value).insert(b.id.value).second) successors.at(b.id.value).push_back(target.value);
        }
        std::vector<std::uint32_t> work{f.entry.value}; reachable.at(f.entry.value) = true;
        while (!work.empty()) {
            const auto b = work.back(); work.pop_back();
            for (const auto s : successors.at(b)) {
                budget.charge();
                if (!reachable.at(s)) { reachable[s] = true; work.push_back(s); }
            }
        }
        for (auto& incoming : predecessors) {
            for (auto p = incoming.begin(); p != incoming.end();) {
                budget.charge();
                if (!reachable.at(*p)) p = incoming.erase(p);
                else ++p;
            }
        }
    }
};
bool coherentHardware(const IRFunction& f, const std::vector<bool>& reachable) {
    std::map<std::uint32_t, unsigned> live;
    for (const auto& b : f.blocks) for (const auto& i : b.instructions) if (i.loop_id) {
        auto& mask = live[i.loop_id];
        if (!reachable.at(b.id.value)) continue;
        mask |= i.opcode == IROpcode::HardwareLoop ? 1u : i.opcode == IROpcode::HardwareLoopEnd ? 2u : 4u;
        if (i.loop_target.isValid() && !reachable.at(i.loop_target.value)) return false;
    }
    for (const auto& p : live) if (p.second != 0 && p.second != 7) return false;
    return true;
}
IRInstruction constant(const IRInstruction& i, std::int64_t value) {
    IRInstruction out; out.type = i.type; out.result = i.result; out.immediate = value;
    out.source = i.source; out.in_plot_context = i.in_plot_context; return out;
}
IRInstruction branch(const IRInstruction& i, IRBlockId target) {
    IRInstruction out; out.opcode = IROpcode::Branch; out.targets = {target};
    out.source = i.source; out.in_plot_context = i.in_plot_context; return out;
}
IRValueId resolve(IRValueId v, std::vector<IRValueId>& aliases) {
    auto root = v;
    while (aliases.at(root.value).isValid()) root = aliases.at(root.value);
    while (aliases.at(v.value).isValid()) { const auto next = aliases[v.value]; aliases[v.value] = root; v = next; }
    return root;
}
void rewrite(IRFunction& f, const Analysis& analysis, Budget& budget) {
    // Never leave half of an R12/R13 save/restore scope behind. A future IR
    // shape that prunes only its latch conservatively retains the original CFG.
    const bool prune = coherentHardware(f, analysis.reachable);
    if (prune) for (auto& b : f.blocks) {
        auto& i = b.instructions.back();
        if (i.opcode != IROpcode::CondBranch && i.opcode != IROpcode::Switch) continue;
        const auto fact = analysis.facts.at(i.operands[0].value);
        if (i.opcode == IROpcode::CondBranch && (fact.kind == Kind::Constant || i.targets[0].value == i.targets[1].value))
            i = branch(i, i.targets[fact.kind == Kind::Constant && fact.value == 0 ? 1 : 0]);
        else if (i.opcode == IROpcode::Switch && fact.kind == Kind::Constant) {
            std::size_t selected = i.case_values.size();
            for (std::size_t c = 0; c < i.case_values.size(); ++c) {
                budget.charge();
                if (i.case_values[c] == fact.value) { selected = c; break; }
            }
            if (selected < i.targets.size()) i = branch(i, i.targets[selected]);
        }
    }
    Graph graph(f, budget);
    std::vector<IRValueId> aliases(static_cast<std::size_t>(f.value_count) + 1);
    for (auto& b : f.blocks) if (graph.reachable[b.id.value]) {
        std::vector<IRInstruction> phis, folded, body;
        for (auto& i : b.instructions) {
            budget.charge();
            if (i.opcode == IROpcode::Phi) {
                std::vector<IRValueId> values; std::vector<IRBlockId> edges;
                for (std::size_t n = 0; n < i.targets.size(); ++n) {
                    budget.charge();
                    if (graph.reachable.at(i.targets[n].value) && graph.predecessors[b.id.value].count(i.targets[n].value)) {
                        values.push_back(i.operands[n]); edges.push_back(i.targets[n]);
                    }
                }
                i.operands = std::move(values); i.targets = std::move(edges);
                if (i.operands.empty()) throw CompilerError("O2 SCCP: reachable PHI lost all incoming edges.", i.source);
                if (i.operands.size() == 1) { aliases.at(i.result.value) = i.operands[0]; continue; }
            }
            if (i.result.isValid() && analysis.facts.at(i.result.value).kind == Kind::Constant) {
                auto literal = constant(i, analysis.facts[i.result.value].value);
                if (i.opcode == IROpcode::Phi) folded.push_back(std::move(literal));
                else body.push_back(std::move(literal));
            } else if (i.opcode == IROpcode::Phi) phis.push_back(std::move(i));
            else body.push_back(std::move(i));
        }
        // Constants replacing PHIs belong AFTER the remaining PHI prefix.
        phis.insert(phis.end(), std::make_move_iterator(folded.begin()), std::make_move_iterator(folded.end()));
        phis.insert(phis.end(), std::make_move_iterator(body.begin()), std::make_move_iterator(body.end()));
        b.instructions = std::move(phis);
    }
    for (auto& b : f.blocks) if (graph.reachable[b.id.value]) for (auto& i : b.instructions)
        for (auto& v : i.operands) v = resolve(v, aliases);

    // Contract ordinary chains without deleting hardware/R13 entry blocks.
    // Such an entry may absorb its sole successor: its identity and the
    // incoming hardware edges stay fixed, before or after PHI edge splitting.
    std::set<std::uint32_t> protected_blocks;
    for (const auto& b : f.blocks) if (graph.reachable[b.id.value]) for (const auto& i : b.instructions) if (i.loop_id) {
        protected_blocks.insert(b.id.value);
        for (const auto t : i.targets) protected_blocks.insert(t.value);
        if (i.loop_target.isValid()) protected_blocks.insert(i.loop_target.value);
    }
    std::deque<std::uint32_t> pending;
    for (const auto& b : f.blocks) if (graph.reachable[b.id.value]) pending.push_back(b.id.value);
    while (!pending.empty()) {
        budget.charge();
        const auto from = pending.front(); pending.pop_front();
        if (!graph.reachable[from]) continue;
        auto& source = f.blocks[from]; const auto& terminal = source.instructions.back();
        if (terminal.opcode != IROpcode::Branch) continue;
        const auto to = terminal.targets[0].value;
        if (to == from || to == f.entry.value || protected_blocks.count(to) ||
            !graph.reachable.at(to) || graph.predecessors[to].size() != 1) continue;
        auto& target = f.blocks[to];
        if (target.instructions.front().opcode == IROpcode::Phi) continue;
        budget.charge(target.instructions.size());
        source.instructions.pop_back();
        source.instructions.insert(source.instructions.end(), std::make_move_iterator(target.instructions.begin()), std::make_move_iterator(target.instructions.end()));
        target.instructions.clear(); graph.reachable[to] = false;
        graph.successors[from] = graph.successors[to];
        for (const auto s : graph.successors[from]) {
            graph.predecessors[s].erase(to); graph.predecessors[s].insert(from);
            for (auto& phi : f.blocks[s].instructions) {
                if (phi.opcode != IROpcode::Phi) break;
                for (auto& incoming : phi.targets) { budget.charge(); if (incoming.value == to) incoming.value = from; }
            }
        }
        pending.push_back(from);
    }
    std::vector<IRBlockId> blocks(f.blocks.size());
    std::vector<IRValueId> values(aliases.size());
    std::uint32_t next_block = 0, next_value = 0;
    for (const auto& b : f.blocks) if (graph.reachable[b.id.value]) {
        blocks[b.id.value] = IRBlockId{next_block++};
        for (const auto& i : b.instructions) if (i.result.isValid()) values.at(i.result.value) = IRValueId{++next_value};
    }
    std::vector<IRBasicBlock> compact;
    for (auto& b : f.blocks) if (graph.reachable[b.id.value]) {
        b.id = blocks.at(b.id.value);
        for (auto& i : b.instructions) {
            for (auto& v : i.operands) {
                v = values.at(resolve(v, aliases).value);
                if (!v.isValid()) throw CompilerError("O2 SCCP: retained use lost its definition.", i.source);
            }
            if (i.result.isValid()) i.result = values.at(i.result.value);
            for (auto& t : i.targets) {
                t = blocks.at(t.value);
                if (!t.isValid()) throw CompilerError("O2 SCCP: retained edge lost its block.", i.source);
            }
            if (i.loop_target.isValid()) i.loop_target = blocks.at(i.loop_target.value);
        }
        compact.push_back(std::move(b));
    }
    f.entry = blocks.at(f.entry.value); f.blocks = std::move(compact); f.value_count = next_value;
}
}

void IRConditionalOptimizer::run(IRModule& module) {
    // Bound input before verifier graph allocation or cloning the function.
    for (const auto& function : module.functions) validateResources(function);
    IRVerifier::verify(module);
    for (auto& function : module.functions) {
        Budget budget;
        auto candidate = function;
        const auto analysis = analyze(candidate, budget);
        rewrite(candidate, analysis, budget);
        function = std::move(candidate);
    }
    IRVerifier::verify(module);
}
