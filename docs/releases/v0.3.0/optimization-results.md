# 0.3.0 preparation: functional benchmark results

Measured on **2026-11-16** with the freshly built Linux Release tools.
All **36/36** workload/level combinations passed their independent functional
oracles and assembly round-trip checks. The source tree is uncommitted;
these are preparation results, not validation of a final commit or tag.

The [validation record](validation.md) and [evidence identity](evidence.json)
tie the reports to the exact on-disk build/test inputs and tool binaries.
[Reproduction commands](reproduce.md) use the existing benchmark harness.

## Payload bytes

Payload bytes include CODE, ROM DATA and required alignment. Mutable RAM
reservations are separate. These rows use the same LoROM execution profile,
workload/model identities and native tools at every level.

| Workload | O0 | O1 | O2 | Os |
| --- | ---: | ---: | ---: | ---: |
| `triangle_fill` | 1,171 | 362 | 216 | 160 |
| `horizontal_span` | 454 | 198 | 127 | 87 |
| `rom_palette_plot` | 816 | 420 | 164 | 124 |
| `ram_palette_plot` | 703 | 316 | 190 | 149 |
| `memcpy` | 935 | 481 | 282 | 238 |
| `function_call` | 710 | 335 | 176 | 176 |
| `switch_dense` | 1,257 | 657 | 313 | 313 |
| `switch_sparse` | 1,282 | 707 | 318 | 318 |
| `arithmetic` | 1,681 | 470 | 260 | 235 |

## Executed opcodes

These are functional-model instruction counts, including executed prefixes,
delay slots and STOP. They are **not GSU cycles, presentation time or FPS**.

| Workload | O0 | O1 | O2 | Os |
| --- | ---: | ---: | ---: | ---: |
| `triangle_fill` | 1,148,649 | 363,982 | 26,651 | 80,867 |
| `horizontal_span` | 12,292 | 3,585 | 329 | 568 |
| `rom_palette_plot` | 37,442 | 16,769 | 3,160 | 4,028 |
| `ram_palette_plot` | 31,973 | 9,250 | 2,427 | 3,167 |
| `memcpy` | 287,232 | 17,346 | 3,376 | 4,876 |
| `function_call` | 19,436 | 8,629 | 5,745 | 5,745 |
| `switch_dense` | 4,389 | 2,126 | 1,047 | 1,047 |
| `switch_sparse` | 4,448 | 2,214 | 1,036 | 1,036 |
| `arithmetic` | 53,367 | 25,334 | 11,461 | 11,824 |

Os demonstrates the size/work tradeoff: `triangle_fill` is 160 payload bytes
versus O2's 216, but executes 80,867 opcodes versus 26,651. No cycle or
throughput conclusion follows from these counts alone.

## Raw reports and correctness

- [36-row CSV](benchmark-results.csv): results, all functional counters and payload hashes.
- O0: [JSON](raw/opcodes-O0.json), [CSV](raw/opcodes-O0.csv).
- O1: [JSON](raw/opcodes-O1.json), [CSV](raw/opcodes-O1.csv).
- O2: [JSON](raw/opcodes-O2.json), [CSV](raw/opcodes-O2.csv).
- Os: [JSON](raw/opcodes-Os.json), [CSV](raw/opcodes-Os.csv).

Every level reports results 9409, 160, 160, 160, 128, 2019, 239, 135 and
40225 in workload order. The runner independently validates graphics
bitplanes, copy bytes/sentinels, host-visible results, stack/bank state
and linked-object/payload bytes as applicable. Identical scalar results
alone are not treated as proof of equivalent memory behavior.

The raw reports retain `toolchain.revision = "unknown"` and the exact
tool/workload/model/payload fingerprints. No fresh nine-workload emulator
timing matrix or comparison against 0.2.0 is claimed. Older reports use
their own model/source identities; remeasure older tools with the same
runner before asserting a strict before/after improvement. Historical
demo cycle tables remain distinct from this functional benchmark matrix.
