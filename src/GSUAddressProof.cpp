#include "GSUAddressProof.hpp"

#include <algorithm>
#include <array>

#include "ABI.hpp"
#include "IRControlFlow.hpp"
#include "IRScalarFold.hpp"

namespace {
bool nearRam(const Type& type) {
    return type.pointer_level > 0 && !isFarPointer(type) && type.space == AddressSpace::RAM;
}
}

GSUAddressProof::Fact GSUAddressProof::scalarRange(const Type& type) {
    if (!IRScalarFold::scalar(type)) return {};
    if (type.base == BaseType::BOOL) return {Kind::Scalar, 0, 1, 1};
    if (type.base == BaseType::BYTE)
        return {Kind::Scalar, type.is_unsigned ? 0 : -128, type.is_unsigned ? 255 : 127, 1};
    return {Kind::Scalar, type.is_unsigned ? 0 : -32768, type.is_unsigned ? 65535 : 32767, 1};
}

unsigned GSUAddressProof::commonAlignment(unsigned alignment, std::int64_t offset) {
    // All participating alignments are powers of two. Offsets are bounded to
    // at most one bank; taking their magnitude cannot overflow int64_t.
    const auto magnitude = static_cast<std::uint64_t>(offset < 0 ? -offset : offset);
    while (alignment > 1 && magnitude % alignment) alignment >>= 1;
    return alignment;
}

GSUAddressProof::Fact GSUAddressProof::get(IRValueId value) const {
    if (!value.isValid() || value.value >= m_facts.size()) return {};
    return m_facts[value.value];
}

GSUAddressProof::Fact GSUAddressProof::get(IRValueId value, std::uint32_t block) const {
    auto fact = get(value);
    if (fact.kind != Kind::Scalar || block >= m_constraints.size()) return fact;
    const auto constraint = m_constraints[block].find(value.value);
    if (constraint == m_constraints[block].end()) return fact;
    const auto low = std::max(fact.low, constraint->second.low);
    const auto high = std::min(fact.high, constraint->second.high);
    // Contradictory conditions describe a dead path. Do not manufacture a
    // pointer proof from an empty interval; retain conservative facts instead.
    if (low <= high) { fact.low = low; fact.high = high; }
    return fact;
}

