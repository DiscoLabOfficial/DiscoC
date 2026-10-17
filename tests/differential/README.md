# Deterministic GSU differential tests

This is a bounded source-generator/oracle regression suite, not a new emulator
or a full-language interpreter. The same generated DiscoC source runs at O0,
O1, O2 and Os. **No optimization level is the oracle.**

`IntegerOracle.hpp` implements the frozen language's integer rules using
unsigned bit patterns and widened host arithmetic. It does not include or link
compiler types, constant evaluators, optimizer code or numeric helpers.
Hand-calculated anchors check wrap, signed division/remainder, shifts, byte
narrowing/sign extension and checked failures before executing generated code.
The test-only GSU instruction model executes the resulting payloads; it is not
independent Mesen/hardware validation and does not measure hardware cycles.

## Run

Build the native tools and tests normally, then:

```sh
ctest --preset release -L differential --output-on-failure
```

Or use any existing build directory:

```sh
ctest --test-dir build/release -L differential --output-on-failure
```

The direct `build.sh --backend direct --test` and `build.ps1 -Backend Direct
-Test` paths run the same corpus and reporting contract. No Python, SDK,
WLA-DX, emulator installation or network access is required.

## Corpus and observations

The default seed is `0xd15c0c32`. A specified xorshift32 generator and a bounded
depth-three unsigned expression grammar generate **24 valid programs**. Every
program combines arithmetic with modular signed sums/products/negation,
signed division/remainder, arithmetic right shift, byte casts/unary operations,
branch joins, exhaustive switch returns,
nested calls, initialized local arrays with runtime indices, `continue`, and
countdown loops. Boundary inputs include 0/1, byte signedness boundaries,
signed-word limits, and all-one bits. Shift counts include 0, 1, 7, 8, 9, 14, 15.

An additional **8 dynamic-fault programs** cover signed/unsigned division and
remainder by zero, invalid shifts, odd word addresses and near null access.
Fault checks require the pre-operation store and prohibit later stores and
illegal reads. Compile failure, crash, unsupported instruction or timeout is
never accepted as a checked language fault.

For each of these 32 programs, each optimization level:

1. Compiles and links actual objects with `discc` and `discld`.
2. Executes direct and compiler-assembly/`discas` payloads against the oracle.
3. Requires byte identity between those paths and reassembled final assembly.

That gives **256 model executions**, plus byte-exact assembly round trips.
CTest registers all **128 case/optimization combinations** separately, so a
slow sanitized build can schedule them in parallel without making 32 programs
share one timeout. Splitting the entries does not add or remove corpus cases.
Cases alternate ROM `$00:8000` and RAM `$70:6000` execution, and exercise both
near RAM banks. Runtime startup must correct deliberately poisoned RAMBR/R10.

Observations include normal R0 return, R6 fault category, normal stack cleanup,
RAM bank, all 256 bytes and read/write counts in a reserved window, prohibited
near-null accesses, and untouched sentinels in the other RAM bank. A volatile
sequence counter and individual
logged argument values witness left-to-right calls and skipped short-circuit
operands. This is a dependency-sensitive ordering witness, not a complete
chronological bus trace. Stack/spill contents and RAM-resident machine code are
not compared as source-visible memory.

## Replay and extend

Use absolute tool paths (append `.exe` on Windows) for a single generated case:

```sh
cmake -DDISCC=/path/to/bin/discc -DDISCAS=/path/to/bin/discas \
  -DDISCLD=/path/to/bin/discld \
  -DDIFFERENTIAL_RUNNER=/path/to/bin/disco_gsu_differential_tests \
  -DOUTPUT_DIR=/path/to/build/differential-replay \
  -DSEED=0xd15c0c32 -DCASE_INDEX=7 -DOPTIMIZATION=2 \
  -P tests/run_gsu_differential.cmake
```

Omit `CASE_INDEX` for all 32 programs; omit `OPTIMIZATION` for all four levels.
Seeds are unsigned 32-bit integers. Case indices are 0..31, with 24..31 reserved
for faults. Changing the seed varies the generated valid program sources and
non-boundary inputs; it does not silently change the grammar's size limits.

Every run uses a new directory. It retains source, expected bytes/counts,
seed/case/placement, tool SHA-256 hashes, commands, stage stdout/stderr, objects,
linked binaries and assembly. `progress.txt` records the active stage even when
an outer timeout interrupts the harness; `timings.txt` records completed
subprocess wall times in whole seconds (not GSU cycles). On failure,
`failure.txt` identifies the stage;
the diagnostic points to a literal-argv `reproduce.cmake`:

```sh
cmake -P /path/to/failing/case/reproduce.cmake
```

Replays regenerate the case using the current harness and tools; they are not
a self-contained archived toolchain. Preserve the repository revision, original
source/expectations and tool binaries alongside the recorded tool and harness
hashes when investigating a confirmed divergence. `commands.txt` records argv;
`reproduce.cmake`, not that text file, is the executable replay.

If the Ubuntu sanitizer job fails, CI uploads these differential artifacts and
the CTest logs as `sanitizers-differential-failure`, retained for seven days.

Replays write new artifacts and preserve the original evidence. Each matrix-stage
subprocess has a 45-second timeout; model execution has its existing 10-million-instruction
budget, and each CTest case/optimization entry has a 600-second overall limit.
The direct-build runner still executes all cases and levels through the same
harness, with the same subprocess limits. The negative
reporting contract deliberately invokes a non-compiler executable and checks
nonzero failure, saved evidence and replay, including paths with spaces. Oracle
self-tests also deliberately corrupt expectations to check mismatch detection.

Confirmed divergences should be minimized into permanent regressions. The
suite does not claim full-language coverage, far-pointer/capability/graphics
conformance, array bounds checking, ABI stability, complete memory-race
observation, physical hardware testing or fuzzing completeness.
