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
* `address` materializes a local, parameter, global data, function, member, or indexed address.
* `load.indirect` reads through an address.
* `store.indirect` writes through an address.
* `binary`, `unary`, and `cast` represent typed expression operations.
* `call` represents a named function call and may produce a value.

### Hardware operations

The IR has explicit operations for target-visible statements such as `plot`, `set_color`, `cmode`, `rpix`, and hardware loops. This keeps their side effects visible to the backend rather than hiding them in AST-specific code-generation visitors.

### Control flow

* `branch` has one target.
* `condbr` has one condition and two targets: true first, false second.
* `switch` has one condition, one target per case, and an optional default target.
* `return` and `return.void` terminate a function path.
* `unreachable` marks a merge block that cannot be reached after both branches terminate.

## Basic-block invariants

Every basic block must:

1. Have a valid stable ID.
2. Contain at least one instruction.
3. End in a terminator.
4. Contain no instruction after its terminator.
5. Refer only to valid value IDs and block IDs.

The verifier also checks the shape of branch, switch, and return instructions. For a switch, the case-value count must match the non-default targets, and the default target is stored last when present.

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

The backend currently materializes expression trees on demand. Introducing a register allocator, phi nodes, or multi-pass optimization should preserve the existing ID and verifier invariants.
