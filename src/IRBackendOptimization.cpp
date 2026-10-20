#include "IRCodeGenerator.hpp"

#include <algorithm>
#include <limits>

#include "ABI.hpp"
#include "GSUCostModel.hpp"
#include "IRControlFlow.hpp"

namespace {
std::string functionSymbol(const IRFunction& f) { return f.link_name.empty() ? f.name : f.link_name; }
std::string blockSymbol(const IRFunction& f, IRBlockId b) {
    return std::string(1, '\x01') + f.name + "#" + std::to_string(b.value);
}
std::size_t symbolOffset(const ObjectFile& object, const std::string& name) {
    for (const auto& s : object.symbol_table)
        if (s.section == SymbolSection::CODE && s.name == name) return s.offset;
    return object.code_section.size();
}
std::size_t functionEnd(const IRModule& module, const ObjectFile& object, const IRFunction& f) {
    const auto begin = symbolOffset(object, functionSymbol(f));
    auto end = object.code_section.size();
    for (const auto& other : module.functions) {
        const auto offset = symbolOffset(object, functionSymbol(other));
        if (offset > begin) end = std::min(end, offset);
    }
    for (const auto& s : object.symbol_table)
        if (s.section == SymbolSection::CODE && s.offset > begin &&
            s.name.compare(0, 19, std::string(1, '\x01') + "__disco_os_divmod_") == 0)
            end = std::min<std::size_t>(end, s.offset);
    return end;
}
}

const Analyzer::LocalSymbolTable& IRCodeGenerator::localSymbols(const IRFunction& function) const {
    if (function.storage_layout_valid) return function.storage_layout;
    const auto found = m_all_local_symbols.find(function.name);
    static const Analyzer::LocalSymbolTable empty;
    return found == m_all_local_symbols.end() ? empty : found->second;
}

void IRCodeGenerator::planCallClobbers(const IRModule& module) {
    m_call_clobbers.clear();
    // Exact allocated homes plus a conservative fixed-register envelope for
    // each internal callee. The envelope includes transitive hardware use;
    // an absent LOOP in this function is not evidence about its callees.
    constexpr std::uint16_t unknown = 0x7fff;
    if (module.functions.size() > 256) return;
    std::map<std::string, std::set<std::string>> calls;
    std::size_t work = 0;
    for (const auto& f : module.functions) {
        if (f.value_count > 4096 || f.blocks.size() > 256) { m_call_clobbers.clear(); return; }
        const auto key = functionSymbol(f);
        std::uint16_t mask = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 6) | (1u << 11);
        LinearScanAllocator allocator;
        const auto policy = m_function_policies.find(f.name);
        allocator.runGlobal(f, allocatableRegisters(f), policy == m_function_policies.end() ? m_allocation_policy : policy->second);
        for (const auto& location : allocator.locations()) if (location.second.has_register)
            mask |= static_cast<std::uint16_t>(1u << location.second.physical_register);
        std::map<std::uint32_t, const IRInstruction*> definitions;
        for (const auto& b : f.blocks) for (const auto& i : b.instructions)
            if (i.result.isValid()) definitions.emplace(i.result.value, &i);
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
            if (++work > 200000) { m_call_clobbers.clear(); return; }
            mask |= i.hardwareEffects().writes_registers;
            if (i.opcode == IROpcode::Call) calls[key].insert(i.symbol);
            if (isFarPointer(i.type) || i.type.pointer_level || i.opcode == IROpcode::PointerOffset ||
                i.opcode == IROpcode::HardwareLoop ||
                (i.opcode == IROpcode::Binary && i.operation == "*")) mask |= 1u << 4;
            if (i.opcode == IROpcode::DivMod || i.opcode == IROpcode::DivModResult ||
                (i.opcode == IROpcode::Binary && (i.operation == "/" || i.operation == "%")))
                mask |= (1u << 2) | (1u << 4);
            if ((i.opcode == IROpcode::LoadIndirect || i.opcode == IROpcode::Load) && !i.operands.empty() &&
                definitions.at(i.operands[0].value)->type.space == AddressSpace::ROM) mask |= 1u << 14;
            if (i.opcode == IROpcode::MemoryInitialize && i.initialization_values.size() >= 32)
                mask |= (1u << 12) | (1u << 13);
        }
        m_call_clobbers.emplace(key, mask);
    }
    // Monotone finite bitset propagation also covers recursive SCCs. A call
    // to an unresolved/external name is always the full volatile envelope.
    for (std::size_t round = 0; round <= module.functions.size(); ++round) {
        bool changed = false;
        for (const auto& edge : calls) {
            auto& mask = m_call_clobbers.at(edge.first);
            const auto before = mask;
            for (const auto& name : edge.second) {
                if (++work > 1000000) { m_call_clobbers.clear(); return; }
                const auto callee = m_call_clobbers.find(name);
                mask |= callee == m_call_clobbers.end() ? unknown : callee->second;
            }
            changed = changed || before != mask;
        }
        if (!changed) return;
    }
    m_call_clobbers.clear();
}

