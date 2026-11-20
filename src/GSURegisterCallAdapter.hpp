#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "ABI.hpp"
#include "ObjectFile.hpp"

// Link-time half of the GSU register-entry convention (GSUAbi::
// RegisterCallPrefix). The returned object defines every register entry that
// the inputs reference but none defines (an O0/O1 or assembly definition, or
// one whose body keeps stack arguments). Each adapter is an ordinary
// stack-convention caller:
//
//   checked PUSH R11, then checked PUSH of each argument register, last first
//   LINK #4 ; IWT R15,#name ; NOP
//   SP += 2N ; POP R11 ; JMP R11 ; NOP
//
// Every push keeps its own stack-limit check, so an exhausted stack stops
// with R6=2 at the same word it would have reached through a stack call.
// R0/R4 (the return value) and R9 survive; R3/R6 are volatile scratch.
// An empty object (no code) means no adapter is needed.
inline ObjectFile makeGSURegisterCallAdapters(const std::vector<ObjectFile>& objects) {
    ObjectFile adapters;
    if (objects.empty() || objects.front().config.target != TargetKind::GSU) return adapters;
    adapters.config = objects.front().config;
    std::set<std::string> defined;
    for (const auto& object : objects) {
        std::set<std::string> exported;
        for (const auto& symbol : object.symbol_table)
            if (symbol.section == SymbolSection::CODE && !symbol.name.empty() && symbol.name.front() != '\x01')
                exported.insert(symbol.name);
        for (const auto& symbol : object.symbol_table) {
            std::string name;
            std::size_t count = 0;
            if (!GSUAbi::parseRegisterCallSymbol(symbol.name, name, count)) continue;
            // The entry is only meaningful next to the function it enters.
            if (symbol.section != SymbolSection::CODE || !exported.count(name))
                throw std::runtime_error("Register entry '" + symbol.name + "' is defined without its function '" + name + "'.");
            defined.insert(symbol.name);
        }
    }
    std::map<std::string, std::size_t> missing;
    for (const auto& object : objects) for (const auto& relocation : object.relocation_table) {
        std::string name;
        std::size_t count = 0;
        if (GSUAbi::parseRegisterCallSymbol(relocation.target_symbol_name, name, count) &&
            !defined.count(relocation.target_symbol_name))
            missing.emplace(relocation.target_symbol_name, count);
    }
    auto& code = adapters.code_section;
    const auto byte = [&](std::uint8_t value) { code.push_back(value); };
    const auto relocate = [&](const std::string& symbol, RelocationType type) {
        adapters.relocation_table.push_back({symbol, SymbolSection::CODE, static_cast<std::uint32_t>(code.size()), type});
    };
    const auto checkedPush = [&](std::uint8_t reg) {
        // IWT R3,#limit ; FROM R10 ; ALT3 CMP R3 ; BCS over ; NOP ;
        // IBT R6,#2 ; STOP ; NOP -- then the generated PUSH sequence.
        relocate(std::string(GSUAbi::StackLimitPrefix) + "2", RelocationType::ADDR16_RAM);
        byte(0xf3); byte(0); byte(0);
        byte(0xba); byte(0x3f); byte(0x63);
        byte(0x0d); byte(5); byte(0x01);
        byte(static_cast<std::uint8_t>(0xa0 | GSUAbi::AddressFaultRegister)); byte(2); byte(0x00); byte(0x01);
        if (reg) byte(static_cast<std::uint8_t>(0x20 | reg));
        byte(0x3a); byte(0xea); byte(0xea);
    };
    for (const auto& entry : missing) {
        std::string name;
        std::size_t count = 0;
        GSUAbi::parseRegisterCallSymbol(entry.first, name, count);
        adapters.symbol_table.push_back({entry.first, SymbolSection::CODE, static_cast<std::uint32_t>(code.size())});
        checkedPush(GSUAbi::LinkRegister);
        for (std::size_t p = count; p-- > 0;) checkedPush(GSUAbi::RegisterArguments[p]);
        byte(0x94);
        relocate(name, RelocationType::ADDR16_JAL);
        byte(0xff); byte(0); byte(0);
        byte(0x01);
        byte(static_cast<std::uint8_t>(0x20 | GSUAbi::StackPointerRegister)); byte(0x3e);
        byte(static_cast<std::uint8_t>(0x50 | (count * GSUAbi::ParameterSlotSize)));
        byte(static_cast<std::uint8_t>(0xd0 | GSUAbi::StackPointerRegister));
        byte(static_cast<std::uint8_t>(0xd0 | GSUAbi::StackPointerRegister));
        byte(static_cast<std::uint8_t>(0x20 | GSUAbi::LinkRegister)); byte(0x4a);
        byte(static_cast<std::uint8_t>(0x90 | GSUAbi::LinkRegister)); byte(0x01);
        // At most 99 bytes each; stop before input-controlled growth exceeds
        // the one program bank that layout would reject anyway.
        if (code.size() > 65536u)
            throw std::runtime_error("GSU register-entry adapters exceed one program bank.");
    }
    return adapters;
}
