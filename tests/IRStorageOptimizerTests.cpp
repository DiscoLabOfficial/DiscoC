#include "IRStorageOptimizer.hpp"
#include "IRCompactOptimizer.hpp"
#include "IRGlobalOptimizer.hpp"
#include "GSUAddressProof.hpp"
#include "IRScalarFold.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"

#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    IRModule module;
    std::map<std::string, Analyzer::LocalSymbolTable> locals;
};
Fixture prepare(const std::string& source, bool global = true) {
    Lexer lexer(source); const auto tokens = lexer.scanTokens(); Parser parser(tokens); auto program = parser.parseProgram();
    DataSegmentManager data; Analyzer analyzer(data); analyzer.analyze(program); IRLowerer lowerer;
    Fixture result{lowerer.lower(program), analyzer.getAllLocalSymbols()};
    if (global) IRGlobalOptimizer::run(result.module, result.locals);
    else IRCompactOptimizer::run(result.module, result.locals, OptimizationLevel::Size);
    return result;
}
IRStorageOptions pooled() { IRStorageOptions options; options.immutable_arrays = IRImmutableArrayPolicy::CopiedNearRamPayload; return options; }
void reject(const IRModule& module, const std::string& message) {
    try { IRVerifier::verify(module); }
    catch (const CompilerError& error) {
        require(error.getMessage().find(message) != std::string::npos, "Wrong storage verifier diagnostic"); return;
    }
    throw std::runtime_error("Invalid storage metadata was accepted");
}
SymbolId symbol(const IRFunction& function, const std::string& name) {
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
        if (instruction.opcode == IROpcode::Address && instruction.operation.empty() && instruction.symbol == name)
            return instruction.symbol_id;
    throw std::runtime_error("Fixture lost its expected storage address");
}