std::uint16_t IRCodeGenerator::callClobbers(const IRInstruction& instruction) const {
    const auto found = m_call_clobbers.find(instruction.symbol);
    return globallyOptimized() && found != m_call_clobbers.end() ? found->second : 0x7fff;
}

bool IRCodeGenerator::phiCopiesEmpty(std::uint32_t from, IRBlockId target) const {
    for (const auto& phi : m_current_function->blocks.at(target.value).instructions) {
        if (phi.opcode != IROpcode::Phi) break;
        const auto uses = m_use_counts.find(phi.result.value);
        if (uses == m_use_counts.end() || !uses->second) continue;
        for (std::size_t e = 0; e < phi.targets.size(); ++e) {
            if (phi.targets[e].value != from || phi.result.value == phi.operands[e].value) continue;
            const auto* a = m_register_allocator.find(phi.result);
            const auto* b = m_register_allocator.find(phi.operands[e]);
            if (a && b && a->has_register && b->has_register && a->physical_register == b->physical_register) continue;
            const auto x = m_spill_offsets.find(phi.result.value), y = m_spill_offsets.find(phi.operands[e].value);
            if (x != m_spill_offsets.end() && y != m_spill_offsets.end() && x->second == y->second) continue;
            return false;
        }
    }
    return true;
}

bool IRCodeGenerator::phiCopiesUseR6(std::uint32_t from, IRBlockId target) const {
    std::vector<std::pair<std::uint8_t, std::uint8_t>> moves;
    for (const auto& phi : m_current_function->blocks.at(target.value).instructions) {
        if (phi.opcode != IROpcode::Phi) break;
        const auto uses = m_use_counts.find(phi.result.value);
        if (uses == m_use_counts.end() || !uses->second) continue;
        for (std::size_t e = 0; e < phi.targets.size(); ++e) {
            if (phi.targets[e].value != from || phi.result.value == phi.operands[e].value) continue;
            const auto* a = m_register_allocator.find(phi.result);
            const auto* b = m_register_allocator.find(phi.operands[e]);
            if (a && b && a->has_register && b->has_register && a->physical_register == b->physical_register) continue;
            const auto x = m_spill_offsets.find(phi.result.value), y = m_spill_offsets.find(phi.operands[e].value);
            if (x != m_spill_offsets.end() && y != m_spill_offsets.end() && x->second == y->second) continue;
            // Mixed-memory cycles use checked stack saves, not R6. Their
            // scalar stores/reloads and rematerialized constants use R0/R3.
            if (!a || !b || !a->has_register || !b->has_register) return false;
            moves.push_back({a->physical_register, b->physical_register});
        }
    }
    if (moves.size() > 64) return true;
    while (!moves.empty()) {
        auto ready = moves.end();
        for (auto i = moves.begin(); i != moves.end(); ++i) {
            bool needed = false;
            for (auto j = moves.begin(); j != moves.end(); ++j)
                if (i != j && i->first == j->second) needed = true;
            if (!needed) { ready = i; break; }
        }
        if (ready == moves.end()) return true;
        moves.erase(ready);
    }
    return false;
}