void GSUAddressProof::refineControlFlow(
    const IRFunction& function, const std::vector<const IRInstruction*>& definitions) {
    // Dominance/loop recognition is intentionally smaller than the ordinary
    // scalar analysis. Large functions simply retain their runtime checks.
    if (function.blocks.empty() || function.blocks.size() > 512 || function.value_count > 8192 ||
        !function.entry.isValid() || function.entry.value >= function.blocks.size()) return;
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
        if ((instruction.opcode == IROpcode::HardwareLoop || instruction.opcode == IROpcode::HardwareLoopEnd) &&
            instruction.targets.empty()) return;
    const IRControlFlow cfg(function);
    m_inductions.resize(m_facts.size());
    m_constraints.resize(function.blocks.size());
    const auto definition = [&](IRValueId value) -> const IRInstruction* {
        return value.isValid() && value.value < definitions.size() ? definitions[value.value] : nullptr;
    };
    const auto literal = [&](IRValueId value, std::int64_t& result) {
        const auto* source = definition(value);
        if (!source || source->opcode != IROpcode::Constant || !IRScalarFold::scalar(source->type)) return false;
        result = ConstantEvaluator::convert(source->immediate, source->type); return true;
    };
    const auto sameInteger = [&](const Type& a, const Type& b) {
        return IRScalarFold::scalar(a) && IRScalarFold::scalar(b) && a.base == b.base && a.is_unsigned == b.is_unsigned;
    };
    const auto predecessorThroughEdges = [&](std::uint32_t block) {
        // PHI lowering splits the hardware setup/backedge through empty
        // branch-only blocks. Those copies do not add loop iterations, but
        // no conditional edge, value definition or observable work is skipped.
        for (std::size_t depth = 0; depth < function.blocks.size(); ++depth) {
            const auto& code = function.blocks[block].instructions;
            if (code.size() != 1 || code.back().opcode != IROpcode::Branch ||
                cfg.predecessors[block].size() != 1) return block;
            block = cfg.predecessors[block][0];
        }
        return IRBlockId::Invalid;
    };
    const auto throughEdgesTo = [&](std::uint32_t block, std::uint32_t destination) {
        for (std::size_t depth = 0; depth < function.blocks.size(); ++depth) {
            if (block == destination) return true;
            const auto& code = function.blocks[block].instructions;
            if (code.size() != 1 || code.back().opcode != IROpcode::Branch || code.back().targets.size() != 1) return false;
            block = code.back().targets[0].value;
        }
        return false;
    };
    std::size_t work = 0;
    for (const auto& loop : cfg.loops) {
        if (loop.latches.size() != 1 || cfg.predecessors[loop.header].size() != 2) continue;
        const auto latch = *loop.latches.begin();
        const auto& header = function.blocks[loop.header];
        if (header.instructions.empty()) continue;
        const auto& terminal = header.instructions.back();
        const IRInstruction* comparison = nullptr;
        bool guarded = false;
        if (terminal.opcode == IROpcode::CondBranch && terminal.operands.size() == 1 && terminal.targets.size() == 2 &&
            loop.blocks.count(terminal.targets[0].value) && !loop.blocks.count(terminal.targets[1].value) &&
            cfg.predecessors[terminal.targets[0].value].size() == 1 && cfg.dominates(terminal.targets[0].value, latch)) {
            comparison = definition(terminal.operands[0]);
            guarded = comparison && comparison->opcode == IROpcode::Binary && comparison->operands.size() == 2;
        }
        std::int64_t guarded_trips = -1;
        if (guarded) {
            const auto* control = definition(comparison->operands[0]);
            const auto* end = definition(comparison->operands[1]);
            if (control && end && control->opcode == IROpcode::Phi && control->operands.size() == 2 &&
                control->targets.size() == 2 && sameInteger(control->type, end->type)) {
                IRValueId first, update;
                for (std::size_t edge = 0; edge < 2; ++edge) {
                    if (control->targets[edge].value == latch) update = control->operands[edge];
                    else if (!loop.blocks.count(control->targets[edge].value)) first = control->operands[edge];
                }
                const auto* next = definition(update);
                std::int64_t start = 0, bound = 0, stride = 0;
                if (literal(first, start) && literal(comparison->operands[1], bound) && next &&
                    next->opcode == IROpcode::Binary && next->operands.size() == 2 &&
                    next->operands[0].value == control->result.value && sameInteger(next->type, control->type) &&
                    literal(next->operands[1], stride) && (next->operation == "+" || next->operation == "-")) {
                    if (next->operation == "-") stride = -stride;
                    const auto& op = comparison->operation;
                    if (stride > 0 && (op == "<" || op == "<=")) {
                        const auto last = bound - (op == "<" ? 1 : 0);
                        guarded_trips = start > last ? 0 : (last - start) / stride + 1;
                    } else if (stride < 0 && (op == ">" || op == ">=")) {
                        const auto last = bound + (op == ">" ? 1 : 0);
                        guarded_trips = start < last ? 0 : (start - last) / -stride + 1;
                    }
                    const auto full = scalarRange(control->type);
                    const auto after = start + std::max<std::int64_t>(0, guarded_trips) * stride;
                    if (guarded_trips > 65535 || after < full.low || after > full.high) guarded_trips = -1;
                }
            }
        }
        for (const auto& phi : header.instructions) {
            if (++work > 1000000) return;
            if (phi.opcode != IROpcode::Phi || phi.operands.size() != 2 || phi.targets.size() != 2 ||
                phi.type.base != BaseType::WORD || !IRScalarFold::scalar(phi.type)) continue;
            IRValueId initial, update; std::uint32_t preheader = IRBlockId::Invalid;
            for (std::size_t edge = 0; edge < 2; ++edge) {
                if (phi.targets[edge].value == latch) update = phi.operands[edge];
                else if (!loop.blocks.count(phi.targets[edge].value)) {
                    initial = phi.operands[edge]; preheader = phi.targets[edge].value;
                }
            }
            std::int64_t start = 0, step = 0;
            const auto* next = definition(update);
            if (!initial.isValid() || preheader >= function.blocks.size() || !literal(initial, start) ||
                !next || next->opcode != IROpcode::Binary || next->operands.size() != 2 ||
                next->operands[0].value != phi.result.value || !sameInteger(next->type, phi.type) ||
                !literal(next->operands[1], step) || (next->operation != "+" && next->operation != "-")) continue;
            if (next->operation == "-") step = -step;
            if (!step) continue;
            const auto full = scalarRange(phi.type);
            auto low = start, high = start;
            if (guarded && comparison->operands[0].value == phi.result.value) {
                std::int64_t bound = 0;
                const auto* end = definition(comparison->operands[1]);
                if (!end || !sameInteger(end->type, phi.type) || !literal(comparison->operands[1], bound)) continue;
                const auto& op = comparison->operation;
                if (step > 0 && (op == "<" || op == "<=")) {
                    const auto active_high = bound - (op == "<" ? 1 : 0);
                    if (start <= active_high) high = active_high + step;
                } else if (step < 0 && (op == ">" || op == ">=")) {
                    const auto active_low = bound + (op == ">" ? 1 : 0);
                    if (start >= active_low) low = active_low + step;
                } else continue;
                // Include the terminal header value, not only body values.
                // A wrapping last update can restart the loop; reject it.
                if (low < full.low || high > full.high) continue;
            } else if (guarded && guarded_trips >= 0) {
                // Strength reduction creates a second PHI with a different
                // stride but the same backedge. Bound it by the controlling
                // induction's proven trip count, including its final value.
                const auto after = start + guarded_trips * step;
                low = std::min(start, after); high = std::max(start, after);
                if (low < full.low || high > full.high) continue;
            } else {
                // Counted LOOP has no comparison after compacting. Its exact
                // nonzero trip count still bounds a retained scalar induction.
                const auto setup_block = predecessorThroughEdges(preheader), finish_block = predecessorThroughEdges(latch);
                if (setup_block == IRBlockId::Invalid || finish_block == IRBlockId::Invalid) continue;
                const auto& entry = function.blocks[setup_block].instructions;
                const auto& tail = function.blocks[finish_block].instructions;
                if (entry.empty() || tail.empty()) continue;
                const auto& setup = entry.back(); const auto& finish = tail.back();
                std::int64_t trips = 0;
                if (setup.opcode != IROpcode::HardwareLoop || finish.opcode != IROpcode::HardwareLoopEnd ||
                    !setup.compiler_generated_loop || setup.loop_id != finish.loop_id || setup.operands.size() != 1 ||
                    setup.targets.size() != 1 || !setup.loop_target.isValid() || setup.loop_target.value >= function.blocks.size() ||
                    finish.targets.size() != 2 || !throughEdgesTo(setup.targets[0].value, loop.header) ||
                    !throughEdgesTo(setup.loop_target.value, loop.header) || !throughEdgesTo(finish.targets[0].value, loop.header) ||
                    loop.blocks.count(setup_block) || !loop.blocks.count(finish_block) || loop.blocks.count(finish.targets[1].value) ||
                    !literal(setup.operands[0], trips) || trips < 1 || trips > 65535) continue;
                const auto last = start + (trips - 1) * step, after = start + trips * step;
                low = std::min(start, last); high = std::max(start, last);
                if (low < full.low || high > full.high || after < full.low || after > full.high) continue;
            }
            m_inductions[phi.result.value] = {Kind::Scalar, low, high,
                commonAlignment(commonAlignment(65536, start), step)};
        }
    }
    std::size_t entries = 0;
    for (const auto& block : function.blocks) {
        if (block.instructions.empty() || !cfg.reachable[block.id.value]) continue;
        const auto& terminal = block.instructions.back();
        if (terminal.opcode != IROpcode::CondBranch || terminal.operands.size() != 1 || terminal.targets.size() != 2) continue;
        const auto* comparison = definition(terminal.operands[0]);
        if (!comparison || comparison->opcode != IROpcode::Binary || comparison->operands.size() != 2) continue;
        auto value = comparison->operands[0]; std::int64_t bound = 0;
        std::string operation = comparison->operation;
        if (!literal(comparison->operands[1], bound)) {
            if (!literal(comparison->operands[0], bound)) continue;
            value = comparison->operands[1];
            if (operation == "<") operation = ">"; else if (operation == "<=") operation = ">=";
            else if (operation == ">") operation = "<"; else if (operation == ">=") operation = "<=";
        }
        const auto* source = definition(value);
        const auto* other = definition(comparison->operands[value.value == comparison->operands[0].value ? 1 : 0]);
        if (!source || !other || !sameInteger(source->type, other->type)) continue;
        for (std::size_t edge = 0; edge < 2; ++edge) {
            const auto target = terminal.targets[edge].value;
            if (cfg.predecessors[target].size() != 1 || !cfg.reachable[target]) continue;
            auto fact = scalarRange(source->type);
            const bool positive = edge == 0;
            if (operation == "<") { if (positive) fact.high = std::min(fact.high, bound - 1); else fact.low = std::max(fact.low, bound); }
            else if (operation == "<=") { if (positive) fact.high = std::min(fact.high, bound); else fact.low = std::max(fact.low, bound + 1); }
            else if (operation == ">") { if (positive) fact.low = std::max(fact.low, bound + 1); else fact.high = std::min(fact.high, bound); }
            else if (operation == ">=") { if (positive) fact.low = std::max(fact.low, bound); else fact.high = std::min(fact.high, bound - 1); }
            else if (operation == "==" && positive) fact.low = fact.high = bound;
            else if (operation == "!=" && !positive) fact.low = fact.high = bound;
            else continue;
            if (fact.low > fact.high) continue;
            for (std::uint32_t candidate = 0; candidate < function.blocks.size(); ++candidate) {
                if (++work > 1000000 || entries >= 65536) return;
                if (!cfg.dominates(target, candidate)) continue;
                auto& constraints = m_constraints[candidate];
                const auto inserted = constraints.emplace(value.value, fact);
                if (inserted.second) ++entries;
                else {
                    auto& existing = inserted.first->second;
                    existing.low = std::max(existing.low, fact.low); existing.high = std::min(existing.high, fact.high);
                }
            }
        }
    }
}