// Small independent typed-IR evaluator. Storage addresses really overlap when
// the plan says so, and pooled bytes come from their owned little-endian image.
std::int64_t evaluate(const IRFunction& function, const Analyzer::LocalSymbolTable& original, std::int64_t argument) {
    const auto& locals = function.storage_layout_valid ? function.storage_layout : original;
    std::map<std::int64_t, std::uint8_t> memory;
    std::map<SymbolId, std::int64_t> pool_addresses;
    std::int64_t pool_cursor = 0x2000;
    for (const auto& pool : function.immutable_local_arrays) {
        pool_addresses.emplace(pool.first, pool_cursor);
        for (const auto byte : pool.second.bytes) memory[pool_cursor++] = byte;
    }
    const auto store = [&](std::int64_t address, std::int64_t value, int width) {
        require(width == 1 || width == 2, "Unsupported evaluator storage width");
        memory[address] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) & 255);
        if (width == 2) memory[address + 1] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) >> 8) & 255);
    };
    if (!function.parameters.empty()) {
        const auto& parameter = function.parameters[0];
        store(0x8000 + locals.at(parameter.symbol_id).stackOffset, argument, parameter.type.sizeInBytes);
    }
    std::vector<std::int64_t> values(static_cast<std::size_t>(function.value_count) + 1);
    std::vector<const IRInstruction*> definitions(values.size(), nullptr);
    for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
        if (instruction.result.isValid()) definitions.at(instruction.result.value) = &instruction;
    IRBlockId current = function.entry, previous;
    for (unsigned steps = 0; steps < 20000; ++steps) {
        const auto& block = function.blocks.at(current.value);
        std::map<std::uint32_t, std::int64_t> phis;
        for (const auto& instruction : block.instructions) {
            if (instruction.opcode != IROpcode::Phi) break;
            bool found = false;
            for (std::size_t edge = 0; edge < instruction.targets.size(); ++edge)
                if (instruction.targets[edge].value == previous.value) {
                    phis[instruction.result.value] = values.at(instruction.operands[edge].value); found = true;
                }
            require(found, "Missing evaluator PHI edge");
        }
        for (const auto& phi : phis) values.at(phi.first) = phi.second;
        bool branch = false;
        for (const auto& instruction : block.instructions) {
            const auto operand = [&](std::size_t index) { return values.at(instruction.operands.at(index).value); };
            auto& result = values.at(instruction.result.value);
            switch (instruction.opcode) {
                case IROpcode::Phi: break;
                case IROpcode::Constant: result = ConstantEvaluator::convert(instruction.immediate, instruction.type); break;
                case IROpcode::Address:
                    if (instruction.operation == "member") result = operand(0) + instruction.immediate;
                    else if (pool_addresses.count(instruction.symbol_id)) result = pool_addresses.at(instruction.symbol_id);
                    else {
                        const auto& local = locals.at(instruction.symbol_id);
                        result = 0x8000 + local.stackOffset;
                        if (local.type.alignment > 2) result = (result + local.type.alignment - 1) & -local.type.alignment;
                    }
                    break;
                case IROpcode::PointerOffset:
                    require(instruction.operands.size() == 2, "Unsupported evaluator scaled offset");
                    result = operand(0) + (instruction.operation == "-" ? -1 : 1) * operand(1) * instruction.immediate; break;
                case IROpcode::MemoryInitialize:
                    for (std::size_t n = 0; n < instruction.initialization_values.size(); ++n)
                        store(operand(0) + static_cast<std::int64_t>(n) * instruction.type.sizeInBytes,
                              instruction.initialization_values[n], instruction.type.sizeInBytes);
                    break;
                case IROpcode::StoreIndirect: store(operand(0), operand(1), instruction.type.sizeInBytes); break;
                case IROpcode::LoadIndirect: {
                    auto raw = static_cast<std::int64_t>(memory[operand(0)]);
                    if (instruction.type.sizeInBytes == 2) raw |= static_cast<std::int64_t>(memory[operand(0) + 1]) << 8;
                    result = ConstantEvaluator::convert(raw, instruction.type); break;
                }
                case IROpcode::Binary:
                case IROpcode::Unary:
                case IROpcode::Cast: {
                    if (instruction.opcode == IROpcode::Cast && instruction.type.pointer_level > 0) {
                        // These fixtures use only verifier-checked literal
                        // near addresses, not a new arithmetic oracle.
                        result = operand(0); break;
                    }
                    std::array<IRScalarFold::Operand, 2> inputs{};
                    for (std::size_t n = 0; n < instruction.operands.size(); ++n)
                        inputs[n] = {&definitions.at(instruction.operands[n].value)->type, operand(n)};
                    require(IRScalarFold::evaluate(instruction, inputs, result), "Unsupported evaluator arithmetic"); break;
                }
                case IROpcode::BitExtract:
                    result = (static_cast<std::uint64_t>(operand(0)) >> static_cast<unsigned>(instruction.immediate)) & 1; break;
                case IROpcode::Branch:
                case IROpcode::CondBranch:
                    previous = current;
                    current = instruction.targets.at(instruction.opcode == IROpcode::CondBranch && !operand(0) ? 1 : 0);
                    branch = true; break;
                case IROpcode::Return: return operand(0);
                default: throw std::runtime_error("Unsupported evaluator opcode");
            }
            if (branch) break;
        }
        require(branch, "Evaluator failed to branch/return");
    }
    throw std::runtime_error("Evaluator exhausted its instruction budget");
}
void equivalent(const IRModule& before, const Fixture& after) {
    const auto& locals = after.locals.at(before.functions[0].name);
    for (int argument = -64; argument < 128; ++argument)
        require(evaluate(before.functions[0], locals, argument) == evaluate(after.module.functions[0], locals, argument),
                "Storage planning changed typed IR semantics");
}

