# Frozen-candidate optimization results

Source: `d967c3ef45a948ff457d81052448732da3bd73dc`, measured 2026-10-09.
All nine benchmark sources are identical across levels; no workload rewrite
was used to obtain the comparison. **O0 here is the current candidate's default
policy, not a fresh measurement of the historical v0.1.0 release.**

Each cell below is **linked CODE bytes / calibrated fast-GSU cycles**. CODE
includes startup and padding. The ROM-palette payload adds eight DATA bytes;
other official workloads have no DATA bytes. Static RAM reservations, native
tool executable sizes and padded SNES ROM sizes are not CODE size.

| Workload | O0 bytes / cycles | O1 bytes / cycles | O2 bytes / cycles | Os bytes / cycles |
| --- | ---: | ---: | ---: | ---: |
| `triangle_fill` | 1,198 / 7,359,320 | 381 / 1,997,695 | 268 / 243,130 | 231 / 1,920,665 |
| `horizontal_span` | 477 / 80,895 | 217 / 21,710 | 132 / 1,473 | 98 / 3,975 |
| `rom_palette_plot` | 854 / 284,075 | 442 / 117,715 | 326 / 21,549 | 288 / 23,179 |
| `ram_palette_plot` | 751 / 239,750 | 348 / 63,130 | 253 / 9,254 | 208 / 42,880 |
| `memcpy` | 1,014 / 2,503,610 | 530 / 118,490 | 386 / 78,015 | 350 / 93,160 |
| `function_call` | 742 / 124,585 | 361 / 51,730 | 196 / 38,800 | 196 / 38,800 |
| `switch_dense` | 1,304 / 31,065 | 688 / 13,710 | 366 / 8,025 | 366 / 8,025 |
| `switch_sparse` | 1,344 / 32,160 | 750 / 14,975 | 373 / 7,995 | 373 / 7,995 |
| `arithmetic` | 1,742 / 351,119 | 501 / 146,434 | 285 / 25,419 | 264 / 26,820 |

All 36 cases passed independent result/memory checks and cross-level observable
fingerprints. Expected results are 9409, 160, 160, 160, 128, 2019, 239, 135 and
40225 respectively. Full load/store/stack/PLOT/COLOR/GETC/RPIX/branch/cache and
opcode counters are in [JSON](benchmark-results.json) and [CSV](benchmark-results.csv).
Executed opcodes are **not** cycles; immediate operand bytes contribute fetch
cost but are not additional opcodes.

Under this fixed cold-entry LoROM profile, O2 reduces triangle CODE by **77.63%**
and cycles by **96.70%** relative to current O0. This is a workload-specific
emulator result, not a general speedup guarantee or FPS measurement.

## Os: retain the tradeoff, not a speed claim

Os never exceeds O2's payload size in these nine cases, but only three cases
have identical timing. Six smaller cases are slower:

- Triangle saves 37 bytes (13.81%) but takes **7.90x** the O2 cycles. Its size
  candidate spills loop state/reconstructs frame addresses instead of retaining
  O2's R12/LOOP path. Neither measured triangle executes CACHE, so automatic
  CACHE selection does not explain this penalty.
- RAM palette saves 45 bytes but takes **4.63x** the O2 cycles.
- Horizontal span saves 34 bytes but takes **2.70x** the cycles.
- ROM palette, memcpy and arithmetic also trade cycles for fewer bytes; see the
  exact numbers above rather than assuming Os behaves like O2.

The bounded candidate search follows the size contract, not a proof that these
penalties are unavoidable or optimal. Prefer O2 for speed-critical drawing;
measure Os in the deployment profile. See the earlier
[Os investigation](../../optimization.md#phase-2-investigating-extreme-os-cycle-costs)
for the prior countdown/branch/division-cache corrections. Those historical
working-tree figures are not substituted for this committed-source evidence.

## RAM execution demos: separate workloads and profiles

| Demo | Level | Payload bytes | Mean fast-GSU cycles | Completed-image FPS |
| --- | --- | ---: | ---: | ---: |
| Rotating checkerboard triangle | O2 | 2,733 | 945,534.515625 | 20.032931 |
| Rotating checkerboard triangle | Os | 2,733 | 944,432.640625 | 20.032937 |
| Interactive shapes, wire | O2 | 12,987 | 407,584 | Not measured |
| Interactive shapes, wire | Os | 12,978 | 418,768 | Not measured |
| Interactive shapes, filled | O2 | 12,987 | 1,739,658 | Not measured |
| Interactive shapes, filled | Os | 12,978 | 1,850,435 | Not measured |

Rotation means use the first 64 poses; the 65th GSU sample checks wrap. Both
levels pass full-image/ABI checks and produce matching hashes for all 64 PNGs.
Their 65,536-byte padded SNES ROM size is separate from payload size. The nearly
identical 20.03 FPS includes host/VRAM DMA/presentation intervals and is not a
claim that dividing 21 MHz by a single pose's GSU cycles predicts display FPS.

Shapes timings average four wire or four filled samples at yaw=8, pitch=5,
size=12. Os saves nine payload bytes while both render modes cost more cycles.
Independent controller tests pass at both levels; the timings are not a scan
of all poses, sizes or controller events. O0/O1 do not fit this host's code
reservation and were not used for this demo. Full samples, PASS evidence and
hashes are in [demo-results.json](demo-results.json) and [CSV](demo-results.csv).

## Measurement boundaries

The nine workloads execute from LoROM `$00:8000`, NTSC CLSR=1, GSU=100%, CFGR=0,
with cold caches/buffers on entry. The demos execute from RAM at the origins
and bitmap configurations recorded in [validation.md](validation.md). These
profiles must not be mixed in a before/after ratio. Emulator version, external
core hash, exact tool/source fingerprints and commands are recorded in
[environment.json](environment.json) and [reproduce.md](reproduce.md).

Cycles include a guarded STOP-completion fetch adjustment but exclude initial
prefetch and host copying/DMA/display. No physical-hardware timing, universal
warm-cache performance, independent per-instruction latency or interactive
FPS claim is made. All reports were generated before documentation edits; the
compiler/test/benchmark/demo sources stayed frozen throughout execution.
