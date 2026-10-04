# Testing and Fuzzing

The repository includes an automated CTest suite covering the compiler, linear-scan allocator, switch dispatch, ABI call sequences, textual assembly path, standalone assembler, linker, object-file reader, diagnostics, CFG output, lexical shadowing, and multi-object data relocations.

## Run the regression suite

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The native CI matrix runs these tests for Debug and Release builds. A separate Ubuntu job builds the same targets with AddressSanitizer and UndefinedBehaviorSanitizer enabled.

The `gsu_memory_map` unit test checks documented ROM/RAM windows, invalid
SNES-only regions, exact bank boundaries, arithmetic overflow, and near-target
bank checks. The `gsu_mapping` regression exercises the actual linker with
hand-authored objects and compiler output: low/high-bank origins, all relocation
types, multi-object DATA relocations, combined code/data boundary failures,
unrepresentable targets, and preservation of an existing output on rejection.
Golden payload bytes check the encoded addresses, and the instruction model
checks calls relocated to `$40:8000`, `$70:8000`, and `$71:0000`. That model checks
the PC and execution results, not cartridge RAM capacity, the physical bus,
ROM-file placement, or the SNES host's startup routine.

`project_manifest` covers the TOML subset, UTF-8/escape decoding, numeric and
resource bounds, duplicates/types, truncation, and flag-looking values.
`project_build`, `project_configuration`, `project_diagnostics`, and
`project_targets` exercise automatic/explicit discovery, manifest-relative and
CLI-relative paths, ordered overrides, collisions, source/link failures, static
initialization, HiROM, and frontend-only SPC700 checks. Objects, final payloads,
and assembly are compared with explicit tool invocations; final assembly is
reassembled byte-for-byte. The GSU model checks RAM bank/stack initialization
and the 46 + 103 result. These run in both CTest and the direct-build registry.

`target_foundation` checks LoROM defaults, retained HiROM support, optional RAM
execution, CLI argument ordering/default selection, explicit origins, region mismatches, and GSU-only
diagnostics. `gsu_execution_memory` checks golden object-header bytes for both
backends, mixed IR/assembly multi-file linking, rejected incompatible origins,
malformed assembly metadata, and relocated call results. It also compares
LoROM/RAM payload bytes when only the bank changes and all references are near.

`assembly_export` checks byte-exact reassembly of all 256 raw opcodes with four
prefix configurations (1,024 encoding cases), plus private/far relocations,
placement, DATA bytes, and malformed export input. These are encoding tests,
not execution coverage for every opcode.

`gsu_runtime_loading` links multi-file programs at `$70:6000`, `$70:0000`, and
`$71:6000`, verifies hand-encoded bootstrap bytes, and starts the instruction
model with deliberately incorrect RAMBR/R10 state. It checks both data banks,
shared RAM code/data storage, nested calls, wrong-offset loading failures,
invalid origins/options, stack-word overlap, output preservation, and final
linked assembly round trips. Additional ABI tests enter `main` when it is not
the first function in CODE. The model implements RAMB's bank-bit selection but
does not emulate cache timing, bus ownership, interrupts, or a complete SNES.

## Language baseline contract

The `language_contract` test checks the supported baseline described in
[language-spec.md](language-spec.md): scalar and near/far-pointer widths, signed byte
widening, structure member offsets and array stride, stable lexical shadowing,
compatible prototypes, contextual literals, ROM bytes, and rejection of invalid
conversions/declarations, per-level pointer reach, mixed-width parameter offsets,
and pointer-depth boundaries. It runs analysis, IR lowering, and IR verification. The verifier unit also
checks unsupported target capabilities and malformed coordinate/cache IR.
It also checks independent qualifiers, strict numeric conversions, bool,
aggregate-by-value diagnostics, linkage, lexical plot scope, parser nesting,
and expression-depth limits. Target execution is covered by the separate
GSU regressions. Both CTest and direct-build runners include this test.
Alias checks cover signedness/width, const/volatile and near/far metadata,
inactive declarations, name conflicts, and alias-count/pointer-depth boundaries.

## Near/far execution regressions

Eight `pointer_*` groups cover the data-pointer contract:

- `pointer_far_abi`: four-byte locals/arguments/returns, nested calls, widening,
  configured near banks, and final linked assembly round trips.
- `pointer_nested`: near/far pointer levels, four-byte indexed descriptors,
  structure fields, opaque void pointers, narrowing, and plot-register preservation.
- `pointer_arithmetic`: actual element stride, ascending local arrays, signed
  displacements, RAM byte carry/borrow, and multi-window LoROM traversal.
- `pointer_rom`: ROMB/GETB word reads, mirror windows, and near-bank restoration.
- `pointer_guards`: dynamic alignment, word carry/borrow, domain escape,
  descriptor padding, narrowing failures, the 32-KiB aggregate-stride boundary,
  and normal completion through a scalar-only entry calling checked code.
- `pointer_diagnostics`: statically invalid addresses/conversions/accesses and
  unsupported function-pointer operations receive user-facing diagnostics.
- `pointer_multifile`: direct, assembled, and mixed objects execute at $70:0900,
  preserve the stack, return 149, and produce identical payload bytes.
