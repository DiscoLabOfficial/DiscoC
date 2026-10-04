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

The current representation is SSA-like rather than a complete SSA implementation. Most expression results are single-definition values, while mutable variables and memory are represented through address, load, and store instructions. Phi nodes and general data-flow optimization are not implemented yet.

## Instruction categories

### Values and memory

* `constant` creates a literal value.
* `address` materializes local, parameter, global, member, or compiler-temporary storage.
* `pointer.offset` applies the analyzed element stride and checked address reach.
* `pointer.compare` compares offset and bank (far) and produces bool.
* `load.indirect` reads through a typed address, carrying verified volatility.
* `store.indirect` writes through a typed, writable address, also carrying volatility.
* `binary`, `unary`, and `cast` represent typed expression operations.
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

The backend retains loads/calls and potentially faulting arithmetic at their
definitions in explicit aligned spill slots. Pure expressions may be
materialized on demand; scalar allocation is conservative, with checked
pointer/volatile/plot functions using the spill path. Introducing phi nodes or
multi-pass optimization must preserve IDs, memory effects, evaluation order,
and verifier invariants.
