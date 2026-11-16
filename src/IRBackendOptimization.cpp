#include "IRCodeGenerator.hpp"

#include <algorithm>
#include <functional>
#include <limits>

#include "ABI.hpp"
#include "GSUCostModel.hpp"
#include "IRControlFlow.hpp"
#include "IRGlobalOptimizer.hpp"

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
    // Code outside this module follows the ABI and preserves R9.
    constexpr std::uint16_t unknown = 0x7fff & ~(1u << GSUAbi::FramePointerRegister);
    if (module.functions.size() > 256) return;
    std::map<std::string, std::set<std::string>> calls;
    std::set<std::string> fp_free;
    for (const auto& f : module.functions) if (framePointerAllocatable(f)) fp_free.insert(functionSymbol(f));
    std::size_t work = 0;
    for (const auto& f : module.functions) {
        if (f.value_count > 4096 || f.blocks.size() > 256) { m_call_clobbers.clear(); return; }
        const auto key = functionSymbol(f);
        std::uint16_t mask = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 6) | (1u << 11);
        // The caller itself writes the argument registers before LINK.
        const auto arguments = m_register_arguments.find(key);
        if (arguments != m_register_arguments.end())
            for (std::size_t p = 0; p < arguments->second; ++p) mask |= static_cast<std::uint16_t>(1u << GSUAbi::RegisterArguments[p]);
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
            // A caller outside fp_free saves R9 around such calls itself.
            const std::uint16_t keep = fp_free.count(edge.first) ? 0xffffu : 0xffffu & ~(1u << GSUAbi::FramePointerRegister);
            for (const auto& name : edge.second) {
                if (++work > 1000000) { m_call_clobbers.clear(); return; }
                const auto callee = m_call_clobbers.find(name);
                mask |= static_cast<std::uint16_t>((callee == m_call_clobbers.end() ? unknown : callee->second) & keep);
            }
            changed = changed || before != mask;
        }
        if (!changed) return;
    }
    m_call_clobbers.clear();
}

namespace {
// One 16-bit word with no bank or aggregate part: what a register carries.
bool registerArgumentType(const Type& type) {
    if (type.array_size != 0) return false;
    if (type.pointer_level > 0) return !isFarPointer(type);
    return type.base == BaseType::WORD;
}
bool sameRegisterArgument(const Type& a, const Type& b) {
    return a.base == b.base && a.pointer_level == b.pointer_level && a.is_unsigned == b.is_unsigned &&
        a.array_size == b.array_size && a.structName == b.structName && a.enum_name == b.enum_name &&
        isFarPointer(a) == isFarPointer(b) && (a.pointer_level == 0 || (a.space == b.space && samePointerLayers(a, b)));
}
// Parameters must be read-only SSA inputs: each parameter address feeds only
// non-volatile loads, and all of those addresses and loads form the prefix
// of an entry block that nothing branches back to. Then nothing can write a
// register between function entry and the last parameter load.
bool entryParameterLoads(const IRFunction& f, std::map<std::uint32_t, std::size_t>& loads) {
    loads.clear();
    if (f.blocks.empty() || f.parameters.empty() || f.parameters.size() > GSUAbi::RegisterArgumentCount) return false;
    for (const auto& parameter : f.parameters)
        if (!registerArgumentType(parameter.type) || !parameter.symbol_id.isValid()) return false;
    std::map<std::uint32_t, std::size_t> addresses;
    for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
        if (i.opcode != IROpcode::Phi)
            for (const auto target : i.targets) if (target.value == f.entry.value) return false;
        if (i.opcode == IROpcode::Phi || !i.symbol_id.isValid()) continue;
        std::size_t parameter = f.parameters.size();
        for (std::size_t p = 0; p < f.parameters.size(); ++p) if (f.parameters[p].symbol_id == i.symbol_id) parameter = p;
        if (parameter == f.parameters.size()) continue;
        if (i.opcode != IROpcode::Address || !i.operation.empty() || !i.result.isValid() || b.id.value != f.entry.value) return false;
        addresses.emplace(i.result.value, parameter);
    }
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        for (std::size_t o = 0; o < i.operands.size(); ++o) {
            const auto address = addresses.find(i.operands[o].value);
            if (address == addresses.end()) continue;
            if (i.opcode != IROpcode::LoadIndirect || o != 0 || i.memory_volatile || !i.result.isValid() ||
                b.id.value != f.entry.value || !sameRegisterArgument(i.type, f.parameters[address->second].type))
                return false;
            loads.emplace(i.result.value, address->second);
        }
    bool prefix = true;
    for (const auto& i : f.blocks.at(f.entry.value).instructions) {
        const bool input = i.result.isValid() && (addresses.count(i.result.value) || loads.count(i.result.value));
        if (!input) prefix = false;
        else if (!prefix) return false;
    }
    return true;
}
} // namespace