GSUAddressProof::Fact GSUAddressProof::displaced(Fact address, std::int64_t low,
                                              std::int64_t high, unsigned alignment) const {
    if (address.kind != Kind::Frame && address.kind != Kind::Absolute) return {};
    if (low < -65535 || high > 65535 || low > high) return {};
    address.low += low;
    address.high += high;
    address.alignment = std::min(address.alignment, alignment);
    if (address.kind == Kind::Absolute && (address.low < 0 || address.high > 65535)) return {};
    if (address.kind == Kind::Frame &&
        (!m_checked_frame || address.low < m_frame_low || address.high >= m_frame_end)) return {};
    return address;
}

GSUAddressProof::Fact GSUAddressProof::transfer(
    const IRInstruction& instruction, const Analyzer::LocalSymbolTable& locals,
    const std::vector<const IRInstruction*>& definitions) const {
    const auto full = scalarRange(instruction.type);
    const auto block = instruction.result.isValid() && instruction.result.value < m_definition_blocks.size() ?
        m_definition_blocks[instruction.result.value] : IRBlockId::Invalid;
    const auto operand = [&](std::size_t n) {
        return n < instruction.operands.size() ? get(instruction.operands[n], block) : Fact{};
    };
    const auto boundedScalar = [&](std::int64_t low, std::int64_t high, unsigned alignment = 1) {
        // Numeric operations wrap in DiscoC. A mathematical interval is useful
        // only when every result is representable, not merely its endpoints.
        if (full.kind != Kind::Scalar || low < full.low || high > full.high || low > high) return full;
        return Fact{Kind::Scalar, low, high, alignment};
    };
    const auto a = operand(0), b = operand(1);
    if (instruction.opcode == IROpcode::Constant) {
        if (full.kind == Kind::Scalar) {
            const auto value = ConstantEvaluator::convert(instruction.immediate, instruction.type);
            return {Kind::Scalar, value, value, commonAlignment(65536, value)};
        }
        if (nearRam(instruction.type) && instruction.immediate >= 0 && instruction.immediate <= 65535) {
            const auto value = instruction.immediate;
            return {Kind::Absolute, value, value, commonAlignment(65536, value)};
        }
        return {};
    }
    if (instruction.opcode == IROpcode::Phi) {
        if (instruction.result.value < m_inductions.size() && m_inductions[instruction.result.value].kind == Kind::Scalar)
            return m_inductions[instruction.result.value];
        if (instruction.operands.empty()) return full;
        auto merged = a;
        for (const auto incoming : instruction.operands) {
            const auto next = get(incoming);
            // Unknown backedges are never ignored. A cyclic pointer PHI is
            // intentionally left unproved instead of assuming its first trip.
            if (next.kind == Kind::Unknown || next.kind != merged.kind) return full;
            merged.low = std::min(merged.low, next.low);
            merged.high = std::max(merged.high, next.high);
            merged.alignment = std::min(merged.alignment, next.alignment);
        }
        return merged;
    }
    if (instruction.opcode == IROpcode::Address && nearRam(instruction.type)) {
        if (instruction.operation == "member" && instruction.operands.size() == 1 &&
            instruction.immediate >= 0 && instruction.immediate <= 65535)
            return displaced(a, instruction.immediate, instruction.immediate,
                             commonAlignment(a.alignment, instruction.immediate));
        if (!m_checked_frame) return {};
        std::int64_t offset = instruction.immediate;
        if (instruction.operation.empty()) {
            const auto found = locals.find(instruction.symbol_id);
            if (!instruction.symbol_id.isValid() || found == locals.end() || found->second.type.alignment > 2) return {};
            offset = found->second.stackOffset;
        } else if (instruction.operation != "temporary") return {};
        if (offset < m_frame_low || offset >= m_frame_end) return {};
        return {Kind::Frame, offset, offset, commonAlignment(2, offset)};
    }
    if (instruction.opcode == IROpcode::PointerOffset && nearRam(instruction.type) &&
        instruction.operands.size() == 2 && (instruction.operation == "+" || instruction.operation == "-") &&
        instruction.immediate > 0 && instruction.immediate <= 65535 && b.kind == Kind::Scalar) {
        const auto stride = instruction.immediate;
        auto low = b.low * stride, high = b.high * stride;
        if (instruction.operation == "-") { const auto old_low = low; low = -high; high = -old_low; }
        if (low < -65535 || high > 65535) return {};
        const auto alignment = low == high ? commonAlignment(a.alignment, low) :
            commonAlignment(a.alignment, stride);
        return displaced(a, low, high, alignment);
    }
    if (instruction.opcode == IROpcode::Cast && nearRam(instruction.type)) {
        // Near/far and ROM/RAM conversions carry bank semantics and are not
        // part of this proof domain. Unknown pointer loads never gain facts.
        if (instruction.operands.size() != 1 || instruction.operands[0].value >= definitions.size()) return {};
        const auto* source = definitions[instruction.operands[0].value];
        if (!source) return {};
        if (nearRam(source->type)) return a;
        if (a.kind == Kind::Scalar && a.low >= 0 && a.high <= 65535)
            return {Kind::Absolute, a.low, a.high, a.alignment};
        return {};
    }
    if (full.kind != Kind::Scalar) return {};
    if (!instruction.operands.empty() && instruction.operands.size() <= 2 && a.kind == Kind::Scalar &&
        a.low == a.high && (instruction.operands.size() == 1 || (b.kind == Kind::Scalar && b.low == b.high))) {
        std::array<IRScalarFold::Operand, 2> inputs{};
        for (std::size_t n = 0; n < instruction.operands.size(); ++n) {
            const auto id = instruction.operands[n].value;
            if (id >= definitions.size() || !definitions[id]) return full;
            inputs[n] = {&definitions[id]->type, operand(n).low};
        }
        std::int64_t value = 0;
        if (IRScalarFold::evaluate(instruction, inputs, value))
            return {Kind::Scalar, value, value, commonAlignment(65536, value)};
    }
    if (instruction.opcode == IROpcode::BitExtract) return boundedScalar(0, 1);
    if (a.kind != Kind::Scalar) return full;
    if (instruction.opcode == IROpcode::Cast) return boundedScalar(a.low, a.high, a.alignment);
    if (instruction.opcode == IROpcode::Unary) {
        if (instruction.operation == "!") return boundedScalar(0, 1);
        if (instruction.operation == "-") return boundedScalar(-a.high, -a.low, a.alignment);
        return full;
    }
    if (instruction.opcode != IROpcode::Binary || b.kind != Kind::Scalar) return full;
    const auto& op = instruction.operation;
    if (op == "+") return boundedScalar(a.low + b.low, a.high + b.high, std::min(a.alignment, b.alignment));
    if (op == "-") return boundedScalar(a.low - b.high, a.high - b.low, std::min(a.alignment, b.alignment));
    if (op == "*") {
        const std::array<std::int64_t, 4> products = {{a.low * b.low, a.low * b.high, a.high * b.low, a.high * b.high}};
        const auto alignment = static_cast<unsigned>(std::min<std::uint64_t>(65536,
            static_cast<std::uint64_t>(a.alignment) * b.alignment));
        return boundedScalar(*std::min_element(products.begin(), products.end()), *std::max_element(products.begin(), products.end()), alignment);
    }
    if (op == "&") {
        const auto mask = a.low == a.high && a.low >= 0 ? a.low : b.low == b.high && b.low >= 0 ? b.low : -1;
        if (mask >= 0) return boundedScalar(0, mask, std::max(a.alignment, b.alignment));
    }
    if (op == ">>" && b.low == b.high && b.low >= 0 && b.low <= 15) {
        const auto divisor = std::int64_t{1} << static_cast<unsigned>(b.low);
        const auto shift = [&](std::int64_t value) { return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor); };
        return boundedScalar(shift(a.low), shift(a.high));
    }
    return full;
}

