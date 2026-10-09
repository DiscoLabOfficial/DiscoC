#include "IRControlFlow.hpp"

#include <algorithm>
#include <queue>

namespace {
void limit(bool condition) {
    if (condition) throw CompilerError("O2 analysis resource limit exceeded (CFG/liveness).", 1, 1);
}
void add(std::vector<std::uint32_t>& values, std::uint32_t value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}
}

IRControlFlow::IRControlFlow(const IRFunction& function) {
    const auto n = function.blocks.size();
    limit(n == 0 || n > 8192 || function.value_count > 100000);
    successors.resize(n); predecessors.resize(n); children.resize(n); frontier.resize(n);
    reachable.assign(n, false); loop_depth.assign(n, 0);
    immediate_dominator.assign(n, IRBlockId::Invalid);
    live_in.resize(n); live_out.resize(n);
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if ((instruction.opcode == IROpcode::HardwareLoop || instruction.opcode == IROpcode::HardwareLoopEnd) && instruction.targets.empty())
                throw CompilerError("O2 CFG requires exposed hardware-loop backedges.", instruction.source);
            if (instruction.opcode == IROpcode::Phi) continue;
            for (const auto target : instruction.targets) {
                if (!target.isValid() || target.value >= n) throw CompilerError("O2 CFG: invalid edge.", instruction.source);
                add(successors.at(block.id.value), target.value);
                add(predecessors.at(target.value), block.id.value);
            }
        }
    }
    std::vector<std::uint32_t> work{function.entry.value};
    while (!work.empty()) {
        const auto b = work.back(); work.pop_back();
        if (reachable.at(b)) continue;
        reachable[b] = true;
        work.insert(work.end(), successors[b].begin(), successors[b].end());
    }
    const auto words = (n + 63) / 64;
    m_dominators.assign(n, std::vector<std::uint64_t>(words, 0));
    std::vector<std::uint64_t> all(words, 0);
    for (std::size_t b = 0; b < n; ++b) if (reachable[b]) all[b / 64] |= std::uint64_t{1} << (b % 64);
    for (std::size_t b = 0; b < n; ++b) if (reachable[b]) m_dominators[b] = all;
    auto& entry = m_dominators.at(function.entry.value);
    std::fill(entry.begin(), entry.end(), 0);
    entry[function.entry.value / 64] |= std::uint64_t{1} << (function.entry.value % 64);
    bool changed = true;
    std::size_t rounds = 0;
    while (changed) {
        limit(++rounds > n + 1); changed = false;
        for (std::size_t b = 0; b < n; ++b) {
            if (!reachable[b] || b == function.entry.value) continue;
            auto next = all;
            for (const auto p : predecessors[b]) if (reachable[p])
                for (std::size_t w = 0; w < words; ++w) next[w] &= m_dominators[p][w];
            next[b / 64] |= std::uint64_t{1} << (b % 64);
            if (next != m_dominators[b]) { m_dominators[b] = std::move(next); changed = true; }
        }
    }
    // Strict dominators form a chain. The one with greatest dominator count
    // is the nearest; this avoids recursive dominator-tree construction.
    std::vector<std::size_t> ranks(n, 0);
    for (std::size_t b = 0; b < n; ++b) for (auto bits : m_dominators[b])
        while (bits) { bits &= bits - 1; ++ranks[b]; }
    for (std::size_t b = 0; b < n; ++b) {
        if (!reachable[b] || b == function.entry.value) continue;
        std::uint32_t parent = function.entry.value;
        for (std::size_t d = 0; d < n; ++d)
            if (d != b && dominates(d, b) && ranks[d] > ranks[parent]) parent = static_cast<std::uint32_t>(d);
        immediate_dominator[b] = parent; children[parent].push_back(static_cast<std::uint32_t>(b));
    }
    // Some predecessors appear later in physical order. All idoms must be
    // complete before walking dominance frontiers.
    for (auto& values : frontier) values.clear();
    for (std::size_t b = 0; b < n; ++b) if (reachable[b] && predecessors[b].size() > 1)
        for (const auto p : predecessors[b]) {
            if (!reachable[p]) continue;
            auto runner = p;
            while (runner != immediate_dominator[b] && runner != IRBlockId::Invalid) {
                add(frontier[runner], static_cast<std::uint32_t>(b)); runner = immediate_dominator[runner];
            }
        }

    std::map<std::uint32_t, Loop> natural;
    for (std::size_t b = 0; b < n; ++b) if (reachable[b]) for (const auto h : successors[b]) {
        if (!dominates(h, b)) continue;
        auto inserted = natural.emplace(h, Loop{h, {h}, {}});
        auto& loop = inserted.first->second; loop.latches.insert(static_cast<std::uint32_t>(b));
        work = {static_cast<std::uint32_t>(b)};
        while (!work.empty()) {
            const auto current = work.back(); work.pop_back();
            if (!loop.blocks.insert(current).second || current == h) continue;
            for (const auto p : predecessors[current]) if (reachable[p]) work.push_back(p);
        }
    }
    for (auto& item : natural) {
        for (const auto b : item.second.blocks) ++loop_depth[b];
        loops.push_back(std::move(item.second));
    }
    std::vector<std::set<std::uint32_t>> uses(n), defs(n), phi_defs(n);
    for (const auto& block : function.blocks) for (const auto& i : block.instructions) {
        if (i.opcode != IROpcode::Phi) {
            for (const auto operand : i.operands)
                if (!defs[block.id.value].count(operand.value)) uses[block.id.value].insert(operand.value);
        } else if (i.result.isValid()) phi_defs[block.id.value].insert(i.result.value);
        if (i.result.isValid()) defs[block.id.value].insert(i.result.value);
    }
    changed = true; rounds = 0;
    while (changed) {
        limit(++rounds > n + 2); changed = false;
        std::size_t entries = 0;
        for (std::size_t reverse = n; reverse > 0; --reverse) {
            const auto b = reverse - 1;
            if (!reachable[b]) continue;
            std::set<std::uint32_t> out;
            for (const auto s : successors[b]) {
                for (const auto id : live_in[s]) if (!phi_defs[s].count(id)) out.insert(id);
                for (const auto& i : function.blocks[s].instructions) {
                    if (i.opcode != IROpcode::Phi) break;
                    for (std::size_t edge = 0; edge < i.targets.size(); ++edge)
                        if (i.targets[edge].value == b) out.insert(i.operands.at(edge).value);
                }
            }
            auto in = uses[b];
            for (const auto id : out) if (!defs[b].count(id)) in.insert(id);
            if (in != live_in[b] || out != live_out[b]) {
                live_in[b] = std::move(in); live_out[b] = std::move(out); changed = true;
            }
            entries += live_in[b].size() + live_out[b].size();
            limit(entries > 1000000);
        }
    }
}