- `pointer_alignment`: golden DATA placement after odd CODE/byte data and
  across multiple objects, including byte-exact linked-export reconstruction.

Positive and runtime-failure fixtures execute both direct and assembled paths;
the suite checks memory, banks, fault R6, and selected stack/register results.
The instruction model includes ROMBR, ROM-buffer reads, two RAM banks, and
shared RAM code/data. Hand-encoded self-tests check bank selection, ROM aliases,
and a write that corrupts a future RAM instruction. This is not a complete
execution oracle for graphics, cache/bus timing, interrupts, or the SNES host,
and it does not verify a far-code call ABI.

## Language execution regressions

Seven baseline `language_*` execution groups cover:

- Qualifiers/volatile: read/write counts, unused reads, duplicate writes,
  load snapshots across calls, and observable induction variables.
- Numerics: signed/unsigned wrapping, full-width multiply, casts, and bool.
- Operators: bitwise/shift boundaries, short-circuit side effects, software
  division/remainder vectors, and dynamic arithmetic failures.
- Aggregates: structure arrays, pointer parameters, member qualifiers/layout.
- Globals: zeroing poisoned RAM, scalar/far initializers, two RAM banks,
  static/payload/stack placement checks, and stack-floor faults.
- Linkage: repeated private names across objects, public globals/externs,
  inaccessible private symbols, mixed backends, and duplicate exports.
- Plot blocks: branch/switch scope, break/return, calls/division with coordinate
  preservation, empty-frame logical expressions, and raw bool normalization.

Direct and assembled paths execute and compare payload hashes. Initialized
RAM payloads also round-trip through final linked assembly. Drawing instructions
are compiled/reassembled byte-exactly; rendered pixel accuracy is not covered.
The test model is not a complete SNES emulator or hardware validation.

## Language conformance and extensions

The native suite has 69 CTest groups: 8 unit/model groups, 36 earlier CLI
regressions, 16 language-conformance categories and 9 extension/integration
groups. The physical positive/negative cases under `tests/language/` run through
`discc --check` on both GSU and SPC700, with explicit capability exceptions.
They are independent of opcode golden files and do not imply SPC700 emission.

The extensions execute constant/enumeration/layout examples, real for/continue,
single-evaluation compound/postfix updates, null casts/equality, aggregate
initializers, pointer/struct arrays and ASCII/NUL strings. Imports are tested
with missing/invalid files, cycles and deduplication; a multi-file .dci example
compares direct, assembled and mixed linking. Aligned RAM sections start from
a deliberately non-16-byte-aligned origin to exercise linker padding.
Warning tests check all categories, suppression, Werror and definite assignment.
CLI tests check migration errors, invalid origins and target restrictions.
The object-reader unit checks malformed v6 DATA/RAM alignment, retaining legacy
v3/v4/v5 compatibility checks.

Model execution is not cache timing, rendered-pixel, hardware or full-emulator
validation. The shared test registry is also used by direct build helpers;
all new CLI groups are runnable against C++14 production tools.

## Freestanding library regressions

`language_fixed_point` checks 119 raw outputs per format (238 total) against
independent signed 64-bit integer references: addition/subtraction, scaled
multiplication/division, wrapping and conversion, including negative fractions
and signed endpoints. Separate workloads keep the same coverage within the
one-bank payload limit. Both formats also test zero-divisor fault STOPs. The
public fixed-point/memory example executes with its documented RAM result bytes.
Cost-warning tests reject the fixed library under `-Wall -Werror` unless its
intentional software division is explicitly acknowledged with
`-Wno-expensive-helper`; other warnings stay enabled.

`language_memory_library` checks unsigned byte copy/fill, both overlap directions,
self move, zero-count null pointers, the final byte of a RAM bank, and checked
null/bank-escape failures. ROM/volatile arguments are rejected rather than
silently losing address-space or observable-access semantics.

Both groups execute direct, assembled and mixed objects, compare payload
hashes, and reassemble final linked exports for byte-exact reconstruction.
`language_target_libraries` checks GSU graphics encoding/linking, target assertion
failures and distinct GSU/SPC700 conditional IR. Alias conformance additionally
checks shared transitive imports, conflicting declarations and imported casts
in attributes. Disabled declarations/imports retain syntax validation.

Core modules pass frontend/IR checks on SPC700, not SPC700 execution. Graphics
wrappers receive encoding/round-trip coverage, not pixel rendering. No complete
SNES, timing, inline-ASM or interrupt execution coverage is implied.

## Optional libFuzzer target

The optional frontend fuzzer exercises the lexer, parser, textual assembler,
in-memory object-file reader, and bounded TOML manifest parser. It requires
Clang with libFuzzer support:

```bash
cmake -S . -B build-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDISCO_BUILD_FUZZERS=ON
cmake --build build-fuzz --target disco_frontend_fuzzer
mkdir -p corpus
./build-fuzz/disco_frontend_fuzzer -max_len=4096 corpus
```

Malformed input is expected to be rejected. A crash, sanitizer report, hang, or unbounded resource use is a compiler defect and should become a minimized regression test under `tests/`.
