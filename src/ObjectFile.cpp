#include "ObjectFile.hpp"
#include "ABI.hpp"
#include "GSUCodeLayout.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>

void ObjectFile::write_u32_le(std::ostream& out, std::uint32_t value) {
    const std::uint8_t bytes[4] = {
        static_cast<std::uint8_t>(value & 0xffu),
        static_cast<std::uint8_t>((value >> 8) & 0xffu),
        static_cast<std::uint8_t>((value >> 16) & 0xffu),
        static_cast<std::uint8_t>((value >> 24) & 0xffu)};
    out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

std::uint32_t ObjectFile::read_u32_le(std::istream& in, const char* field_name) {
    std::uint8_t bytes[4] = {};
    in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!in) throw std::runtime_error(std::string("Object file: truncated ") + field_name + ".");
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void ObjectFile::write_u8(std::ostream& out, std::uint8_t value) {
    out.put(static_cast<char>(value));
}

std::uint8_t ObjectFile::read_u8(std::istream& in, const char* field_name) {
    char value = 0;
    in.get(value);
    if (!in) throw std::runtime_error(std::string("Object file: truncated ") + field_name + ".");
    return static_cast<std::uint8_t>(static_cast<unsigned char>(value));
}

void ObjectFile::write_string(std::ostream& out, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.length());
    write_u32_le(out, len);
    out.write(s.c_str(), len);
}

std::uint64_t ObjectFile::remaining_bytes(std::istream& in) {
    const auto current = in.tellg();
    if (current < 0) throw std::runtime_error("Object file: unable to inspect stream position.");
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    in.seekg(current);
    if (end < current) throw std::runtime_error("Object file: invalid stream bounds.");
    return static_cast<std::uint64_t>(end - current);
}

std::string ObjectFile::read_string(std::istream& in, const char* field_name) {
    const auto len = read_u32_le(in, field_name);
    if (len > MaxStringBytes || static_cast<std::uint64_t>(len) > remaining_bytes(in)) {
        throw std::runtime_error(std::string("Object file: invalid ") + field_name + " length.");
    }
    std::string s;
    try {
        s.resize(len);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error(std::string("Object file: unable to allocate ") + field_name + ".");
    }
    if (len > 0) in.read(&s[0], static_cast<std::streamsize>(len));
    if (!in) throw std::runtime_error(std::string("Object file: truncated ") + field_name + ".");
    return s;
}

template<typename T>
void ObjectFile::write_vec(std::ofstream& out, const std::vector<T>& vec) {
    uint32_t size = static_cast<uint32_t>(vec.size());
    write_u32_le(out, size);
    out.write(reinterpret_cast<const char*>(vec.data()), size * sizeof(T));
}

template<typename T>
void ObjectFile::read_vec(std::istream& in, std::vector<T>& vec,
                          std::uint32_t max_elements, const char* field_name) {
    const uint32_t size = read_u32_le(in, field_name);
    if (size > max_elements ||
        static_cast<std::uint64_t>(size) >
            std::numeric_limits<std::uint64_t>::max() / sizeof(T) ||
        static_cast<std::uint64_t>(size) * sizeof(T) > remaining_bytes(in)) {
        throw std::runtime_error(std::string("Object file: invalid ") + field_name + " size.");
    }
    try {
        vec.resize(size);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error(std::string("Object file: unable to allocate ") + field_name + ".");
    }
    if (size > 0) {
        in.read(reinterpret_cast<char*>(vec.data()), static_cast<std::streamsize>(size * sizeof(T)));
        if (!in) throw std::runtime_error(std::string("Object file: truncated ") + field_name + ".");
    }
}


// Main I/O Methods

bool ObjectFile::hasStaticRamProof() const {
    return std::any_of(symbol_table.begin(), symbol_table.end(),
        [](const SymbolEntry& symbol) { return symbol.name == GSUAbi::StaticRamProofSymbol; });
}

