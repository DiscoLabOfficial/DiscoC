# Reproducing the 0.3.0 preparation checks

Run from the repository root with a C++23 compiler, CMake 3.21+ and Ninja.
Use a fresh output directory; the commands below keep generated artifacts
under ignored `build/`. The source version is 0.3.0 with an empty prerelease
suffix. Do not substitute a base commit SHA for an uncommitted working tree.

## Native Release

```sh
cmake -S . -B build/release-v0.3.0-preparation -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release-v0.3.0-preparation --parallel 4
ctest --test-dir build/release-v0.3.0-preparation \
  --parallel 4 --output-on-failure
```

For Debug with ASan/UBSan on Linux, use the repository presets in separate
directories. The focused run below matches the additional relevant regressions
completed during preparation; the broader sanitizer attempt was interrupted.

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --preset sanitizers --output-on-failure \
  -R '^(object_file_hardening|linker_hardening|assembly_export|ir_global_optimizer|gsu_machine_scheduler|gsu_size_optimization_unit|project_manifest|require_no_spills_cli|project_build|project_configuration|project_diagnostics|gsu_checked_proofs_execution|gsu_size_policy_execution|gsu_global_optimization)$'
```

Leak detection was disabled because LeakSanitizer cannot run under this
environment's ptrace. This keeps ASan/UBSan enabled and makes no leak-checking
claim. The actual incremental sanitizer build used `build/linux-asan`; the
presets above select `build/sanitizers` for reproduction. Omit the test filter
to attempt the complete suite, which this preparation does not claim to have
completed under sanitizers.

## Functional benchmark matrix

```sh
for level in 0 1 2 s; do
  cmake -DTOOLS_DIR=build/release-v0.3.0-preparation/bin \
    -DOPTIMIZATION="$level" \
    -DTOOLCHAIN_LABEL=v0.3.0-preparation \
    -DTOOLCHAIN_REVISION=unknown \
    -DOUTPUT_DIR="build/release-v0.3.0-preparation/benchmarks/O$level" \
    -P benchmarks/run.cmake
done
```

Each level writes `report.json` and `report.csv` only after all nine workloads
pass their functional checks. Outputs retain payload and tool hashes, the
fixed profile, normalized workload/model fingerprints, all counters and result
oracles. The script's Os directory is `Os` (the level is the string `s`).
The retained [raw reports](raw/) are copied from those outputs without
relabeling their `revision = "unknown"` as a clean commit.

These runs measure functional opcodes, not emulated cycles. Compare levels
only after checking profile, workload, model and tool identities. A strict
comparison with older checked-in reports can reject a changed model signature;
remeasure older tools with the same current runner before claiming a speedup.

## Complete SNES ROMs

Install WLA-DX and a compatible Mesen exposing the Lua/testrunner API. Pass
actual executable paths; neither emulator nor WLA-DX is bundled by this record.
For example, run the following for each `level` in `2 s`, using separate
output directories:

```sh
cmake -DDISCO_TOOLS_DIR=build/release-v0.3.0-preparation/bin \
  -DOPTIMIZATION=2 -DVERIFY_MESEN=ON -DMESEN=/absolute/path/to/Mesen \
  -DWLA_65816=/absolute/path/to/wla-65816 \
  -DWLALINK=/absolute/path/to/wlalink \
  -DOUTPUT_DIR=build/release-v0.3.0-preparation/demos/rotation-O2 \
  -P tests/graphics/rotating_triangle/build-snes.cmake
```

Use `tests/graphics/interactive_shapes/build-snes.cmake` or
`examples/snes/gem_chess/build-snes.cmake` with their own output directory for
the other demos. Omitting `VERIFY_MESEN=ON` checks build and assembly round-trip
paths only; it does not validate a displayed image. See the demo guides and
[validation record](validation.md) for the actual results and limits.

## Source identity and final packaging

The retained `source-inputs.sha256` lists exact on-disk build/test inputs,
including the new `src/GSUStaticStackProof.hpp`, with repository-relative paths.
On Linux, check it from the repository root:

```sh
sha256sum --check docs/releases/v0.3.0/source-inputs.sha256
```

The manifest excludes prose, images, generated outputs and release evidence;
it identifies tested inputs, not a complete Git tree or an exact-source archive.
CRLF/LF conversion changes these byte hashes. Workload/model hashes inside the
benchmark reports normalize text line endings separately.

For a release rebuild, check out the `v0.3.0` tag in a clean checkout and
repeat the gates with that exact SHA. Use [native packaging](../../../cmake/PACKAGING.md)
to stamp a fresh build before configuring and package only after its checks.
The commands in this document reproduce the dated preparation measurements;
they do not publish or replace the release's package receipts.
