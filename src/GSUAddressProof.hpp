#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "IR.hpp"

// Hardware-address proofs, not an alias analysis or an array-bounds checker.
// Facts are owned by the backend and contain no borrowed AST/IR pointers.
// The input IR is verified. checked_frame means the emitted entry checks
// establish the frame's extent, even alignment and parameter-area upper bound.
class GSUAddressProof {
public:
    void run(const IRFunction& function, const Analyzer::LocalSymbolTable& locals,
             std::size_t frame_bytes, bool checked_frame);
    bool provesAccess(IRValueId value, const Type& pointer, int width) const;
    bool provesOffset(const IRInstruction& instruction) const;

private:
    enum class Kind { Unknown, Scalar, Frame, Absolute };
    struct Fact {
        Kind kind = Kind::Unknown;
        std::int64_t low = 0, high = 0;
        unsigned alignment = 1;
        bool operator==(const Fact& other) const {
            return kind == other.kind && low == other.low && high == other.high && alignment == other.alignment;
        }
    };

    static Fact scalarRange(const Type& type);
    static unsigned commonAlignment(unsigned alignment, std::int64_t offset);
    Fact get(IRValueId value) const;
    Fact get(IRValueId value, std::uint32_t block) const;
    void refineControlFlow(const IRFunction& function,
                           const std::vector<const IRInstruction*>& definitions);
    Fact transfer(const IRInstruction& instruction, const Analyzer::LocalSymbolTable& locals,
                  const std::vector<const IRInstruction*>& definitions) const;
    Fact displaced(Fact address, std::int64_t low, std::int64_t high, unsigned alignment) const;

    std::vector<Fact> m_facts;
    std::vector<Fact> m_inductions;
    std::vector<std::uint32_t> m_definition_blocks;
    std::vector<std::map<std::uint32_t, Fact>> m_constraints;
    std::int64_t m_frame_low = 0, m_frame_end = 0;
    bool m_checked_frame = false;
};