namespace {
bool staticStackMarker(const std::string& name) {
    const std::string prefix(GSUAbi::StaticStackBoundPrefix);
    return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0;
}
// Canonical decimal 1..65535 only, so equal bounds have one spelling.
std::uint32_t staticStackValue(const std::string& name) {
    const auto digits = name.substr(std::string(GSUAbi::StaticStackBoundPrefix).size());
    if (digits.empty() || digits.size() > 5 || digits.front() == '0') return 0;
    std::uint32_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return 0;
        value = value * 10u + static_cast<std::uint32_t>(c - '0');
    }
    return value <= 65535u ? value : 0;
}
} // namespace

std::uint32_t ObjectFile::staticStackBound() const {
    for (const auto& symbol : symbol_table)
        if (staticStackMarker(symbol.name)) return staticStackValue(symbol.name);
    return 0;
}

bool ObjectFile::usesShortRam() const {
    return !short_ram_section.empty() ||
        std::any_of(symbol_table.begin(), symbol_table.end(),
            [](const SymbolEntry& symbol) { return symbol.section == SymbolSection::SHORT_RAM; }) ||
        std::any_of(relocation_table.begin(), relocation_table.end(),
            [](const RelocationEntry& relocation) { return relocation.type == RelocationType::ADDR8_SHORT_RAM; });
}

bool ObjectFile::usesSPC700Relocations() const {
    return std::any_of(relocation_table.begin(), relocation_table.end(),
        [](const RelocationEntry& relocation) { return isSPC700Relocation(relocation.type); });
}