void IRCodeGenerator::planRegisterArguments(const IRModule& module) {
    m_register_arguments.clear();
    if (!globallyOptimized() || m_config.target != TargetKind::GSU) return;
    // Every caller of an unaddressed internal function is in this module, so
    // both sides of the call change together; any address use keeps the
    // stack area, since an indirect caller could not know the convention.
    std::set<std::string> referenced;
    for (const auto& f : module.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.opcode == IROpcode::Address && !i.symbol.empty()) referenced.insert(i.symbol);
    std::map<std::uint32_t, std::size_t> loads;
    for (const auto& f : module.functions) {
        const auto symbol = functionSymbol(f);
        if (f.link_name.empty() || f.link_name.front() != '\x01' || f.name == "main" ||
            referenced.count(symbol) || referenced.count(f.name) || !entryParameterLoads(f, loads)) continue;
        m_register_arguments.emplace(symbol, f.parameters.size());
    }
}

void IRCodeGenerator::planStaticFrames(const IRModule& module) {
    m_static_frame_functions.clear();
    // Static RAM needs runtime initialization, which this contract implies.
    if (!globallyOptimized() || !m_config.static_stack_proof || m_config.target != TargetKind::GSU) return;
    // Only grow static RAM the project already places: a unit without its
    // own static RAM may link it at origin 0, under a framebuffer.
    bool owns_ram = false;
    for (const auto& entry : m_data_manager.getEntries())
        owns_ram = owns_ram || (!entry.second.is_extern && entry.second.storage == AddressSpace::RAM);
    if (!owns_ram) return;
    // A frame may be static only if its function can never be active twice:
    // no address use (no indirect entry), not on a call cycle, and no path
    // from it leaves the module (where a callback could re-enter it).
    std::map<std::string, std::set<std::string>> callees, callers;
    std::set<std::string> names, referenced, external;
    for (const auto& f : module.functions) names.insert(functionSymbol(f));
    std::map<std::string, std::string> by_name;
    for (const auto& f : module.functions) by_name.emplace(f.name, functionSymbol(f));
    for (const auto& f : module.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
        if (i.opcode == IROpcode::Call) {
            if (names.count(i.symbol)) { callees[functionSymbol(f)].insert(i.symbol); callers[i.symbol].insert(functionSymbol(f)); }
            else external.insert(functionSymbol(f));
        } else if (i.opcode == IROpcode::Address && !i.symbol.empty()) {
            referenced.insert(i.symbol);
            const auto named = by_name.find(i.symbol);
            if (named != by_name.end()) referenced.insert(named->second);
        }
    }
    // A path leaves the module if any function on it calls outside: one
    // reverse worklist over call edges, O(V + E).
    std::vector<std::string> work(external.begin(), external.end());
    while (!work.empty()) {
        const auto current = work.back(); work.pop_back();
        for (const auto& caller : callers[current]) if (external.insert(caller).second) work.push_back(caller);
    }
    // Calling a recursive function does not make a caller re-entrant; only
    // reaching itself does. Bounded like the clobber analysis: O(V (V + E)).
    if (names.size() > 256) return;
    for (const auto& name : names) {
        if (external.count(name) || referenced.count(name)) continue;
        std::set<std::string> seen;
        std::vector<std::string> pending(callees[name].begin(), callees[name].end());
        bool cyclic = false;
        while (!pending.empty() && !cyclic) {
            const auto current = pending.back(); pending.pop_back();
            cyclic = current == name;
            if (!seen.insert(current).second) continue;
            for (const auto& next : callees[current]) pending.push_back(next);
        }
        if (!cyclic) m_static_frame_functions.insert(name);
    }
}

