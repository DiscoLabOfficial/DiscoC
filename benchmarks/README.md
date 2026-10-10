# GSU benchmarks

Fresh **O0/O1/O2/Os** results for the frozen merged source candidate are in the
[v0.2.0 evidence](../docs/releases/v0.2.0/optimization-results.md), with
[validation and reproduction](../docs/releases/v0.2.0/validation.md), raw
JSON/CSV and independently checked RAM demos. Those reports identify their
full source SHA; they do not publish a v0.2.0 release or replace the historical
v0.1.0 reference below.

This suite measures generated code size and executed GSU opcodes before further
optimization. The reference is the published **v0.1.0 toolchain**, running the
same workloads through the same instruction model as the current tools.
The default suite provides deterministic functional measurements, **not
cycles**. A separate optional Mesen mode adds calibrated **emulated** timing;
neither mode is a physical-hardware or FPS measurement.

## Run and compare

From the repository root, using the native build prerequisites described in
[Building DiscoC](../docs/building.md):

```sh
cmake --preset release
cmake --build --preset release
cmake --build --preset release --target benchmarks
```

The last command compiles all nine programs, links with runtime initialization,
exports/reassembles the final assembly, executes and verifies each payload,
then compares against [the v0.1.0 reference](baselines/v0.1.0.json). Reports are
written to `build/release/benchmark-results/`:

- `report.json`: measurements, profile, source/model fingerprints, tool hashes.
- `report.csv`: one row per workload, including the payload SHA256.
- `comparison.csv`: one row per workload/metric, with baseline/current/delta.
- One directory per workload: source objects, `final.s`, `linked.o`, `payload.bin`.

Positive deltas mean more bytes or executed operations; negative deltas mean
fewer. A reduction in executed opcodes does not by itself establish a cycle-time
speedup. Different GSU instructions, memory placement and bus ownership have
different costs.

Correctness and counter/report tests also run in ordinary CTest, without
requiring Mesen or WLA-DX:

```sh
ctest --preset release -L benchmark
```

The benchmark target requires `BUILD_TESTING=ON` and a native build. Use the
same target with `debug`, `release-mingw`, or another native build directory.
Direct GCC/Clang helpers include the runner and checks when `--test`/`-Test` is
selected. The DOS cross-build does not build the native benchmark runner.

### Compare optimization levels

```sh
cmake --build --preset release --target benchmarks-O1
cmake --build --preset release --target benchmarks-O2
cmake --build --preset release --target benchmarks-Os
```

This runs the same suite at `-O1` and compares it with v0.1.0, writing
`build/release/benchmark-O1-results/`. The default `benchmarks` target stays O0.
For explicit levels/output locations:

```sh
cmake "-DOPTIMIZATION=0" "-DTOOLCHAIN_LABEL=main-O0" "-DOUTPUT_DIR=build/benchmarks/O0" -P benchmarks/run.cmake
cmake "-DOPTIMIZATION=1" "-DTOOLCHAIN_LABEL=main-O1" "-DOUTPUT_DIR=build/benchmarks/O1" "-DBASELINE=build/benchmarks/O0/report.json" "-DFAIL_ON_REGRESSION=ON" -P benchmarks/run.cmake
cmake "-DOPTIMIZATION=2" "-DTOOLCHAIN_LABEL=main-O2" "-DOUTPUT_DIR=build/benchmarks/O2" "-DBASELINE=benchmarks/results/gsu-O1-selection.json" -P benchmarks/run.cmake
cmake "-DOPTIMIZATION=s" "-DTOOLCHAIN_LABEL=main-Os" "-DOUTPUT_DIR=build/benchmarks/Os" -P benchmarks/run.cmake
```

