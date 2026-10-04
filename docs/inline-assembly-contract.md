# Inline assembly and target ABI extension contracts

## Status

This is a **design contract for future implementation**, not accepted source
syntax. Inline assembly is not implemented. DiscoC does not provide an opaque
`asm("text")` escape hatch: text alone cannot describe the registers and effects
needed by register allocation, scheduling, observable memory, or checked frames.
No example below claims an executable inline-ASM interface.

The current attribute parser provides extension points. `@interrupt`, `@naked`,
`@section`, `@bank`, `@calling_convention`, and `@inline` are reserved names;
all uses are rejected with an explicit not-implemented diagnostic. Reserving
them does not change the default ABI, placement, or code generation.

## Minimum inline-ASM contract

Before assembly can enter the IR, every block must describe:

- **Target:** the exact processor and required capabilities. Unknown or
  unavailable targets cannot fall back to another instruction set.
- **Inputs/uses:** typed expressions, evaluated once in language order, with
  checked value width, register class, and optional fixed-register constraints.
- **Outputs:** typed values or explicit writable destinations. Tied input/output
  operands and early-clobbered outputs must be distinguished; an output cannot
  overwrite an input that still has to be read.
- **Clobbers:** registers, register pairs, arithmetic flags, and target state,
  not merely the registers visible in the text. Impossible/conflicting
  constraints are diagnostics, not unchecked register assignments.
- **Memory effects:** no access, declared reads, or declared reads/writes;
  unknown memory effects conservatively block movement of conflicting accesses.
  Volatile/observable blocks execute exactly once when reached, even without
  used outputs, and preserve language ordering with other observable effects.
- **Control flow:** initially one entry and normal fallthrough only. Hidden
  branches out of the block, calls, return, STOP, or stack manipulation require
  a separately verified control-flow/ABI extension, not undocumented text.

Inputs and outputs must remain explicit IR use/definition relationships.
Allocation must account for the block's interference: a value live across an
ASM clobber is spilled, preserved, or allocated elsewhere. Output registers must
not alias incompatible live values. Optimizers cannot rematerialize observable
blocks or move memory accesses across conflicting effects. The verifier must
check operand types, constraints, effects, and target-state boundaries.

## GSU state is more than integer registers

The present ABI allocates scalar values in R5/R7/R8. A block that overwrites one
must declare that clobber; the compiler cannot assume ordinary call preservation
will cover it. R9/R10 own the frame/stack, R11 carries the link, and R15 is PC;
they are not freely clobberable scratch registers. R1/R2 also represent lexical
plot coordinates, R4 participates in far-pointer returns, R6 is scratch/fault
state, R12/R13 own hardware loops, and R14 serves ROM reads.

The implementation must additionally model arithmetic flags, ALT prefixes,
source/destination selectors, RAMBR/ROMBR/PBR, ROM-buffer state, instruction cache,
graphics state, and any pending/delay-slot effects used by the selected
instructions. A block must exit in the state expected by compiler-generated
instructions. Temporary data-bank changes must restore the configured near
bank before any compiler spill, stack access, or following near access.
Unmodeled interbank control transfers are not acceptable inline-ASM operations.

Assembler parsing/encoding should reuse the existing checked assembler, with
source locations for operand/encoding failures. Arbitrary instruction bytes
cannot bypass the declared-effects contract. Compiler exports must retain their
byte-exact assembly round-trip guarantee.

## Requirements for reserved attributes

These are minimum obligations, not implemented argument grammars:

| Attribute | Required contract before enabling |
| --- | --- |
| `@interrupt` | Target-specific entry/vector convention, saved registers/flags/banks, stack ownership, reentrancy policy, and interrupt return sequence |
| `@naked` | No generated frame or cleanup; a restricted, verified body/exit contract that does not use ordinary local/call assumptions |
| `@section` | Object section metadata, alignment, linker placement and diagnostics, distinct from source address-space qualifiers |
| `@bank` | Target-visible bank bounds, code/data relocation rules, and explicit near/far interactions; no silent bank crossing |
| `@calling_convention` | Named typed ABI, matching prototypes/definitions, register/stack lowering, and link-time incompatibility checks |
| `@inline` | A verified transformation preserving side effects, scopes, control flow, diagnostics, and resource bounds; not a promise of raw substitution |

Ordinary source attributes must not impersonate linker configuration or silently
enable aggregate-by-value/function-pointer ABIs. Existing unannotated code must
retain its calling convention when these extensions arrive.

## Acceptance before enabling the feature

Tests must cover values live across ASM in R5/R7/R8, tied operands and
early-clobbers, impossible constraints, unused observable outputs, duplicate
volatile reads/writes, memory barriers around calls, protected frame/loop/plot
registers, bank restoration, flags/selectors, and delay slots. Include positive
execution results, negative diagnostics, malformed assembly, bounded parsing,
and direct/assembled/linked byte comparisons. Interrupt/naked/alternate-ABI
features additionally require target-specific entry/exit tests. They remain
disabled until these contracts can be verified.