void IRCodeGenerator::layoutStaticFrames(const IRModule& module) {
    if (m_static_frame_slots.empty()) return;
    // Compiled stack: a frame starts above every frame that can be active
    // with it, i.e. above each static caller's frame on any call path. A
    // non-static caller passes its callers' depth through unchanged. Static
    // frames are acyclic, so the relaxation settles within V rounds. The
    // short RAM parts form a second compiled stack of their own sizes.
    std::vector<std::pair<std::string, std::string>> edges;
    for (const auto& f : module.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.opcode == IROpcode::Call) edges.emplace_back(functionSymbol(f), i.symbol);
    const auto stack = [&](const std::map<std::string, int>& sizes, std::map<std::string, std::uint32_t>& depth) {
        const auto size = [&](const std::string& name) -> std::uint32_t {
            const auto found = sizes.find(name);
            return found == sizes.end() ? 0u : static_cast<std::uint32_t>(found->second);
        };
        bool changed = true;
        for (std::size_t round = 0; changed; ++round) {
            if (round > module.functions.size() + 1) fail("IR codegen: static frame layout did not converge.", Token(TokenType::UNKNOWN, "", 0, 0));
            changed = false;
            for (const auto& edge : edges) {
                const auto candidate = depth[edge.first] + size(edge.first);
                if (candidate > 65535u) fail("Static frames exceed one RAM bank.", Token(TokenType::UNKNOWN, edge.first, 1, 1));
                if (candidate > depth[edge.second]) { depth[edge.second] = candidate; changed = true; }
            }
        }
        std::uint32_t total = 0;
        for (const auto& frame : sizes) total = std::max(total, depth[frame.first] + static_cast<std::uint32_t>(frame.second));
        return total;
    };
    std::map<std::string, std::uint32_t> depth, short_depth;
    const auto total = stack(m_static_frame_sizes, depth);
    const auto short_total = stack(m_short_frame_sizes, short_depth);
    // Admission summed every short part, which bounds the compiled stack.
    if (short_total > m_config.short_ram_bytes || short_total > GSUAbi::ShortRamCapacity)
        fail("IR codegen: short RAM frames exceed their window.", Token(TokenType::UNKNOWN, "", 1, 1));
    while (m_object_file.ram_section.size() % 2) m_object_file.ram_section.push_back(0);
    const auto base = m_object_file.ram_section.size();
    if (total > 65536u - base) fail("Static frames exceed one RAM bank.", Token(TokenType::UNKNOWN, "", 1, 1));
    m_object_file.ram_section.resize(base + total, 0);
    m_object_file.short_ram_section.assign(short_total, 0);
    for (const auto& slot : m_static_frame_slots) {
        auto owner = slot.first;
        if (!owner.empty() && owner.front() == '\x01') owner.erase(0, 1);
        const auto name = std::string(1, '\x01') + "__disco_frame." + owner + "." + std::to_string(slot.second);
        const auto layout = m_short_frame_layout.find(slot.first);
        if (layout != m_short_frame_layout.end() && layout->second.count(slot.second)) {
            m_object_file.symbol_table.push_back({name, SymbolSection::SHORT_RAM,
                short_depth[slot.first] + static_cast<std::uint32_t>(layout->second.at(slot.second))});
            continue;
        }
        m_object_file.symbol_table.push_back({name, SymbolSection::RAM,
            static_cast<std::uint32_t>(base + depth[slot.first] + static_cast<std::uint32_t>(slot.second))});
    }
    // Frame addresses are proved like in-object static RAM: nonzero base.
    m_static_ram_proof_used = true;
}

