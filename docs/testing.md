# Testing and Fuzzing

The repository includes an automated CTest suite covering the compiler, linear-scan allocator, switch dispatch, ABI call sequences, textual assembly path, standalone assembler, linker, object-file reader, diagnostics, CFG output, lexical shadowing, and multi-object data relocations.

## Run the regression suite

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The native CI matrix runs the documented Debug/Release presets on Windows,
Ubuntu and macOS. MinGW static and direct/helper paths have separate coverage;
the Ubuntu `sanitizers` preset enables ASan/UBSan. All tool/test executables use
`<build-dir>/bin`. See [building](building.md) for prerequisites and manual CMake.

## Baseline 0.1: separate conformance from backend checks

[language-spec.md](language-spec.md) is the frozen Baseline 0.1 contract. The
[rule-to-test index](../tests/language/README.md) maps it to source fixtures and
independent runtime oracles. Run only language conformance or GSU graphics with:

```sh
ctest --preset debug -L conformance
ctest --preset debug -L graphics
ctest --preset debug -L 'linker|object'
```

Conformance categories include types, conversions, qualifiers, pointers, far,
volatile, structs, arrays, globals, imports, control-flow, plot and bitmap,
alongside constants/layout, strings, attributes, aliases and target selection.
They run positive and negative `--check` cases on both target models, with
explicit GSU capability exceptions. Negative tests require exit status 1 and
a matching source-located language diagnostic; crashes, timeouts and internal
IR failures do not count as successful rejections.

## Linker/object/runtime acceptance

`object_file_hardening` checks independent little-endian fixtures, versions
3–7, unknown versions/enums, impossible counts, section/name/total-file limits,
truncated/trailing records, duplicate/control-character names, symbol bounds
and overlapping relocation operands. Adjacent bank/offset records remain legal.
Writer rejection must leave an existing destination unchanged. Test directories
are process-private so concurrent build suites do not remove each other's files.

`linker_hardening` uses the canonical in-process linker, not a second layout
implementation. Golden payload bytes cover CODE, aligned multi-object DATA,
RAM bases and all five relocation types. Negative cases cover non-CODE/end-label
calls, invalid RAM relocation targets, missing/private/duplicate symbols,
reserved contexts, incompatible metadata, origin/bank/stack constraints,
alignment-induced bank crossing and bitmap/static/payload/stack conflicts.
Existing binary and assembly sentinels must survive every rejected layout.
Independent startup bytes cover RAMBR banks 0/1, R10 and the entry jump;
threshold tests reserve RAM code below the descending stack.

`language_globals` additionally executes recursive exhaustion with R6=2 through
direct and assembled paths. A statically impossible frame is now a link failure;
this separate recursion test retains the dynamic guard acceptance criterion.
Existing multi-file, mapping, runtime-loading and assembly-equivalence tests
remain part of the native suite. A model/sanitizer job is not hardware proof.
See the [internal candidate checklist](release-readiness-0.1.md) for verified
local evidence and outstanding platform gates.

## Frontend and GSU regression details

`graphics_state` additionally executes prefix/postfix/compound cursor updates,
checking both snapshots and the hardware PLOT increment. `graphics_bitmaps`
checks the last logical pixel and final bitplane byte of every one of the
twelve mode/depth profiles, not only a representative interior pixel.

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

Nesting fixtures retain the 128-entry parser and 256-depth expression limits,
including accepted/rejected cases at the parser boundary. MSVC tools and tests
reserve an 8 MiB stack so Debug exception unwinding can report excessive
nesting safely; the input limits and negative-test coverage are not relaxed.

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

`graphics_triangle` uses the official [RAM triangle](../examples/snes/triangle/main.dc)
shown in the README. It checks 9,409 PLOT, 97 COLOR, zero GETC, one RPIX,
representative planar framebuffer bytes, result/stack/bank state, and both
compiler and final linked assembly round trips. The public `discoc.toml`
project build must also reproduce the manual payload byte for byte and execute
the same result. Startup begins with incorrect
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

## Correctness-checked GSU benchmarks

The [benchmark suite](../benchmarks/README.md) adds nine fixed workloads plus
hand-encoded counter checks and report-contract tests. Ordinary native CTest
and direct-build test runners verify them without emulator dependencies:

```sh
ctest --preset release -L benchmark
cmake --build --preset release --target benchmarks
cmake --build --preset release --target benchmarks-O1
cmake --build --preset release --target benchmarks-O2
```

