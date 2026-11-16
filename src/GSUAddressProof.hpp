#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "IR.hpp"

// Hardware-address proofs, not an alias analysis or an array-bounds checker.
// Facts are owned by the backend and contain no borrowed AST/IR pointers.
// The input IR is verified. checked_frame means the emitted entry checks
// establish the frame's extent, even alignment and parameter-area upper bound.
class GSUAddressProof {
public:
    // A static RAM object defined by the object file being emitted. The
    // linker places it inside one RAM bank at its alignment. An object whose
    // emitted code used such a proof (usedStaticObjectProof) carries
    // GSUAbi::StaticRamProofSymbol, and the linker then also rejects a static
    // RAM segment at address zero. Only then are its addresses nonnull.
    // A ROM object is defined in this object's DATA and read through the
    // near ROM bank. discld keeps all DATA with CODE inside one bank
    // (validatePayload) after at least one CODE byte, and rejects a near
    // DATA address outside the selected bank's ROM window in any object that
    // selects that bank. Every proved ROM read does so (NearRomBankSymbol).
    // Its facts never request the RAM marker. Object IDs are unique across
    // both spaces.
    struct StaticObject {
        std::uint32_t id = 0;
        std::uint32_t bytes = 0;
        unsigned alignment = 2;
        AddressSpace space = AddressSpace::RAM;
    };
    using StaticObjects = std::map<std::string, StaticObject>;

    void run(const IRFunction& function, const Analyzer::LocalSymbolTable& locals,
             const StaticObjects& statics, std::size_t frame_bytes, bool checked_frame);
    // Without static objects: frame/absolute/scalar facts only.
    void run(const IRFunction& function, const Analyzer::LocalSymbolTable& locals,
             std::size_t frame_bytes, bool checked_frame) {
        run(function, locals, StaticObjects{}, frame_bytes, checked_frame);
    }
    bool provesAccess(IRValueId value, const Type& pointer, int width) const;
    // True once a static-object fact proved an access since the last run().
    bool usedStaticObjectProof() const { return m_static_proof_used; }
    bool provesOffset(const IRInstruction& instruction) const;
    struct ScalarBounds {
        std::int64_t low = 0, high = 0;
    };
    // Numeric facts only; the caller must independently prove provenance,
    // object extent, mutability and lifetime. Failure leaves output intact.
    bool scalarBounds(IRValueId value, IRBlockId block, ScalarBounds& bounds) const;
    enum class MultiplyKind { FullWord, SignedByte, UnsignedByte };
    MultiplyKind multiplyKind(const IRInstruction& instruction) const;

private:
    // Symbol: low/high are byte displacements from the base of static object
    // `object`, whose size is `extent`. Displacements of different objects
    // are unrelated and never merge.
    enum class Kind { Unknown, Scalar, Frame, Absolute, Symbol };
    struct Fact {
        Kind kind = Kind::Unknown;
        std::int64_t low = 0, high = 0;
        unsigned alignment = 1;
        std::uint32_t object = 0, extent = 0;
        AddressSpace space = AddressSpace::RAM; // Symbol only: the object's space.
        bool operator==(const Fact& other) const {
            return kind == other.kind && low == other.low && high == other.high && alignment == other.alignment &&
                object == other.object && extent == other.extent && space == other.space;
        }
    };

    static Fact scalarRange(const Type& type);
    static unsigned commonAlignment(unsigned alignment, std::int64_t offset);
    Fact get(IRValueId value) const;
    Fact get(IRValueId value, std::uint32_t block) const;
    void refineControlFlow(const IRFunction& function,
                           const std::vector<const IRInstruction*>& definitions);
    Fact transfer(const IRInstruction& instruction, const Analyzer::LocalSymbolTable& locals,
                  const StaticObjects& statics, const std::vector<const IRInstruction*>& definitions) const;
    Fact displaced(Fact address, Fact displacement) const;

    std::vector<Fact> m_facts;
    std::vector<Fact> m_inductions;
    std::vector<std::uint32_t> m_definition_blocks;
    std::vector<std::map<std::uint32_t, Fact>> m_constraints;
    std::int64_t m_frame_low = 0, m_frame_end = 0;
    bool m_checked_frame = false;
    // Observability only: provesAccess stays logically const. Spurious
    // queries (planning) merely make the linker's requirement stricter.
    mutable bool m_static_proof_used = false;
    std::size_t m_block_count = 0;
};