// An internal static-frame function without stack parameters never reads FP,
// and all of its callers are in this module and see R9 in its call-clobber
// mask, so it may hold values in R9. Every other function preserves R9: a
// caller outside this set saves it around a call whose callee may write it.
bool IRCodeGenerator::framePointerAllocatable(const IRFunction& function) const {
    const auto symbol = functionSymbol(function);
    return globallyOptimized() && function.name != "main" && !symbol.empty() && symbol.front() == '\x01' &&
        m_static_frame_functions.count(symbol) && (function.parameters.empty() || m_register_arguments.count(symbol));
}

// Word slots of the current static frame that every emitter accesses by
// the slot's own address: non-far spills and divmod components. A far spill
// steps R3 to its second word, so both words keep their frame positions.
// Functions are admitted in emission order while the summed short sizes fit
// the claimed window; that sum bounds the compiled stack laid out later.
void IRCodeGenerator::planShortFrame() {
    m_short_frame_offsets.clear();
    if (m_static_frame_owner.empty() || !m_config.short_ram_bytes) return;
    std::set<int> words, far;
    for (const auto& spill : m_spill_offsets) {
        if (isFarPointer(m_values.at(spill.first)->type)) { far.insert(spill.second); far.insert(spill.second + 2); }
        else words.insert(spill.second);
    }
    for (const auto& divmod : m_divmod_offsets) words.insert(divmod.second);
    for (const auto offset : far) words.erase(offset);
    const std::size_t bytes = 2u * words.size();
    const std::size_t budget = std::min<std::size_t>(m_config.short_ram_bytes, GSUAbi::ShortRamCapacity);
    if (words.empty() || bytes > budget || m_short_ram_admitted > budget - bytes) return;
    m_short_ram_admitted += bytes;
    auto& layout = m_short_frame_layout[m_static_frame_owner];
    int next = 0;
    for (const auto offset : words) { layout.emplace(m_static_frame_bytes + offset, next); next += 2; }
    m_short_frame_sizes[m_static_frame_owner] = next;
    m_short_frame_offsets = std::move(words);
}

// LMS/SMS Rn, (slot): ALT1/ALT2, the IBT opcode, then the word index the
// linker writes. The named register is accessed directly; no selector is
// involved and R3 is left untouched.
void IRCodeGenerator::emitShortFrameAccess(bool load, std::uint8_t reg, int offset) {
    const auto slot = m_static_frame_bytes + offset;
    if (!shortFrameSlot(offset) || reg > 15)
        fail("IR codegen: short RAM access outside the short frame.", Token(TokenType::UNKNOWN, m_static_frame_owner, 1, 1));
    auto owner = m_static_frame_owner;
    if (!owner.empty() && owner.front() == '\x01') owner.erase(0, 1);
    m_last_ram_word_address = IRValueId{};
    if (load) m_spill_cache.clobber(reg);
    emitByte(static_cast<std::uint8_t>(load ? OpCode::ALT1 : OpCode::ALT2));
    const auto patch = m_object_file.code_section.size();
    emitByte(static_cast<std::uint8_t>(0xa0 | reg));
    emitByte(0);
    addRelocation(std::string(1, '\x01') + "__disco_frame." + owner + "." + std::to_string(slot), patch,
                  RelocationType::ADDR8_SHORT_RAM);
    m_static_frame_slots.emplace(m_static_frame_owner, slot);
}

void IRCodeGenerator::requireFramePointer(const Token& source) const {
    if (m_frameless) fail("IR codegen: frameless function addressed its stack frame.", source);
}

void IRCodeGenerator::emitFrameAddress(Register reg, int offset) {
    if (!m_static_frame_owner.empty() && offset < 0) {
        const auto slot = m_static_frame_bytes + offset;
        if (slot < 0) fail("IR codegen: static frame slot is outside its frame.", Token(TokenType::UNKNOWN, m_static_frame_owner, 1, 1));
        auto owner = m_static_frame_owner;
        if (!owner.empty() && owner.front() == '\x01') owner.erase(0, 1);
        const auto patch = m_object_file.code_section.size();
        emitWordLiteral(reg, 0); // The linker patches an IWT, never a shortened IBT.
        addRelocation(std::string(1, '\x01') + "__disco_frame." + owner + "." + std::to_string(slot), patch, RelocationType::ADDR16_RAM);
        m_static_frame_slots.emplace(m_static_frame_owner, slot);
        return;
    }
    if (reg != Register::R3) fail("IR codegen: FP-relative frame address must use R3.", Token(TokenType::UNKNOWN, "", 0, 0));
    requireFramePointer(Token(TokenType::UNKNOWN, "", 0, 0));
    emitRegisterLiteral(3, static_cast<std::uint16_t>(offset));
    emitByte(0xb9); emitByte(0x13); emitByte(0x53); // R3 = FP + signed frame displacement.
}

