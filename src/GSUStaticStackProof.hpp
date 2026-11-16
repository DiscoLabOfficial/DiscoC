#pragma once

#include "IR.hpp"
#include "CompilerError.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Opt-in (CompilerConfig::static_stack_proof): `main` is entered only by the
// runtime at its initial SP, which the linker checks against floor + bound().
// Covered functions are `main` and the internal functions reached only from
// covered callers, without recursion or address use. They emit no stack
// guard: each omitted guard is recorded as its requirement plus the depth
// below the function's entry SP, and each covered call as its depth. Then
//   need(f) = max(omitted guards, depth(call) + need(callee))
// bounds SP's descent below `main`'s entry at every omitted guard, so with
// floor + need(main) <= initial SP none of them could have failed, and the
// precise-fault behaviour of the remaining (uncovered) guards is unchanged.
// Depths are upper bounds: a block starts at the post-prologue depth plus four
// bytes for every stack-saved hardware loop, and straight-line emission order
// counts a superset of any path's pushes within one block.
class GSUStaticStackProof {
public:
    void clear() { *this = GSUStaticStackProof{}; }

    void plan(const IRModule& module) {
        clear();
        std::map<std::string, const IRFunction*> functions;
        std::map<std::string, std::string> by_name;
        for (const auto& f : module.functions) { functions.emplace(symbol(f), &f); by_name.emplace(f.name, symbol(f)); }
        if (!functions.count("main")) return;
        std::set<std::string> candidates, referenced;
        std::map<std::string, std::set<std::string>> callees, callers;
        for (const auto& f : module.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
            if (i.opcode == IROpcode::Call && functions.count(i.symbol)) {
                callees[symbol(f)].insert(i.symbol);
                callers[i.symbol].insert(symbol(f));
            } else if (i.opcode == IROpcode::Address && !i.symbol.empty()) {
                // Conservative: a same-named local also blocks the function.
                referenced.insert(i.symbol);
                const auto named = by_name.find(i.symbol);
                if (named != by_name.end()) referenced.insert(named->second);
            }
        }
        if (!callers["main"].empty() || referenced.count("main")) return;
        for (const auto& f : module.functions) {
            const auto name = symbol(f);
            const bool internal = !f.link_name.empty() && f.link_name.front() == '\x01' && f.name != "main";
            if (name == "main" || (internal && !referenced.count(name))) candidates.insert(name);
        }
        // Kahn's topological order over the whole call graph, O(V + E). A
        // function on or below a cycle is never ordered, hence never covered.
        // In this order every caller is decided before its callees, so one
        // pass covers exactly the functions whose callers are all covered.
        std::map<std::string, std::size_t> pending;
        for (const auto& f : functions) pending[f.first] = callers[f.first].size();
        std::vector<std::string> ready, order;
        for (const auto& entry : pending) if (!entry.second) ready.push_back(entry.first);
        while (!ready.empty()) {
            const auto current = ready.back(); ready.pop_back();
            order.push_back(current);
            for (const auto& next : callees[current]) if (--pending.at(next) == 0) ready.push_back(next);
        }
        std::set<std::string> covered;
        for (const auto& name : order) {
            if (!candidates.count(name)) continue;
            bool all = name == "main" || !callers[name].empty();
            if (name != "main") for (const auto& caller : callers[name]) all = all && covered.count(caller);
            if (all) covered.insert(name);
        }
        m_covered = std::move(covered);
    }

    bool covered(const std::string& function_symbol) const { return m_covered.count(function_symbol) != 0; }

    void enter(const std::string& function_symbol) {
        m_current = function_symbol;
        m_active = covered(function_symbol);
        m_depth = m_block_base = 0;
        if (m_active) m_records[function_symbol] = Record{};
    }
    void leave() { m_active = false; }
    bool active() const { return m_active; }

    void push(std::size_t bytes) { if (m_active) m_depth += static_cast<std::int64_t>(bytes); }
    void pop(std::size_t bytes) { if (m_active) m_depth -= static_cast<std::int64_t>(bytes); }
    // SP := FP. The prologue sets FP after saving R11 and R9.
    void restoreFrame() { if (m_active) m_depth = 4; }
    void beginBody(std::size_t saved_loop_bytes) {
        if (m_active) m_block_base = m_depth + static_cast<std::int64_t>(saved_loop_bytes);
    }
    void beginBlock() { if (m_active) m_depth = m_block_base; }

    void guard(std::size_t required) {
        auto& record = m_records.at(m_current);
        record.local = std::max(record.local, depth() + required);
        ++m_omitted;
    }
    void call(const std::string& callee) {
        if (m_active && covered(callee)) m_records.at(m_current).calls.emplace_back(depth(), callee);
    }

    // Bytes needed above the floor at `main`'s entry, or 0 if nothing was
    // omitted. Iterative post-order: covered calls form a DAG by plan().
    std::size_t bound() const {
        if (!m_omitted || !m_records.count("main")) return 0;
        std::map<std::string, std::size_t> need;
        std::set<std::string> open;
        std::vector<std::pair<std::string, bool>> work{{"main", false}};
        while (!work.empty()) {
            const auto item = work.back(); work.pop_back();
            if (need.count(item.first)) continue;
            const auto found = m_records.find(item.first);
            if (found == m_records.end()) throw CompilerError("Static stack proof: covered function was not emitted.", 1, 1);
            if (item.second) {
                std::size_t value = found->second.local;
                for (const auto& call : found->second.calls) value = std::max(value, call.first + need.at(call.second));
                need[item.first] = value; open.erase(item.first);
                continue;
            }
            if (!open.insert(item.first).second) throw CompilerError("Static stack proof: covered calls are recursive.", 1, 1);
            work.emplace_back(item.first, true);
            for (const auto& call : found->second.calls)
                if (!need.count(call.second)) work.emplace_back(call.second, false);
        }
        return need.at("main");
    }

private:
    struct Record {
        std::size_t local = 0;
        std::vector<std::pair<std::size_t, std::string>> calls; // depth at the call, callee
    };
    static std::string symbol(const IRFunction& f) { return f.link_name.empty() ? f.name : f.link_name; }
    std::size_t depth() const { return m_depth > 0 ? static_cast<std::size_t>(m_depth) : 0; }

    std::set<std::string> m_covered;
    std::map<std::string, Record> m_records;
    std::string m_current;
    bool m_active = false;
    std::int64_t m_depth = 0, m_block_base = 0;
    std::size_t m_omitted = 0;
};