bool IRControlFlow::dominates(std::size_t dominator, std::size_t block) const {
    return reachable.at(block) && (m_dominators.at(block).at(dominator / 64) & (std::uint64_t{1} << (dominator % 64))) != 0;
}

void IRControlFlow::exposeHardwareLoops(IRFunction& function) {
    bool needed = false;
    for (const auto& b : function.blocks) for (const auto& i : b.instructions)
        needed = needed || (i.opcode == IROpcode::HardwareLoop && i.targets.empty());
    if (!needed) return;
    std::vector<IRBasicBlock> blocks;
    std::vector<IRBlockId> first(function.blocks.size());
    std::map<std::uint32_t, IRBlockId> bodies, starts, ends;
    const auto nextBlock = [&](const std::string& label) {
        limit(blocks.size() >= 8192);
        IRBlockId id{static_cast<std::uint32_t>(blocks.size())};
        blocks.push_back({id, label, {}}); return id;
    };
    for (const auto& old : function.blocks) {
        auto current = nextBlock(old.label); first.at(old.id.value) = current;
        for (const auto& original : old.instructions) {
            auto i = original;
            if ((i.opcode == IROpcode::HardwareLoop || i.opcode == IROpcode::HardwareLoopEnd) && i.targets.empty()) {
                if (!i.loop_id) throw CompilerError("O2 CFG: hardware-loop pair has no stable ID.", i.source);
                const auto from = current;
                current = nextBlock(i.opcode == IROpcode::HardwareLoop ? "hardware.body" : "hardware.exit");
                i.targets = {current};
                if (i.opcode == IROpcode::HardwareLoop) { bodies[i.loop_id] = current; starts[i.loop_id] = from; }
                else {
                    ends[i.loop_id] = from;
                    IRInstruction leave; leave.opcode = IROpcode::HardwareLoopLeave;
                    leave.loop_id = i.loop_id; leave.source = i.source; leave.in_plot_context = i.in_plot_context;
                    blocks[current.value].instructions.push_back(std::move(leave));
                }
                blocks[from.value].instructions.push_back(std::move(i));
            } else {
                // Ordinary branch targets still refer to the original block IDs.
                blocks[current.value].instructions.push_back(std::move(i));
            }
        }
    }
    for (auto& b : blocks) for (auto& i : b.instructions) {
        if (i.opcode != IROpcode::HardwareLoop && i.opcode != IROpcode::HardwareLoopEnd)
            for (auto& target : i.targets) target = first.at(target.value);
    }
    for (const auto& pair : starts) {
        const auto end = ends.find(pair.first);
        if (end == ends.end()) throw CompilerError("O2 CFG: unmatched hardware-loop pair.", 1, 1);
        auto& setup = blocks[pair.second.value].instructions.back();
        setup.loop_target = bodies.at(pair.first);
        auto& latch = blocks[end->second.value].instructions.back();
        latch.targets.insert(latch.targets.begin(), setup.loop_target);
    }
    function.entry = first.at(function.entry.value); function.blocks = std::move(blocks);
}

