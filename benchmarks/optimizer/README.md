# O2 scalar-value microbenchmarks

These five fixed programs measure GVN, numeric induction, scalar replacement,
known bits/ranges and hot/cold lifetime partitioning independently of the
original nine [whole-workload benchmarks](../README.md).

| Case | Work | Independently checked word at `$70:0120` |
| --- | --- | ---: |
| `gvn` | Dominating and commutative products across a volatile store and branch | 322 |
| `recurrence` | 32 modular products of increasing values, dynamic captured factor | 14448 |
| `scalar_cells` | Separate struct and constant-array cells across a branch | 236 |
| `known_bits` | Masked range, redundant mask and known-one-bit consumers | 302 |
| `hot_lifetimes` | Captured scalar parameters live across nested loops and cold uses | 558 |

Inputs are host-seeded `seed=46`, `factor=7`. Every case must return 42, leave
R6=0 and R10=`$1FFC`, and read the volatile seed exactly once. Execution begins
at LoROM `$00:8000` with linker runtime initialization, RAMBR=0 and initial
R10=`$2000`. Stack events touch `[$70:1000, $70:2000)`; host seeding/checks are
excluded. The existing independent GSU instruction runner supplies counters.
Its functional checks are not a timing model. No Nintendo SDK/proprietary code
or emulator binary is bundled.

From the repository root after a native test-enabled build:

```sh
cmake -DTOOLCHAIN_LABEL=O2-current -DOUTPUT_DIR=build/benchmarks/value-current -P benchmarks/optimizer/run.cmake
ctest --preset release -R 'ir_value_optimizer|gsu_value_optimization_execution|benchmark_value_optimizations' --output-on-failure
```

Defaults use `build/release/bin`; `TOOLS_DIR`, `RUNNER` and `OUTPUT_DIR` accept
explicit paths. Add `.exe` to an explicit Windows runner. To compare a saved
compiler snapshot, use its three tools with the **same current runner**:

```sh
cmake "-DTOOLS_DIR=/absolute/path/pre-change/bin" "-DRUNNER=/absolute/path/DiscoC/build/release/bin/disco_gsu_execution_tests" -DTOOLCHAIN_LABEL=O2-before-values -DOUTPUT_DIR=build/benchmarks/value-before -P benchmarks/optimizer/run.cmake
```

Each run compiles at O2, links, reconstructs the compiler assembly and final
linked assembly, and checks all three payload hashes before executing. A result,
state or byte mismatch fails before replacing `report.json`. Reports record
compiler/runner/model, source and payload fingerprints alongside bytes, executed
opcodes, loads/stores, stack events, branches and calls. Use identical checkout,
inputs and runner/model when comparing the raw source fingerprints.

The retained [before](../results/gsu-O2-values-before.json) and
[after](../results/gsu-O2-values-after.json) reports use a frozen pre-change O2
compiler and the current verified runner, not v0.1.0 or a published new release:

| Case | Payload bytes before → after | Executed opcodes before → after | Stack events before → after |
| --- | ---: | ---: | ---: |
| gvn | 239 → 152 | 148 → 105 | 5 → 1 |
| recurrence | 185 → 193 | 2,230 → 1,494 | 130 → 99 |
| scalar_cells | 350 → 236 | 241 → 167 | 11 → 9 |
| known_bits | 132 → 96 | 88 → 66 | 1 → 1 |
| hot_lifetimes | 734 → 734 | 8,149 → 8,128 | 834 → 827 |

This measures executed opcodes, **not cycles or FPS**. The recurrence trades
eight additional bytes for 33.0% fewer executed opcodes. The hot-lifetime case
saves seven net stack events; loads increase by one while stores fall by eight.
Dynamic-index/escaped aggregates are deliberately left in memory. Arbitrary
loop unrolling or unrestricted aggregate splitting is not implied.

The original nine workloads remain byte/counter-identical at O0/O1/O2 to the
pre-change tools. The rotating checkerboard has its own RAM/CACHE/21 MHz
[measurement](../results/rotation-O2-values-summary.json), now reproducible with
`MEASURE_GSU_TIMING=ON` in its public build script. Its 12-byte/0.052%-cycle gain
does not change the VBlank-quantized completed-pose FPS.

The next cost-aware allocator snapshot is measured independently against that
scalar-value baseline, with [before](../results/gsu-O2-cost-values-before.json)
and [after](../results/gsu-O2-cost-values-after.json) reports. Four cases remain
identical; `hot_lifetimes` changes from 734 to 715 payload bytes, 8,128 to 8,112
executed opcodes and 530/297 to 529/297 stack loads/stores. These are still
functional counters, not cycles. The separate
[rotation comparison](../results/rotation-O2-cost-summary.json) measures a
106-byte/4.26%-mean-cycle reduction with unchanged images and effectively
unchanged VBlank-quantized FPS. See
[profiling and allocator scope](../../docs/optimization.md#profiling-cost-model-and-allocation).