void ObjectFile::validate() const {
    GSUCodeLayout::alignment(*this);
    unsigned stack_markers = 0;
    for (const auto& symbol : symbol_table) {
        if (staticStackMarker(symbol.name)) {
            if (config.target != TargetKind::GSU || symbol.section != SymbolSection::RAM || symbol.offset != 0 ||
                !staticStackValue(symbol.name) || ++stack_markers > 1)
                throw std::runtime_error("Invalid GSU static stack-bound marker.");
            continue;
        }
        if (symbol.name != GSUAbi::StaticRamProofSymbol) continue;
        if (config.target != TargetKind::GSU || symbol.section != SymbolSection::RAM || symbol.offset != 0)
            throw std::runtime_error("Invalid GSU static-RAM proof marker.");
    }
    if (static_cast<std::uint8_t>(config.target) > static_cast<std::uint8_t>(TargetKind::SPC700) ||
        static_cast<std::uint8_t>(config.mapping) > static_cast<std::uint8_t>(MemoryMapping::HiROM))
        throw std::runtime_error("Object file: invalid target configuration.");
    config.bitmap.validate();
    if (config.bitmap.enabled && config.target != TargetKind::GSU) throw std::runtime_error("Bitmap configuration requires GSU.");
    if (!data_alignment || data_alignment > 128 || (data_alignment & (data_alignment - 1)) ||
        !ram_alignment || ram_alignment > 128 || (ram_alignment & (ram_alignment - 1)))
        throw std::runtime_error("Object file: section alignment must be a power of two in 1..128.");
    if (code_section.size() > MaxSectionBytes || data_section.size() > MaxSectionBytes || ram_section.size() > 65536 ||
        short_ram_section.size() > GSUAbi::ShortRamCapacity) {
        throw std::runtime_error("Object file section exceeds the supported size limit.");
    }
    if (usesShortRam() && config.target != TargetKind::GSU)
        throw std::runtime_error("Object file: short RAM requires GSU.");
    if (symbol_table.size() > MaxSymbolCount || relocation_table.size() > MaxRelocationCount) {
        throw std::runtime_error("Object file table exceeds the supported entry limit.");
    }
    const auto valid_name = [](const std::string& name) {
        if (name.empty() || name.size() > MaxStringBytes || (name.front() == '\x01' && name.size() == 1))
            throw std::runtime_error("Object file: invalid symbol name length.");
        for (std::size_t index = 0; index < name.size(); ++index) {
            const auto byte = static_cast<unsigned char>(name[index]);
            if ((byte < 32 && !(index == 0 && byte == 1)) || byte == 127)
                throw std::runtime_error("Object file: symbol names cannot contain control characters.");
        }
    };
    std::uint64_t serialized_bytes = 35u + (config.bitmap.enabled ? 10u : 0u) +
        static_cast<std::uint64_t>(code_section.size()) + data_section.size() + ram_section.size() +
        (usesShortRam() ? 4u + short_ram_section.size() : 0u);
    const auto charge = [&](std::uint64_t bytes) {
        if (serialized_bytes > MaxObjectBytes || bytes > MaxObjectBytes - serialized_bytes)
            throw std::runtime_error("Object file exceeds the supported total size limit.");
        serialized_bytes += bytes;
    };
    charge(0);
    try {
        std::set<std::string> names;
        for (const auto& symbol : symbol_table) {
            valid_name(symbol.name);
            if (static_cast<std::uint8_t>(symbol.section) > static_cast<std::uint8_t>(SymbolSection::SHORT_RAM))
                throw std::runtime_error("Object file: invalid symbol section.");
            const auto& section = symbol.section == SymbolSection::CODE ? code_section :
                symbol.section == SymbolSection::DATA ? data_section :
                symbol.section == SymbolSection::RAM ? ram_section : short_ram_section;
            if (symbol.offset > section.size())
                throw std::runtime_error("Object file has a symbol outside its section.");
            charge(9u + symbol.name.size());
            if (!names.insert(symbol.name).second)
                throw std::runtime_error("Object file has a duplicate symbol: " + symbol.name);
        }
        // Only operand bytes are patched; adjacent opcode/operand records may
        // legitimately touch. Sorting is O(n log n), independent of input order.
        struct Span { SymbolSection section; std::uint64_t begin, end; };
        std::vector<Span> spans;
        spans.reserve(relocation_table.size());
        for (const auto& relocation : relocation_table) {
            if (relocation.target_symbol_name == GSUCodeLayout::CacheAlignmentSymbol)
                throw std::runtime_error("Object file: CACHE layout hint is not an addressable symbol.");
            if (relocation.target_symbol_name == GSUAbi::StaticRamProofSymbol)
                throw std::runtime_error("Object file: static-RAM proof marker is not an addressable symbol.");
            if (staticStackMarker(relocation.target_symbol_name))
                throw std::runtime_error("Object file: static stack-bound marker is not an addressable symbol.");
            valid_name(relocation.target_symbol_name);
            validate_relocation(relocation, *this);
            charge(10u + relocation.target_symbol_name.size());
            const auto operand = relocationSpan(relocation.type);
            const auto begin = static_cast<std::uint64_t>(relocation.patch_offset) + operand.operand_offset;
            spans.push_back({relocation.section_to_patch, begin, begin + operand.operand_bytes});
        }
        std::sort(spans.begin(), spans.end(), [](const Span& left, const Span& right) {
            return left.section != right.section ? left.section < right.section : left.begin < right.begin;
        });
        for (std::size_t index = 1; index < spans.size(); ++index)
            if (spans[index].section == spans[index - 1].section && spans[index].begin < spans[index - 1].end)
                throw std::runtime_error("Object file has overlapping relocation operands.");
    } catch (const std::bad_alloc&) {
        throw std::runtime_error("Object file: unable to allocate validation tables.");
    }
    if (serialized_bytes > MaxObjectBytes)
        throw std::runtime_error("Object file exceeds the supported total size limit.");
}

