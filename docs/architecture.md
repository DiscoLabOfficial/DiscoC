# DiscoC Architecture

DiscoC is a small compiler toolkit for specialized hardware targets. The current
code-generation backend targets the SuperFX/GSU processor; the repository also
contains the initial target model for a future SPC-700 backend.
The normative source semantics are [Language Baseline 0.1](language-spec.md);
architecture describes its implementation, not a second language contract.

## End-to-end pipeline

The normal object-file path is:

```text
.dc source
    |
    v
Lexer -> Parser -> AST -> Analyzer -> Optimizer
                                      |
                                      v
                               IRLowerer
                                      |
                                      v
                               IRVerifier
                                      |
                                      v
                       target backend selection
                         /              \\
                        v                v
                 IRCodeGenerator   future SPC700 backend
                                      |
                                      v
                              relocatable .o
                                      |
                                      v
                               discld linker
                                      |
                                      v
                         linked target payload
```

The compiler also exposes inspection and alternate-emission paths:

* `discc --emit-ast` prints the optimized abstract syntax tree.
* `discc --emit-ir` prints the verified IR and its basic blocks.
* `discc --emit-asm` exports the canonical backend's encoded GSU object as textual assembly. That assembly can be passed to `discas` to create a relocatable object with equivalent bytes and references.
* `discld --emit-asm final.s` exports the linked payload with numeric, resolved addresses, including any requested startup code.

The linked `.bin` is a GSU payload. It is not a complete SNES ROM image: it does not provide a SNES header, host-side startup integration, cartridge metadata, or other ROM-level resources.

## Compiler stages

### Project orchestration

`discc build` reads a bounded `discoc.toml` manifest and runs the same in-process
compiler and linker drivers as individual invocations. The CLI adapters own
argument strings; no shell/subprocess protocol or mutable global driver state
is involved. `disco_project_core` owns TOML schema decoding, option precedence,
and path handling without depending on AST/backend code. `ProjectBuild` discovers
the full source/interface graph before any writes, then compiles implementations
dependency-first and links only if all units succeed. Explicit roots retain link
order; discovered sources are appended once. See [project-manifest.md](project-manifest.md)
for supported schema, target limitations, and failure behavior.

The source-level contract and ordered language refactoring are documented in
[language-spec.md](language-spec.md). That document distinguishes implemented
semantics from planned rules, including the implemented near/far data-pointer
ABI, independent access qualifiers, deterministic integers, static storage,
and unsupported aggregate-by-value/code-far ABI.

### Lexer and parser

The lexer converts source text into tokens. The parser constructs an owning
AST using `std::unique_ptr`. Syntax errors are reported before analysis; parser
recursion and expression-tree depth have explicit diagnostic limits. Lexical
`plot` blocks own their body rather than representing context only as pairs.
`ModuleLoader` parses the module/import preamble first, loads bounded source and
interface graphs, then supplies owned alias bindings before parsing the remaining unit.
Transitive aliases use stable table IDs and declaration-origin identity for
deduplication; the parser never borrows another parser's token/type storage.
Inactive `@cfg` declarations are syntactically checked and discarded before
semantic analysis. Aliases preserve canonical type metadata and generate no IR.

### AST optimization

The analyzer runs before AST transformations. This ordering guarantees that
an optimization cannot erase an invalid expression before it receives a
diagnostic. The optimizer then operates on resolved `SymbolId` references;
current transformations include recognizing suitable loops for the GSU
hardware `LOOP` instruction and simplifying selected small arithmetic
operations. Transformations are deliberately conservative when a loop value
is observable outside the loop or control flow can escape it.

### Semantic analysis

The analyzer resolves functions, scopes, variables, structures, ROM/RAM data,
linkage, types, pointer operations, and control flow. Each pointer layer carries
reach, address space, and const/volatile access qualifiers. Structs are
memory-resident; unsupported by-value ABI forms are rejected. Analysis also
calculates stack offsets/local allocation sizes.

