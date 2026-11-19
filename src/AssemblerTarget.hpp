#pragma once

// Boundary between the target-neutral discas driver and one instruction set.
// The driver (Assembler.cpp) owns parsing, sections, labels, the branch
// relaxation fixpoint, local branch resolution and symbol export. An encoder
// owns only what depends on the processor: instruction encodings, target
// configuration directives, extra sections and object metadata.

#include "AssemblerSupport.hpp"
#include "ObjectFile.hpp"
#include "TargetConfig.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AsmSupport {

// Services the driver lends to an encoder during one encode() call. The
// encoder never retains the reference beyond that call.
class EncodeContext {
public:
    virtual ~EncodeContext() = default;
    EncodeContext(const EncodeContext&) = delete;
    EncodeContext& operator=(const EncodeContext&) = delete;

    // True once the layout fixpoint selected the long form for this line. The
    // set only grows, so a line's size never shrinks between layout passes.
    virtual bool isRelaxed(int line_number) const = 0;
    // Records a relocation. Encoders call this only while emitting bytes
    // (output != nullptr), never during sizing passes.
    virtual void addRelocation(const RelocationEntry& relocation) = 0;
    // Records a signed 8-bit displacement byte at `patch_offset` in CODE. The
    // driver resolves it as `target - (patch_offset + 1)`, i.e. relative to the
    // byte after the displacement, which both supported ISAs place last.
    virtual void addBranchFixup(std::size_t patch_offset, const std::string& target, int line_number) = 0;

protected:
    EncodeContext() = default;
};

class TargetEncoder {
public:
    virtual ~TargetEncoder() = default;
    TargetEncoder(const TargetEncoder&) = delete;
    TargetEncoder& operator=(const TargetEncoder&) = delete;

    virtual TargetKind target() const = 0;

    // Handles `.define NAME VALUE` at parse time (`rest` is the text after the
    // directive). Throws a CompilerError for unsupported names or values.
    virtual void parseDefine(const std::string& rest, int line_number) = 0;
    // Called once after parsing, before layout.
    virtual void validateDefinitions() = 0;
    // Sections other than CODE/DATA/RAM are target extensions.
    virtual bool supportsSection(Section section) const = 0;
    // Called after every layout pass with the final size of target sections.
    virtual void validateLayout(std::size_t short_ram_size) const = 0;

    // Returns the encoded size of `line`. Appends bytes when `out` is non-null;
    // a null `out` is a sizing pass and must return the same size.
    virtual std::size_t encode(const ParsedLine& line, std::vector<std::uint8_t>* out, EncodeContext& context) = 0;

    // Reports whether `line` is a relaxable local branch. On success it names
    // the target label and the size of the short form; the displacement is
    // measured from the end of that short form.
    virtual bool shortBranch(const ParsedLine& line, std::string& target, std::size_t& short_size) const = 0;

    // Fills target configuration and marker symbols into a fresh object
    // before any section bytes are emitted.
    virtual void configureObject(ObjectFile& object) const = 0;

protected:
    TargetEncoder() = default;
};

// Returns the encoder for `target`. Throws CompilerError for a target whose
// assembler is not implemented.
std::unique_ptr<TargetEncoder> makeTargetEncoder(TargetKind target);

} // namespace AsmSupport