void ObjectFile::write(const std::string& path) {
    // Validate before opening/truncating the destination, just as the reader
    // validates before exposing an object to relocation or assembly export.
    validate();

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Failed to open object file for writing: " + path);

    // Magic Header
    out.write("DISCO", 5);
    const std::uint8_t version = usesSPC700Relocations() ? SPC700RelocationFormatVersion :
        usesShortRam() ? ShortRamFormatVersion : staticStackBound() ? StaticStackFormatVersion :
        hasStaticRamProof() ? StaticRamProofFormatVersion : BaselineFormatVersion;
    write_u8(out, version);

    const auto target = static_cast<std::uint8_t>(config.target);
    write_u8(out, target);

    const auto mapping = static_cast<uint8_t>(config.mapping);
    write_u8(out, mapping);
    write_u32_le(out, config.code_start_address);
    write_u8(out, data_alignment);
    write_u8(out, ram_alignment);
    write_u8(out, config.bitmap.enabled ? 1 : 0);
    if (config.bitmap.enabled) {
        write_u8(out, config.bitmap.object_mode ? 1 : 0);
        write_u8(out, config.bitmap.depth);
        write_u32_le(out, config.bitmap.height);
        write_u32_le(out, config.bitmap.base);
    }

    // Write sections
    write_vec(out, code_section);
    write_vec(out, data_section);
    write_vec(out, ram_section);
    if (version >= ShortRamFormatVersion) write_vec(out, short_ram_section);

    // Write symbol table
    uint32_t sym_count = static_cast<uint32_t>(symbol_table.size());
    write_u32_le(out, sym_count);
    for (const auto& sym : symbol_table) {
        write_string(out, sym.name);
        write_u8(out, static_cast<std::uint8_t>(sym.section));
        write_u32_le(out, sym.offset);
    }

    // Write relocation table
    uint32_t reloc_count = static_cast<uint32_t>(relocation_table.size());
    write_u32_le(out, reloc_count);
    for (const auto& reloc : relocation_table) {
        write_string(out, reloc.target_symbol_name);
        write_u8(out, static_cast<std::uint8_t>(reloc.section_to_patch));
        write_u32_le(out, reloc.patch_offset);
        write_u8(out, static_cast<std::uint8_t>(reloc.type));
    }
    out.flush();
    if (!out) throw std::runtime_error("Failed to write object file: " + path);
    out.close();
    if (!out) throw std::runtime_error("Failed to close object file: " + path);
}

ObjectFile ObjectFile::read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Failed to open object file for reading: " + path);

    return read_stream(in, path);
}

ObjectFile ObjectFile::readBytes(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() > MaxObjectBytes)
        throw std::runtime_error("Object file exceeds the supported total size limit.");
    std::string serialized;
    if (!bytes.empty()) {
        serialized.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    std::istringstream in(serialized, std::ios::in | std::ios::binary);
    return read_stream(in, "<memory>");
}