std::map<std::uint32_t, std::uint8_t> IRCodeGenerator::parameterInputs(const IRFunction& function) const {
    std::map<std::uint32_t, std::uint8_t> inputs;
    std::map<std::uint32_t, std::size_t> loads;
    if (!m_register_arguments.count(functionSymbol(function)) || !entryParameterLoads(function, loads)) return inputs;
    for (const auto& load : loads) inputs.emplace(load.first, GSUAbi::RegisterArguments[load.second]);
    return inputs;
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
    m_regional_loop_values.clear(); m_regional_software_loops.clear();
    const auto& f = *m_current_function;
    if (f.blocks.size() > 256 || f.value_count > 4096) return;
    const IRControlFlow cfg(f);
    std::map<std::uint32_t, std::uint32_t> definition_blocks;
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.result.isValid()) definition_blocks.emplace(i.result.value, b.id.value);
    std::size_t work = 0;
    bool exhausted = false;
    // Closed-region proof shared by hardware scopes and innermost software
    // loops: values defined outside, never written inside, are loaded once at
    // `entry_block`'s edge into R4/R6 when nothing in the region uses them.
    // Software loops (`latches` nonempty) load once per entry, so a candidate
    // must repay that: either the header reads it (every entry, zero-trip
    // ones included, already paid that read), or every trip reads it outside
    // a PHI and its frame reload is the multi-instruction FP-relative form,
    // not a single short-RAM LMS. Rare-path reads never qualify.
    const auto plan = [&](const std::set<std::uint32_t>& region, std::uint32_t entry_block, bool hardware,
                          std::uint32_t loop_id, IRBlockId header, const std::set<std::uint32_t>& latches,
                          RegionalLoopValues& selected) -> bool {
        std::set<std::uint32_t> every_trip, header_reads;
        unsigned available = (1u << 4) | (1u << 6);
        bool closed = true, latch = false;
        std::map<std::uint32_t, unsigned> reads;
        std::set<int> written_slots;
        for (const auto b : region) {
            for (const auto p : cfg.predecessors.at(b)) {
                if (!region.count(p) && p != entry_block) closed = false;
                if (phiCopiesUseR6(p, IRBlockId{b})) available &= ~(1u << 6);
            }
            for (const auto& i : f.blocks[b].instructions) {
                if (++work > 1000000) { exhausted = true; return false; }
                if (i.result.isValid()) {
                    const auto spill = m_spill_offsets.find(i.result.value);
                    if (spill != m_spill_offsets.end()) written_slots.insert(spill->second);
                }
                const bool each_trip = !hardware && i.opcode != IROpcode::Phi &&
                    std::all_of(latches.begin(), latches.end(), [&](std::uint32_t l) { return cfg.dominates(b, l); });
                for (const auto v : i.operands) {
                    ++reads[v.value];
                    if (each_trip) every_trip.insert(v.value);
                    if (!hardware && b == header.value && i.opcode != IROpcode::Phi) header_reads.insert(v.value);
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
                        latch = latch || (hardware && i.loop_id == loop_id);
                        if (!hardware || i.loop_id != loop_id) closed = false;
                        break;
                    default: closed = false; break;
                }
            }
        }
        // A software loop's own CondBranch is its latch; only a hardware
        // scope needs its matching end. Software regions also give up R4
        // while an allocated value lives in it (hardware scopes never
        // allocate R4), and leave without exit-edge copies that could
        // reuse R6 while a regional source is still being read.
        if (!hardware) {
            latch = true;
            for (const auto b : region) {
                for (const auto next : cfg.successors.at(b))
                    if (!region.count(next) && !phiCopiesEmpty(b, IRBlockId{next})) closed = false;
                std::set<std::uint32_t> present(cfg.live_in.at(b).begin(), cfg.live_in.at(b).end());
                present.insert(cfg.live_out.at(b).begin(), cfg.live_out.at(b).end());
                for (const auto& i : f.blocks[b].instructions) if (i.result.isValid()) present.insert(i.result.value);
                for (const auto value : present) {
                    const auto* home = m_register_allocator.find(IRValueId{value});
                    if (home && home->has_register && home->physical_register == 4) available &= ~(1u << 4);
                }
            }
        }
        if (!closed || !latch || !available) return false;
        std::vector<std::uint32_t> candidates;
        for (const auto& use : reads) {
            const auto spill = m_spill_offsets.find(use.first);
            if (spill == m_spill_offsets.end()) continue;
            if (!hardware && !header_reads.count(use.first) &&
                (!every_trip.count(use.first) || shortFrameSlot(spill->second))) continue;
            if (region.count(definition_blocks.at(use.first)) ||
                written_slots.count(spill->second) || !GSUCostModel::scalar(m_values.at(use.first)->type)) continue;
            // The definition must dominate the setup, not just be located in
            // an earlier emitted block. Its frame home is already initialized.
            if (!cfg.dominates(definition_blocks.at(use.first), entry_block)) continue;
            candidates.push_back(use.first);
        }
        std::sort(candidates.begin(), candidates.end(), [&](std::uint32_t a, std::uint32_t b) {
            if (reads.at(a) != reads.at(b)) return reads.at(a) > reads.at(b);
            return m_spill_offsets.at(a) < m_spill_offsets.at(b);
        });
        selected.blocks = region;
        for (const auto reg : {std::uint8_t{4}, std::uint8_t{6}}) {
            if (!(available & (1u << reg)) || candidates.empty()) continue;
            selected.values.push_back({IRValueId{candidates.front()}, reg});
            candidates.erase(candidates.begin());
        }
        return !selected.values.empty();
    };
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
        RegionalLoopValues selected;
        if (plan(region, setup_block.id.value, true, setup.loop_id, IRBlockId{}, {}, selected))
            m_regional_loop_values.emplace(setup.loop_id, std::move(selected));
        if (exhausted) { m_regional_loop_values.clear(); return; }
    }
    // Innermost software loops of functions without hardware scopes (whose
    // R4/R6 may hold saved R12/R13): one outside predecessor that ends in an
    // unconditional branch to the header, which emitBranch extends with the
    // loads after its PHI copies.
    for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.opcode == IROpcode::HardwareLoop || i.opcode == IROpcode::HardwareLoopEnd ||
            i.opcode == IROpcode::HardwareLoopLeave) return;
    for (const auto& loop : cfg.loops) {
        if (loop.blocks.size() > 64 || !cfg.reachable[loop.header]) continue;
        bool innermost = true;
        for (const auto& other : cfg.loops)
            if (other.header != loop.header && loop.blocks.count(other.header)) innermost = false;
        std::uint32_t entry = IRBlockId::Invalid; unsigned outside = 0;
        for (const auto p : cfg.predecessors.at(loop.header)) if (!loop.blocks.count(p)) { entry = p; ++outside; }
        if (!innermost || outside != 1 || m_regional_software_loops.count(entry)) continue;
        const auto& terminal = f.blocks.at(entry).instructions;
        if (terminal.empty() || terminal.back().opcode != IROpcode::Branch || terminal.back().targets.size() != 1 ||
            terminal.back().targets[0].value != loop.header) continue;
        RegionalLoopValues selected;
        if (!loop.latches.empty() && plan(loop.blocks, entry, false, 0, IRBlockId{loop.header}, loop.latches, selected))
            m_regional_software_loops.emplace(entry, std::move(selected));
        if (exhausted) { m_regional_loop_values.clear(); m_regional_software_loops.clear(); return; }
    }
}