The target additionally compares current JSON/CSV results with the measured
v0.1.0 reference. Both versions run through the shared `GSUInstructionModel`,
also used by the existing execution regressions; this is not a second execution
oracle. Independent expected values, framebuffer bitplanes and copy sentinels
are checked before counters are reported. Counter tests distinguish opcodes
from operands/synthetic NOPs, taken/untaken branches, LINK/jumps, word/byte and
bank-aware stack accesses, GETB/GETC and graphics operations. Report tests reject
incompatible fingerprints/profiles, invalid counters, malformed input and
optional budget regressions. Counts describe whole-program opcode/data-access
events, not cycle timing, bus transfers or hardware performance.

The separate `benchmarks-mesen` target optionally measures emulated master
clocks/fast GSU ticks through Mesen with a fixed ROM/clock/cold-entry profile.
It requires WLA-DX/Mesen explicitly and runs hand-encoded hook/cache/STOP
calibration before all workloads. Observed opcode-entry timestamps and the
model-derived final STOP fetch cost are retained separately. Host-delay and
no-STOP/stale-evidence tests cover the measurement boundary and failure path.
Mesen RAM/register snapshots reuse the independent benchmark result,
bitplane and copy-boundary checks; executed opcode counts must also agree.
Snapshot tests reject truncated/trailing data, wrong banks/results and boundary
overwrites. Timing report tests reject changed clock/cache profiles, emulator
or harness hashes, invalid totals and optional cycle-budget regressions.