IRBlockId IRCodeGenerator::threadedLoopEntry(IRBlockId target) const {
    if (!globallyOptimized()) return target;
    std::set<std::uint32_t> visited;
    for (unsigned depth = 0; depth < 32 && visited.insert(target.value).second; ++depth) {
        const auto& b = m_current_function->blocks.at(target.value);
        if (!m_register_allocator.boundaryStores(target.value).empty() ||
            !m_register_allocator.boundaryReloads(target.value).empty()) break;
        auto i = b.instructions.begin();
        while (i != b.instructions.end() && i->opcode == IROpcode::Phi) ++i;
        if (i == b.instructions.end() || i->opcode != IROpcode::Branch || i->targets.size() != 1 ||
            i + 1 != b.instructions.end() || !phiCopiesEmpty(target.value, i->targets[0])) break;
        target = i->targets[0];
    }
    return target;
}

void IRCodeGenerator::planRegionalLoopValues() {
    m_regional_loop_values.clear();
    const auto& f = *m_current_function;
    if (f.blocks.size() > 256 || f.value_count > 4096) return;
    const IRControlFlow cfg(f);
    std::map<std::uint32_t, std::uint32_t> definition_blocks;
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.result.isValid()) definition_blocks.emplace(i.result.value, b.id.value);
    std::size_t work = 0;
    for (const auto& setup_block : f.blocks) {
        const auto& setup = setup_block.instructions.back();
        if (setup.opcode != IROpcode::HardwareLoop || setup.targets.size() != 1 || !setup.loop_target.isValid() ||
            m_register_hardware_loops.count(setup.loop_id)) continue;
        const IRControlFlow::Loop* scope = nullptr;
        for (const auto& loop : cfg.loops) if (loop.blocks.count(setup.loop_target.value)) {
            if (!scope || loop.blocks.size() < scope->blocks.size()) scope = &loop;
        }
        if (!scope || scope->blocks.size() > 64) continue;
        auto region = scope->blocks;
        // PHI splitting may put a private initial-copy edge between setup
        // and the natural-loop header. Include that bounded one-way chain in
        // the same fixed-register/slot proof: setup values must survive it too.
        auto entry = setup.targets.front().value;
        std::set<std::uint32_t> entry_chain;
        while (!region.count(entry) && entry_chain.size() < 32 && entry_chain.insert(entry).second) {
            const auto& code = f.blocks.at(entry).instructions;
            auto i = code.begin();
            while (i != code.end() && i->opcode == IROpcode::Phi) ++i;
            if (i == code.end() || i->opcode != IROpcode::Branch || i + 1 != code.end()) break;
            entry = i->targets.front().value;
        }
        if (!region.count(entry) || region.size() + entry_chain.size() > 64) continue;
        region.insert(entry_chain.begin(), entry_chain.end());
        unsigned available = (1u << 4) | (1u << 6);
        bool closed = true, latch = false;
        std::map<std::uint32_t, unsigned> reads;
        std::set<int> written_slots;
        for (const auto b : region) {
            for (const auto p : cfg.predecessors.at(b)) {
                if (!region.count(p) && p != setup_block.id.value) closed = false;
                if (phiCopiesUseR6(p, IRBlockId{b})) available &= ~(1u << 6);
            }
            for (const auto& i : f.blocks[b].instructions) {
                if (++work > 1000000) { m_regional_loop_values.clear(); return; }
                if (i.result.isValid()) {
                    const auto spill = m_spill_offsets.find(i.result.value);
                    if (spill != m_spill_offsets.end()) written_slots.insert(spill->second);
                }
                for (const auto v : i.operands) {
                    ++reads[v.value];
                    // A scalar result does not imply scalar materialization:
                    // e.g. Cast can reload an external far pair into R0/R4.
                    // Validated IR is supported independently of the frontend.
                    if (isFarPointer(producer(v, i.source).type)) closed = false;
                }
                if (isFarPointer(i.type)) { closed = false; continue; }
                switch (i.opcode) {
                    case IROpcode::Phi:
                        break;
                    case IROpcode::Constant:
                    case IROpcode::Address:
                        if (i.type.pointer_level && (isFarPointer(i.type) || i.type.space != AddressSpace::RAM)) closed = false;
                        break;
                    case IROpcode::LoadIndirect: {
                        const auto& address = producer(i.operands[0], i.source);
                        if (!GSUCostModel::scalar(i.type) || isFarPointer(address.type) || address.type.space != AddressSpace::RAM ||
                            !provenAddress(i.operands[0], address.type, i.type.sizeInBytes)) closed = false;
                        break;
                    }
                    case IROpcode::StoreIndirect: {
                        const auto& address = producer(i.operands[0], i.source);
                        // Flat numeric pointers can alias any frame word.
                        // Only a direct, disjoint compiler-planned local has
                        // a strong enough identity for these invariant homes.
                        if (address.opcode != IROpcode::Address || !address.operation.empty() ||
                            !address.symbol_id.isValid() || !localSymbols(f).count(address.symbol_id) ||
                            !GSUCostModel::scalar(i.type)) closed = false;
                        if (!usesRegisterAllocation(i.operands[1])) available &= ~(1u << 6);
                        break;
                    }
                    case IROpcode::Binary: {
                        const auto* left = m_register_allocator.find(i.operands[0]);
                        const auto* right = m_register_allocator.find(i.operands[1]);
                        std::int64_t literal = 0;
                        const bool constant = constantValue(i.operands[1], literal);
                        const bool simple = i.operation == "+" || i.operation == "-" || i.operation == "&" ||
                            i.operation == "|" || i.operation == "^";
                        const bool compare = i.operation == "==" || i.operation == "!=" || i.operation == "<" ||
                            i.operation == ">" || i.operation == "<=" || i.operation == ">=";
                        const bool shift = (i.operation == "<<" || i.operation == ">>") && constant && literal >= 0 && literal < 16;
                        if (simple) {
                            const bool frame_right = (i.operation == "+" || i.operation == "-") &&
                                usesRegisterAllocation(i.result) && left && left->has_register &&
                                m_spill_offsets.count(i.operands[1].value) && i.type.base == BaseType::WORD;
                            if (!constant && !(right && right->has_register) && !frame_right) available &= ~(1u << 6);
                        } else if (compare) {
                            if (!(left && left->has_register) && !(right && right->has_register)) available &= ~(1u << 6);
                        } else if (!shift) available = 0;
                        break;
                    }
                    case IROpcode::Unary:
                    case IROpcode::BitExtract:
                    case IROpcode::Cast:
                        if (!GSUCostModel::scalar(i.type)) closed = false;
                        break;
                    case IROpcode::PointerOffset:
                        if (i.operands.size() != 2 || !m_address_proof.provesOffset(i) ||
                            producer(i.operands[1], i.source).opcode != IROpcode::Constant ||
                            i.type.space != AddressSpace::RAM) closed = false;
                        break;
                    case IROpcode::SetColor:
                        if (i.operation == "rom.byte") closed = false;
                        break;
                    case IROpcode::PlotCoordinateRead:
                    case IROpcode::PlotCoordinateWrite:
                    case IROpcode::Plot:
                    case IROpcode::PlotBegin:
                    case IROpcode::PlotEnd:
                    case IROpcode::CMode:
                    case IROpcode::Cache:
                    case IROpcode::Branch:
                    case IROpcode::CondBranch:
                        break;
                    case IROpcode::HardwareLoopEnd:
                        latch = latch || i.loop_id == setup.loop_id;
                        if (i.loop_id != setup.loop_id) closed = false;
                        break;
                    default: closed = false; break;
                }
            }
        }
        if (!closed || !latch || !available) continue;
        std::vector<std::uint32_t> candidates;
        for (const auto& use : reads) {
            const auto spill = m_spill_offsets.find(use.first);
            if (spill == m_spill_offsets.end() || region.count(definition_blocks.at(use.first)) ||
                written_slots.count(spill->second) || !GSUCostModel::scalar(m_values.at(use.first)->type)) continue;
            // The definition must dominate the setup, not just be located in
            // an earlier emitted block. Its frame home is already initialized.
            if (!cfg.dominates(definition_blocks.at(use.first), setup_block.id.value)) continue;
            candidates.push_back(use.first);
        }
        std::sort(candidates.begin(), candidates.end(), [&](std::uint32_t a, std::uint32_t b) {
            if (reads.at(a) != reads.at(b)) return reads.at(a) > reads.at(b);
            return m_spill_offsets.at(a) < m_spill_offsets.at(b);
        });
        RegionalLoopValues selected; selected.blocks = region;
        for (const auto reg : {std::uint8_t{4}, std::uint8_t{6}}) {
            if (!(available & (1u << reg)) || candidates.empty()) continue;
            selected.values.push_back({IRValueId{candidates.front()}, reg});
            candidates.erase(candidates.begin());
        }
        if (!selected.values.empty()) m_regional_loop_values.emplace(setup.loop_id, std::move(selected));
    }
}