void GSUAddressProof::run(const IRFunction& function, const Analyzer::LocalSymbolTable& locals,
                          std::size_t frame_bytes, bool checked_frame) {
    *this = GSUAddressProof{};
    // A bounded conservative analysis must never turn hostile IR into an
    // unbounded allocation/fixed-point computation. Reaching a cap loses only
    // optimization, not runtime checks.
    if (function.value_count > 100000 || function.blocks.size() > 8192 || frame_bytes > 65526) return;
    std::size_t count = 0, operands = 0, parameters = 0;
    for (std::size_t n = 0; n < function.blocks.size(); ++n) {
        const auto& block = function.blocks[n];
        if (!block.id.isValid() || block.id.value != n) return;
        for (const auto& instruction : block.instructions) {
            if (++count > 200000 || instruction.operands.size() > 1000000 - operands) return;
            operands += instruction.operands.size();
            if (instruction.result.value > function.value_count ||
                (instruction.opcode == IROpcode::Phi && instruction.targets.size() > instruction.operands.size())) return;
            for (const auto input : instruction.operands) if (!input.isValid() || input.value > function.value_count) return;
            for (const auto target : instruction.targets) if (!target.isValid() || target.value >= function.blocks.size()) return;
        }
    }
    for (const auto& parameter : function.parameters) {
        parameters += isFarPointer(parameter.type) ? 4 : 2;
        if (parameters > 65526) return;
    }
    m_checked_frame = checked_frame;
    // The linker permits stack_floor=0. Keep the bottom empty word out of
    // the proof: FP-frame_bytes itself could be the null address.
    m_frame_low = -static_cast<std::int64_t>(frame_bytes) + 2;
    m_frame_end = GSUAbi::FirstParameterOffset + static_cast<std::int64_t>(parameters);
    m_facts.resize(static_cast<std::size_t>(function.value_count) + 1);
    m_definition_blocks.assign(m_facts.size(), IRBlockId::Invalid);
    std::vector<const IRInstruction*> definitions(m_facts.size(), nullptr);
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
        if (!instruction.result.isValid()) continue;
        if (definitions[instruction.result.value]) { *this = GSUAddressProof{}; return; }
        definitions[instruction.result.value] = &instruction;
        m_definition_blocks[instruction.result.value] = block.id.value;
        // Start with TOP, never an assumed first-iteration value. Each
        // refinement is true even if the bounded iteration stops early.
        m_facts[instruction.result.value] = scalarRange(instruction.type);
    }
    try { refineControlFlow(function, definitions); }
    catch (const CompilerError&) {
        // CFG analysis limits disable only additional proofs, not compilation
        // or runtime validation. No borrowed CFG state survives this call.
        m_inductions.clear(); m_constraints.clear();
    }
    std::size_t work = 0;
    for (unsigned round = 0; round < 64; ++round) {
        bool changed = false;
        for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
            if (!instruction.result.isValid()) continue;
            const auto cost = instruction.operands.size() + 1;
            if (cost > 25000000 - work) return;
            work += cost;
            const auto next = transfer(instruction, locals, definitions);
            auto& fact = m_facts[instruction.result.value];
            if (!(fact == next)) { fact = next; changed = true; }
        }
        if (!changed) return;
    }
}