Omit `OPTIMIZATION` when measuring published v0.1.0 tools: that compiler does
not accept the new flags. Each source object receives the selected level,
including separate helpers. Objects/ABI and the fixed execution profile stay
the same. `benchmarks-O2` writes `benchmark-O2-results` and compares with the
retained O1 selection snapshot. O2 comparisons report tradeoffs by default:
code size and cycles need not improve together. See
[optimization and measured results](../docs/optimization.md).
`benchmarks-Os` writes `benchmark-Os-results`. Os minimizes emitted bytes,
not cycles: compare size and timing separately rather than enabling an
all-metrics regression gate against O2. Report `toolchain.optimization` is
the string `"s"` for Os; existing levels retain numeric 0/1/2. All result,
memory, stack and assembly round-trip checks still run.
The retained [Os opcode report](results/gsu-Os.json) and
[timing report](results/gsu-Os-mesen.json) compare with the
[historical O2 reference](results/gsu-Os-reference-O2-mesen.json): eight workloads
are byte-identical; `triangle_fill` drops 334 → 307 bytes but runs about 2.60
times slower. The separate [signed/unsigned helper probe](size/README.md)
drops 657 → 536 bytes for 9,535 → 9,735 emulated cycles. Measure the tradeoff
before choosing Os for a speed-critical loop.
The [Phase 2 Os investigation](../docs/optimization.md#phase-2-investigating-extreme-os-cycle-costs)
records newer compiler-only measurements, byte-neutral division caching and
software countdown compaction. Those measurements supersede the older
snapshots for that phase; the rotating demo was not rerun in its initial pass.
The subsequent [loop-state refinements](../docs/optimization.md#phase-2-loop-state-refinements-and-demo-measurements)
rerun all nine cases and both RAM-execution demos with frozen tools, preserving
sources and reporting bytes, GSU cycles and rotation publication FPS separately.
See the [working-tree evidence summary](results/loop-state-summary.json); this
does not replace validation against a final committed release-candidate SHA.
The newer [compaction measurements](../docs/optimization.md#measured-compaction-tradeoffs)
retain fresh frozen-tool comparisons for O2/Os, including automatic LOOP/CACHE
speed/size tradeoffs and the separate 64-pose RAM-execution triangle check.
The initial O1 snapshots are [opcode/code metrics](results/gsu-O1.json) and
[Mesen timings](results/gsu-O1-mesen.json). They identify measured tool binaries
and inputs; they are not a v0.2.0 release or universal performance guarantees.
These pre-SBK tools were preserved and remeasured using the current model and
timing harness. The [SBK opcode report](results/gsu-O1-sbk.json) and
[SBK Mesen report](results/gsu-O1-sbk-mesen.json) use identical workloads/profile;
[the isolated comparison](../docs/optimization.md#isolated-sbk-comparison-within-o1)
separates this selection improvement from the earlier O0-to-O1 changes.
The earlier local-O1 snapshots are [opcode metrics](results/gsu-O1-local.json)
and [Mesen timings](results/gsu-O1-local-mesen.json). The preceding O1 tools
and the published release were remeasured with the updated model/calibration:
[previous O1](results/gsu-O1-before-local-mesen.json) and
[v0.1.0](baselines/v0.1.0-local-mesen.json). Their opcode companions use the
same filenames without `-mesen`. Preserve the older reports as historical
measurements; use the matching fresh pair for a strict fingerprint comparison.
The [rotating-triangle summary](results/rotation-O1-local-summary.json) retains
64-pose data in its distinct cache-enabled RAM-execution profile; do not mix
those cycles with this suite's LoROM timing profile.
The current [selection opcode report](results/gsu-O1-selection.json) and
[selection Mesen report](results/gsu-O1-selection-mesen.json) use that same
suite/model/profile. Compared strictly with `gsu-O1-local`, five workloads
improve and four remain identical in code size and measured cycles; none
regresses. The [selection rotation summary](results/rotation-O1-selection-summary.json)
retains the separate 64-pose RAM/CACHE comparison and byte-identical screenshot
checks. Workload sources and the triangle algorithm were not rewritten.

The [O2 opcode report](results/gsu-O2.json) and
[O2 Mesen report](results/gsu-O2-mesen.json) retain the global-optimization
comparison with O1 selection. Eight workloads take fewer cycles; `arithmetic`
takes 0.52% more despite smaller code. The unchanged triangle fill takes
56.35% fewer cycles with 12 more CODE bytes. The separate
[O2 rotation summary](results/rotation-O2-summary.json) records 64 matching
screenshots, 0.62% fewer mean cycles and 45 more payload bytes. These are
measured tradeoffs, not universal improvements. See
[the complete table and scope](../docs/optimization.md#global-o2-ssa-allocation-and-loops).

## Workloads and independent result checks

The separate [O2 scalar-value suite](optimizer/README.md) adds five small cases
for global expressions, numeric recurrence, local cells, known bits and hot/cold
lifetimes. It retains before/after bytes and opcode/stack counts, with no inferred
cycle column. These cases supplement, rather than replace, the original nine
workloads and their versioned baseline. The cache-enabled rotating-demo
[comparison](results/rotation-O2-values-summary.json) records the independent
emulator-cycle/FPS measurements for this optimization step.

| Workload | Fixed work | Verified result |
| --- | --- | --- |
| `triangle_fill` | 97 scanlines, widths 1, 3, ..., 193; scanline color bands | 9,409 pixels and every framebuffer byte |
| `horizontal_span` | 128 pixels, one COLOR, one cursor initialization | Final X = 160 and every framebuffer byte |
| `rom_palette_plot` | 128 pixels using a direct ROM byte palette | Final X = 160 and every framebuffer byte |
| `ram_palette_plot` | The same span using an initialized mutable RAM palette | Final X = 160 and every framebuffer byte |
| `memcpy` | Copy 128 host-seeded bytes through a separate helper object | All destination/source bytes, boundary sentinels, result 128 |
| `function_call` | 64 calls to a separately compiled addition helper | `3 + 0 + ... + 63 = 2019` |
| `switch_dense` | Eight consecutive keys plus a default input | Sum 239 |
| `switch_sparse` | Eight widely spaced keys plus a default input | Sum 135 |
| `arithmetic` | 32 iterations of wrapping multiply/add, shifts, XOR, division and remainder | Raw 16-bit result 40225 from an independent integer reference |

Each program returns a value in R0 and writes it to a volatile host-visible
word at `$70:0120`; both must match. Framebuffers are checked against bitplanes
constructed directly from the expected geometry/palette, not by reading pixels
through the model's own RPIX implementation. The runner also checks the linked
object's CODE/DATA against the entire payload byte for byte. No report is
replaced unless every selected workload passes.

The two palette workloads exercise different address spaces: direct ROM colors
use GETC, while RAM colors use loads followed by COLOR. RAM static initialization
is included, not timed separately. Do not interpret their totals as an isolated
ROM-versus-RAM bus benchmark.

The copy helper uses the core library's simple near-RAM byte-copy loop without
linking unrelated memory functions. The checked pointer operations and their
cost remain part of this workload.

### Current switch limit

Both switch workloads deliberately use eight cases, supported by v0.1.0.
An attempted 16-case balanced dispatch is rejected by the current backend with
`optimized switch branch is out of range`: its internal local branches do not
yet benefit from the general CFG branch relaxation. This suite does not fix or
silently bypass that limitation. Larger switch coverage belongs to subsequent
correctness work before optimizing dispatch.

## Measurement contract

The fixed profile is LoROM GSU execution at `$00:8000`, near RAM bank 0,
static RAM starting at `$0400`, R10 initialized to `$FFFE`, and entry `main`.
Graphics use 4bpp, 256x192, SCBR base `$6000` (SCMR `$21`). The runner starts
with deliberately incorrect R10/RAMBR to exercise runtime initialization.
Stack accesses are classified by physical addresses in `[$70:E000, $71:0000)`.
Input seeding and host-side validation are excluded from execution counters.

These are **whole-program** measurements: startup, main, called helpers,
pointer checks, loop control, result stores and STOP are included. Compilation
defaults to O0, preserving the v0.1.0 baseline. Set `OPTIMIZATION=1`, `2` or `s` in
the scripts or use the O1/O2/Os targets to select the backend policy. The report
records this under `toolchain.optimization`, not as a different execution
profile. CODE size includes all linked code, startup and padding, including code
that is not executed. DATA and total payload size are reported separately;
mutable RAM reservations are not payload bytes.

| Field | Meaning |
| --- | --- |
| `instructions_executed` | Real executed opcodes, including prefixes, delay-slot NOPs and STOP; excludes immediate operand bytes and the initial synthetic pipeline NOP |
| `loads` | `ram_loads + rom_reads` |
| `ram_loads`, `stores` | Ordinary RAM load/store instruction events, one per instruction even for a word |
| `rom_reads` | GETB/GETC data-read events, not instruction fetches |
| `stack_accesses` | `stack_loads + stack_stores`, the subset of RAM events touching the declared stack window, regardless of address register |
| `plot`, `color`, `getc`, `rpix` | Executed PLOT, COLOR, GETC and RPIX operations; CMODE/RAMB/ROMB are not miscounted as these operations |
| `branches`, `taken_branches` | Relative branches (including BRA), and the taken subset |
| `jumps` | Non-branch writes to R15, including entry/call/return jumps |
| `calls` | Executed LINK operations, the current direct-call ABI marker |
| `cache` | Executed CACHE requests, not cache hits/misses or bus stalls |

Instruction-cache fills and pixel-cache framebuffer transfers are not counted
as ordinary RAM loads/stores. PLOT/RPIX retain their separate counters. The
shared [GSU instruction model](../tests/GSUInstructionModel.hpp) checks functional
execution/cache/graphics behavior; it does not model cycles, ROM-buffer latency,
CPU/GSU contention, interrupts, or the complete SNES. Unsupported opcodes, invalid
fetches and instruction-limit exhaustion fail rather than yielding measurements.
Counter self-tests use hand-encoded programs independently of the compiler.

## Optional Mesen timing

Install a GSU-capable Mesen/MesenCE exposing the Mesen 2 Lua/testrunner API and
WLA-DX's `wla-65816` and `wlalink`. Nothing is downloaded or bundled by the
benchmark. Configure their executable paths (or put them on PATH):

```sh
cmake --preset release "-DDISCO_MESEN=/absolute/path/Mesen" "-DDISCO_WLA_65816=/absolute/path/wla-65816" "-DDISCO_WLALINK=/absolute/path/wlalink"
cmake --build --preset release --target benchmarks-mesen
cmake --build --preset release --target benchmarks-mesen-O1
cmake --build --preset release --target benchmarks-mesen-O2
cmake --build --preset release --target benchmarks-mesen-Os
```

Use `.exe` paths on Windows. Outputs go to
`build/release/benchmark-mesen-results/report.json` and `report.csv`, with
per-case ROMs, timing evidence, RAM/register snapshots and opcode reports under
`opcodes/`. The O1 and O2 targets write the same artifacts under
`build/release/benchmark-mesen-O1-results/` and `benchmark-mesen-O2-results/`.
Os writes `benchmark-mesen-Os-results/` and has its own optional
`benchmark_mesen_Os_workloads` CTest with the same calibration and oracles.
Selecting any timing target with
missing tools is an error, not a skip.
Byte/opcode/event columns come from the functional model on the same verified
payload; the additional timing columns come from the emulator probe.
Normal builds and CTest do not require an emulator. To opt into calibration and
all nine emulator workload tests as well:

```sh
cmake --preset release "-DDISCO_TEST_MESEN_BENCHMARKS=ON"
ctest --preset release -L mesen --output-on-failure
```

### Fixed timing profile and scope

The same payloads execute directly from LoROM at `$00:8000`, with NTSC,
Mesen GSU speed **100%**, CLSR=1 (fast), CFGR=0 (normal multiplication),
SCMR=`$39` (4bpp, 256x192, ROM and RAM access), and SCBR=`$18` (base `$6000`).
The instruction cache, ROM buffer and pixel caches are cold at entry.
Generated CACHE instructions still work normally after entry: this is not a
cache-disabled profile. Both 64 KiB RAM banks exist; input RAM is seeded with
the same fixture as the functional model. Initial R10=`$1234` and RAMBR=1 are
checked before the payload's runtime initialization.

The original 65816 host boots away from the payload and runs its start/poll
routine in **SNES WRAM**, so it does not fetch code from GSU-owned ROM. The
measurement includes the linked startup, user code, helpers, memory/graphics
stalls, result stores and STOP. It excludes host setup/input seeding, the
initial synthetic pipeline prefetch, host polling/delay after STOP, and
copying/DMA/display. There is no PPU display loop in this timing harness.
This is a controlled whole-program profile, not a claim about every SNES
deployment or contention scenario. The host contains no Nintendo SDK or
proprietary Nintendo code.

### Observed interval versus STOP completion

Mesen's GSU `cycleCount` advances while stopped too, so reading it once per
frame would include host idle time. The Lua probe samples it at the first
real opcode entry and at STOP entry. Mesen does not expose a post-STOP exec
callback. The report therefore preserves both the **observed** interval and
a separate **model-derived STOP fetch cost**:

| Timing field | Meaning |
| --- | --- |
| `master_clocks_to_stop_entry` | Difference between the two GSU opcode-entry counter samples |
| `stop_fetch_master_clocks` | Guarded Mesen-model completion cost: 5 for an uncached ROM fetch, 1 for a valid cache line, or 81 for a cold 16-byte cache-line fill plus fetch |
| `master_clocks` | Observed interval + STOP fetch cost |
| `gsu_cycles` | The same total expressed in fast GSU clock ticks; CLSR=1 means one tick per master clock in this fixed 100% profile |

These are not SNES CPU instruction cycles, a host stopwatch, or a new hardware
trace event. The STOP adjustment relies on Mesen's STOP having no additional
internal `Step()` beyond its opcode prefetch. Unsupported STOP delay-slot,
pending bus-operation, bank or clock states fail instead of guessing a cost.
The guarded rules follow the primary
[Mesen GSU timing implementation](https://github.com/SourMesen/Mesen2/blob/master/Core/SNES/Coprocessors/GSU/Gsu.cpp)
and [GSU instructions](https://github.com/SourMesen/Mesen2/blob/master/Core/SNES/Coprocessors/GSU/Gsu.Instructions.cpp).
The state fields/hooks follow the
[Mesen Lua API](https://github.com/SourMesen/Mesen2/blob/master/UI/Debugger/Documentation/LuaDocumentation.json).
Changing emulator timing or Lua semantics requires revalidation, not relabeling
the old reference.

Every timing run first executes hand-encoded calibration programs for NOP,
immediate operands, cache fill/hit, a STOP cache-line boundary, HIB/LOB byte-sign
flags, LOB/SWAP packing, LOOP delay slots and SBK word writes. A host-delay
variant must produce the same timing; a no-STOP variant must fail and remove
stale evidence. For each workload the Mesen opcode count must equal the
functional-model count. Independent result/framebuffer/copy-boundary checks
then validate the emulator RAM snapshot, R0, RAMBR and final stack. No completed
timing report is replaced if calibration or any workload fails.

### Compare timings

Use the same emulator binary, WLA tools, harness, workload/model fingerprints
and profile for both toolchains. The measured
[v0.1.0 Mesen reference](baselines/v0.1.0-mesen.json) records hashes of the local
Windows Mesen executable (file version 2.2.1) and release tools. Its profile is
not a portable promise about other emulator builds. Remeasure **both** versions
when changing emulator, host, fixtures or profile.

The standalone timing script accepts the same DiscoC tool/label/revision paths
as `run.cmake`, plus `MESEN`, `WLA_65816` and `WLALINK`:

```sh
cmake "-DTOOLS_DIR=/absolute/path/v0.1.0/bin" "-DGSU_BENCHMARK_RUNNER=/absolute/path/DiscoC/build/release/bin/disco_gsu_benchmarks" "-DMESEN=/absolute/path/Mesen" "-DWLA_65816=/absolute/path/wla-65816" "-DWLALINK=/absolute/path/wlalink" "-DTOOLCHAIN_LABEL=v0.1.0" "-DTOOLCHAIN_REVISION=c5e902efdde9b4c2b1c5101b7b40c844b4319f87" "-DOUTPUT_DIR=build/benchmarks/v0.1.0-mesen" -P benchmarks/run-mesen.cmake
cmake "-DBASELINE=build/benchmarks/v0.1.0-mesen/report.json" "-DCURRENT=build/release/benchmark-mesen-results/report.json" -P benchmarks/compare.cmake
```

Add `.exe` to the explicit runner path on Windows. `CASE=horizontal_span`
selects a single case; comparison requires the same cases/order. Timing reports
cannot be compared with opcode-only reports. An optional `FAIL_ON_REGRESSION=ON`
gate defaults to `code_bytes` and `master_clocks` for timing reports. Clock/cache
profiles, emulator/harness/tool hashes and inconsistent timing totals are
validated before producing deltas. RAM-executed payload timing, warmed-entry
profiles and physical hardware measurements remain separate future work.

To compare levels in Mesen, measure both with the same tools/profile:

```sh
cmake "-DMESEN=/absolute/path/Mesen" "-DWLA_65816=/absolute/path/wla-65816" "-DWLALINK=/absolute/path/wlalink" "-DOPTIMIZATION=0" "-DTOOLCHAIN_LABEL=main-O0" "-DOUTPUT_DIR=build/benchmarks/mesen-O0" -P benchmarks/run-mesen.cmake
cmake "-DMESEN=/absolute/path/Mesen" "-DWLA_65816=/absolute/path/wla-65816" "-DWLALINK=/absolute/path/wlalink" "-DOPTIMIZATION=1" "-DTOOLCHAIN_LABEL=main-O1" "-DOUTPUT_DIR=build/benchmarks/mesen-O1" "-DBASELINE=build/benchmarks/mesen-O0/report.json" "-DFAIL_ON_REGRESSION=ON" -P benchmarks/run-mesen.cmake
cmake "-DMESEN=/absolute/path/Mesen" "-DWLA_65816=/absolute/path/wla-65816" "-DWLALINK=/absolute/path/wlalink" "-DOPTIMIZATION=2" "-DTOOLCHAIN_LABEL=main-O2" "-DOUTPUT_DIR=build/benchmarks/mesen-O2" "-DBASELINE=build/benchmarks/mesen-O1/report.json" -P benchmarks/run-mesen.cmake
```

## GSU profiling

With the optional Mesen/WLA-DX tools configured, profile the current O2 code:

```sh
cmake --build --preset release --target benchmarks-profile-O2
```

Or add `-DPROFILE_GSU=ON` to `run-mesen.cmake`:

```sh
cmake "-DMESEN=/absolute/path/Mesen" "-DWLA_65816=/absolute/path/wla-65816" "-DWLALINK=/absolute/path/wlalink" -DOPTIMIZATION=2 -DPROFILE_GSU=ON -DOUTPUT_DIR=build/benchmarks/profile -P benchmarks/run-mesen.cmake
```

Each case produces `profile.json` and a validated `profile-map.lua` derived
from its exact linked `final.s`. The combined report embeds profiles. Functions,
linked machine-label regions, opcode categories and PCs include counts and
master-clock intervals; region records include label/function/address metadata.
Prefix and delay-slot instructions count, operand bytes do not. Every partition
must sum to the independent boundary timing and functional instruction count.
The profiler's synthetic timeline/map/ALT/STOP tests run before measurements.

Intervals between opcode entries include prefetch/cache/bus waits and are
charged to the preceding opcode; they are **not isolated ISA latencies**.
Startup and calibrated STOP fetch are included, CPU polling/copy/DMA/display
are not. Full emulator-state observation costs host wall time, not simulated
cycles; profiling raises only its subprocess wall timeout, never instruction,
clock or correctness limits. Ordinary timing remains lightweight and unchanged.
Missing/bad maps and inconsistent evidence fail visibly, without stale PASS.

For the cache-enabled RAM rotating demo, add `PROFILE_GSU=ON` alongside
`VERIFY_MESEN=ON` and `MEASURE_GSU_TIMING=ON` to its public builder. Only the
first pose is profiled, with state snapshots at **region transitions**; that
observer is removed before subsequent poses. Region-only reports intentionally
omit opcode-category and per-PC timing (PC counts remain), and sum to phase 0's
independent boundary timing. Full 64-pose, wrap, framebuffer/FPS and red-failure
checks still run. See the [demo guide](../tests/graphics/rotating_triangle/README.md#measuring-animation-fps).

Profiling identifies expensive code; it does not automatically supply PGO
weights to the compiler. O2's cost model uses conservative static loop weights
and fetch/RAM/call pressure estimates. See
[allocation scope and measured results](../docs/optimization.md#profiling-cost-model-and-allocation).

## Compare two toolchains explicitly

The standalone scripts accept tool/output paths. Quote complete `-D...`
arguments, especially on PowerShell. For the current tools:

```sh
cmake "-DTOOLS_DIR=build/release/bin" "-DTOOLCHAIN_LABEL=main" "-DOUTPUT_DIR=build/benchmarks/main" "-DBASELINE=benchmarks/baselines/v0.1.0.json" -P benchmarks/run.cmake
```

To remeasure the historical release, extract its tools into a separate
directory, then use those three tools with the **current benchmark runner**:

```sh
cmake "-DTOOLS_DIR=/absolute/path/v0.1.0/bin" "-DGSU_BENCHMARK_RUNNER=/absolute/path/DiscoC/build/release/bin/disco_gsu_benchmarks" "-DTOOLCHAIN_LABEL=v0.1.0" "-DTOOLCHAIN_REVISION=c5e902efdde9b4c2b1c5101b7b40c844b4319f87" "-DOUTPUT_DIR=build/benchmarks/v0.1.0" -P benchmarks/run.cmake
```

On Windows, add `.exe` to the explicit runner path. `TOOLS_DIR` supplies the
platform suffix automatically. Individual `DISCC`, `DISCAS`, `DISCLD` paths can
also be supplied. `CASE=horizontal_span` measures just one program; compare it
only with a report containing that same case.

```sh
cmake "-DBASELINE=build/benchmarks/v0.1.0/report.json" "-DCURRENT=build/benchmarks/main/report.json" -P benchmarks/compare.cmake
```

An optional budget gate rejects increases in code size or executed instructions:

```sh
cmake "-DBASELINE=benchmarks/baselines/v0.1.0.json" "-DCURRENT=build/benchmarks/main/report.json" "-DFAIL_ON_REGRESSION=ON" -P benchmarks/compare.cmake
```

Set `REGRESSION_METRICS` to a quoted semicolon-separated list to choose other
metrics or allow a documented size/work tradeoff. The default comparison reports
deltas without imposing a blanket performance budget; correctness always fails
the run when violated.

Reports must share the schema, suite version, workload/model fingerprints,
execution profile, case set/order and verified results. Invalid numeric fields,
inconsistent totals, malformed/oversized reports and incompatible measurements
are rejected. Source hashes normalize CRLF/LF so cross-platform checkouts remain
comparable; executable hashes record provenance but may differ across hosts.

If a workload, input, model or profile changes, **remeasure both toolchains**.
Never relabel old measurements as comparable. The checked-in reference was
measured using the published Windows x86-64 MinGW v0.1.0 binaries; their hashes
were verified against the release's `BUILD-INFO.json`. Compiler and runner hashes
are recorded in the report, with no host paths or timestamps. Later toolchains
may legitimately produce different bytes while preserving the checked results.

The retained [SCCP functional report](results/gsu-O2-sccp.json) and
[SCCP timing report](results/gsu-O2-sccp-mesen.json) compare with the preceding
[pipeline functional](results/gsu-O2-pipeline.json) and
[pipeline timing](results/gsu-O2-pipeline-mesen.json) snapshots under matching
profiles. Four workloads improve and five remain equal in size/cycles; memory
and graphics-op counts are unchanged. The separate cache-enabled 21 MHz RAM
[rotation summary](results/rotation-O2-sccp-summary.json) records all 64 poses
plus wrap, exact screenshot agreement, and the 11-byte/0.23%-cycle tradeoff.
See [measured results](../docs/optimization.md#sccp-and-cfg-measurements).

The subsequent [proven-check functional](results/gsu-O2-guards.json) and
[proven-check Mesen](results/gsu-O2-guards-mesen.json) snapshots compare with
SCCP under matching fingerprints. All nine cases shrink/improve without
changing loads/stores/stack or graphics-op counts. O0/O1 payload hashes and
functional counters were separately confirmed unchanged. The separate
[rotation summary](results/rotation-O2-guards-summary.json) records the
4,748 → 3,453 byte payload and 3.6% mean-cycle reduction at 21 MHz with CACHE,
64 identical screenshots and a phase-wrap sample. See the
[proof rules and scope](../docs/optimization.md#proven-check-elimination-measurements).

The [divmod/shared-code functional](results/gsu-O2-size.json) and
[timing](results/gsu-O2-size-mesen.json) reports compare with the frozen
proven-check snapshot. All nine shrink; eight keep identical timing/counters,
while `arithmetic` falls from 391 to 296 CODE bytes and from 148,479 to 81,229
fast GSU cycles, with fewer division/stack operations. O0/O1 payloads/counters
remain unchanged. The separate [rotation summary](results/rotation-O2-size-summary.json)
records 3,453 → 3,126 payload bytes and 3.56% fewer mean cycles, with all 64
screenshots identical. See [fusion/sharing scope and measurements](../docs/optimization.md#divmod-and-shared-code-measurements).

The [unthrottled presentation comparison](results/rotation-unthrottled-summary.json)
uses those same two O2 payloads with the same revised SNES host: the six-VBlank
post-upload hold is removed, the GSU starts the next render immediately, and
only complete images are uploaded during VBlank. The verified
[FPS probe](../tests/graphics/rotating_triangle/measure-fps.lua) counts 64
steady-state completed-image intervals, including host/DMA/presentation costs
and repeated refreshes. This is distinct from the older GSU-only timing samples
and GIF playback. Build/reproduction details live in the
[rotating-triangle guide](../tests/graphics/rotating_triangle/README.md#measuring-animation-fps).

The later [bottleneck pass](results/rotation-bottleneck-summary.json) freezes
the [original and counter-only renderers](rotation/README.md) to distinguish
compiler gains from incremental scanline edges. It records 64 exact images,
phase wrap, byte-exact assembly, GSU cycles and completed-pose FPS. See the
[methods and measured tradeoffs](../docs/optimization.md#rotating-triangle-bottleneck-pass),
including O2 speed improvements that deliberately add a few CODE bytes and
the corresponding Os comparisons.