int IRCodeGenerator::regionalRegister(IRValueId value) const {
    for (const auto& loop : m_regional_loop_values) if (loop.second.blocks.count(static_cast<std::uint32_t>(m_current_block_index)))
        for (const auto& entry : loop.second.values) if (entry.first.value == value.value) return entry.second;
    return -1;
}

void IRCodeGenerator::emitRegionalLoopValues(std::uint32_t loop_id) {
    const auto found = m_regional_loop_values.find(loop_id);
    if (found == m_regional_loop_values.end()) return;
    auto values = found->second.values;
    std::sort(values.begin(), values.end(), [&](const std::pair<IRValueId, std::uint8_t>& a,
                                               const std::pair<IRValueId, std::uint8_t>& b) {
        return m_spill_offsets.at(a.first.value) < m_spill_offsets.at(b.first.value);
    });
    int previous = 0; bool have_address = false;
    for (const auto& entry : values) {
        const auto offset = m_spill_offsets.at(entry.first.value);
        if (have_address && offset == previous + 2) { emitByte(0xd3); emitByte(0xd3); }
        else {
            emitRegisterLiteral(3, static_cast<std::uint16_t>(offset));
            emitByte(0xb9); emitByte(0x13); emitByte(0x53);
        }
        emitByte(static_cast<std::uint8_t>(0x10 | entry.second)); emitByte(0x43);
        m_last_ram_word_address.rememberFrame(offset);
        previous = offset; have_address = true;
    }
    m_accumulator_value = IRValueId{};
}

