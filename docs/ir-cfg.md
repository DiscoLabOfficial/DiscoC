# DiscoC IR and CFG

DiscoC lowers analyzed AST nodes into a typed intermediate representation (IR) before the default binary backend runs. The IR is designed to make control flow explicit, keep value references stable, and provide a verification boundary between the front end and target-specific emission.

## Module structure

The IR hierarchy is:

```text
IRModule
  └── IRFunction
        └── IRBasicBlock
              └── IRInstruction
```

An `IRModule` contains functions. Each function contains an entry block and zero or more additional basic blocks. Each instruction records its opcode, type, operands, optional result, control-flow targets, operation metadata, and source token.

## Stable identifiers

Values and blocks are referenced with small domain-specific IDs:

```cpp
struct IRValueId { std::uint32_t value; };
struct IRBlockId { std::uint32_t value; };
```

The IDs are stable handles within their owning function. Passes do not retain raw pointers into vectors that may reallocate when instructions or blocks are appended. `IRValueId{0}` is reserved as invalid, and block IDs use an explicit invalid sentinel.

The frontend representation is SSA-like: expression results have one definition,
while mutable storage uses addresses, loads and stores. O2 promotes eligible
scalar and eligible near-pointer locals to pruned SSA with typed PHI nodes;
escaped/volatile storage and unsupported aggregate paths remain explicit memory.
Near-pointer PHIs retain their full pointee/address-space/qualifier type; far
pairs are not promoted by this extension. This is a mode-specific IR transformation,
not a different source-language contract.

## Instruction categories

### Values and memory

* `constant` creates a literal value.
* `phi` selects an incoming value on a predecessor edge; operands and targets
  are paired values/predecessor IDs, not additional CFG successors.
* `address` materializes local, parameter, global, member, or compiler-temporary storage.
* `pointer.offset` applies the analyzed element stride and checked address reach.
* `pointer.compare` compares offset and bank (far) and produces bool.
* `load.indirect` reads through a typed address, carrying verified volatility.
* `store.indirect` writes through a typed, writable address, also carrying volatility.
* O2/Os `memory.initialize declare` performs ordered constant stores to one
  proven local near-RAM array. It owns 4–256 raw byte/word/bool values and one
  base-address operand, with a verified bounded extent. It is a memory-write
  barrier, not a pure value, ROM read, memcpy, or volatile-store substitute.
  Scalar stores remain the frontend representation.
* `binary`, `unary`, and `cast` represent typed expression operations.
* O2 `divmod /` or `divmod %` computes a word quotient/remainder pair, selecting
  the indicated primary component. `divmod.result` selects its other component;
  its one operand must be a dominating pair of the same type. The hidden second
  word belongs to the function, not a source variable or a second ABI result.
  Potential zero-divisor failure remains at the primary instruction, so DCE
  cannot discard it merely because its scalar result is unused.
* `call` represents a named function call and may produce a value.

### Hardware operations

The module records its target. Capabilities are verified independently of the
analyzer, including graphics, cache, hardware loops and far data. Explicit
`cursor.read/write` replace magic source symbol names; coordinate reads
are snapshotted like observable loads. `cache` records instruction-cache requests.

Graphics instructions have explicit `IRHardwareEffects`:

| Operation | State and effects |
| --- | --- |
| `pixel` | Read R1/R2, COLR/POR; write framebuffer/pixel caches; increment R1. |
| `color` | Update COLR using POR and the previous COLR. |
| `color rom.byte` | Address a typed ROM byte; use R14/ROM buffer and update COLR. |
| `cmode` | Update POR from a verified symbolic option mask. |
| `%value = rpix` / `rpix discard` | Read R1/R2, flush caches/read framebuffer; never increment R1. |

Only a direct final nonvolatile ROM byte read uses `rom.byte`; RAM, casts and
computed values remain ordinary loads/expressions. GETC selection is GSU-specific
and configures ROMB/R14; it is not an address-taking instruction. RPIX always
has observable effects, regardless of result use. Allocation excludes implicit
hardware registers, and cursor reads are retained snapshots rather than
rematerialized after PLOT. CMODE deduplication is local to known straight-line
state, with calls and block boundaries invalidating it.

Selected bitmap configuration is module metadata, separately validated and
serialized for the SNES host. It is not an executable GSU operation. See
[SuperFX graphics](gsu-graphics.md).

### Control flow

* `branch` has one target.
* `condbr` has one condition and two targets: true first, false second.
* `switch` has one condition, one target per case, and an optional default target.
* `return` and `return.void` terminate a function path.
* `unreachable` marks a merge block that cannot be reached after both branches terminate.
* At O2, `hardware_loop` is a setup terminator with one initial successor and
  a separate R13 `loop_target`; `hardware_loop.end` has backedge and exit
  successors. `hardware_loop.leave` restores the matching saved R12/R13 scope.
  A proven constant-trip ordinary loop can use this same representation;
  `automatic` is an internal emission hint, not new source syntax.