ObjectFile ObjectFile::read_stream(std::istream& in, const std::string& path) {
    if (remaining_bytes(in) > MaxObjectBytes)
        throw std::runtime_error("Object file exceeds the supported total size limit: " + path);
    ObjectFile obj;
    char magic[5] = {0};
    in.read(magic, 5);
    if (!in || std::string(magic, sizeof(magic)) != "DISCO") {
        throw std::runtime_error("File is not a valid DiscoC object file: " + path);
    }

    const uint8_t version = read_u8(in, "format version");
    if (version < 3 || version > CurrentFormatVersion) {
        throw std::runtime_error("File has an unsupported DiscoC object format version: " + path);
    }

    const uint8_t target = read_u8(in, "target");
    if (target > static_cast<uint8_t>(TargetKind::SPC700)) {
        throw std::runtime_error("File has an invalid compiler target: " + path);
    }
    obj.config.target = static_cast<TargetKind>(target);

    const uint8_t mapping = read_u8(in, "memory mapping");
    obj.config.code_start_address = read_u32_le(in, "code start address");
    if (mapping > static_cast<uint8_t>(MemoryMapping::HiROM)) {
        throw std::runtime_error("File has an invalid target configuration: " + path);
    }
    obj.config.mapping = static_cast<MemoryMapping>(mapping);
    if (version >= 4) {
        obj.data_alignment = read_u8(in, "DATA alignment");
        if (!obj.data_alignment || obj.data_alignment > (version >= 6 ? 128 : 2) || (obj.data_alignment & (obj.data_alignment - 1)))
            throw std::runtime_error("Object file: invalid DATA alignment.");
    }
    
    if (version >= 6) {
        obj.ram_alignment = read_u8(in, "RAM alignment");
        if (!obj.ram_alignment || obj.ram_alignment > 128 || (obj.ram_alignment & (obj.ram_alignment - 1)))
            throw std::runtime_error("Object file: invalid RAM alignment.");
    }
    if (version >= 7) {
        const auto enabled = read_u8(in, "bitmap presence");
        if (enabled > 1) throw std::runtime_error("Object file: invalid bitmap presence flag.");
        obj.config.bitmap.enabled = enabled != 0;
        if (enabled) {
            const auto mode = read_u8(in, "bitmap mode");
            if (mode > 1 || obj.config.target != TargetKind::GSU) throw std::runtime_error("Object file: invalid bitmap target/mode.");
            obj.config.bitmap.object_mode = mode != 0;
            obj.config.bitmap.depth = read_u8(in, "bitmap depth");
            const auto height = read_u32_le(in, "bitmap height");
            if (height > 256) throw std::runtime_error("Object file: invalid bitmap height.");
            obj.config.bitmap.height = static_cast<std::uint16_t>(height);
            obj.config.bitmap.base = read_u32_le(in, "bitmap base");
            obj.config.bitmap.validate();
        }
    }
    read_vec(in, obj.code_section, MaxSectionBytes, "code section");
    read_vec(in, obj.data_section, MaxSectionBytes, "data section");
    if (version >= 5) read_vec(in, obj.ram_section, 65536u, "RAM section");
    if (version >= ShortRamFormatVersion) read_vec(in, obj.short_ram_section, GSUAbi::ShortRamCapacity, "short RAM section");

    const uint32_t sym_count = read_u32_le(in, "symbol count");
    if (sym_count > MaxSymbolCount || sym_count > remaining_bytes(in) / 10u) {
        throw std::runtime_error("Object file has an invalid symbol count: " + path);
    }
    try {
        obj.symbol_table.resize(sym_count);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error("Object file cannot allocate its symbol table: " + path);
    }
    for (uint32_t i = 0; i < sym_count; ++i) {
        obj.symbol_table[i].name = read_string(in, "symbol name");
        uint8_t section = 0;
        section = read_u8(in, "symbol section");
        if (section > static_cast<uint8_t>(version >= ShortRamFormatVersion ? SymbolSection::SHORT_RAM :
                version >= 5 ? SymbolSection::RAM : SymbolSection::DATA)) {
            throw std::runtime_error("Object file has an invalid symbol section: " + path);
        }
        obj.symbol_table[i].section = static_cast<SymbolSection>(section);
        obj.symbol_table[i].offset = read_u32_le(in, "symbol offset");
        const auto kind = obj.symbol_table[i].section;
        const auto section_size = kind == SymbolSection::CODE ? obj.code_section.size() :
            kind == SymbolSection::DATA ? obj.data_section.size() :
            kind == SymbolSection::RAM ? obj.ram_section.size() : obj.short_ram_section.size();
        if (obj.symbol_table[i].offset > section_size) {
            throw std::runtime_error("Object file has a symbol outside its section: " + path);
        }
    }

    const uint32_t reloc_count = read_u32_le(in, "relocation count");
    if (reloc_count > MaxRelocationCount || reloc_count > remaining_bytes(in) / 11u) {
        throw std::runtime_error("Object file has an invalid relocation count: " + path);
    }
    try {
        obj.relocation_table.resize(reloc_count);
    } catch (const std::bad_alloc&) {
        throw std::runtime_error("Object file cannot allocate its relocation table: " + path);
    }
    for (uint32_t i = 0; i < reloc_count; ++i) {
        obj.relocation_table[i].target_symbol_name = read_string(in, "relocation target name");
        uint8_t section = 0;
        section = read_u8(in, "relocation section");
        if (section > static_cast<uint8_t>(SymbolSection::DATA)) {
            throw std::runtime_error("Object file has an invalid relocation section: " + path);
        }
        obj.relocation_table[i].section_to_patch = static_cast<SymbolSection>(section);
        obj.relocation_table[i].patch_offset = read_u32_le(in, "relocation offset");
        const uint8_t type = read_u8(in, "relocation type");
        if (type > static_cast<uint8_t>(version >= SPC700RelocationFormatVersion ? RelocationType::SPC_IMM8_HI :
                version >= ShortRamFormatVersion ? RelocationType::ADDR8_SHORT_RAM :
                version >= 5 ? RelocationType::ADDR16_RAM : RelocationType::ADDR24_OFFSET)) {
            throw std::runtime_error("Object file has an invalid relocation type: " + path);
        }
        obj.relocation_table[i].type = static_cast<RelocationType>(type);
        validate_relocation(obj.relocation_table[i], obj);
    }

    if (in.peek() != std::istream::traits_type::eof()) {
        throw std::runtime_error("Object file has trailing data: " + path);
    }
    obj.validate();
    // Writers emit the marker only in version 8, which older linkers refuse.
    if (version < StaticRamProofFormatVersion && obj.hasStaticRamProof())
        throw std::runtime_error("Object file has a static-RAM proof marker before format version 8: " + path);
    if (version < StaticStackFormatVersion && obj.staticStackBound())
        throw std::runtime_error("Object file has a static stack-bound marker before format version 9: " + path);
    // Section/enum bounds above already confine short RAM to version 10.
    if (version == ShortRamFormatVersion && !obj.usesShortRam())
        throw std::runtime_error("Object file uses format version 10 without short RAM: " + path);
    if (version >= SPC700RelocationFormatVersion && !obj.usesSPC700Relocations())
        throw std::runtime_error("Object file uses format version 11 without SPC700 relocations: " + path);
    return obj;
}