std::uint64_t IRCodeGenerator::candidateSpeed(const IRModule& module, const ObjectFile& object,
                                            const IRFunction* only) const {
    std::uint64_t score = 0;
    for (const auto& f : module.functions) {
        if (only && only->name != f.name) continue;
        const auto begin = symbolOffset(object, functionSymbol(f)), end = functionEnd(module, object, f);
        if (begin >= end) continue;
        IRControlFlow cfg(f);
        score = GSUCost::saturatedAdd(score, end - begin);
        for (std::size_t n = 0; n < f.blocks.size(); ++n) {
            const auto start = symbolOffset(object, blockSymbol(f, f.blocks[n].id));
            const auto next = n + 1 < f.blocks.size() ? symbolOffset(object, blockSymbol(f, f.blocks[n + 1].id)) : end;
            if (next < start || next > end) continue;
            // Actual scheduled bytes weighted by structural loop frequency.
            // This is only a deterministic speed tie-break, never a timing claim.
            score = GSUCost::saturatedAdd(score, GSUCost::saturatedMultiply(next - start,
                GSUCostModel::blockWeight(cfg.loop_depth.at(f.blocks[n].id.value)) - 1));
        }
    }
    return score;
}

void IRCodeGenerator::resetCandidateEmissionState() noexcept {
    m_current_function = nullptr; m_values.clear();
    m_collect_function_candidates = false; m_share_division = false; m_pool_enabled = true;
    m_allow_auto_cache = true; m_force_long_branches = false;
    m_function_policies.clear(); m_function_division_policies.clear(); m_function_pool_policies.clear();
    m_division_candidates.clear(); m_division_helpers.clear();
    m_long_branches.clear(); m_long_branch_functions.clear(); m_relaxation_pending.clear();
}