bool GSUAddressProof::provesAccess(IRValueId value, const Type& pointer, int width) const {
    if (!nearRam(pointer) || width <= 0 || width > 65535) return false;
    const auto fact = get(value);
    const auto alignment = storageAlignment(pointeeType(pointer));
    if (width > 1 && (alignment <= 0 || fact.alignment < static_cast<unsigned>(alignment))) return false;
    if (fact.kind == Kind::Frame)
        return m_checked_frame && fact.low >= m_frame_low && fact.high <= m_frame_end - width;
    if (fact.kind == Kind::Absolute) return fact.low > 0 && fact.high <= 65536 - width;
    return false;
}

bool GSUAddressProof::provesOffset(const IRInstruction& instruction) const {
    if (instruction.opcode != IROpcode::PointerOffset || instruction.operands.size() != 2 ||
        (instruction.operation != "+" && instruction.operation != "-") ||
        instruction.immediate <= 0 || instruction.immediate > 65535 ||
        (instruction.immediate & (instruction.immediate - 1))) return false;
    const auto block = instruction.result.value < m_definition_blocks.size() ?
        m_definition_blocks[instruction.result.value] : IRBlockId::Invalid;
    const auto index = get(instruction.operands[1], block);
    return index.kind == Kind::Scalar && index.low * instruction.immediate >= -65535 &&
        index.high * instruction.immediate <= 65535 &&
        provesAccess(instruction.operands[0], instruction.type, static_cast<int>(instruction.immediate)) &&
        provesAccess(instruction.result, instruction.type, static_cast<int>(instruction.immediate));
}