int IRCodeGenerator::regionalRegister(IRValueId value) const {
    for (const auto* regions : {&m_regional_loop_values, &m_regional_software_loops})
        for (const auto& loop : *regions) if (loop.second.blocks.count(static_cast<std::uint32_t>(m_current_block_index)))
            for (const auto& entry : loop.second.values) if (entry.first.value == value.value) return entry.second;
    return -1;
}

void IRCodeGenerator::emitRegionalLoopValues(std::uint32_t loop_id) {
    const auto found = m_regional_loop_values.find(loop_id);
    if (found != m_regional_loop_values.end()) emitRegionalValues(found->second);
}

void IRCodeGenerator::emitSoftwareRegionalValues(std::uint32_t entry_block, IRBlockId target) {
    const auto found = m_regional_software_loops.find(entry_block);
    if (found != m_regional_software_loops.end() && found->second.blocks.count(target.value)) emitRegionalValues(found->second);
}

void IRCodeGenerator::emitRegionalValues(const RegionalLoopValues& region) {
    auto values = region.values;
    std::sort(values.begin(), values.end(), [&](const std::pair<IRValueId, std::uint8_t>& a,
                                               const std::pair<IRValueId, std::uint8_t>& b) {
        return m_spill_offsets.at(a.first.value) < m_spill_offsets.at(b.first.value);
    });
    int previous = 0; bool have_address = false;
    for (const auto& entry : values) {
        const auto offset = m_spill_offsets.at(entry.first.value);
        if (shortFrameSlot(offset)) {
            emitShortFrameAccess(true, entry.second, offset);
            have_address = false;
        } else {
            if (have_address && offset == previous + 2) { emitByte(0xd3); emitByte(0xd3); }
            else emitFrameAddress(Register::R3, offset);
            emitByte(static_cast<std::uint8_t>(0x10 | entry.second)); emitByte(0x43);
            have_address = true;
        }
        m_last_ram_word_address.rememberFrame(offset);
        previous = offset;
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

// Callee-inclusive static estimate: a function's own loop-weighted bytes plus,
// for each call, the call block's loop weight times the callee's inclusive
// estimate. A call inside a cycle contributes only the callee's own bytes.
// Like candidateSpeed, a deterministic proxy for selection, not a timing claim.
std::uint64_t IRCodeGenerator::inclusiveSpeed(const IRModule& module, const ObjectFile& object,
                                              const IRFunction* only,
                                              const std::map<std::string, std::uint64_t>* credit) const {
    if (module.functions.size() > 64) return 0;
    std::map<std::string, std::size_t> index;
    for (std::size_t n = 0; n < module.functions.size(); ++n) {
        const auto& f = module.functions[n];
        index.emplace(f.name, n); index.emplace(functionSymbol(f), n);
    }
    std::vector<std::uint64_t> own(module.functions.size()), memo(module.functions.size());
    std::vector<unsigned char> state(module.functions.size(), 0); // 0 new, 1 active, 2 done
    std::vector<bool> called(module.functions.size(), false);
    for (std::size_t n = 0; n < module.functions.size(); ++n) {
        own[n] = candidateSpeed(module, object, &module.functions[n]);
        const auto found = credit ? credit->find(module.functions[n].name) : decltype(credit->end()){};
        // Keep a nonzero estimate: zero means "no evidence" to the exchange.
        if (credit && found != credit->end()) own[n] = found->second < own[n] ? own[n] - found->second : 1;
    }
    // Bounded by 64 functions: an explicit stack is unnecessary for depth.
    std::function<std::uint64_t(std::size_t)> visit = [&](std::size_t n) -> std::uint64_t {
        if (state[n] == 2) return memo[n];
        if (state[n] == 1) return own[n];
        state[n] = 1;
        const auto& f = module.functions[n];
        IRControlFlow cfg(f);
        auto total = own[n];
        for (const auto& b : f.blocks) for (const auto& i : b.instructions) {
            if (i.opcode != IROpcode::Call) continue;
            const auto callee = index.find(i.symbol);
            if (callee == index.end()) continue;
            if (callee->second != n) called[callee->second] = true;
            total = GSUCost::saturatedAdd(total, GSUCost::saturatedMultiply(
                GSUCostModel::blockWeight(cfg.loop_depth.at(b.id.value)), visit(callee->second)));
        }
        state[n] = 2; memo[n] = total;
        return total;
    };
    if (only) {
        const auto found = index.find(only->name);
        return found == index.end() ? 0 : visit(found->second);
    }
    for (std::size_t n = 0; n < module.functions.size(); ++n) visit(n);
    std::uint64_t score = 0;
    bool rooted = false;
    for (std::size_t n = 0; n < module.functions.size(); ++n)
        if (!called[n]) { score = GSUCost::saturatedAdd(score, memo[n]); rooted = true; }
    if (!rooted) for (const auto value : own) score = GSUCost::saturatedAdd(score, value);
    return score;
}

// Os stays size-first. A candidate may cost a few bytes more than the smallest
// one seen only when its inclusive estimate is at least 25% lower: then a hot
// call is never kept merely to save a couple of bytes. The budget is fixed by
// the smallest candidate, so repeated exchanges cannot accumulate growth.
bool IRCodeGenerator::sizeFirstPreferred(const SizeChoice& proposed, const SizeChoice& existing, std::size_t smallest) {
    if (smallest && proposed.inclusive && existing.inclusive) {
        const auto budget = smallest + std::max<std::size_t>(2, smallest / 64);
        const auto clearly_faster = [](std::uint64_t a, std::uint64_t b) { return a < b && a <= b - b / 4; };
        if (proposed.bytes <= budget && clearly_faster(proposed.inclusive, existing.inclusive)) return true;
        if (existing.bytes <= budget && clearly_faster(existing.inclusive, proposed.inclusive)) return false;
    }
    if (proposed.bytes != existing.bytes) return proposed.bytes < existing.bytes;
    return proposed.speed < existing.speed;
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
        const auto inclusive = inclusiveSpeed(module, candidate.object, &f, &candidate.cache_credit);
        auto& smallest = m_function_smallest[f.name];
        smallest = smallest ? std::min(smallest, bytes) : bytes;
        const auto current = m_function_candidates.find(f.name);
        if (current != m_function_candidates.end()) {
            const auto& old = current->second;
            if (m_config.require_no_spills && old.no_spills != no_spills) { if (old.no_spills) continue; }
            else if (!sizeFirstPreferred({bytes, inclusive, speed}, {old.bytes, old.inclusive_score, old.speed_score}, smallest))
                continue;
        }
        FunctionCandidate best;
        best.function = f; best.policy = m_allocation_policy; best.shared_division = m_share_division;
        best.pool_enabled = m_pool_enabled;
        best.no_spills = no_spills; best.bytes = bytes; best.speed_score = speed; best.inclusive_score = inclusive;
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
    // Every candidate inlined or folded away an internal function without a
    // snapshot: keep it out only if no chosen function still calls it. Any
    // other missing snapshot is an error below.
    std::set<std::string> absent;
    for (const auto& f : mixed.functions)
        if (!m_function_candidates.count(f.name) && !f.link_name.empty() && f.link_name.front() == '\x01' && f.name != "main")
            absent.insert(f.name);
    mixed.functions.erase(std::remove_if(mixed.functions.begin(), mixed.functions.end(),
        [&](const IRFunction& f) { return absent.count(f.name) != 0; }), mixed.functions.end());
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
    IRGlobalOptimizer::removeDeadInternalFunctions(mixed);
    for (const auto& f : mixed.functions) for (const auto& b : f.blocks) for (const auto& i : b.instructions)
        if (i.opcode == IROpcode::Call)
            for (const auto& name : absent)
                if (i.symbol == name || i.symbol == std::string(1, '\x01') + name)
                    fail("IR codegen: missing bounded function candidate.", Token(TokenType::UNKNOWN, name, 1, 1));
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
        if (preferUncachedCandidate(uncached, result)) result = std::move(uncached);
    }
    m_allow_auto_cache = true; m_force_long_branches = false;
    m_function_policies.clear(); m_function_division_policies.clear(); m_function_pool_policies.clear();
    return result;
}