void IRCodeGenerator::recordCandidateMetrics(const IRModule& module, GeneratedCandidate& candidate) const {
    candidate.function_metrics.clear();
    if (module.functions.size() > 64) return;
    std::size_t instructions = 0;
    for (const auto& f : module.functions) for (const auto& b : f.blocks) {
        if (b.instructions.size() > 16000 - instructions) return;
        instructions += b.instructions.size();
    }
    const auto present = [&](const std::string& name) {
        return std::any_of(candidate.object.symbol_table.begin(), candidate.object.symbol_table.end(),
            [&](const SymbolEntry& s) { return s.section == SymbolSection::CODE && s.name == name; });
    };
    for (const auto& f : module.functions) {
        const auto begin = symbolOffset(candidate.object, functionSymbol(f));
        const auto end = functionEnd(module, candidate.object, f);
        if (!present(functionSymbol(f)) || begin >= end || end > candidate.object.code_section.size()) {
            candidate.function_metrics.clear(); return;
        }
        bool valid = true;
        for (std::size_t n = 0; n < f.blocks.size(); ++n) {
            const auto name = blockSymbol(f, f.blocks[n].id);
            const auto start = symbolOffset(candidate.object, name);
            const auto next = n + 1 < f.blocks.size() ? symbolOffset(candidate.object, blockSymbol(f, f.blocks[n + 1].id)) : end;
            if (!present(name) || start < begin || start > next || next > end) { valid = false; break; }
        }
        // Missing/invalid evidence must never become a partial or zero score.
        if (!valid) { candidate.function_metrics.clear(); return; }
        FunctionMetric metric;
        metric.speed_score = candidateSpeed(module, candidate.object, &f);
        metric.no_spills = std::none_of(candidate.issues.begin(), candidate.issues.end(),
            [&](const SpillIssue& issue) { return issue.function == f.name; });
        candidate.function_metrics.emplace(f.name, metric);
    }
}

bool IRCodeGenerator::admissibleMetric(const FunctionMetric& proposed, const FunctionMetric& legacy) const {
    if (m_config.require_no_spills && proposed.no_spills != legacy.no_spills) return proposed.no_spills;
    return proposed.speed_score <= legacy.speed_score;
}

bool IRCodeGenerator::admissibleExtension(const GeneratedCandidate& proposed, const CandidateReference& legacy) const {
    if (legacy.function_metrics.empty() || proposed.function_metrics.size() != legacy.function_metrics.size()) return false;
    for (const auto& entry : legacy.function_metrics) if (!proposed.function_metrics.count(entry.first)) return false;
    // Complete evidence comes first; global strict eligibility then outranks
    // local proxy differences, just as it outranks payload bytes in selection.
    if (m_config.require_no_spills && proposed.issues.empty() != legacy.no_spills) return proposed.issues.empty();
    for (const auto& entry : legacy.function_metrics) {
        const auto metric = proposed.function_metrics.find(entry.first);
        if (metric == proposed.function_metrics.end() || !admissibleMetric(metric->second, entry.second)) return false;
    }
    return admissibleMetric({proposed.speed_score, proposed.issues.empty()}, {legacy.speed_score, legacy.no_spills});
}