void ObjectFile::validate_relocation(const RelocationEntry& relocation,
                                     const ObjectFile& object) {
    if (static_cast<std::uint8_t>(relocation.section_to_patch) > static_cast<std::uint8_t>(SymbolSection::DATA))
        throw std::runtime_error("Object file: invalid relocation section.");
    if (static_cast<std::uint8_t>(relocation.type) > static_cast<std::uint8_t>(RelocationType::SPC_IMM8_HI))
        throw std::runtime_error("Object file: invalid relocation type.");
    // Relocation kinds are tied to one instruction set; patching another
    // target's bytes with them would silently corrupt instructions.
    if (isSPC700Relocation(relocation.type) != (object.config.target == TargetKind::SPC700))
        throw std::runtime_error("Object file: relocation type does not match the object target.");
    if (relocation.target_symbol_name.empty()) {
        throw std::runtime_error("Object file has a relocation without a target symbol.");
    }
    const auto& section = relocation.section_to_patch == SymbolSection::CODE
        ? object.code_section : object.data_section;
    const std::uint64_t patch_size = relocationSpan(relocation.type).extent();
    if (static_cast<std::uint64_t>(relocation.patch_offset) + patch_size > section.size()) {
        throw std::runtime_error("Object file relocation extends beyond its section.");
    }
    // The operand is a word index only in ALT1/ALT2 + IBT-opcode form; any
    // other placement would patch an unrelated instruction byte.
    if (relocation.type == RelocationType::ADDR8_SHORT_RAM &&
        (object.config.target != TargetKind::GSU || relocation.section_to_patch != SymbolSection::CODE ||
         relocation.patch_offset == 0 || (section[relocation.patch_offset] & 0xf0u) != 0xa0u ||
         (section[relocation.patch_offset - 1] != 0x3d && section[relocation.patch_offset - 1] != 0x3e)))
        throw std::runtime_error("Object file: short RAM relocation must patch an LMS/SMS operand.");
}