void deadAndSharedStorage() {
    auto dead = prepare("word f(word p) { word dead[31]; word live[4]={1,3,5,7}; return live[p&3]; }");
    const auto before = dead.module; const auto original = dead.locals;
    IRStorageOptimizer::run(dead.module, dead.locals);
    const auto& f = dead.module.functions[0];
    require(f.storage_layout_valid && f.total_local_alloc_size == 10, "Dead array slots were not reclaimed");
    require(f.storage_layout.at(f.parameters[0].symbol_id).stackOffset == 6, "Parameter ABI offset changed");
    require(dead.locals.at("f").size() == original.at("f").size(), "Analyzer table was modified");
    for (const auto& entry : original.at("f"))
        require(dead.locals.at("f").at(entry.first).stackOffset == entry.second.stackOffset, "Analyzer offsets were mutated");
    equivalent(before, dead);
    const auto first = dumpIR(dead.module); IRStorageOptimizer::run(dead.module, dead.locals);
    require(first == dumpIR(dead.module), "Repeated storage planning is not idempotent");

    auto shared = prepare("word f(word p) { word a[4]={1,3,5,7}; if(p>0) return a[p&3]; word b[4]={2,4,6,8}; return b[p&3]; }", false);
    const auto unshared = shared.module;
    IRStorageOptimizer::run(shared.module, shared.locals);
    const auto& s = shared.module.functions[0];
    require(s.storage_layout_valid && s.total_local_alloc_size == 10, "Disjoint CFG lifetimes did not share a home");
    require(s.storage_layout.at(symbol(s, "a")).stackOffset == s.storage_layout.at(symbol(s, "b")).stackOffset,
            "Disjoint arrays did not reuse storage");
    equivalent(unshared, shared);

    auto live = prepare("word f(word p) { word a[4]={1,3,5,7}; word b[4]={2,4,6,8}; return a[p&3]+b[p&3]; }", false);
    const auto simultaneous = live.module; IRStorageOptimizer::run(live.module, live.locals);
    auto& l = live.module.functions[0];
    require(l.storage_layout.at(symbol(l, "a")).stackOffset != l.storage_layout.at(symbol(l, "b")).stackOffset,
            "Interfering arrays shared storage");
    equivalent(simultaneous, live);
    l.storage_layout.at(symbol(l, "b")).stackOffset = l.storage_layout.at(symbol(l, "a")).stackOffset;
    reject(live.module, "lifetime conflict");

    auto aligned = prepare("@align(16) struct Packet { word value; }; word f(word p) { word dead[8]; struct Packet packet; packet.value=p; return packet.value; }", false);
    const auto padded = aligned.module; IRStorageOptimizer::run(aligned.module, aligned.locals);
    const auto& a = aligned.module.functions[0];
    require(a.storage_layout_valid && a.total_local_alloc_size == 32 && a.storage_layout.at(symbol(a, "packet")).stackOffset == -30,
            "Over-aligned object lost its upward-rounding padding");
    equivalent(padded, aligned);
    auto empty = prepare("word f(){return 3;}"); IRStorageOptimizer::run(empty.module, empty.locals, pooled());
    require(empty.module.functions[0].storage_layout_valid && empty.module.functions[0].total_local_alloc_size == 0,
            "Empty storage acquired an unnecessary bottom word");
}

void malformedHomes() {
    auto fixture = prepare("word f(word p){word t[4]={11,22,33,44};return t[p&3];}");
    IRStorageOptimizer::run(fixture.module, fixture.locals);
    const auto root = symbol(fixture.module.functions[0], "t");
    const auto parameter = fixture.module.functions[0].parameters[0].symbol_id;
    require(fixture.module.functions[0].total_local_alloc_size == 10 &&
        fixture.module.functions[0].storage_layout.at(root).stackOffset == -8,
        "Exact-fit word storage lost its checked-frame boundary");
    IRVerifier::verify(fixture.module);
    auto broken = fixture.module;
    broken.functions[0].storage_layout.at(parameter).stackOffset = 8;
    reject(broken, "parameter ABI offsets");
    broken = fixture.module; broken.functions[0].storage_layout.at(root).stackOffset = -7;
    reject(broken, "identity or alignment");
    broken = fixture.module;
    broken.functions[0].storage_layout.at(root).stackOffset = -broken.functions[0].total_local_alloc_size;
    reject(broken, "exceeds the checked frame");
    broken = fixture.module;
    broken.functions[0].storage_layout.at(root).stackOffset = -broken.functions[0].total_local_alloc_size - 2;
    reject(broken, "exceeds the checked frame");
    broken = fixture.module; broken.functions[0].storage_layout.at(root).stackOffset = -6;
    reject(broken, "exceeds the checked frame");
    broken = fixture.module; broken.functions[0].storage_layout.at(root).type.alignment = 3;
    reject(broken, "identity or alignment");
    broken = fixture.module; broken.functions[0].storage_layout.at(root).type.sizeInBytes = std::numeric_limits<int>::max();
    reject(broken, "local object extent");
    broken = fixture.module; broken.functions[0].storage_layout.at(root).type.array_size = std::numeric_limits<int>::max();
    reject(broken, "local object extent");
    broken = fixture.module; broken.functions[0].storage_layout.erase(root);
    reject(broken, "absent from its owned storage layout");

    auto odd = prepare("word f(word p){byte t[3]={11,22,33};return t[p&1];}", false);
    IRStorageOptimizer::run(odd.module, odd.locals);
    const auto odd_root = symbol(odd.module.functions[0], "t");
    require(odd.module.functions[0].total_local_alloc_size == 6 &&
        odd.module.functions[0].storage_layout.at(odd_root).stackOffset == -4,
        "Odd-sized storage did not round up to an exact-fit even reservation");
    IRVerifier::verify(odd.module);
    odd.module.functions[0].storage_layout.at(odd_root).stackOffset = -2;
    reject(odd.module, "exceeds the checked frame");
}