void IRCodeGenerator::considerFunctionCandidates(const IRModule& module, const GeneratedCandidate& candidate,
    const CandidateReference* legacy) {
    if (!m_collect_function_candidates) return;
    for (const auto& f : module.functions) {
        if (legacy) {
            const auto proposed = candidate.function_metrics.find(f.name), reference = legacy->function_metrics.find(f.name);
            if (proposed == candidate.function_metrics.end() || reference == legacy->function_metrics.end() ||
                !admissibleMetric(proposed->second, reference->second)) continue;
        }
        const auto begin = symbolOffset(candidate.object, functionSymbol(f));
        const auto end = functionEnd(module, candidate.object, f);
        if (end < begin) continue;
        std::map<std::size_t, std::size_t> data_ranges;
        for (const auto& array : f.immutable_local_arrays) {
            const auto label = std::string(GSUAbi::ImmutablePoolPrefix) + f.name + "." + std::to_string(array.first.value);
            for (const auto& symbol : candidate.object.symbol_table) if (symbol.section == SymbolSection::DATA && symbol.name == label)
                data_ranges[symbol.offset] = std::max(data_ranges[symbol.offset], array.second.bytes.size());
        }
        std::size_t data_bytes = 0, previous_end = 0;
        bool first = true;
        for (const auto& range : data_ranges) {
            // Count the actual owned DATA ranges (deduplicated aliases once),
            // including alignment holes between adjacent arrays. Cross-function
            // sharing and final CODE/DATA padding are measured by re-emission.
            if (!first && range.first >= previous_end && range.first - previous_end <= 1)
                data_bytes += range.first - previous_end;
            data_bytes += range.second; previous_end = range.first + range.second; first = false;
        }
        const auto bytes = end - begin + data_bytes;
        const bool no_spills = std::none_of(candidate.issues.begin(), candidate.issues.end(),
            [&](const SpillIssue& issue) { return issue.function == f.name; });
        const auto speed = candidateSpeed(module, candidate.object, &f);
        const auto current = m_function_candidates.find(f.name);
        if (current != m_function_candidates.end()) {
            const auto& old = current->second;
            if (m_config.require_no_spills && old.no_spills != no_spills) { if (old.no_spills) continue; }
            else if (bytes > old.bytes || (bytes == old.bytes && speed >= old.speed_score)) continue;
        }
        FunctionCandidate best;
        best.function = f; best.policy = m_allocation_policy; best.shared_division = m_share_division;
        best.pool_enabled = m_pool_enabled;
        best.no_spills = no_spills; best.bytes = bytes; best.speed_score = speed;
        m_function_candidates[f.name] = std::move(best);
    }
}

IRCodeGenerator::GeneratedCandidate IRCodeGenerator::generateMixedCandidate(const IRModule& module) {
    struct Cleanup {
        IRCodeGenerator& generator;
        ~Cleanup() { generator.resetCandidateEmissionState(); }
    } cleanup{*this};
    auto mixed = module;
    m_function_policies.clear(); m_function_division_policies.clear(); m_function_pool_policies.clear(); m_division_candidates.clear();
    std::map<bool, unsigned> divisions;
    for (auto& f : mixed.functions) {
        const auto chosen = m_function_candidates.find(f.name);
        if (chosen == m_function_candidates.end()) fail("IR codegen: missing bounded function candidate.", Token(TokenType::UNKNOWN, f.name, 1, 1));
        f = chosen->second.function;
        m_function_policies[f.name] = chosen->second.policy;
        m_function_division_policies[f.name] = chosen->second.shared_division;
        m_function_pool_policies[f.name] = chosen->second.pool_enabled;
        if (chosen->second.shared_division) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
            if (i.opcode == IROpcode::DivMod || (i.opcode == IROpcode::Binary && (i.operation == "/" || i.operation == "%")))
                ++divisions[i.type.is_unsigned];
    }
    for (const auto& count : divisions) if (count.second >= 2) m_division_candidates.insert(count.first);
    IRVerifier::verify(mixed);
    m_force_long_branches = false; m_long_branches.clear(); m_long_branch_functions.clear();
    const auto emit = [&]() {
        for (unsigned pass = 0; pass < 8; ++pass) {
            try { return generateInternal(mixed); }
            catch (const NeedsLongBranch&) { continue; }
        }
        m_long_branch_functions.insert(m_relaxation_pending.begin(), m_relaxation_pending.end());
        return generateInternal(mixed);
    };
    m_allow_auto_cache = true;
    auto result = emit();
    if (m_had_cache_probe) {
        m_allow_auto_cache = false;
        m_long_branches.clear(); m_long_branch_functions.clear();
        auto uncached = emit();
        if (preferCandidate(uncached, result)) result = std::move(uncached);
    }
    m_allow_auto_cache = true; m_force_long_branches = false;
    m_function_policies.clear(); m_function_division_policies.clear(); m_function_pool_policies.clear();
    return result;
}