Function prototypes may declare a function without defining its body. The
prototype participates in semantic checking of calls in that compilation
unit, while the definition can be emitted by another source file and linked
later. Prototypes do not generate code or duplicate object-file symbols.

### IR lowering and verification

`IRLowerer` converts the analyzed AST into a typed, control-flow-aware IR. `IRVerifier` checks structural invariants before code generation. This keeps target-independent compiler structure separate from GSU or SPC-700 byte encoding.

### Target backends

The default object path uses `IRCodeGenerator`. It consumes only verified IR plus analyzed symbol/data information and emits GSU instructions into the project object format.

The `SPC700Target` model records the SPC-700 address width, memory-mapped
regions, register roles, and initial return-value convention. It is a
foundation-only target at present: `--emit-ir` can inspect programs selected
for SPC-700, while object and assembly emission reject that target until its
lowering and assembler stages exist.

`IRCodeGenerator` supports baseline O0 (default), local O1 and global O2. All reserve
hardware/ABI registers and use R5/R7/R8 for scalar allocation. R0 is the
expression accumulator; plotting reserves R1/R2 and uses R3 as scratch.
Observable loads/calls/RPIX and faulting arithmetic retain definition-time
evaluation across policies, except proven private-local memory eliminated by
SSA. Const/volatile qualifiers describe memory
accesses, not register-save categories; calls preserve caller-live values.

O0 retains the v0.1.0 materializer and aligned observable-result spills. Its
checked-pointer/volatile/plot path bypasses scalar allocation and retains
checked values in frame slots. O1 instead emits nontrivial definitions eagerly
and allocates single-use/reused block-local scalar results, spilling only when
required. Only constants/plain addresses can be rematerialized. Far pairs,
cross-block results and functions with implicit hardware-loop backedges remain
conservative frame values. Far slots are four bytes; no ABI is changed.
Before O1 allocation, `IRLocalOptimizer` rewrites an owned copy of verified IR:
scalar constant/cast folding, identity-copy elimination, and basic-block
forwarding for nonvolatile, nonescaping scalar locals identified by SymbolId.
It excludes pointer-valued locals, arrays, aggregates and globals; calls,
unknown writes, volatile accesses, framebuffer effects and implicit loop
boundaries fence memory facts. It compacts value IDs and verifies the result
before emission. Frontend AST/IR inspection still shows the language IR, not
this backend-local copy. Individual local/CFG branches widen only when needed,
with a bounded long-form fallback; O0's emission policy is unchanged.
See [optimization](optimization.md) for instruction selection and boundaries.

O2 first runs `IRGlobalOptimizer` on the owned copy: explicit hardware-loop
CFG edges, pruned scalar-local SSA/PHIs, bounded same-unit leaf inlining and
constant specialization, safe LICM and checked scaled-index induction.
`IRConditionalOptimizer` performs bounded SCCP/CFG cleanup after SSA promotion,
again after inlining and after loop transforms. Its scalar evaluator is shared
with the local pass. Executable edges, not merely structural predecessors,
determine PHI constants; hardware counters and unknown memory are not inferred.
`IRControlFlow` owns dominator/frontier/natural-loop/liveness snapshots.
Small nonescaping local aggregate cells use `(SymbolId, byte offset)` as
independent SSA identities; storage types, alignment and definite assignment
remain explicit. A bounded promotion/constant-folding fixed point discovers
static indices exposed by earlier cells. `IRValueOptimizer` owns pure scalar
dominator-scoped GVN, conservative bits/intervals, modular numeric recurrences
and late hot/cold lifetime partitioning. It never numbers memory or graphics
state, infers pointer-address intervals, or speculates checked arithmetic.
The allocator colors exact CFG interference rather than physical block-order
interval hulls, favors hot loop uses, reuses noninterfering scalar spill slots
and preserves CFG-live values at calls. PHI edge blocks schedule simultaneous
copies directly between allocated registers; register-only cycles use volatile
R6 scratch, while mixed register/spill cycles retain guarded stack snapshots.
Loop-weighted, bounded two-color recoloring and spill-slot preferences preserve
the exact interference graph. Explicit hardware scopes
save/restore R12/R13, including nested loops and loop-using callees.
Types, IDs, predecessor coverage, dominance and hardware scope stacks are
verified after transformations. O0/O1 policies and object/call ABI remain
unchanged. `--emit-ir -O2` and `--check -O2` expose/verify this optimized IR.
An internal `live.split` representation-identical scalar Cast separates a
loop snapshot from cold uses. The verifier validates its type/dominance, and
cleanup preserves its marker. Each SSA part is still assigned one location;
this is bounded IR lifetime partitioning, not arbitrary instruction-position
location changes or an ABI/object-format extension.