For statements have separate condition, body, increment and exit blocks.
Their continue target is the increment block; while continues target the
condition. Switch break targets do not override loop continuation. Compound
updates lower one address, one load and one store rather than duplicating AST
l-values. Constants/layout queries and interface headers disappear before
machine lowering; aggregate initializers become ordered scalar stores.

## Basic-block invariants

Every basic block must:

1. Have a valid stable ID.
2. Contain at least one instruction.
3. End in a terminator.
4. Contain no instruction after its terminator.
5. Refer only to valid value IDs and block IDs.

The verifier also checks the shape of branch, switch, and return instructions. For a switch, the case-value count must match the non-default targets, and the default target is stored last when present. Reachable-block dominance is computed with packed `uint64_t` bitsets so verifier memory scales with the number of blocks rather than the number of set-node allocations.

Before a backend consumes a function, verification also ensures that value IDs
have exactly one definition, form a contiguous sequence, and are used only
after their definitions. Definitions in different reachable blocks must
dominate their uses. Operand categories are checked for indirect loads/stores,
binary and unary operations, calls, conditions, switches, and returns. A
non-void function must return a value with a compatible type, while a void
function must use `return.void`.

The O1 GSU backend optimizes an owned copy through `IRLocalOptimizer` before
register allocation. Scalar casts/constants and representation-identical
copies are simplified; only unescaped, nonvolatile scalar locals can be
forwarded within a basic block. Effects and joins fence those memory facts.
Bounded local value numbering reuses pure scalar expressions with the same
captured SSA operands, not repeated lexical variable names. Overwritten local
stores can disappear only before observation, escape or a potentially failing
operation. Cascading DCE retains loads, checked addresses, calls and RPIX.
Mask/shift fusion introduces backend-local `bit.extract N %value`: one matching
word operand, no control-flow targets, and an index in 0..15. It returns that
raw bit as 0/1 in the same word type. Signed `(x & 0x8000) >> 15` is **not**
this operation: its result can be -1. The rewritten operand remains a real use
so the allocator preserves its lifetime. This is not new source syntax.
Removed definitions are aliased to dominating definitions, uses are rewritten,
and value IDs are compacted before verification runs again. The allocator
therefore sees the extended lifetimes created by forwarding, including spills
under pressure. Pointer checks, volatile loads, calls and RPIX are not removed.
`--emit-ir`/`--dump-ir` remain frontend inspection, identical between O0/O1.

The lowerer may retain a synthetic unreachable merge block after both arms of
an `if` terminate. Such a block is valid only when it contains a single
`unreachable` instruction; arbitrary disconnected IR is rejected.

## Example

The source:

```c
word choose(word value) {
    if (value > 0) {
        return 1;
    } else {
        return 2;
    }
}
```

is represented conceptually as:

```text
function choose() -> word {
  entry:
    %value = address @value
    %loaded = load.indirect %value
    %zero = const 0
    %condition = binary > %loaded, %zero
    condbr %condition -> if.then, if.else

  if.then:
    %one = const 1
    return %one

  if.else:
    %two = const 2
    return %two

  if.end:
    unreachable
}
```

The merge block is still represented so the CFG remains explicit, even though both incoming paths return.

## Lowering rules

Expressions are lowered recursively. L-values are lowered through `lowerAddress`; reads then add a `load.indirect`. Assignments lower the address and value before emitting a store. Array subscripting computes an indexed address using the analyzed element size. Structure member access adds the analyzer-provided member offset.

`if`, `while`, and `switch` create dedicated blocks and explicit edges. `break` resolves to the nearest active loop or switch exit block. Hardware-loop lowering emits a start instruction, lowers the body, and emits a matching hardware-loop end marker.

Logical AND/OR lower to conditional branches rather than eager binary
instructions. A word-aligned temporary holds their canonical bool result.
Comparisons and logical negation produce bool. Access types preserve per-layer
const/volatile qualifiers; the verifier checks memory flags and pointee types
and rejects ROM/const stores except permitted const declaration initialization.

Lexical plot context is per-instruction metadata, independent of block emission
order. Plot blocks can leave through break or return without needing a closing
hardware instruction on that edge.

The lowerer limits the number of generated blocks and values per function. These limits prevent malformed or adversarial source from growing one IR function without bound.

## Verification boundary

Normal object compilation follows this order:

```text
Analyzer -> IRLowerer -> IRVerifier -> IRCodeGenerator
```

If verification fails, target-specific emission does not start. This makes malformed IR a compiler error rather than an instruction-emission problem.

## Inspecting IR

Use:

```bash
discc path/to/program.dc --emit-ir
```

The command prints each function, block label, value definition, operands, and control-flow target. It does not write an object file.

## Extending the IR

When adding an instruction or pass:

1. Add the opcode and its metadata semantics.
2. Update `producesValue()` and `isTerminator()` when applicable.
3. Add verifier rules for operands, results, and targets.
4. Lower the relevant AST construct.
5. Teach `IRCodeGenerator` how to materialize or emit it.
6. Add a source example covering success and invalid forms.
7. Re-run `--emit-ir`, object compilation, assembly emission, and linking.

The backend retains loads/calls, cursor/RPIX snapshots and potentially faulting
arithmetic at their definitions. O0 uses the conservative aligned-spill path;
O1 keeps block-local results in scalar registers where available, with real
pressure/cross-block/far spills and only constant/plain-address rematerialization.
O1 comparison/branch fusion changes emission, not the inspected typed IR.
See [optimization](optimization.md). PHI and
multi-pass transformations must preserve IDs, memory effects, evaluation order,
and verifier invariants.

## O2 global analysis and SSA

`IRGlobalOptimizer` first exposes the lowerer's implicit hardware-loop pairs as
real CFG edges, with stable pair IDs. `IRControlFlow` computes successors,
predecessors, dominators/frontiers, natural loops and edge-aware fixed-point
liveness. Analysis owns a snapshot: rebuild it after changing values/edges.
It rejects unexposed hardware loops rather than omit their backedges.

Definitely initialized scalar locals/parameters without escape or volatility
are promoted using pruned dominance-frontier placement and dominator-tree
renaming. PHIs precede ordinary instructions, cover each actual predecessor
exactly once, match their incoming types and require each value to dominate
its predecessor edge. Loop PHIs may refer to a later-printed latch definition.
No source variable names are re-resolved by these passes.
Small aligned, statically indexed aggregate cells additionally use the original
declaration's `SymbolId` plus byte offset, with separate rename stacks/PHIs.
Unknown indices, escape, volatility, overlapping cells and illegal packed
accesses retain checked memory. GVN uses resolved scalar SSA operands and
dominance, never a reused source name or guessed load alias. Known bits/ranges
start at the full typed range and include every PHI backedge; wrap is not assumed
monotonic. Derived numeric induction PHIs preserve exact word wrap.

SCCP tracks Unknown/Constant/Variable facts and executable CFG edges to a fixed
point. Unknown is not a language-level undef or a guessed zero. A newly executed
backedge can change a PHI from Constant to Variable and expose the exit path.
Typed scalar constants flow across joins; constant conditional/switch edges
are selected, unreachable blocks removed, singleton PHIs resolved, and ordinary
single-predecessor chains merged. Constants replacing PHIs move after any
remaining PHI prefix. Block/value IDs, PHI predecessors and R13 targets are
compacted together before verification. Hardware setup/end/leave scopes remain
matched, and LOOP retains both taken/final edges independently of its count.

Conditional, switch and hardware edges into PHI blocks are split before
parallel-copy lowering. An initial hardware entry edge and its R13 backedge
can differ; both still reach the same loop body. The verifier separately tracks
the active hardware-scope stack across joins and exits, rejecting unmatched
setup/end/leave, changed R13 destinations and bypassed/nonnested restores.

O2 allocation uses exact interference, loop-weighted uses, PHI coalescing,
reusable aligned scalar spill slots and CFG-based live-across-call sets.
Only R5/R7/R8 are allocatable; cursor/ROM/stack/LOOP state stays reserved.
Late `live.split` Cast copies can partition captured invariant scalar uses
between a unique preheader/hot loop and cold code. They do not reread memory.
The verifier requires identical scalar representations; optimizers keep the
marker, and allocation checks each new value's real CFG interference. Trial
allocation must improve the bounded loop-weighted spill/copy estimate.
Safe LICM and bounded scalar-leaf inlining preserve observable memory and
fault points. A `pointer.offset scaled+`/`scaled-` carries an additional
unsigned-word scaled-index operand; original index/address checks remain.
See [optimization](optimization.md#what-o2-adds) for eligibility and budgets.

The O2 backend also derives conservative scalar intervals and near-RAM frame
or absolute-address facts from this typed, verified IR after frame sizing.
PHIs include every incoming value; unknown backedges are not discarded.
Only proved legal spans elide address/alignment/overflow checks. No new IR
opcode or speculative load is introduced. Stack-check credit is a separate
emitter fact fenced at joins; neither analysis changes the printed SSA.

After PHI edge splitting, bounded natural-loop layout keeps loop/backedge-copy
regions contiguous. Every block ID, PHI predecessor and hardware-loop target
is remapped before verification and allocation. Post-allocation byte scheduling
is separate from IR optimization: it handles proven one-byte slots, ROM-buffer
latency and CACHE-entry alignment while preserving encoded CODE references.

Inspect this optimized representation with `discc -O2 --emit-ir program.dc`.
`--check -O2` executes/verifies the same transformations without emission.
O0/O1 inspection remains unchanged, and the encoded-object assembly export
still round-trips byte for byte at every level.