void inlineLoopFallbacks() {
    for (const int count : {0, 1, 2}) {
        auto fixture = prepare("word f(word p){word t[4]={11,22,33,44};return t[p&3]+7;}", false);
        auto& f = fixture.module.functions[0]; require(f.blocks.size() == 1, "Inline scope requires one fixture block");
        auto original_code = f.blocks[0].instructions;
        IRInstruction saved; saved.opcode = IROpcode::Constant; saved.type = Type{BaseType::WORD, "", 2, false};
        saved.result = IRValueId{++f.value_count}; saved.immediate = 19;
        IRInstruction trips = saved; trips.result = IRValueId{++f.value_count}; trips.immediate = count;
        IRInstruction setup; setup.opcode = IROpcode::HardwareLoop; setup.loop_id = 1; setup.operands = {trips.result};
        IRInstruction end; end.opcode = IROpcode::HardwareLoopEnd; end.loop_id = 1;
        IRInstruction joined; joined.opcode = IROpcode::Binary; joined.type = saved.type; joined.operation = "+";
        joined.result = IRValueId{++f.value_count}; joined.operands = {saved.result, original_code.back().operands[0]};
        auto ret = original_code.back(); ret.operands = {joined.result}; original_code.pop_back();
        f.blocks[0].instructions = {saved, trips, setup};
        f.blocks[0].instructions.insert(f.blocks[0].instructions.end(), original_code.begin(), original_code.end());
        f.blocks[0].instructions.push_back(end); f.blocks[0].instructions.push_back(joined); f.blocks[0].instructions.push_back(ret);
        IRVerifier::verify(fixture.module);
        const auto before = dumpIR(fixture.module); const auto frame = f.total_local_alloc_size;
        IRStorageOptimizer::run(fixture.module, fixture.locals, pooled());
        require(dumpIR(fixture.module) == before && !f.storage_layout_valid && f.total_local_alloc_size == frame,
                "Empty-target hardware scope did not retain the original storage contract");
    }
    // Source-level zero trips are not the raw LOOP count zero (65536 trips).
    for (const int trips : {0, 2}) {
        auto fixture = prepare("word f(word p){word t[4]={11,22,33,44};for(word i=0;i<" +
            std::to_string(trips) + ";i++){}return t[p&3];}", false);
        const auto before = fixture.module; IRStorageOptimizer::run(fixture.module, fixture.locals, pooled());
        equivalent(before, fixture);
    }
}

void flatAliasSemantics() {
    for (const bool write : {false, true}) {
        // This evaluator's independently fixed FP is $8000. The first local
        // four-word array occupies $7FF8..$7FFF in the original layout.
        auto fixture = prepare(write
            ? "word f(word p){word t[4]={11,22,33,44};*(volatile word*)0x7ff8=99;return t[p&3];}"
            : "word f(word p){word t[4]={11,22,33,44};return t[p&3]+*(volatile word*)0x7ff8;}");
        const auto root = symbol(fixture.module.functions[0], "t");
        require(fixture.locals.at("f").at(root).stackOffset == -8, "Raw alias fixture lost its independent stack address");
        const auto before = fixture.module; const auto snapshot = dumpIR(before);
        IRStorageOptimizer::run(fixture.module, fixture.locals, pooled());
        require(dumpIR(fixture.module) == snapshot && !fixture.module.functions[0].storage_layout_valid &&
                fixture.module.functions[0].immutable_local_arrays.empty(), "Flat RAM alias acquired a storage/pool proof");
        equivalent(before, fixture);
        const int table[] = {11, 22, 33, 44};
        for (int index = 0; index < 4; ++index)
            require(evaluate(fixture.module.functions[0], fixture.locals.at("f"), index) ==
                (write ? (index ? table[index] : 99) : table[index] + 11), "Raw alias storage semantics were not observed");
    }
}