`GSUCostModel` counts selection/allocation-related fetch bytes, RAM transfer
bytes and caller preservation under capped static loop weights. O2 compares
the previous interference coloring with one bounded cost-priority candidate;
the candidate must reduce the warm-CACHE pressure score without increasing
the uncached score. This is a heuristic, not exact cycle simulation or PGO.
`GSUSpillCache` tracks borrowed SSA snapshots, never owning memory or changing
an SSA value's authoritative location. Exact point liveness excludes operands,
destinations and PHI-edge/backedge values from its spare-register set. A copy
in R5/R7/R8 may replace another load of the private spill, but the frame store
remains; calls, CFG/helper labels, bank changes and physical writes invalidate
copies. Ordinary memory/volatile reads still execute once per distinct IR load.
The optional Mesen profiler uses labels from the byte-exact final assembly,
not IR offsets predating machine scheduling/relocation. See the
[cost/profiling contract](optimization.md#profiling-cost-model-and-allocation).

`IRDivModFusion` matches resolved word operands using dominance after scalar
cleanup. Internal `divmod`/`divmod.result` keep the first operation's fault point
and share its sixteen-step helper. The backend reserves an aligned secondary
frame word per pair; the verifier checks type, component and dominance.
Final local cleanup compacts IDs after layout. Emission also shares identical
in-range terminal faults and function-local epilogues, using existing branch
relaxation and retaining per-path loop/plot cleanup and R0/R4 return values.

After the complete local/spill frame is known, `GSUAddressProof` owns bounded
scalar-range and near-RAM address facts keyed by IR IDs. It can prove member
and masked/PHI-index accesses inside the validated ABI frame or known absolute
RAM spans; unknown/far/ROM accesses remain checked. It changes selection, not
IR or alias semantics. `GSUStackCheckCredit` separately reuses established
lower bounds at guarded pushes without moving failures ahead of effects.
CFG/helper entries and calls fence the credit. See the
[proof contract](optimization.md#what-o2-adds) for limits and exclusions.

Natural-loop layout keeps hot blocks and their PHI backedge copies together
before allocation. After relaxation, `GSUMachineScheduler` makes bounded,
proven one-byte slot and ROM-read scheduling changes and remaps CODE references.
Cached function alignment is honored at final link time through a private
version-7-compatible hint. Compiler and linked assembly remain byte-exact.
Machine scheduling is not part of the frontend's printed SSA or `--check` path.

The analyzer records every pointer layer's reach/address space and each
l-value's storage-address type. IR carries these types and an explicit
`pointer.offset` operation with its validated element stride. The backend does
not recover a pointer variable's address space from its leaf type or spelling.
IR load/store volatility is checked against its address type. Comparisons
produce byte-sized bool; logical AND/OR lower to short-circuit CFG blocks with
an aligned temporary. Each instruction carries lexical plot-context metadata,
so return/break and block emission order cannot leak plotting state.
Software integer division uses a bounded 16-step core; shifts validate counts.
Integer overflow/casts follow the modular contract in the language specification.

Stateful graphics IR tracks R1/R2 cursor access, COLR/POR updates, ROM-buffer
dependencies and pixel-cache effects. `pixel` emits one PLOT, relying on its
hardware R1 increment. `read_pixel` and `flush` share RPIX with value/discard
forms; both stay observable. The backend uses GETC only for a direct final ROM
byte, configuring ROMB/R14 and restoring a temporary far bank. Other colors
evaluate normally and use COLOR. Bitmap configuration is owned module/object
metadata for host SCMR/SCBR setup, not a stream of GSU writes.

Comparisons produce `0` or `1` values. O1/O2 may fuse an adjacent single-use
comparison/conditional branch without materializing its boolean. Signed
relations use the GSU signed branch conditions and
unsigned relations use carry conditions, so `>`, `>=`, `<`, and `<=` remain
distinct at equality and sign-bit boundaries. This also makes nested
comparisons ordinary expressions in direct and assembly workflows.

`AssemblyGenerator` borrows the encoded `ObjectFile` and decodes its instruction
stream; it no longer lowers AST expressions independently. Symbols and
relocations are preserved in compiler exports. Uncommon encodings use explicit
`.byte` directives rather than potentially different instruction expansions.
Reassembling an unedited export and linking with the same settings must preserve
the complete payload byte for byte.

## Object and link stages

Each compilation unit can produce a relocatable `.o` file. The object stores
code, ROM data, a separate RAM initialization image, public/private symbols,
and relocation records. All multi-byte object
fields use explicit little-endian encoding. `discld` verifies that all input
objects use the same target configuration, concatenates code and data sections,
resolves symbols, applies relocations, and writes the final payload. Local
branches that exceed the short displacement range are relaxed by the assembler
or canonical IR backend into object-relative absolute jumps.

The linker lays out CODE then ROM DATA in the payload. Object versions 4
through 7 record DATA alignment. Version 5 introduced static RAM; version 6
adds its explicit alignment; version 7 adds optional bitmap host metadata.
RAM is allocated separately from `--ram-origin`
in the near RAM bank; optional startup zeroes/initializes it. Padding is included in each symbol/relocation base.
Internal names are object-scoped across CODE, DATA, and RAM; external lookup
cannot resolve them from another object.
Selected bitmap profiles must agree across objects. The linker validates their
framebuffer reservations against RAM payloads, static data and initial stack
placement, and includes known framebuffer boundaries in stack-floor checks.
Both the canonical IR backend and `discas` re-emit out-of-range local branches
using object-relative absolute jumps; short branches retain their compact encoding.

For switches, constant selectors are lowered to a direct branch. Dynamic
switches with four or more cases use a balanced comparison tree; smaller
switches retain the compact linear form. Case blocks and fall-through edges
remain explicit in the IR, so this is a dispatch optimization only.

## GSU ABI conventions

The compiler targets the instruction set described in Nintendo's official SNES
Development Manual, Book II, Super FX section. The calling convention below is
DiscoC's own ABI: the hardware manual does not define its pointer layout, stack
slots, or fault-reporting protocol. External assembly support remains optional.

The relevant register conventions used by the compiler are:

| Register | Convention |
| --- | --- |
| `R0` | expression result and first return-value register |
| `R1` / `R2` / `R3` | backend temporaries; R1/R2 preserved inside plot contexts |
| `R4` | canonical bank word for a far pointer; R0 carries its offset |
| `R5` / `R7` / `R8` | allocated scalar value registers |
| `R6` | caller-saved scratch; address/arithmetic failure category at a fault STOP |
| `R9` | frame pointer used by generated functions |
| `R10` / `SP` | stack pointer |
| `R11` | link/return address register |
| `R12` | hardware-loop counter when a `LOOP` is emitted |
| `R13` | hardware-loop target register when required by setup |
| `R14` | ROM buffer/address register for ROM reads |
| `R15` / `PC` | program counter and call target register |

Generated functions save the link and frame registers, establish `R9` as the
frame pointer, allocate aligned local storage, and restore the frame before
returning. The stack is empty descending: `push` stores at `SP`, then decrements
it by two. Consequently, `FP` points to an empty slot, saved `R9` is at `FP + 2`,
saved `R11` at `FP + 4`, and the first parameter at `FP + 6`. Locals use negative
offsets. Frames with locals reserve an additional empty word below the last
local so expression pushes and nested calls cannot overwrite local storage.
Stack arguments are word-aligned. Scalars and near pointers use two bytes;
far pointers use four: bank plus zero padding, then little-endian offset. For
arguments `(byte, far word*, word)`, parameter offsets are `FP+6`, `FP+8`, and
`FP+12`. The caller pushes a far argument's offset before its bank word and
cleans up the sum of actual slot widths. Far returns use `(R0 offset, R4 bank)`.

Calls emit `LINK #4; IWT R15, target; NOP`. The `NOP` fills the instruction
already prefetched before the program counter changes; the link address points
after that delay slot. Caller argument cleanup starts there. Returns use
`JMP R11; NOP`. Long local jumps use `IWT R15, target; NOP`; `JMP R15` is not a
GSU instruction (`0x9F` encodes `FMULT`). Conditional long jumps also reserve a
`NOP` after the short inverted branch before the `IWT` sequence.

Register copies use `WITH source; TO destination`, including copies from `R0`.
Without `WITH`, `TO` only selects the destination of a later instruction.

The ABI classifies `R9` and `R11` as callee-preserved. `R0`, `R1`, `R2`, `R3`, `R4`, `R6`, and
the allocated value registers `R5`, `R7`, and `R8` are volatile/caller-saved; the
caller saves any allocated value that remains live across a call. `R10` is
owned by the stack frame, `R12`/`R13` are reserved for hardware loops, `R14`
is the ROM address register, and `R15` is the program counter. Calls within a
plot context additionally save and restore R1/R2; far return registers remain
intact during cleanup.

Far data accesses select ROMBR/RAMBR temporarily and restore the link-configured
near bank immediately after access, before spill/stack traffic. Checks enforce
even typed word addresses, canonical banks, descriptor padding, bank-local
multi-byte arithmetic, and representable narrowing. Dynamic failure emits
`STOP; NOP` with an R6 category; it must not be resumed. Category definitions and
byte-pointer carry/borrow domains are specified in
[language-spec.md](language-spec.md#nearfar-data-pointers).

## Target configuration

The object format carries the target, memory mapping, and code start address.
The supported target identifiers are `GSU` and `SPC700`; the supported SNES
mappings are `LoROM` and `HiROM`. The linker rejects a set of input objects
when their target configurations are incompatible.

For GSU payloads, the linker additionally validates the execution origin against
the documented ROM/RAM windows, limits the combined code/data to one program
bank, and validates relocation address widths and near-target banks before
writing output. Mapping labels supply compatibility metadata and default
origins; they do not implement cartridge ROM construction or bank switching.
`discld --origin` overrides input origins consistently before layout/relocation;
target and mapping compatibility are still required. Optional `--init-runtime`
prepends RAMBR/R10 setup and an entry jump, rebasing all sections and relocations.
With globals, startup also encodes RAM zero/scalar initialization. Without
it, nonempty RAM requires the explicit `--host-initialized-globals` contract.
Without globals the default remains host-owned startup with no added bytes. See
[gsu-loading.md](gsu-loading.md) for RAM bank, stack, and entry requirements.
See [object-format.md](object-format.md#gsu-address-and-bank-validation) for the
placement contract and host-initialization requirements.

Target selection and placement are build-level CLI/manifest configuration:

```bash
discc --target gsu --memory-mapping lorom --execution-memory ram --origin 0x710000 program.dc -o program.o
```

LoROM defaults to ROM execution at `$00:8000`. Explicit HiROM defaults to
`$40:8000`; RAM execution defaults to `$70:8000`. An explicit origin wins
regardless of argument order and must match an explicitly chosen region.
Source-level `set` configuration is rejected with a migration diagnostic.

Execution memory is encoded in the full object origin, not a third mapping.
Format v6 adds RAM alignment and generalizes alignment to powers of two through
128. Readers retain v3/v4/v5 support; older writers/readers do not understand v6.

The assembly emitter preserves mapping and origin through reserved `.define`
metadata understood by `discas`. RAM selection does not generate a loader:
the SNES host must copy the payload, configure GSU program registers/bus access, and
keep the stack and writable data away from the code. The flat payload layout
does not provide separate ROM storage for `rom const` data in a RAM build.

The SPC-700 target model and ABI proposal are documented in
[`spc700-target.md`](spc700-target.md).

## Language front-end extensions

`ModuleLoader` owns bounded source intake, dependency edges and cached ASTs for
one invocation/build. It resolves local-first imports plus configured search
directories, deduplicates physical files and diagnoses cycles before output.
Each source transfers its owning AST to one compiler invocation; graph edges use
stable indices, never references into growing vectors. `ModuleInterface` creates
owned public declaration projections without bodies/storage or `internal` names.
Imports never borrow another unit's mutable AST. Dependency-first analysis
refreshes public constants and layouts before an importer uses them, so private
compile-time implementation names do not leak. Failed discovery clears the
partial graph; imported tokens preserve diagnostic origins. `ConstantEvaluator`
evaluates pure typed expressions; layout/enum constants never require runtime
calls. `LanguageWarnings` is a read-only source pass, with conservative scalar
definite assignment and bounded, configurable diagnostics. `PlacementOptions`
separates CLI/build choices from program syntax. Real ForStmt and UpdateExpr
nodes preserve lexical scopes, continuation targets and single-evaluation
l-values until IR lowering. Attributes are parsed uniformly and validated by
explicit handlers; capabilities are centralized in TargetConfig.

The freestanding `lib/core` uses ordinary language/IR operations for fixed-point
and byte memory functions. `lib/targets/gsu` layers graphics wrappers over target
capabilities; libraries are explicitly compiled/linked, not injected intrinsics.
`@cfg` selects declarations/imports without a preprocessor, while `@target`
asserts a function's processor. Inline ASM and reserved ABI attributes remain
disabled pending the [effects/ABI contract](inline-assembly-contract.md).

## Ownership and stability model

AST nodes own their child nodes. IR graphs do not store pointers into resizable instruction or block vectors; values and blocks are referenced by stable numeric IDs owned by an `IRFunction`. The backend builds short-lived lookup tables while processing one function and does not make the IR own target byte buffers.

This separation is intentional: the AST and IR are compiler-phase data, while `ObjectFile` owns the emitted code/data vectors and serialized object contents.

## Current boundaries

The execution regressions use a bounded instruction-level model of the GSU
prefetch pipeline, register selectors, two RAM banks, RAM instruction fetch,
RAMB/ROMB, ROM buffer reads and mirror views, stack memory, comparisons, calls,
branches, functional instruction caching and pixel-cache/bitplane behavior.
Hand-encoded checks validate the model, and linked payloads validate
both the IR backend and assembly path. Unsupported opcodes, invalid program
reads, mismatched expectations, and instruction-limit exhaustion fail tests.
This model does not emulate a complete SNES, cache timing, bus ownership,
PPU display, or interrupts; emulator and hardware validation remain necessary.

The project is pre-1.0 compiler infrastructure. O1 allocation remains block-local
with real pressure spills; far pairs and cross-block values retain aligned
frame slots. O2 extends scalar SSA allocation across CFG/loop edges but keeps
far pairs, aggregates, escapes and volatile storage conservative. O0 preserves
the baseline policy. Far code
calls and multi-bank code placement are not implemented. Assembly output uses DiscoC's
assembler dialect, not WLA-DX. Fixed-origin linked exports cannot be moved to an
arbitrary execution offset without relinking; native WLA-DX export and
position-independent loading are separate future features.
