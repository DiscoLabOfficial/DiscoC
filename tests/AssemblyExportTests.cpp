#include "Assembler.hpp"
#include "AssemblyGenerator.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(const ObjectFile& object) {
    const auto reconstructed = Assembler().assemble(AssemblyGenerator(object).generate());
    if (object.code_section != reconstructed.code_section ||
        object.data_section != reconstructed.data_section || object.config != reconstructed.config)
        throw std::runtime_error("Assembly round trip changed bytes or placement");
}
}
int main() {
    try {
        // Independent hand-encoded coverage of every raw opcode and ALT form.
        // These are encoding tests, not claims that every sequence is runnable.
        for (unsigned op = 0; op < 256; ++op) {
            for (const unsigned prefix : {0u, 0x3du, 0x3eu, 0x3fu}) {
                ObjectFile object;
                const std::size_t offset = prefix ? 1 : 0;
                object.code_section.resize(5 + offset, 0);
                if (prefix) object.code_section[0] = static_cast<std::uint8_t>(prefix);
                object.code_section[offset] = static_cast<std::uint8_t>(op);
                object.code_section.back() = 1;
                check(object);
            }
        }
        ObjectFile object;
        object.config.code_start_address = 0x706000;
        object.code_section = {0xa1, 0, 0xf2, 0, 0, 0xff, 0, 0, 1, 0, 1};
        object.data_section = {0xff, 0x00, 0x80};
        object.symbol_table = {{"main", SymbolSection::CODE, 0},
            {std::string(1, '\x01') + "private.block", SymbolSection::CODE, 9},
            {"value", SymbolSection::DATA, 0}};
        object.relocation_table = {{"value", SymbolSection::CODE, 0, RelocationType::ADDR24_BANK},
            {"value", SymbolSection::CODE, 2, RelocationType::ADDR24_OFFSET},
            {std::string(1, '\x01') + "private.block", SymbolSection::CODE, 5, RelocationType::ADDR16_IWT}};
        check(object);
        const auto rebuilt = Assembler().assemble(AssemblyGenerator(object).generate());
        if (rebuilt.relocation_table.size() != 3 ||
            rebuilt.relocation_table[0].type != RelocationType::ADDR24_BANK ||
            rebuilt.relocation_table[1].type != RelocationType::ADDR24_OFFSET ||
            rebuilt.relocation_table[2].target_symbol_name.front() != '\x01')
            throw std::runtime_error("Assembly export lost far/private relocation semantics");
        for (const auto& bytes : {std::vector<std::uint8_t>{0xf0}, {0xa0}, {0x05, 0x7f}}) {
            ObjectFile malformed;
            malformed.code_section = bytes;
            bool rejected = false;
            try { (void)AssemblyGenerator(malformed).generate(); }
            catch (const std::runtime_error&) { rejected = true; }
            if (!rejected) throw std::runtime_error("Malformed assembly export input was accepted");
        }
        std::cout << "Assembly export round trips passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