void IRControlFlow::splitPhiEdges(IRFunction& function) {
    IRControlFlow cfg(function);
    const auto n = function.blocks.size();
    for (std::size_t successor = 0; successor < n; ++successor) {
        if (function.blocks[successor].instructions.front().opcode != IROpcode::Phi) continue;
        for (const auto predecessor : cfg.predecessors[successor]) {
            auto& terminal = function.blocks[predecessor].instructions.back();
            // Already a single unconditional copy edge. No redundant split.
            if (terminal.opcode == IROpcode::Branch && cfg.successors[predecessor].size() == 1) continue;
            limit(function.blocks.size() >= 8192);
            const IRBlockId edge{static_cast<std::uint32_t>(function.blocks.size())};
            for (auto& target : terminal.targets) if (target.value == successor) target = edge;
            if (terminal.opcode == IROpcode::HardwareLoopEnd) {
                const auto loop = terminal.loop_id;
                for (auto& b : function.blocks) for (auto& setup : b.instructions)
                    if (setup.opcode == IROpcode::HardwareLoop && setup.loop_id == loop) setup.loop_target = terminal.targets[0];
            }
            for (auto& phi : function.blocks[successor].instructions) {
                if (phi.opcode != IROpcode::Phi) break;
                for (auto& target : phi.targets) if (target.value == predecessor) target = edge;
            }
            IRInstruction branch; branch.opcode = IROpcode::Branch; branch.targets = {IRBlockId{static_cast<std::uint32_t>(successor)}};
            branch.source = terminal.source; branch.in_plot_context = terminal.in_plot_context;
            function.blocks.push_back({edge, "phi.edge", {branch}});
        }
    }
}

void IRControlFlow::layoutHotBlocks(IRFunction& function) {
    IRControlFlow cfg(function);
    if (cfg.loops.size() > 32) return; // Bounded optional layout analysis; correctness does not depend on it.
    std::vector<std::uint32_t> order;
    for (const auto& b : function.blocks) order.push_back(b.id.value);
    // Outer regions first, inner regions second. Skip overlapping non-nested
    // loops: no assumed branch probabilities or arbitrary irreducible layout.
    std::sort(cfg.loops.begin(), cfg.loops.end(), [](const Loop& a, const Loop& b) {
        return a.blocks.size() != b.blocks.size() ? a.blocks.size() > b.blocks.size() : a.header < b.header;
    });
    for (const auto& loop : cfg.loops) {
        bool nested = true;
        for (const auto& other : cfg.loops) {
            bool intersects = false;
            for (const auto b : loop.blocks) intersects = intersects || other.blocks.count(b) != 0;
            if (intersects && !std::includes(loop.blocks.begin(), loop.blocks.end(), other.blocks.begin(), other.blocks.end()) &&
                !std::includes(other.blocks.begin(), other.blocks.end(), loop.blocks.begin(), loop.blocks.end())) nested = false;
        }
        if (!nested || (loop.blocks.count(function.entry.value) && loop.header != function.entry.value)) continue;
        std::vector<std::uint32_t> next, region{loop.header};
        for (const auto b : order) if (loop.blocks.count(b) && b != loop.header) region.push_back(b);
        bool inserted = false;
        for (const auto b : order) {
            if (loop.blocks.count(b)) {
                if (!inserted) { next.insert(next.end(), region.begin(), region.end()); inserted = true; }
            } else next.push_back(b);
        }
        order = std::move(next);
    }
    std::vector<IRBlockId> ids(function.blocks.size());
    for (std::size_t n = 0; n < order.size(); ++n) ids.at(order[n]) = IRBlockId{static_cast<std::uint32_t>(n)};
    std::vector<IRBasicBlock> blocks;
    for (const auto old : order) {
        auto b = std::move(function.blocks.at(old)); b.id = ids.at(old);
        for (auto& i : b.instructions) {
            for (auto& target : i.targets) target = ids.at(target.value);
            if (i.loop_target.isValid()) i.loop_target = ids.at(i.loop_target.value);
        }
        blocks.push_back(std::move(b));
    }
    function.entry = ids.at(function.entry.value); function.blocks = std::move(blocks);
}
