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

`module_loader` checks the actual dependency graph, diamond edges, physical-file
deduplication, deterministic order, owned API projections, cached AST transfer,
private visibility, cycle locations, failed-discovery cleanup and graph/depth
boundaries. `module_imports`, `module_import_paths` and
`module_import_diagnostics` exercise root-only source discovery, shared RAM/ROM
data, private constants behind public layouts, `.dci` compatibility, configured
search order, CLI-list precedence, inactive imports, malformed dependencies,
name conflicts and output protection. Project/manual objects and direct/assembly
payloads are compared byte-for-byte; the GSU model checks their results. Source
imports also run frontend conformance on both supported target models.

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
execution oracle for cycle-accurate graphics, cache/bus timing, interrupts, or the SNES host,
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
are compiled/reassembled byte-exactly and exercised by the graphics groups below.
The test model is not a complete SNES emulator or hardware validation.

## Language conformance and extensions

The native suite includes unit/model tests, CLI regressions, language-conformance
categories and extension/integration groups. The physical positive/negative
cases under `tests/language/` run through
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
v3/v4/v5/v6 compatibility checks and malformed v7 bitmap records.

Model execution is not cache timing, SNES/PPU display, hardware or full-emulator
validation. The shared test registry is also used by direct build helpers;
all new CLI groups are runnable against C++14 production tools.

## Stateful graphics regressions

`graphics_state` checks persistent R1/R2 cursor snapshots, exactly one hardware
X increment per PLOT, word-sized cursor wrap, scanline loops without an extra
increment, draw/read sugar and its single-evaluation order,
calls/continue/switch, redundant CMODE and unused RPIX results. `flush` must
remain in IR and in executed code.

`graphics_colors` checks immediate/computed COLOR, direct ROM GETC, far-ROM bank
restoration, volatile-ROM fallback, RAM copies modified at runtime, casts,
mutable globals and far-RAM loads. COLOR and GETC share high-nibble/freeze-high
checks; additional cases cover transparency, dithering and POR OBJ addressing.

`graphics_bitmaps` covers all twelve mode/depth combinations, logical pixel
reads and raw bitplane bytes (including the last byte of cartridge RAM), final
assembly reconstruction, imported/conditional profiles and host metadata.
The full triangle example executes 4,225 PLOT and 4,225 GETC operations, with
one final RPIX. `graphics_diagnostics` covers context/capability errors, removed
API spellings, invalid fields/alignment/ranges, conflicting profiles and RAM/
stack overlap. Object tests reject malformed/truncated bitmap records.

`GSUGraphicsModel` independently tracks both eight-pixel caches and bitplanes.
Hand-encoded execution-model self-tests verify PLOT/RPIX cursor behavior and
GETC's ROM-buffer/color transformation, without relying on compiler selection.
This verifies instruction-level color/layout/flush behavior, not cycle timing,
CPU/GSU contention, a complete SNES framebuffer display or physical hardware.

`graphics_triangle` uses the tracked [RAM triangle](../tests/graphics/triangle/triangle.dc)
shown in the README. It checks 9,409 PLOT, 97 COLOR, zero GETC, one RPIX,
representative planar framebuffer bytes, result/stack/bank state, and both
compiler and final linked assembly round trips. Startup begins with incorrect
RAMBR/R10 to exercise runtime initialization. It runs in the native CTest and
direct-build registries without external emulator or assembler dependencies.

`graphics_rotation` executes the [rotating checkerboard](../tests/graphics/rotating_triangle/triangle.dc)
with ten host-input vectors through all three payload paths. It checks phase
masking, independent pixel/color counts, planar byte landmarks, poisoned-RAM
clearing, untouched memory outside the bitmap, startup/stack/bank state, one
RPIX, zero GETC and identical payload hashes. The renderer stays within the
existing instruction-model execution limit. Both explicit CACHE requests must
remain in the final assembly export, with checked execution counts and CBR.
Hand-encoded model self-tests cover the functional 512-byte instruction cache:
selector/ALT reset, prefetched-PC alignment, preserving/rebasing cached lines,
RAM fallback outside the window and payload-bound checks. No cache-cycle or
CPU/GSU bus timing is modeled.

## Optional full SNES triangle integration

The [triangle test directory](../tests/graphics/triangle/README.md) includes a
minimal WLA-DX 65816 host and Mesen Lua checks. Install `wla-65816`, `wlalink`,
and Mesen separately. From the repository root, with the current DiscoC tools
and those optional tools on `PATH`:

```sh
cmake -DVERIFY_MESEN=ON -P tests/graphics/triangle/build-snes.cmake
```

The script also accepts explicit tool paths and an output directory. It fails
on missing dependencies or failed commands; it does not silently skip checks.
Without `VERIFY_MESEN`, it builds the two complete SNES ROMs but does not run
the emulator. Artifacts default to `build/plot-triangle`.

The host copies the fixed-origin payload from ROM to `$70:6000`, lets the GSU
draw and flush, reclaims cartridge RAM after STOP, checks the result, and
DMA-transfers the framebuffer to VRAM for PPU display. The scripts independently
decode all 49,152 logical pixels, compare all framebuffer/VRAM bytes and all
1,024 tilemap entries, and check the displayed image. A deliberately incorrect
expected result checks the red-screen failure path. Verification logs and
screenshots are produced beside the ROMs, without saving emulator settings.

The README image is a real capture of this integration test in Mesen. This
adds complete-ROM emulator evidence for this demo, not physical-hardware
validation or exhaustive timing coverage for the toolchain.

The [rotating checkerboard test](../tests/graphics/rotating_triangle/README.md)
uses the same optional tools and shared build helpers:

```sh
cmake -DVERIFY_MESEN=ON -P tests/graphics/rotating_triangle/build-snes.cmake
```

Its host retains the last completed image while the GSU renders, then uploads
4,096 bytes during VBlank. An independent per-pixel reference checks all
49,152 pixels at each of 64 poses, framebuffer/VRAM bytes, tilemap, color counts,
CPU results, STOP/stack/bank/CACHE state and VBlank DMA completion. It then checks
phase wrap and a separate red-screen failure ROM. Captures/logs are written
under `build/plot-rotating-triangle`; stale evidence is removed before execution.
The animated README preview is accelerated playback of actual Mesen captures,
not an animation-performance claim. This software-division-heavy demo has not
been optimized for smooth rotation or tested on physical hardware.

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
wrappers additionally execute rectangle/pixel/flush color checks in direct,
assembled and mixed-object builds. No complete
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