void poolAndFallbacks() {
    const std::string sine = "word sine(word phase) { word quarter[17]={0,6,12,19,24,30,36,41,45,49,53,56,59,61,63,64,64};"
        "phase &= 63; word index=phase&15; if((phase&16)!=0) index=16-index; word value=quarter[index];"
        "if((phase&32)!=0) value=-value; return value; }";
    auto fixture = prepare(sine); const auto before = fixture.module;
    auto disabled = fixture; IRStorageOptimizer::run(disabled.module, disabled.locals);
    require(disabled.module.functions[0].immutable_local_arrays.empty(), "Pool capability was enabled implicitly");
    IRStorageOptimizer::run(fixture.module, fixture.locals, pooled());
    auto& f = fixture.module.functions[0];
    require(f.storage_layout_valid && f.immutable_local_arrays.size() == 1 && f.total_local_alloc_size == 0,
            "Closed sine table was not pooled");
    const auto& bytes = f.immutable_local_arrays.begin()->second.bytes;
    require(bytes.size() == 34 && bytes[2] == 6 && bytes[3] == 0 && bytes[32] == 64, "Incorrect little-endian pool image");
    equivalent(before, fixture);
    for (const auto& block : f.blocks) for (const auto& instruction : block.instructions)
        require(instruction.opcode != IROpcode::MemoryInitialize, "Pooled initializer still executes");
    auto copy = fixture.module; copy.functions[0].immutable_local_arrays.begin()->second.bytes.pop_back();
    reject(copy, "array extent");
    copy = fixture.module; copy.functions[0].immutable_local_arrays.begin()->second.type.sizeInBytes = 1;
    reject(copy, "array extent");
    copy = fixture.module; copy.functions[0].storage_layout_valid = false; reject(copy, "valid owned layout");
    copy = fixture.module; copy.functions[0].storage_layout.clear(); reject(copy, "parameter ABI");
    copy = fixture.module;
    for (const auto& block : before.functions[0].blocks) for (const auto& initializer : block.instructions) {
        if (initializer.opcode != IROpcode::MemoryInitialize) continue;
        auto& entry = copy.functions[0].blocks[0].instructions;
        for (auto root = entry.begin(); root != entry.end(); ++root) if (root->result.value == initializer.operands[0].value) {
            entry.insert(std::next(root), initializer); break;
        }
    }
    copy.functions[0].total_local_alloc_size = before.functions[0].total_local_alloc_size;
    reject(copy, "lifetime conflict");
    copy = fixture.module;
    auto& code = copy.functions[0].blocks[0].instructions;
    const IRInstruction* address = nullptr; const IRInstruction* constant = nullptr;
    for (const auto& instruction : code) {
        if (instruction.opcode == IROpcode::Address && instruction.symbol == "quarter") address = &instruction;
        if (instruction.opcode == IROpcode::Constant && instruction.type.base == BaseType::WORD) constant = &instruction;
    }
    require(address && constant, "Mutation fixture has no pool root/constant");
    IRInstruction write; write.opcode = IROpcode::StoreIndirect; write.type = constant->type;
    write.operands = {address->result, constant->result}; code.insert(std::prev(code.end()), write);
    reject(copy, "lifetime conflict");

    const std::vector<std::string> frozen = {
        "word f(word* other,word p){word t[4]={1,2,3,4};return t[p&3]+*other;}",
        "word f(word p){word t[4]={1,2,3,4};return t[p&3]+*(word*)0x8000;}",
        "unsigned word f(word p){word t[4]={1,2,3,4};return (unsigned word)&t[p&3];}",
        "word f(word p){word t[4]={1,2,3,4};return t[(p&7)-1];}",
        "word f(word p){word t[4]={1,2,3,4};return t[4]+p;}",
        "word f(far word* other,word p){word t[4]={1,2,3,4};return t[p&3]+*other;}",
        "word f(word p){word t[4]={1,2,3,4};word* other=*(word**)0x0100;return t[p&3]+*other;}",
        "word f(word p){volatile word t[4]={1,2,3,4};return t[p&3];}",
        "word external(word* p);word f(word p){word t[4]={1,2,3,4};return external(&t[p&3]);}"
    };
    for (const auto& source : frozen) {
        auto negative = prepare(source); const auto frame = negative.module.functions[0].total_local_alloc_size;
        IRStorageOptimizer::run(negative.module, negative.locals, pooled());
        require(!negative.module.functions[0].storage_layout_valid && negative.module.functions[0].immutable_local_arrays.empty() &&
                negative.module.functions[0].total_local_alloc_size == frame, "Unknown alias/identity/out-of-object path changed layout");
    }
    auto mutable_array = prepare("word f(word p){word t[4]={1,2,3,4};t[p&3]=p;return t[p&3];}");
    const auto mutable_before = mutable_array.module;
    IRStorageOptimizer::run(mutable_array.module, mutable_array.locals, pooled());
    require(mutable_array.module.functions[0].immutable_local_arrays.empty(), "Mutable local array was pooled");
    equivalent(mutable_before, mutable_array);

    auto partial = prepare("word f(word p){word t[8]={1,2,3,4,5,6,7,8};return t[p&7];}");
    for (auto& block : partial.module.functions[0].blocks) for (auto& instruction : block.instructions)
        if (instruction.opcode == IROpcode::MemoryInitialize) instruction.initialization_values.pop_back();
    IRStorageOptimizer::run(partial.module, partial.locals, pooled());
    require(partial.module.functions[0].immutable_local_arrays.empty(), "Partial initializer was pooled");
}