Set `DISCO_TEST_MESEN_BENCHMARKS=ON` to include
`benchmark_mesen_calibration`, `benchmark_mesen_workloads`, and
`benchmark_mesen_O1_workloads`, `benchmark_mesen_O2_workloads`,
`benchmark_mesen_Os_workloads` and `benchmark_mesen_size_pool` in native CTest;
they are serial integration tests labeled `mesen`. Missing prerequisites fail
configuration when this option is selected. The default/off path and DOS
cross-build do not acquire an emulator dependency. See the
[timing contract and commands](../benchmarks/README.md#optional-mesen-timing).
No physical-hardware or universal cycle-accuracy claim is implied.
The size-pool probe executes both signed and unsigned private kernels in Mesen,
checks result/host-visible memory/entry stack, requires exact opcode-model count
agreement and reassembles the linked export byte for byte. It reuses the
seed-3/result-2019 snapshot contract, not the function-call benchmark source.

## GSU optimization regressions

`gsu_optimization` checks default/O0 byte identity, the `-O`/O1 alias, strict
TOML boolean validation and CLI precedence, source-import propagation,
mixed-level objects, and byte-exact compiler/final assembly exports.
Runtime checks cover volatile read/store counts, a load before an aliasing
call, nested calls exceeding register capacity, signed/unsigned IBT boundaries,
constant shifts, repeated COLOR, PLOT auto-X, and unused RPIX/flush effects.
An independent 8x8 truth table checks all six signed/unsigned comparisons both
as boolean values and branch conditions. Runtime near-pointer endpoint tests
cover signed -32768, unsigned counts above 32767, scaling carry/borrow, null
results, and LoROM window escapes with unchanged fail-stop categories.
Hardware-loop executions cover 0, 1, 2, 127, 128 and 65,535 iterations,
R1 wrap, R13 preparation and changing COLOR/CMODE state across implicit
backedges in both modes and both emission paths. An independent hand-encoded
LOOP/delay-slot microprogram also runs in the instruction model and optional
Mesen calibration.

`gsu_sbk` checks word read-modify-write and repeated stores at O0/O1 through
both direct and assembled paths. Execution asserts SBK counts, results,
volatile access counts and adjacent-byte sentinels. Intervening RAM loads,
no-argument aliasing calls, far RAM banks, CFG joins and register-pressure
spills must not reuse a stale implicit address. The independent model's
microprograms cover source selectors, XOR-1 word byte order, byte accesses,
RAMBR changes and flag preservation. Optional Mesen calibration verifies
SBK word results, including a selected non-R0 source after LDB/ALT1.

`ir_local_optimizer` verifies the backend-local pass before/after rewrites:
nested casts and signed byte boundaries, identity copies, local forwarding,
escaped addresses, calls, volatile barriers, CFG joins and COLOR/PLOT effects.
It also checks bounded pure-expression reuse, overwritten-store elimination,
member-address/division fault barriers, mask/shift fusion, signed bit 15,
cascading DCE and repeated-pass idempotence. The IR verifier rejects invalid
bit indices, pointer operands, missing operands, targets and volatile metadata
on `bit.extract`.
`gsu_local_optimization` executes both O0/O1 on nine runtime seeds, all sixteen
shift counts, signed/unsigned left/right shifts, every extracted bit and the
checkerboard's signed bit-9 expression. It checks compiler assembly byte
equivalence, unchanged fault categories for invalid shifts/zero divisors,
aliasing writes, live values across calls/register pressure, snapshots,
volatile locals and loop/continue behavior. Model microprograms and optional
Mesen calibration independently check HIB/LOB's unusual S=bit7 behavior and
LOB/SWAP packing. Long IR branch/switch execution cases also run under O1.

`gsu_selection_optimization` checks direct allocated ALU destinations,
repeated expressions, unary operations, overwritten/volatile locals and live
values across calls. An independent arithmetic matrix executes 144
power-of-two multiply/divide/remainder results per input on thirteen dynamic
signed/unsigned seeds at both O0 and O1. It covers all sixteen powers, negative
divisors, signed truncation toward zero, -32768/-1 wrapping and the dividend's
remainder sign. Plot-local division/remainder must preserve R1/R2. Compiler
assembly and final linked exports round-trip byte for byte.

The `optimized_*` cases execute the existing pointer, numeric, qualifier,
global/linkage, control-flow, library, call/ABI, and graphics regressions at O1,
without removing their value/byte/state assertions. Each benchmark also has
an O1 correctness case. Linear-scan unit tests require real spills under
pressure and reserve cursor registers for side-effecting RPIX values.
See [the optimization contract](optimization.md); existing O0 regressions stay
enabled alongside these checks.

`ir_global_optimizer` verifies O2 SSA/PHI coverage, type/edge dominance,
idempotence on the covered patterns, natural loops, exposed hardware backedges,
exact register/spill interference, reserved registers, live-across-call values,
escape/volatile/uninitialized fallbacks, safe LICM and bounded scalar inlining.
Malformed hardware setup/end/leave and changed backedge metadata are rejected.
The independent instruction model checks LOOP counts 0/1/65535, including the
zero-count 65,536-trip behavior and every final/taken delay slot.

`ir_conditional_optimizer` separately tests SCCP's executable-edge PHIs,
mixed constant/variable PHI prefixes, a late backedge that invalidates the
initial constant, bool/enum/cast/wrap semantics, constant/default/partial
switches, identical branch destinations, effect preservation and idempotence.
Malformed input, unexposed hardware backedges and resource limits are rejected.
`gsu_sccp` executes eight boundary seeds at O0/O1/O2 through direct and assembly
paths, with byte-identical final-assembly reexports. Checks include surviving
unknown branches, live hardware recurrences, volatile access counts,
short-circuit suppression, cursor auto-increment, COLOR/PLOT/RPIX effects and
division/remainder/shift fail-stops after a constant PHI. Stores before a fault
must execute; stores after it must not.

`gsu_checked_proofs` unit-tests conservative frame/absolute-address intervals,
mask/PHI bounds, signed negative offsets, narrowing/wrap, null/odd/end-of-bank
spans, unknown loads/cyclic PHIs, over-alignment, resource fallback and
stack-credit consumption/reset/saturation. `gsu_checked_proofs_execution`
executes twelve boundary seeds at O0/O1/O2 through direct and assembly paths,
including arrays, struct members, nested/loop/plot calls, far RAM restoration
and RPIX/cursor observations. Linked exports reassemble byte for byte.
Runtime cases retain null/alignment/scaling/wrap fail-stops and stores before
but not after faults. Zero/nonzero stack floors, entry alignment, a minimal
O2 array frame, an untaken call, and O2 callers with O0/O1/O2 callees are
checked independently. Assembly asserts that the entry guard stays while its
two redundant O2 saved-register push checks disappear. Direct build helpers
include the proof test unit.

`gsu_size_optimization_unit` checks conditional/remainder-first divmod fusion,
matching/dominance/type exclusions, malformed projections, bounded/idempotent
pair counts and deterministic allocation. It independently reconstructs live
sets for PHI cycles/pressure/calls and checks that shared epilogues and short
fault islands are actually selected, with out-of-range islands retained.
`gsu_size_optimization_execution` compares 41 signed/unsigned boundary and
deterministic pseudo-random pairs at O0/O1/O2/Os via direct and assembly paths.
It covers `-32768 / -1`, remainder signs, floor division, calls, far RAM bank
restoration, plot cursor/RPIX state, hardware loops and two/three/five-way PHI
cycles. Zero-divisor and short/long fault paths retain observable-store ordering;
widely separated scalar/far returns restore the frame and results. Final linked
exports round-trip byte for byte. Both direct build helpers include the new unit.

The same unit also checks the Os policy: deterministic candidate selection,
one private kernel for profitable repeated divisions, inline single divisions,
explicit CACHE without padding hints, and exact void-call suffix sharing.
Different arguments and separately observed volatile reads must not merge;
the suffix pass must be idempotent.
`gsu_size_policy_execution` additionally links separate units with private
kernels at `$70:6000`, mixes Os with O0/O1/O2/Os callees, selects either RAM
bank and exercises signed boundary inputs. Results, saved stack/cursor/bank
state, access counts and final assembly bytes remain asserted. Shared-tail
branches retain their selected volatile witness and exactly one call/store.
Project builds check `optimization_level = "s"`, discovered dependencies and
CLI precedence on either side of `--config`.

`ir_value_optimizer` checks dominating/commutative GVN versus siblings and
faulting/volatile exclusions; separate struct-field PHIs and constant-array
cells; escape, dynamic indices, uninitialized cells and packed alignment;
constant-index chains; signed/unsigned masks, casts, shifts and wrapped ranges;
pointer-comparison exclusions; modular recurrences and hot/cold lifetime splits.
It reconstructs live interference independently and requires repeat-pass IR
idempotence. Invalid split representations are rejected.

`gsu_value_optimization_execution` executes 37 boundary/deterministic seed and
factor pairs at O0/O1/O2/Os through direct and compiler-assembly paths. Independent
integer references check increasing/decreasing modular products, distinct cells,
escaped mutations, pressure across calls, pointer comparisons, narrowing and
range consumers. Volatile access counts and cursor/PLOT/RPIX effects remain
asserted. Illegal packed pointer accesses must fail after the preceding witness,
never perform the following write. Final linked exports reassemble byte for byte.
`benchmark_value_optimizations` measures the five focused O2 programs only after
result, stack, volatile and both byte-round-trip assertions pass. See the
[focused benchmark contract](../benchmarks/optimizer/README.md).

`gsu_cost_model` checks IBT sign-extension boundaries, short/long spill-address
materialization, scalar/far transfer widths, caller preservation, weighted
and saturating cost arithmetic, deterministic allocation and independently
reconstructed point liveness. Tests forbid cursor/special registers, live-copy
eviction and stale copies after register writes, labels or calls. Existing
O0/O1/O2 execution matrices protect PHIs, hardware backedges, escaped/volatile
accesses, bank restoration, calls, graphics effects and fail-stop ordering.
`benchmark_profile_contract` independently tests final-CODE label origins,
regions/function ownership, ignored DATA exports, gaps, unordered/out-of-range
labels, bank crossings and JSON partition sums/duplicates. Optional
`benchmark_mesen_profile_contract` tests synthetic opcode/region timelines,
ALT-dependent classifications, STOP accounting, out-of-code/malformed maps
and visible startup failures in both probes. `PROFILE_GSU=ON` always runs this
contract before full benchmark profiling. The rotation profiler samples only
the first pose and removes its observer before subsequent renders; all normal
64-pose/FPS and deliberate failure checks remain enabled. See
[profiling commands](../benchmarks/README.md#gsu-profiling).

`gsu_machine_scheduler` verifies one-byte slots, selected-register arithmetic,
WITH/BRA/TO copies, preserved call return/stack state, LOOP/PLOT's final and
taken paths, ROM-result/cursor dependencies, selectors and observable RAM.
Negative cases protect N/Z producers, entry labels, operand bytes, R14,
memory operations and already filled slots. A boundary matrix exercises all
11 relative branch opcodes against ALU/shift/byte-extraction/multiply flag
effects in all four ALT states. It also tests conditional COLOR/CMODE/PLOT,
carry-safe MOVES copies and taken-edge WITH/TO/FROM on both success and fail-stop paths.
Public aliases, other incoming branches, relocation targets, live ALT state
and selector-consuming fallthrough prevent speculative prefix scheduling.
Alignment round trips and range
fallbacks are checked; linker tests cover aligned public/private relocations
at multiple unaligned origins with startup and preceding objects. Both direct
build helpers include the scheduler and local/global optimizer units.
Mesen calibration additionally tests split IWT/IBT slots and two independent
16-byte/eight-opcode cached ROM-read microprograms: 99 versus 96 fast GSU cycles,
with the same checked result. The functional model remains untimed.
Additional independent Mesen microprograms validate IBT at `$7F/$80/$FE`,
conditional AND scheduling (85 to 80 fast GSU cycles), and taken-edge WITH
(75 to 70 cycles on success, unchanged 75 cycles on failure). These figures
include the final uncached STOP fetch, not SNES host setup or presentation.
Linker/runtime tests check exact compact startup bytes, relocated entries,
poisoned RAMBR/R10, IBT/IWT value boundaries, odd global images, writes confined
to the selected RAM allocation and full 64 KiB zero-count clearing.

`gsu_global_optimization` executes O0/O1/O2 on eight signed/unsigned boundary
seeds, with PHI swaps, continue/switch, calls, volatile observations, escaped
locals, byte wrapping, struct-array stride induction and constant specialization.
Direct objects, compiler assembly and final linked exports match byte for byte
within each level. O2 nested hardware loops and a loop-using callee preserve
register/stack state; a zero-trip divide-by-zero case proves faults are not
speculated. Forward/backward induction checks legal reads, zero trips, scaled
overflow, carry/borrow, null and LoROM-window failures at all three levels.

The `o2_*` and `os_*` cases repeat existing ABI, numeric, qualifier, pointer/far,
global, library and graphics execution assertions. Every benchmark has O2 and Os
functional test, and optional Mesen integration measures the same workload
profile at O2. Model counts are not timing; emulator cycles are not hardware
measurements. Optimization does not relax the language conformance contract.

The rotating demo additionally validates an owned `getScreenBuffer()` snapshot
against all 49,152 visible reference pixels before saving a pose PNG. Capture
waits three refreshes for asynchronous screenshot decoding, independently of
the synchronous RAM/VRAM publication checks and completed-pose FPS probe. The
fixed NTSC raw-buffer dimensions/top-border offset are checked; a missing pose
fails after a bounded wait. This prevents a previous-frame PNG from being
silently labeled as the next pose. No host throttle or timing sample changes.

## Optional full SNES triangle integration

The [official SNES triangle](../examples/snes/triangle/README.md) includes a
minimal WLA-DX 65816 host and Mesen Lua checks. Install `wla-65816`, `wlalink`,
and Mesen separately. From the repository root, with the current DiscoC tools
and those optional tools on `PATH`:

```sh
cmake -DVERIFY_MESEN=ON -P examples/snes/triangle/build-snes.cmake
```

The script also accepts explicit tool paths and an output directory. It fails
on missing dependencies or failed commands; it does not silently skip checks.
Without `VERIFY_MESEN`, it builds the two complete SNES ROMs but does not run
the emulator. Artifacts default to `build/snes-triangle`. The old test-directory
script remains a delegating compatibility entry point. The canonical source,
manifest, host and independent oracle exist only in the public example.

The host copies the fixed-origin payload from ROM to `$70:6000`, lets the GSU
draw and flush, reclaims cartridge RAM after STOP, checks the result, and
DMA-transfers the framebuffer to VRAM for PPU display. The scripts independently
decode all 49,152 logical pixels, compare all framebuffer/VRAM bytes and all
1,024 tilemap entries, and check the displayed image. A deliberately incorrect
expected result checks the red-screen failure path. Verification logs and
screenshots are produced beside the ROMs, without saving emulator settings.
The host consumes generated origin/SCBR/SCMR definitions from the final linked
assembly; a mismatched framebuffer profile fails before host assembly.

To include this optional complete-ROM workflow in CTest:

```sh
cmake -S . -B build/integration -DCMAKE_BUILD_TYPE=Release \
  -DDISCO_TEST_SNES_INTEGRATION=ON
cmake --build build/integration --config Release --parallel
ctest --test-dir build/integration -C Release -L integration --output-on-failure
```

Tools must be on `PATH` or supplied as `DISCO_WLA_65816`, `DISCO_WLALINK` and
`DISCO_MESEN` absolute CMake paths. When explicitly enabled, a missing dependency
is a configuration failure, not a skipped test. The test also requires new PASS
logs and screenshots, rejecting stale evidence. It is native-only and OFF by
default; neither ordinary CI tests nor DOS compilation requires WLA-DX/Mesen.

The README image is a real capture of this integration test in Mesen. This
adds complete-ROM emulator evidence for this demo, not physical-hardware
validation or exhaustive timing coverage for the toolchain.

The [rotating checkerboard test](../tests/graphics/rotating_triangle/README.md)
uses the same optional tools and shared build helpers:

```sh
cmake -DOPTIMIZATION=2 -DVERIFY_MESEN=ON -P tests/graphics/rotating_triangle/build-snes.cmake
```

Its host retains the last completed image while the GSU renders, then uploads
4,096 bytes during VBlank and immediately starts the next render, without an
artificial hold. An independent per-pixel reference checks all
49,152 pixels at each of 64 poses, framebuffer/VRAM bytes, tilemap, color counts,
CPU results, STOP/stack/bank/CACHE state and VBlank DMA completion. It then checks
phase wrap and a separate red-screen failure ROM. Captures/logs are written
under `build/plot-rotating-triangle`; stale evidence is removed before execution.
Memory checks occur on publication before RAM reuse; display checks account for
Mesen's scanout/screenshot-buffer latency without slowing the host. `fps.json`
records 64 completed-pose intervals, emulated master-clock time and repeated
refreshes under explicit NTSC, normal-speed GSU settings. Missing or failed FPS
evidence fails the verified build. An accelerated-GSU scheduling check exercises
one publication per refresh; that stress run is not a 21 MHz performance claim.
The animated README preview is accelerated playback of actual Mesen captures,
not an animation-performance claim. This software-division-heavy demo has not
been optimized for smooth rotation or tested on physical hardware.
With `-DMEASURE_GSU_TIMING=ON -DVERIFY_MESEN=ON`, the public rotating-demo script
also records 65 first-opcode-to-STOP samples in `timing.json`, including calibrated
STOP fetch completion. Timing is boundary-only, guarded by the same fixed bank,
clock, cold-CACHE and runtime profile; missing/failed evidence fails the build.
The complete-frame oracle and red-ROM test still run, and `fps.json` remains a
separate CPU/DMA/presentation-inclusive measurement.

The [scaling rainbow triangle](../tests/graphics/scaling_triangle/README.md)
is a separate 4bpp animation, defaulting to O2:

```sh
cmake -DVERIFY_MESEN=ON -DMEASURE_GSU_TIMING=ON -P tests/graphics/scaling_triangle/build-snes.cmake
```

It uses division-free incremental geometry and color bands. Its host initializes
a blank bitmap and advances phase by one modulo 64; the renderer erases only
old edges on shrink. The independent oracle verifies every pixel at all 64
phases plus wrap, all 33 visible sizes, eight colors, runtime state and VBlank
DMA, plus a separate red failure ROM. The 3,840-byte upload is bounded and
guarded; the host never holds poses artificially. Native `graphics_scaling`
regressions cover O0/O1/O2/Os and all three byte-equivalent assembly paths.
Outputs and measured `fps.json`/`timing.json` are separate from the rotating
demo, under `build/plot-scaling-triangle` by default.

## Interactive OBJ geometry

[Interactive cube and sphere](../tests/graphics/interactive_shapes/README.md)
adds a true OBJ/4bpp sprite host and a 12-vertex icosahedral sphere. Its optional
Mesen verifier injects actual controller buttons and independently checks full
framebuffer/VRAM/display images, input edges, both rotation axes, bounded scale,
STOP/stack/banks/CACHE and VBlank DMA. The negative ROM verifies red failure.
The benchmark measures a fixed wire/filled pose from entry to calibrated STOP,
not displayed FPS. Idle interactive poses are not redrawn.

Native `graphics_interactive` cases verify oversized O0 placement rejection;
`optimized_`, `o2_` and `os_graphics_interactive` execute 17 analytic cases through
three byte-equivalent assembly paths. O1 uses full-width ROM mirror placement,
whereas the RAM host is supported at O2/Os. No checks are disabled to fit code.

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

## GSU compaction checks

`gsu_compaction_unit` separately verifies near-pointer SSA/PHIs, ordered
constant local-array initialization, signed values, generated counted LOOPs,
R12/R13 preservation, cold automatic CACHE selection, explicit CACHE exclusion
and oversized-window fallback. It executes at ROM `$00:8000` and deliberately
unaligned RAM code origin `$70:6007`, compares against O1, checks observable RAM
and graphics results, rejects malformed initializer IR, and checks byte-exact
assembly reconstruction, determinism and optimizer idempotence. Loops with
early exits/live-out SSA values and volatile initializers retain conservative
lowering. Exact execution counters are not timing estimates; real cycle
measurements use the optional Mesen harness.
An invalid pointer-parameter test retains its preceding volatile witness before
the alignment fail-stop. Os must select the smaller software span when LOOP's
ABI setup costs more bytes; O2 still requires the profitable automatic CACHE.

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
