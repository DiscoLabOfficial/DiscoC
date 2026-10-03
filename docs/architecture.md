# DiscoC Architecture

DiscoC is a small compiler toolkit for specialized hardware targets. The current
code-generation backend targets the SuperFX/GSU processor; the repository also
contains the initial target model for a future SPC-700 backend.

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

### Lexer and parser

The lexer converts source text into tokens. The recursive-descent parser constructs an owning AST using `std::unique_ptr` for child nodes. Syntax errors are reported before semantic analysis begins.

### AST optimization

The analyzer runs before AST transformations. This ordering guarantees that
an optimization cannot erase an invalid expression before it receives a
diagnostic. The optimizer then operates on resolved `SymbolId` references;
current transformations include recognizing suitable loops for the GSU
hardware `LOOP` instruction and simplifying selected small arithmetic
operations. Transformations are deliberately conservative when a loop value
is observable outside the loop or control flow can escape it.

### Semantic analysis

The analyzer resolves functions, scopes, variables, structures, ROM data, types, pointer operations, and control-flow-related semantic rules. It also calculates stack offsets and local allocation sizes used by the backend.

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

Before emission, `IRCodeGenerator` runs a linear-scan allocation over the
verified `IRValueId` live intervals. Reused values may reside in `R5`, `R7`, or
`R8`; `R0` remains the expression accumulator and `R1`/`R3` remain backend
temporaries. Values used only once may be materialized at their use site. Pure
values that do not fit may be rematerialized, while loads and calls with
multiple uses require a register or an explicit spill slot and are rejected
until such a slot is available. Calls preserve allocated live values while the
existing stack-based argument ABI remains unchanged.

Comparisons are materialized as `0` or `1` values before they are consumed by
control flow. Signed relations use the GSU signed branch conditions and
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
code, ROM data, exported symbols, and relocation records. All multi-byte object
fields use explicit little-endian encoding. `discld` verifies that all input
objects use the same target configuration, concatenates code and data sections,
resolves symbols, applies relocations, and writes the final payload. Local
branches that exceed the short displacement range are relaxed by the assembler
or canonical IR backend into object-relative absolute jumps.

The linker currently lays out all code before all data. Symbol addresses are calculated from the configured code start address and the accumulated section offsets. Both the canonical IR backend and `discas` re-emit out-of-range local branches using object-relative absolute jumps; short branches retain their original compact encoding.

For switches, constant selectors are lowered to a direct branch. Dynamic
switches with four or more cases use a balanced comparison tree; smaller
switches retain the compact linear form. Case blocks and fall-through edges
remain explicit in the IR, so this is a dispatch optimization only.

## GSU ABI conventions

The compiler targets the GSU instruction set and ABI described in Nintendo's official SNES Development Manual, Book II, Super FX section. The generated code follows the project's documented GSU conventions while keeping external assembly support optional.

The relevant register conventions used by the compiler are:

| Register | Convention |
| --- | --- |
| `R0` | expression result and first return-value register |
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
Stack arguments are word-aligned.

Calls emit `LINK #4; IWT R15, target; NOP`. The `NOP` fills the instruction
already prefetched before the program counter changes; the link address points
after that delay slot. Caller argument cleanup starts there. Returns use
`JMP R11; NOP`. Long local jumps use `IWT R15, target; NOP`; `JMP R15` is not a
GSU instruction (`0x9F` encodes `FMULT`). Conditional long jumps also reserve a
`NOP` after the short inverted branch before the `IWT` sequence.

Register copies use `WITH source; TO destination`, including copies from `R0`.
Without `WITH`, `TO` only selects the destination of a later instruction.

The ABI classifies `R9` and `R11` as callee-preserved. `R0`, `R1`, `R3`, and
the allocated value registers `R5`, `R7`, and `R8` are caller-preserved; the
caller saves any allocated value that remains live across a call. `R10` is
owned by the stack frame, `R12`/`R13` are reserved for hardware loops, `R14`
is the ROM address register, and `R15` is the program counter. Every argument
occupies one aligned two-byte stack slot, including byte arguments.

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
The default remains host-owned startup with no added bytes. See
[gsu-loading.md](gsu-loading.md) for RAM bank, stack, and entry requirements.
See [object-format.md](object-format.md#gsu-address-and-bank-validation) for the
placement contract and host-initialization requirements.

Build-level target selection is provided on the command line, while source
directives configure target-specific placement details:

```c
set memory_mapping = lorom;
set code_start_address = 0x8000;
```

```bash
discc --target gsu program.dc -o program.o
```

LoROM is the default cartridge mapping, with ROM execution at `$00:8000`.
HiROM remains available explicitly (`set memory_mapping = hirom;`, default
origin `$40:8000`). Cartridge mapping and execution memory are separate choices:

```c
set execution_memory = ram; // GSU cartridge RAM, default origin $70:8000
```

Use the same configuration in every translation unit. `execution_memory` accepts
`rom` or `ram` and is GSU-only. An explicit `code_start_address` takes precedence
over default origins regardless of directive order; when an execution memory
is selected explicitly, the address must belong to that region. For example,
`set code_start_address = 0x710000;` selects `$71:0000` with RAM execution.
Existing full-address directives without `execution_memory` remain supported.

Execution memory is represented by the full origin already stored in each
object, not by a third LoROM/HiROM mapping value or a new object format version.
The assembly emitter preserves mapping and origin through reserved `.define`
metadata understood by `discas`. RAM selection does not generate a loader:
the SNES host must copy the payload, configure GSU program registers/bus access, and
keep the stack and writable data away from the code. The flat payload layout
does not provide separate ROM storage for `rom const` data in a RAM build.

The SPC-700 target model and ABI proposal are documented in
[`spc700-target.md`](spc700-target.md).

## Ownership and stability model

AST nodes own their child nodes. IR graphs do not store pointers into resizable instruction or block vectors; values and blocks are referenced by stable numeric IDs owned by an `IRFunction`. The backend builds short-lived lookup tables while processing one function and does not make the IR own target byte buffers.

This separation is intentional: the AST and IR are compiler-phase data, while `ObjectFile` owns the emitted code/data vectors and serialized object contents.

## Current boundaries

The execution regressions use a bounded instruction-level model of the GSU
prefetch pipeline, register selectors, two RAM banks, RAM instruction fetch,
RAMB, stack memory, comparisons, calls, and
branches. Hand-encoded checks validate the model, and linked payloads validate
both the IR backend and assembly path. Unsupported opcodes, invalid program
reads, mismatched expectations, and instruction-limit exhaustion fail tests.
This model does not emulate a complete SNES, cache timing, bus ownership,
graphics, or interrupts; emulator and hardware validation remain necessary.

The project is pre-release compiler infrastructure. Register allocation is
still conservative and uses rematerialization rather than explicit spill slots
when register pressure exceeds the current pool. Assembly output uses DiscoC's
assembler dialect, not WLA-DX. Fixed-origin linked exports cannot be moved to an
arbitrary execution offset without relinking; native WLA-DX export and
position-independent loading are separate future features.