void queryAndLimits() {
    auto fixture = prepare("word f(word p){word t[4]={1,2,3,4};return t[p&3];}");
    const auto& f = fixture.module.functions[0]; GSUAddressProof ranges;
    ranges.run(f, fixture.locals.at("f"), static_cast<std::size_t>(f.total_local_alloc_size), true);
    GSUAddressProof::ScalarBounds bounds{123, 456};
    require(!ranges.scalarBounds({}, f.entry, bounds) && bounds.low == 123 && bounds.high == 456, "Invalid scalar ID changed query output");
    require(!ranges.scalarBounds(IRValueId{f.value_count + 1}, f.entry, bounds), "Out-of-range scalar ID was accepted");
    require(!ranges.scalarBounds(IRValueId{1}, {}, bounds), "Invalid block ID was accepted");
    require(!ranges.scalarBounds(IRValueId{1}, IRBlockId{static_cast<std::uint32_t>(f.blocks.size())}, bounds), "Out-of-range block ID was accepted");
    IRValueId scalar, pointer;
    IRBlockId scalar_block, pointer_block;
    for (const auto& block : f.blocks) for (const auto& instruction : block.instructions) {
        if (instruction.opcode == IROpcode::PointerOffset) {
            scalar = instruction.operands.at(1); scalar_block = block.id;
        }
        if (instruction.opcode == IROpcode::Address && instruction.symbol == "t") {
            pointer = instruction.result; pointer_block = block.id;
        }
    }
    require(scalar.isValid() && pointer.isValid(), "Scalar query fixture lost its masked index or pointer");
    require(!ranges.scalarBounds(pointer, pointer_block, bounds), "Non-scalar address was accepted as numeric bounds");
    require(bounds.low == 123 && bounds.high == 456, "Failed scalar queries modified output");
    require(ranges.scalarBounds(scalar, scalar_block, bounds) && bounds.low == 0 && bounds.high == 3,
        "Successful scalar query did not replace the complete output interval");
    const auto snapshot = dumpIR(fixture.module);
    auto local = fixture.locals.at("f").begin()->second;
    for (std::uint32_t id = 1000; id < 1257; ++id) {
        local.id = SymbolId{id}; fixture.locals.at("f").emplace(local.id, local);
    }
    IRStorageOptimizer::run(fixture.module, fixture.locals, pooled());
    require(snapshot == dumpIR(fixture.module), "Bounded analysis fallback changed IR");
}
}

int main() {
    try { deadAndSharedStorage(); malformedHomes(); inlineLoopFallbacks(); flatAliasSemantics(); poolAndFallbacks(); queryAndLimits(); }
    catch (const std::exception& error) { std::cerr << "IR storage optimizer tests: " << error.what() << '\n'; return 1; }
    std::cout << "IR storage optimizer tests passed\n";
}
