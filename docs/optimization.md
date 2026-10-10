# GSU optimization modes

For the latest committed-source measurements, use the
[v0.2.0 candidate results](releases/v0.2.0/optimization-results.md) and
[validation/provenance](releases/v0.2.0/validation.md). They cover all four
levels plus RAM demos on `d967c3ef45a948ff457d81052448732da3bd73dc`.
The development snapshots later in this document remain historical evidence;
they must not be relabeled with the candidate SHA or treated as a published release.

DiscoC provides local **O1** (`-O` or `-O1`), global **O2** (`-O2`), and
size-oriented **Os** (`-Os`).
The default **O0** retains the v0.1.0 generation policy, with correctness fixes
applied across levels. O0 does not disable every longstanding transformation:
AST folding, hardware-loop
recognition, switch selection and branch relaxation remain available at all levels.
O2 includes O1 selection and adds verified CFG/SSA transformations and
SCCP/CFG cleanup, scalar-cell promotion, GVN/known-bit proofs, modular numeric
induction, hot/cold lifetime splitting, cross-block allocation, divmod fusion, shared terminal paths,
proven redundant-check elimination, ISA-aware allocation costs, safe local spill
copies, typed initializer compaction, counted-loop rerolling, automatic CACHE
selection and pipeline-aware machine scheduling. Os uses an explicit byte-size
policy with bounded emitted-code candidate selection. O3/Ofast are
unsupported and diagnosed.

## Selecting the mode

```sh
discc -O main.dc -o main.o
discc -O1 --emit-asm main.dc -o main.s
discc build -O1
discc build -O2
discc -O2 --emit-ir main.dc
discc -Os main.dc -o main.o
discc build -Os
```

For a whole project, including discovered source imports:

```toml
[compiler]
optimize = true
```

For O2, use the integer setting **instead of** `optimize`:

```toml
[compiler]
optimization_level = 2
```

For size-oriented emission:

```toml
[compiler]
optimization_level = "s"
```

`optimization_level` accepts integers 0/1/2 or the string `"s"`. Setting both keys is an error, even
when a CLI option overrides them. `false` or omitted settings keep O0.
Explicit CLI wins regardless of `--config`
position; the last CLI level wins. `discc build -O0` overrides an enabled
manifest. Ordinary compilation still emits one unit, not imported implementations.
The policy does not change language semantics, object version or the call ABI.
O2 can add a private performance-only CODE alignment hint. O0/O1 IR inspection remains the frontend IR;
`--emit-ir -O2` prints the optimized SSA, and `--check -O2` verifies it without
emission. AST inspection is unchanged. It does not add an
executable SPC700 backend. O0/O1/O2/Os objects can be linked together; `discld`
and `discas` do not have optimization-level flags.

A CMake `Release` build optimizes the native C++ toolchain itself; it does not
implicitly select O1 for DiscoC sources. Select the generated-code policy
explicitly through the compiler flag or manifest.

## Size policy (-Os)

Os minimizes emitted CODE plus DATA and necessary DATA alignment, not executed
instruction counts or predicted cycles. It retains the language, object and call
ABI contracts, checked failures, volatile observations, bank restoration and
graphics effects. Smaller code may execute more instructions or cycles.

- **Reuse verified IR infrastructure.** Pruned scalar/cell SSA, SCCP, CFG
  simplification, pure GVN/known-bit proofs, local cleanup, dead-code elimination
  and divmod fusion remain available. The conservative size pipeline restricts
  leaf inlining to two estimated ALU units and omits speed-oriented LICM,
  induction/recurrence expansion and hot/cold lifetime splitting. It does not
  add automatic loop unrolling.
- **Choose by real emitted bytes.** For units of at most 64 functions and
  16,000 input instructions, compare up to eight deterministic IR policies:
  compact O2-policy IR/allocation and conservative size-policy IR/allocation,
  each with or without repeated-division pooling and typed compaction/near-pointer
  promotion. Conservative IR alternatives prevent LOOP setup or extra spills
  from forcing a speed-only size increase. Each candidate undergoes
  verified lowering, bounded branch relaxation and machine emission. When an
  automatic CACHE probe is present, also emit its no-auto-CACHE alternative:
  scheduling a useful delay-slot instruction can beat a CACHE occupying that
  byte. There are at most sixteen emitted alternatives. Keep the
  smallest actual CODE/DATA result; ties retain the earlier candidate. An O2
  transformation is therefore still allowed when its complete candidate wins
  on bytes. This is a bounded search, not a claim that every individual
  transformation is byte-optimal. Larger units retain the compact O2-policy
  fallback without candidate expansion.
- **Use static allocation costs.** The conservative size allocator counts
  static uses, transfers, PHI copies and caller preservation instead of
  weighting loop frequency. Its alternative must strictly reduce estimated
  fetch bytes; the final machine comparison includes real spills and copies.
- **Pool repeated software division.** Eligible signed and unsigned word
  division/remainder sites can call separate private, same-unit divmod kernels.
  A single generic division keeps the inline sequence. A pool is selected only
  if the complete emitted candidate, including calls and preservation, is
  smaller. Power-of-two instruction selection still precedes pooling. Kernels
  retain the checked zero-divisor fault point, fixed 16-step modular arithmetic,
  truncation toward zero and signed remainder rules. They preserve allocated
  R5/R7/R8, frame/stack and hardware-loop registers, R14 and ROMB/RAMB. Plot
  callers save and restore R1/R2. This is private implementation machinery,
  not a new public calling convention or link-time library deduplication.
- **Share exact effectful tails.** Beyond existing machine epilogues/fault
  islands, a bounded IR pass merges identical suffixes before matching returns
  or a shared non-PHI successor. Eligible suffixes include void calls, stores,
  color/cursor operations, PLOT and discarded RPIX, with identical operands,
  types and effect metadata. One original path still executes one ordered
  sequence; volatile reads are not equated, stores/flushes are not removed,
  and distinct arguments do not merge. Value-producing operations other than
  scalar constants and functions with explicit hardware-loop scope markers
  are conservatively excluded. Verification checks dominance after sharing.
- **Avoid speed-only padding.** Keep explicit CACHE instructions and safe
  non-growing pipeline scheduling, but omit optional CACHE-entry padding and
  private performance alignment hints. Required typed/data alignment and
  linker placement checks remain intact.

`--emit-ir -Os` and `--check -Os` inspect/verify the conservative size IR.
Machine emission may choose the smaller alternate O2-policy candidate;
compiler assembly and final linked assembly still describe their actual
selected bytes. Candidate search, suffix matching and branch widening are
bounded and deterministic. No profile input, unchecked-memory mode, arbitrary
sequence outlining or cross-object helper merging is introduced.

Use `benchmarks-Os` and `benchmarks-mesen-Os` to measure bytes and the cycle
tradeoff separately. Do not interpret a cycle regression as a violation of a
size policy; results, faults, memory effects and byte-exact exports must still
pass. In the pre-compaction snapshot, the rotating checkerboard selects the same 3,008-byte payload
as O2: all 64 images match, mean measured fast-GSU cycles remain
2,914,672.734375, and completed-pose throughput remains about 6.70 FPS. That
program has no profitable repeated-kernel pooling opportunity in this search.

### Historical size/cycle snapshot

These are historical working-tree O2 versus Os measurements, not v0.1.0 or a released
v0.2.0. The retained [O2 reference](../benchmarks/results/gsu-Os-reference-O2-mesen.json),
[Os timing report](../benchmarks/results/gsu-Os-mesen.json) and
[opcode report](../benchmarks/results/gsu-Os.json) use the same nine workloads,
model and calibrated Mesen profile. Eight workloads select identical payloads;
the size winner for `triangle_fill` is smaller but substantially slower.

| Workload | O2 payload | Os payload | O2 fast-GSU cycles | Os fast-GSU cycles |
| --- | ---: | ---: | ---: | ---: |
| `triangle_fill` | 334 | 307 | 822,430 | 2,138,500 |
| [Signed/unsigned division pool](../benchmarks/size/README.md) | 657 | 536 | 9,535 | 9,735 |
| Rotating checkerboard, mean of 64 poses | 3,008 | 3,008 | 2,914,672.734375 | 2,914,672.734375 |

The division probe saves 18.4% of its bytes for about 2.1% more cycles. It
executes both new kernel kinds in Mesen and independently verifies results,
stack, RAMBR, opcode counts and final-assembly reconstruction. Its reports are
[O2](../benchmarks/results/division-pool-O2.json) and
[Os](../benchmarks/results/division-pool-Os.json); it is separate from the nine
baseline workloads. `triangle_fill` saves 8.1% but costs about 2.60 times the
cycles: use O2 for speed-critical drawing. The multi-object regression additionally
shrinks repeated signed division/remainder from 857 to 663 payload bytes, and
its exact-tail fixture from 356 to 329, with selected-path volatile effects
and return values still checked.

All 27 frozen O0/O1/O2 benchmark payload hashes and functional counters remain
unchanged. The [rotating demo's 64 image hashes](../benchmarks/results/rotation-Os-summary.json)
also match frozen O2; its padded
SNES ROM stays 65,536 bytes and completed-image throughput stays about 6.70 FPS.
Emulator timing is not a physical-hardware guarantee or isolated opcode latency.

### Phase 2: investigating extreme Os cycle costs

The following measurements compare frozen tools immediately before and after
the contributor-fix reconciliation's second phase. All nine official benchmark
sources are unchanged. These are uncommitted working-tree measurements, not
release-candidate evidence: reports identify the executable hashes and use
`toolchain.revision = "unknown"`. A later release pass must rebuild and validate
the final committed revision.

Three different causes were found:

- `triangle_fill`: the byte-winning candidate spills loop state and repeatedly
  reconstructs frame addresses. O2 instead keeps the pixel count in R12 and
  uses LOOP. Neither measured triangle payload executes CACHE; a cache-policy
  difference does not explain this case.
- `horizontal_span`: Os used an increasing counter, comparison and two-branch
  loop tail without CACHE. O2 uses LOOP/CACHE. The source does not observe the
  counter, so a smaller software countdown can remove much of the overhead.
- `arithmetic`: both levels use the same fixed 16-step division algorithm, but
  Os lacked kernel CACHE. RAM traffic and software outer-loop control contribute
  too; the large timing difference is not explained by opcode counts alone.

Retained improvements preserve the size-first objective:

- Proven, unobserved word induction can become an unsigned software countdown
  in Os. The original entry guard handles zero trips; signed distance is exact
  on the taken path, including 65,535 iterations. Observable induction,
  additional carried PHIs, early exits, calls and potentially wrapping bounds
  remain conservative. This alternative avoids R12/R13 preservation overhead.
- An allocated word updated in place by one uses INC/DEC. A following equality
  or inequality against zero may reuse Z only when the register update is
  adjacent and no emitted work intervenes. Byte operations, spills and unrelated
  flag consumers do not inherit this proof.
- A conditional-exit/BRA-backedge pair can become one inverted conditional
  branch when both original delay slots are NOPs. Independently reachable or
  relocated entries are protected, and range failure retains the old sequence.
- Os can replace the successful zero-divisor guard's existing delay-slot NOP
  with CACHE, without adding bytes or padding. The prefetched success target
  establishes the cache window for the bounded kernel. Explicit CACHE ownership
  suppresses this automatic rebasing; calls still do not preserve a cache base.

| Level / workload | Payload bytes before → after | Fast GSU cycles before → after |
| --- | ---: | ---: |
| O2 `triangle_fill` | 274 → 268 | 246,040 → 243,130 |
| O2 `horizontal_span` | 132 → 132 | 1,473 → 1,473 |
| O2 `arithmetic` | 292 → 292 | 25,652 → 25,652 |
| Os `triangle_fill` | 258 → 253 | 1,933,295 → 1,930,870 |
| Os `horizontal_span` | 108 → 98 | 11,060 → 3,975 |
| Os `arithmetic` | 273 → 270 | 79,564 → 27,172 |

Os span cycles fall **64.06%**, and arithmetic cycles fall **65.85%**, while
their payloads also shrink. The triangle's penalty remains: Os saves 15 bytes
against the new O2 output, but takes about **7.94 times** its cycles. This is
a remaining performance concern in the current byte-first candidate selection,
not a claim that the tradeoff is optimal or unavoidable. Use O2 for
speed-critical drawing. Experiments decoupling the size IR from hot allocation
and changing equal-size allocation tie handling did not improve these cases
and were not retained. No benchmark-specific transformation was added.

All nine workloads pass the functional model before/after at O2 and Os; none
increases payload size or executed opcode count. The three cases above also
pass independent Mesen result, full-RAM and stack comparisons. The focused
Release regression suite passes **57/57**, including contributor loops/switches,
SSA/PHIs, volatile ordering, graphics and byte-exact assembly checks. New cases
cover signed/unsigned countdown boundaries, wrap and intervening effects,
division by zero, cache ownership and protected branch entries. Changed
production implementations pass GCC C++14 warnings-as-errors syntax checks.
The complete Debug/Release matrix and rotating demo were not rerun in this
phase.

Timing uses Mesen **2.2.1+20ba206cef5ba207c21203176d02cb9f43dda9fb** on Windows,
NTSC, fast GSU clock (`CLSR=1`, 100%), LoROM execution at `$00:8000`, RAMBR 0,
SP `$FFFE`, `CFGR=0`, `SCBR=$18`, and host `SCMR=$39`. Instruction cache, pixel
cache and ROM buffer start cold. The calibrated window includes STOP completion
but excludes initial pipeline prefetch and SNES CPU copy/DMA/display time.
Under this profile master clocks equal fast-GSU cycles. These are emulator
measurements, not physical-hardware timing, CPU/PPU presentation time or FPS.

Reproduce functional metrics with the native tools and benchmark runner:

```sh
cmake -DTOOLS_DIR=build/merge-verification-release/bin -DOPTIMIZATION=s -DOUTPUT_DIR=build/phase2-opcodes-Os -P benchmarks/run.cmake
```

For calibrated timing, supply the installed Mesen and WLA-DX executables:

```sh
cmake -DTOOLS_DIR=build/merge-verification-release/bin -DOPTIMIZATION=s -DCASE=horizontal_span -DOUTPUT_DIR=build/phase2-mesen-Os/horizontal_span "-DMESEN=/path/to/Mesen.exe" "-DWLA_65816=/path/to/wla-65816.exe" "-DWLALINK=/path/to/wlalink.exe" -P benchmarks/run-mesen.cmake
```

Use optimization `2` for O2 and separate output directories. Repeat the timing
command for `triangle_fill` and `arithmetic`. Both runners produce JSON and CSV;
preserve pre-change executables to reproduce a before/after comparison, and
use `benchmarks/compare.cmake` to enforce matching workload/model/timing
fingerprints. Investigation reports remain under the ignored
`build/os-investigation-phase2/` directory, not versioned release reports.

### Phase 2: loop-state refinements and demo measurements

The following pass adds five guarded backend improvements at O2/Os, without
rewriting any of the nine benchmark sources, the rotating triangle, or the
interactive-shapes renderer:

- Discarded adjacent `cursor.x/y` updates by one use INC/DEC on R1/R2. Used
  prefix/postfix results, intervening effects and PLOT/RPIX retain snapshots.
  PLOT still performs its own X increment; no extra increment is emitted.
- Single-use, immediately consumed scalar values can stay in R0 instead of
  making a temporary spill/reload. Near-RAM addresses require a proof covering
  the complete access width, revalidated after final frame sizing. Far pairs
  retain their preservation path; a near address cannot borrow R0 across a
  far-pair store's materialization.
- Automatic CACHE selection considers ordinary constant-trip loops as well
  as scoped hardware loops, using post-relaxation bytes and a conservative
  cold-fill cost. Windows reserve alignment headroom inside the 512-byte
  hardware limit. Os also emits an uncached alternative and selects by actual
  payload size; CACHE is not mandatory merely because a loop exists.
- Safe backedges bypass a header's explicit CACHE through a private entry
  immediately after it. External entries still execute the original CACHE at
  the same address, preserving CBR anchoring. This avoids moving CACHE into a
  differently aligned preheader. Calls, nested cache owners and other rebasing
  effects block the bypass; PHI copies and R12/R13 preservation are retained.
- Initial register choices favor hot PHI edges, without changing interference
  rules or allocating special GSU registers. Successful near-RAM checks can
  cover another access to the exact same immutable SSA address and no larger
  width. CFG entries, PHI copies and calls fence that credit; different values,
  wider accesses and unproved addresses keep their checks.

The frozen-tool [before/after evidence](../benchmarks/results/loop-state-summary.json)
identifies executable, source and payload hashes. It is **uncommitted
working-tree evidence**, not validation of a final release SHA. The full
[O2 before](../benchmarks/results/gsu-loop-state-O2-before-mesen.json),
[O2 after](../benchmarks/results/gsu-loop-state-O2-mesen.json),
[Os before](../benchmarks/results/gsu-loop-state-Os-before-mesen.json) and
[Os after](../benchmarks/results/gsu-loop-state-Os-mesen.json) reports retain
functional counters and matching workload/model/harness/emulator fingerprints.

| Workload | O2 payload bytes, before → after | O2 GSU cycles, before → after | Os payload bytes, before → after | Os GSU cycles, before → after |
| --- | ---: | ---: | ---: | ---: |
| `triangle_fill` | 268 → 268 | 243,130 → 243,130 | 253 → 231 | 1,930,870 → 1,920,665 |
| `horizontal_span` | 132 → 132 | 1,473 → 1,473 | 98 → 98 | 3,975 → 3,975 |
| `rom_palette_plot` | 334 → 334 | 97,645 → 21,549 | 296 → 296 | 103,225 → 23,179 |
| `ram_palette_plot` | 261 → 253 | 43,070 → 9,254 | 216 → 208 | 48,000 → 42,880 |
| `memcpy` | 402 → 386 | 88,255 → 78,015 | 372 → 350 | 107,880 → 93,160 |
| `function_call` | 196 → 196 | 38,800 → 38,800 | 196 → 196 | 38,800 → 38,800 |
| `switch_dense` | 366 → 366 | 8,025 → 8,025 | 366 → 366 | 8,025 → 8,025 |
| `switch_sparse` | 373 → 373 | 7,995 → 7,995 | 373 → 373 | 7,995 → 7,995 |
| `arithmetic` | 292 → 285 | 25,652 → 25,419 | 270 → 264 | 27,172 → 26,820 |

All nine preserve or improve payload bytes and cycles. The ROM-palette speedup
has the same executed opcode count: CACHE changes fetch cost, not the work
being computed. `triangle_fill` remains an Os concern: 231 versus 268 bytes
still costs about **7.90 times** the O2 cycles. This pass does not establish
that tradeoff as optimal or unavoidable.

The independent RAM-execution demos use NTSC, fast GSU clock (`CLSR=1`, 100%),
normal multiply and cold CACHE at each entry, with the same host and sources:

| Demo / policy | Payload bytes, before → after | GSU cycles, before → after |
| --- | ---: | ---: |
| Rotating triangle, O2 | 2,806 → 2,733 | 1,149,680.375 → 945,534.515625 mean over 64 poses |
| Rotating triangle, Os | 2,806 → 2,733 | 1,148,578.5 → 944,432.640625 mean over 64 poses |
| Interactive wireframe, O2 | 13,794 → 12,987 | 469,556 → 407,584 at the fixed test pose |
| Interactive filled, O2 | 13,794 → 12,987 | 1,901,192 → 1,739,658 at the steady-state test pose |
| Interactive wireframe, Os | 13,794 → 12,978 | 469,556 → 418,768 at the fixed test pose |
| Interactive filled, Os | 13,794 → 12,978 | 1,901,192 → 1,850,529 at the steady-state test pose |

Rotation executes at `$70:6000`, SP `$FFFE`; interactive shapes at `$70:8000`,
SP `$EFFE`, OBJ/4bpp. The shapes pose is yaw 8, pitch 5, size 12. GSU cycles
exclude host CPU/DMA/display. Separately, the rotation's completed-image
publication probe improves from about **15.02 to 20.03 poses/s**, including
host/presentation costs. This is not the SNES refresh rate or a shapes FPS
measurement. The padded rotation ROM remains 65,536 bytes.

Mesen **2.2.1+20ba206cef5ba207c21203176d02cb9f43dda9fb** verifies all 64
rotation poses plus wrap, 49,152 pixels per pose, byte-exact assembly, and the
deliberate red-screen failure path at both levels. Shapes pass 35 complete OBJ
framebuffers, 255 controller polls, rotation/resize/button-edge checks and the
negative path. Native Windows verification passes **254 MSVC Release tests**
(optional Mesen tests enabled) and **246 MSVC Debug tests**. All seven changed
production implementations pass strict GCC C++14 syntax and
warnings-as-errors checks. Regression cases cover all 16 cache alignments,
cursor wrap and used updates, nested CACHE, PHI affinity, volatile ordering,
byte-to-word access checks, far-pair stores and final-frame proof loss. Direct
compaction-pass tests also preserve valid SSA for uncontracted body/latch chains.
Linux, macOS, DOS, sanitizers and physical hardware were not rerun in this pass.

Reproduce all nine cases with the preceding `benchmarks/run-mesen.cmake`
command, omitting `CASE`, selecting optimization `2` or `s`, and using separate
output directories. For the same demo verification and timing windows, supply
Mesen/WLA-DX through PATH or their named CMake options:

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -DOPTIMIZATION=2 -DOUTPUT_DIR=build/loop-state-rotation-O2 -DVERIFY_MESEN=ON -DMEASURE_GSU_TIMING=ON -P tests/graphics/rotating_triangle/build-snes.cmake
cmake -DDISCO_TOOLS_DIR=build/release/bin -DOPTIMIZATION=2 -DOUTPUT_DIR=build/loop-state-shapes-O2 -DVERIFY_MESEN=ON -DBENCHMARK=ON -P tests/graphics/interactive_shapes/build-snes.cmake
```

Use `s` and separate directories for Os. Generated timing JSON/CSV and demo
logs remain beside their payloads. Rebuild and repeat these checks against the
final committed revision before treating them as release-candidate evidence.

## What O1 does

- Fold scalar constants through narrowing, sign extension, bool conversion
  and nested casts using the language's modular numeric rules. Pointer casts
  and potentially failing shifts/divisions retain their checks.
- Forward loads and remove identical stores only for nonvolatile, unescaped
  scalar locals with matching representations, within one basic block. Calls,
  unknown writes, volatile accesses, framebuffer operations and implicit loop
  boundaries fence this proof. Globals, arrays, aggregates and pointer-valued
  storage remain outside it. COLOR/cursor state changes alone do not write RAM.
- Remove an overwritten local store only when that same eligible local has not
  been observed. Volatile/hardware effects, unknown writes, calls, loads,
  checked member/index addresses and potentially failing arithmetic fence this
  stronger proof; it does not reorder effects across a possible fail-stop.
- Reuse identical pure scalar expressions with the same captured SSA operands
  within one basic block. The value-numbering table is bounded to 64 entries
  and cleared at calls and implicit hardware-loop boundaries. It does not
  coalesce memory reads, division, dynamic checked shifts or cursor/RPIX reads.
- Remove representation-identical scalar casts and arithmetic identities.
  Reuse an adjacent scalar snapshot in R0 only until any byte is emitted or a
  control-flow label is entered; never infer a variable/pointer alias from it.
- Specialize constant shifts after casts: high-byte right shifts use HIB,
  with SEX for signed negative values and ASR as needed. A proven nonnegative
  masked bit-9 extraction uses HIB + LSR instead of a shift loop. Left shifts
  by eight or more use LOB + SWAP and the remaining modular shifts.
- Fuse whole single-bit mask/shift patterns into verified `bit.extract` IR,
  then remove unused pure producers. This includes the checkerboard's bit 9.
  `(x >> 15) & 1` remains 0/1 even for signed x, whereas signed
  `(x & (word)0x8000) >> 15` retains its 0/-1 behavior. The allocator sees the
  original word's extended lifetime rather than a hidden emitter-only use.
- Specialize representable power-of-two multiplication, division and remainder.
  Unsigned division/remainder use shifts/masks. Signed division biases negative
  dividends before ASR to truncate toward zero, including negative divisors;
  signed remainder follows the dividend's sign. -32768/-1 wraps as specified.
  Evaluation remains observable even when the resulting remainder is zero;
  zero divisors still fail-stop. Other divisors retain checked software helpers.
- Keep short CFG/internal branches individually; widen only out-of-range
  branches and invert simple conditions when the false edge is fallthrough.
  Widening is monotonic, capped at eight emission attempts with a safe long-form
  fallback. All branches retain their pipeline delay slots.
- Emit nontrivial SSA definitions once, in IR order. Allocate single-use and
  reused scalar results to R5/R7/R8, including checked-pointer/plot functions.
  Retain real aligned frame spills under register pressure.
- Emit eligible ALU results directly into allocated R5/R7/R8 with FROM/TO/WITH,
  avoiding the R0 round trip when profitable. Selection retains the existing
  accumulator path when it already supplies a cheaper immediate/unary result.
  Scratch selection continues to reserve R1/R2 inside plot contexts.
- Rematerialize constants and plain symbol/frame addresses, not loads,
  calls, cursor snapshots, RPIX or potentially faulting pointer computations.
- Save caller-live allocated values across calls. Keep cross-block values
  and far pointer pairs in frame slots rather than infer unsafe global liveness.
- Combine binary operands and near-RAM stores without expression pushes.
  Load directly from a near-RAM address in R0 into R0.
- Select SBK for a near-RAM word store when the last physical RAM access
  provably used the same checked address. Fence the implicit address across
  spills, stack accesses, bank selection, calls and control-flow boundaries.
- Use IBT only when its sign extension exactly matches the required value.
  Select compact INC/DEC, immediate masks, valid constant shifts and
  register-based large frame/address adjustments where appropriate.
- Fuse an adjacent single-use comparison and branch into CMP and the correct
  signed/unsigned branches. Comparisons used as values still produce bool.
- Scale near-pointer offsets with checked shifts/add/sub for power-of-two
  strides instead of walking elements. Retain carry/borrow, alignment, null,
  bank-window and fault checks. Far transitions and other strides keep the
  explicit baseline algorithm.
- Elide repeated identical literal COLOR writes only within a basic block
  with unchanged POR. Still evaluate expressions; calls, emitted CMODE,
  GETC, block boundaries and implicit hardware-loop boundaries invalidate the
  known color state.

There is no unchecked-memory mode. Fewer guards can result from removing an
unnecessary expression push/spill, not from accepting illegal typed accesses.
Volatile accesses retain their count and order. Aliasing calls cannot cause a
previously sampled value to be reloaded from modified memory.
The local IR pass runs on an owned backend copy and verifies it before/after
rewriting uses and compacting IDs. Register allocation sees forwarded values'
actual extended lifetimes; this is not global mem2reg or speculative aliasing.

## What O2 adds

The pipeline works on an owned IR copy, verifying after transformations:
expose hardware-loop edges → pruned local/cell SSA → SCCP/local cleanup +
known-bit proofs/GVN → bounded leaf inlining/specialization → scalar-cell
fixed point → safe LICM/address and numeric induction → SCCP/local/proof cleanup
→ divmod fusion → initializer/count-loop compaction and CFG cleanup → split PHI edges → hot-block layout/local cleanup → hot/cold
lifetime partitioning → CFG-aware allocation/emission/branch relaxation →
bounded machine scheduling. It has no LLVM dependency.

- **Complete CFG liveness.** Successors/predecessors, packed-bit dominators,
  immediate dominators, dominance frontiers and natural loops include the
  hardware LOOP backedge. PHI incoming uses belong to predecessor edges, not
  the destination block. Liveness also covers non-natural cycles; loop
  transformations require a suitable natural loop/preheader.
- **Pruned mem2reg.** Promote definitely initialized, nonescaping, nonvolatile
  byte/word/bool/enum and near-pointer locals/parameters. Parameter values are loaded
  once from their existing ABI slots. Assignments become SSA values and typed
  PHIs at live joins. Small arrays/structs additionally use the scalar-cell
  rules below. Escaped, far-pointer, dynamically indexed, global and
  volatile storage remains memory-backed; an uninitialized read does not gain
  an invented zero or undef value.
- **Scalar replacement of local aggregates.** Identify each eligible byte/word/
  bool/enum cell by `(SymbolId, byte offset)`, not a fake declaration or field
  name. Static member/constant-index paths must lie completely in the allocation
  and obey typed alignment. Each cell has its own definite-assignment proof,
  rename stack and PHIs. Roots are near-RAM locals of at most 64 bytes/16 cells;
  aggregate parameters, far-pointer cells, escaped/cast/dynamic paths, overlapping
  cells and volatile accesses retain memory. Misaligned packed word accesses
  cannot become valid SSA values. Proven dead address paths and wholly unused
  local frames disappear; unproved checked pointer operations do not. Iterated
  promotion/folding discovers indices exposed by prior SSA promotion, with
  32 rounds/two million scanned instructions as a convergence budget. This is
  not automatic unrolling of the triangle's dynamically indexed edge loop.
- **Global value numbering.** Walk the dominator tree with scoped available
  expressions; a sibling cannot reuse another sibling's value. Keys include
  representation, enum identity, operation and resolved SSA operands, including
  commutative forms. Reuse pure scalar computations only: no loads, cursor/RPIX
  snapshots, pointers, graphics, division/remainder or dynamically checked
  shifts. A captured operand remains the same value across a call/volatile
  write; the read that originally produced it is never merged or repeated.
- **Known bits and intervals.** Propagate conservative zero/one bits and signed/
  unsigned intervals through scalar constants, casts, bitwise operations,
  constant shifts, nonwrapping arithmetic and every PHI incoming edge. Remove
  proved redundant masks or constant comparisons, then let SCCP clean their
  CFG. Signed narrowing/extension and arithmetic right shifts retain their
  exact bit semantics. Arithmetic crossing wrap returns the full range, not a
  guessed monotonic interval. Pointer comparisons and integer/pointer casts do
  not acquire integer-address facts. Loads, volatile effects and faulting
  producers remain even if a later pure consumer is simplified. This is not
  path-sensitive branch refinement or loop-bound inference.
- **SCCP and CFG cleanup.** Propagate typed byte/word/bool/enum constants over
  executable SSA edges. Fold PHIs only from executable predecessors; revisit
  users when a new backedge changes a constant to a variable. Simplify constant
  branches/switches, remove unreachable blocks, resolve singleton PHIs and
  merge ordinary single-predecessor chains. Preserve surviving PHI order and
  coverage, dense value/block IDs and hardware-loop entry identities. Constants
  use the same modular evaluator as O1. Unknown memory, calls, cursor reads,
  RPIX and checked pointer operations are not constant-folded; faulting division
  or invalid shifts remain at their original point. SCCP itself does not perform
  range analysis or cross-block alias/load speculation. LOOP retains both taken/final edges,
  including its zero-count wrap semantics. If pruning would fragment a hardware
  save/restore scope, retain that function's CFG conservatively.
- **Cross-block allocation.** Color exact CFG interference using R5/R7/R8,
  coalescing compatible PHI locations and favoring actual uses in inner loops.
  This replaces O1's conservative interval strategy only at O2. Reuse aligned
  two-byte spill slots for noninterfering values; far pairs keep unique
  four-byte slots. Preserve precisely caller-live allocated registers.
  Bounded two-color component swaps improve loop-weighted PHI affinity while
  preserving exact interference; preferred spill slots must also remain
  noninterfering. Initial register choices also rank the summed incoming PHI
  edge weights, preferring a hot latch over a cold preheader. This never turns
  a register allocation into a spill.
- **Hot/cold lifetime partitioning.** Introduce a representation-identical
  `live.split` scalar copy in an existing unique loop preheader, and redirect
  its loop uses to that captured snapshot. The original keeps cold uses; each
  part gets its own exact interference/allocation. Trial R5/R7/R8 allocation
  must place the hot copy in a register and reduce a loop-weighted spill/copy
  cost; this is a heuristic, not a guarantee of fewer measured cycles. At most
  two copies per loop/12 per function are accepted. The optional search skips
  functions above 2,048 values/32 loops and makes at most 16 allocation trials.
  The verifier checks the copy's identical scalar representation, and later
  folding/CSE preserves the partition marker. No memory read is recreated.
  Emission also retains a known R0 snapshot across a proved non-R0 register
  copy or a scalar spill restored from R6; labels, calls, R0 writes, loads and
  far-pair emission still fence it. No reserved hardware register is allocated.
- **PHI lowering.** Split conditional/switch/hardware edges before inserting
  parallel copies. Register-to-register edges use direct WITH/TO moves.
  Register-only cycles use volatile R6 as edge scratch; mixed spill or
  rematerialization cycles retain paired, guarded stack snapshots. R6 is never
  an allocated value, cursor or preserved ABI register;
  copies never execute on an untaken edge. R13 can target a backedge copy block
  distinct from initial entry. The verifier checks incoming coverage, types,
  edge dominance and PHI ordering.
- **Safe LICM.** Hoist pure nonfaulting scalar calculations and plain addresses
  into an existing single-successor preheader. Never speculate loads, calls,
  graphics effects, pointer arithmetic, division/remainder or dynamically
  checked shifts into a zero-trip loop.
- **Address induction.** For unsigned-word induction and near-pointer strides
  of at least four bytes, carry the modular scaled index through a PHI and
  increment it instead of recomputing the product. Both addition and subtraction
  retain the original index, carry/borrow, null, alignment and bank-window
  checks at the original access. The last iteration does not speculatively
  compute a faulting next pointer. Signed/far and ineligible patterns retain
  their checked paths; at most four recurrences are added per loop.
- **Numeric induction.** Carry `iv * factor` or `(iv +/- constant) * factor`
  through a new word PHI, with a preheader initialization and latch increment
  by `step * factor`. Signed and unsigned calculations use the same exact
  modulo-65,536 identity. Keep the original induction/exits and checked pointer
  accesses; a scalar product can be initialized for a zero-trip loop because
  it cannot fault or perform memory/hardware effects. Require a single latch,
  unique single-successor preheader and an invariant captured factor. Add at
  most two products per eligible inner/non-nested loop; avoid outer lifetimes
  spanning hotter nested loops and constants already handled cheaply by shifts.
- **Bounded inlining and specialization.** Inline only same-unit single-block
  scalar leaves without calls, observable memory, CACHE or bank effects.
  Constant arguments then fold using existing numeric rules. The cost limit
  is 12 operations, or 4 in a caller with CACHE; software division/remainder
  exceeds it. Growth is capped at 64 cost units/256 cloned nodes per caller
  and 512 units/2,048 nodes per module. This is not general CFG inlining, LTO,
  automatic CACHE insertion, or a promise that an entire function fits CACHE.
- **Proven address checks.** After allocation determines the complete frame,
  `GSUAddressProof` derives conservative scalar intervals and near-RAM address
  facts from stable declaration IDs, temporaries, members, constant addresses,
  safe casts, masks and PHIs. The entry guard establishes the frame extent,
  even FP and parameter-area upper bound. Only a proved nonnull, aligned,
  in-bank access can omit its redundant checks. A proved power-of-two pointer
  offset can use raw scaled ADD/SUB without signed-path/carry/borrow guards.
  Both the original base and the complete result span must be legal, and the
  scaled magnitude must fit a word. Numeric wrapping never becomes a guessed
  small interval. Unknown loads/calls, recursive pointer PHIs, over-aligned
  locals, far/ROM addresses and scaled-index recurrences retain their checks.
- **Local successful-check credit.** Within one emitted block, a successful
  access check of an immutable SSA near-RAM address can justify an equal or
  narrower access to that same value. Pointer-offset arithmetic must first
  pass its original carry/borrow, representation and result-span checks.
  CFG block entries, PHI copies and calls discard the credit; a reloaded pointer or a
  cast has its own SSA identity. A byte check does not justify a word access.
- **Adjacent transient values.** A single-use, nonallocated scalar or proved
  near address can remain in R0 until its immediately consuming operation.
  No load is repeated, no faulting address computation is deferred, and no
  intervening operation may overwrite that snapshot. Pointer eligibility is
  revalidated after final spill/divmod frame sizing; if a preliminary proof
  disappears, restore pointer spills in one bounded retry. Far pairs and
  PHIs retain their normal storage. A far-value store cannot consume its near
  destination this way because pair materialization clobbers the accumulator.
- **Discarded cursor updates.** Adjacent `cursor.x/y` reads, word addition or
  subtraction by one, and writes with no separately used result select
  INC/DEC R1/R2. Used prefix/postfix values and updates separated by PLOT,
  RPIX, calls or other work retain their required snapshots. PLOT still owns
  its physical R1 increment; no extra increment is introduced for a pixel.
- **Stack-check reuse.** Track a lower-bound credit relative to the linker's
  common stack floor. The existing frame guard covers the two saved-register
  pushes; later paired pushes/pops can reuse an established bound. Allocation
  and pushes consume credit; cleanup restores it. Calls, SP replacement,
  blocks, helper joins and hardware backedges fence it. No larger requirement
  is hoisted before an observable effect or onto an untaken path.
- **Divmod fusion.** A dominating word division/remainder can own both results
  when a later opposite operation uses the same resolved SSA operands and type.
  `divmod /` (or `%`) keeps the first operation's result; `divmod.result` selects
  the other component. One sixteen-step helper computes both. The secondary
  word lives in a private, aligned frame slot, surviving branches, calls and
  restoration of R1/R2 in plot blocks. Signed truncation, remainder sign and
  modular `-32768 / -1` behavior remain unchanged; the zero-divisor fail-stop
  stays at the first computation. Different volatile reads/SSA operands and
  non-dominating siblings cannot fuse. Constant power-of-two helpers remain
  cheaper shifts/masks. This adds no source operation or calling convention.
- **Shared terminal paths.** Reuse an identical function-local fault island
  only when its signed-byte conditional branch reaches it; otherwise emit
  another island. Keep a one-byte slot and the same R6 fault category. Successful
  guards are not removed. Return sites share their own function's epilogue,
  retaining R0/R4 results and per-site plot/hardware-loop cleanup. Jumps to the
  epilogue use existing short/long relaxation. No tails are merged across
  functions, differing frames or nonterminal arbitrary instruction sequences.

These are hardware-address proofs, not array/object bounds checking, alias
analysis or an unchecked-memory option. They do not remove volatile reads or
stores. `stack_floor=0` is legal: the frame's bottom empty word is excluded
because it could be address zero. Frame facts rely on the existing checked
GSU ABI, not on source names or a guessed fixed stack pointer. The proof pass
does not mutate IR or appear in `--emit-ir`/`--check`; assembly shows selection.

Explicit hardware setup/end/leave operations save and restore R12/R13,
including nested loops and calls to other loop-using functions. Their scope
stack must agree at every CFG join and be empty at function exit. LOOP's count
zero has its real 65,536-trip do-while behavior; source zero-trip loops are not
silently converted into it.

O2 remains bounded: at most 8,192 CFG blocks, 100,000 values, one million
liveness/call-live entries, PHI inputs and interference edges, 256 nested
hardware scopes and 1,024 promotion candidates, with a 25-million
candidate/instruction scan budget.
The optional divmod matcher caps existing plus new pairs at 256 per function
and matching work at 25 million comparisons; remaining operations keep their
original form. PHI recoloring/slot affinity has two rounds and a 2.5-million
work budget, retaining its already-valid allocation when exhausted.
SCCP also caps a function at 200,000 instructions and one million combined
operand/target entries before verification/cloning; each fixed-point/cleanup
invocation has a 25-million work-item budget.
Required structural/SSA resource limits produce a diagnostic, not an unchecked
allocation. Optional GVN/fact/recurrence searches skip functions over 16,000
values; available-expression insertion stops at 8,192 entries and bit/interval
refinement stops after 24 safe-from-top rounds. Remaining expressions retain
their original semantics. The backend address proof separately
caps 8,192 blocks, 100,000 values, 200,000 instructions, one million operands,
64 refinement rounds and 25 million work items. Reaching one of its caps leaves
unproved accesses checked; it does not weaken safety or invent first-trip PHI
facts. O2 can increase code size or lose cycles on a particular
workload; benchmark both levels rather than assume a universal improvement.

## Hardware constraints

Selection was checked against the
[MesenCE opcodes](https://github.com/nesdev-org/MesenCE/blob/master/Core/SNES/Coprocessors/GSU/Gsu.Instructions.cpp)
and [fetch/buffer pipeline](https://github.com/nesdev-org/MesenCE/blob/master/Core/SNES/Coprocessors/GSU/Gsu.cpp).
These are behavioral references, not copied production code or a substitute
for execution tests. Pipeline fetches are bytes: IWT has two operand fetches;
IBT has one but **sign-extends**. `$007F` and `$FF80` fit IBT; `$0080`, `$00FF`
and `$FF7F` do not. The decision uses full bits, not source signedness.
Relocation placeholders and PC/call loads retain IWT and delay slots.
HIB and LOB zero-extend a byte, but their sign flag comes from bit 7, not bit
15. Signed high-byte shifts therefore explicitly sign-extend before ASR.
Tests independently check these flags against Mesen, not just the value bits.

Registers are not an interchangeable ARM-like pool:

| Register | Current backend/hardware responsibility |
| --- | --- |
| R0 | Expression/result and selected RAM address |
| R1 / R2 | Plot X / Y; otherwise arithmetic scratch; division preserves them inside plot |
| R3 | Checks, addresses, scalar scratch and frame adjustments |
| R4 | Far-pointer bank, multiply low result, division/check scratch |
| R5 / R7 / R8 | O1/O2 scalar allocation pool; caller-live values are saved |
| R6 | Multiply operand, scratch and fail-stop category; not allocated |
| R7 / R8 hardware role | MERGE inputs; current operations do not emit MERGE, and future selection must reserve/model them |
| R9 / R10 / R11 | Frame pointer / stack pointer / LINK return address |
| R12 / R13 | Hardware LOOP counter / loop address; not allocated |
| R14 | ROM-buffer address; not allocated |
| R15 | PC, byte-prefetch/delay-slot behavior |

PLOT physically advances R1 exactly once. No optimization level inserts a second
increment or reloads cursor.x for a pixel-only loop iteration. RPIX leaves the
cursor intact and flushes caches even with a discarded result. Cursor reads
are snapshots, not rematerializable aliases of R1/R2. Calls in plot code
preserve both registers. Graphics state stays explicit in IR.

Direct nonvolatile ROM bytes can use GETC through ROMB/R14/the ROM buffer in
all levels. RAM bytes use RAM loads followed by COLOR; computed ROM expressions
evaluate before COLOR. RAM palettes are never replaced by their original ROM
source. COLOR/GETC high-nibble/freeze-high and POR behavior are unchanged.
See [graphics](gsu-graphics.md).

SBK (`msbk` in DiscoC assembly) writes a **16-bit word** at the last RAM
address used by LOAD/STORE, not an address operand or an address register.
It writes the source low byte at that offset and the high byte at offset XOR 1
using the current RAMBR. ALT1 does not make it a byte store. The backend
therefore never selects it for byte, far-pointer or aggregate stores.
It tracks the address by SSA value, also recognizing fresh plain Address
nodes for the same declaration's stable SymbolId. Distinct pointer loads
are not assumed equivalent. Value materialization happens before checking
the latch: any resulting spill/reload invalidates the proof. Successful prior
word accesses justify reuse of their address checks; no check is removed for
an unproven address. Volatile word accesses can use SBK without eliminating
or reordering the read/write. Calls and CFG/implicit hardware-loop boundaries
start with an unknown latch, and no latch knowledge crosses a RAM bank switch.

O0/O1 hardware LOOP preparation copies the prefetched first-body PC from R15
into R13, not the reverse. O2 uses a relocated explicit backedge destination,
including PHI edge copies. LOOP decrements R12 and executes its delay slot on
both taken and final paths. All levels fence COLOR/CMODE state at the backedge;
O0/O1 keep conservative frame-backed values, while O2 uses full loop liveness.
Execution tests cover counter boundaries including zero/65,536 trips, cursor
wrap, changed color/options across iterations and delay
slots; an independently hand-encoded loop is also calibrated in MesenCE.

## Measured comparison with v0.1.0

The same nine [workloads and inputs](../benchmarks/README.md) were measured
using the published v0.1.0 tools and this O1 implementation. O0 payload hashes
match the release for every workload. Reports retain tool, workload, model,
harness and emulator fingerprints; O1 is a working-tree snapshot, **not a
published v0.2.0 release**.
The measured [instruction report](../benchmarks/results/gsu-O1-selection.json) and
[Mesen report](../benchmarks/results/gsu-O1-selection-mesen.json) are retained with the
suite. Published tools were remeasured with the same updated model/calibration:
[opcode reference](../benchmarks/baselines/v0.1.0-local.json),
[Mesen reference](../benchmarks/baselines/v0.1.0-local-mesen.json).

| Workload | CODE bytes v0.1.0 → O1 | Emulated fast GSU cycles v0.1.0 → O1 | Cycle reduction |
| --- | ---: | ---: | ---: |
| triangle_fill | 1,200 → 383 | 7,359,330 → 1,997,705 | 72.9% |
| horizontal_span | 479 → 219 | 80,905 → 21,720 | 73.2% |
| rom_palette_plot | 856 → 444 | 284,085 → 117,725 | 58.6% |
| ram_palette_plot | 755 → 352 | 239,770 → 63,150 | 73.7% |
| memcpy | 1,016 → 537 | 2,503,620 → 121,700 | 95.1% |
| function_call | 744 → 363 | 124,595 → 51,740 | 58.5% |
| switch_dense | 1,306 → 695 | 31,075 → 13,945 | 55.1% |
| switch_sparse | 1,346 → 757 | 32,170 → 15,210 | 52.7% |
| arithmetic | 1,744 → 513 | 351,129 → 148,044 | 57.8% |

These are whole-program timings including startup, checks, helpers, result
stores and STOP. The fixed profile is NTSC, GSU 100%, CLSR=1, CFGR=0, LoROM
code at `$00:8000`, cold instruction/ROM/pixel caches and a WRAM-running host.
Mesen 2.2.1 supplies opcode-entry timestamps; guarded STOP completion cost is
recorded separately. See [the timing contract](../benchmarks/README.md#optional-mesen-timing).
Independent register/RAM/framebuffer checks pass before reports are written.
The triangle still draws 9,409 pixels and flushes; ROM palettes still use
GETC while RAM palettes use COLOR. Triangle stack accesses drop from 138,640
to 39,294; memcpy executed opcodes drop from 322,827 to 20,557.

### Isolated SBK comparison within O1

The pre-SBK compiler binaries were preserved before implementation and
remeasured with the same current model, Mesen binary, timing harness and
inputs as the new compiler. The retained
[pre-SBK opcode report](../benchmarks/results/gsu-O1.json) and
[pre-SBK Mesen report](../benchmarks/results/gsu-O1-mesen.json) isolate this
selection change from the earlier O0-to-O1 improvements.

| Workload | CODE bytes before → after SBK | Emulated fast GSU cycles before → after | Cycle reduction |
| --- | ---: | ---: | ---: |
| triangle_fill | 499 → 483 | 2,379,425 → 2,185,425 | 8.2% |
| horizontal_span | 229 → 225 | 26,860 → 24,300 | 9.5% |
| rom_palette_plot | 480 → 476 | 126,065 → 123,505 | 2.0% |
| ram_palette_plot | 378 → 374 | 73,430 → 70,870 | 3.5% |
| memcpy | 562 → 558 | 136,420 → 133,860 | 1.9% |
| function_call | 379 → 375 | 56,240 → 54,960 | 2.3% |
| switch_dense | 756 → 752 | 15,060 → 14,880 | 1.2% |
| switch_sparse | 812 → 808 | 16,210 → 16,030 | 1.1% |
| arithmetic | 865 → 861 | 249,794 → 249,154 | 0.3% |

Triangle execution drops from 429,181 to 390,381 opcodes (38,800 fewer).
Stack access and logical load/store counts are unchanged in all nine cases:
SBK saves address preparation, not the memory write itself. Its one-byte
encoding is not inherently faster than a one-byte STW with an already-ready
address register. The savings here come from omitted address recomputation
and checks for the already-validated destination. Independent word and
LDB/ALT1/odd-address SBK microprograms also pass Mesen calibration.

These results are not physical-hardware timings, FPS, warmed-cache performance
or arbitrary-placement speedups. Remeasure both versions if the profile,
emulator or harness changes. Opcode reductions alone do not prove cycle savings.

### Local O1 improvements after SBK

The preceding O1 compiler executable was preserved and remeasured with the
current instruction model, Mesen calibration and fixed inputs. The
[before opcode report](../benchmarks/results/gsu-O1-before-local.json) and
[before Mesen report](../benchmarks/results/gsu-O1-before-local-mesen.json)
can be compared directly with the new `gsu-O1-local` reports. No workload or
algorithm was rewritten to obtain these results.

| Workload | CODE bytes O1 + SBK → local snapshot | Fast GSU cycles SBK → local | Cycle reduction |
| --- | ---: | ---: | ---: |
| triangle_fill | 483 → 388 | 2,185,425 → 2,000,130 | 8.5% |
| horizontal_span | 225 → 219 | 24,300 → 21,720 | 10.6% |
| rom_palette_plot | 476 → 444 | 123,505 → 117,725 | 4.7% |
| ram_palette_plot | 374 → 352 | 70,870 → 63,150 | 10.9% |
| memcpy | 558 → 537 | 133,860 → 121,700 | 9.1% |
| function_call | 375 → 365 | 54,960 → 52,380 | 4.7% |
| switch_dense | 752 → 699 | 14,880 → 14,125 | 5.1% |
| switch_sparse | 808 → 761 | 16,030 → 15,390 | 4.0% |
| arithmetic | 861 → 521 | 249,154 → 149,324 | 40.1% |

Arithmetic stack accesses fall from 872 to 456, memcpy from 1,292 to 1,164,
and triangle_fill from 39,488 to 39,294. Logical memory/hardware effects and
independent results still pass; not every omitted copy eliminates a RAM access.

The separate rotating checkerboard demo was measured at `$70:6000`, with
CLSR=1, GSU=100%, CFGR.MSO=0, two source-requested CACHE instructions and cold
instruction-cache state on each entry. This RAM-execution profile is **not**
the nine-workload LoROM profile above. The interval includes startup, clearing,
rotation/clipping, drawing, flush and STOP; it excludes CPU setup/copy, DMA,
display and the initial synthetic pipeline prefetch.

| Rotation mode | Payload bytes | Phase 0 cycles | Mean cycles over 64 poses |
| --- | ---: | ---: | ---: |
| O0, unchanged | 11,143 | 16,966,000 | 17,497,791.375 |
| Previous O1 + SBK | 5,155 | 3,597,021 | 4,131,027.03125 |
| O1 local snapshot | 4,748 | 2,918,335 | 3,362,905.4375 |

This is 407 fewer payload bytes (7.9%) and 18.6% fewer mean cycles than the
preceding O1. All ROMs remain 65,536 bytes because of container padding.
The independent SNES verifier checks all 49,152 pixels for each pose, RAM/VRAM,
black/white counts, cursor/stack/bank/CACHE/STOP and VBlank DMA. All 64 screenshot
hashes match the previous O1; phase 0's repeat also matches its timing exactly.
The [measurement summary](../benchmarks/results/rotation-O1-local-summary.json)
links the retained raw 65-sample runs (64 distinct poses plus wrap).
These are emulated GSU clock ticks, not an FPS or physical-hardware claim.

### Allocated ALU, local CSE/DSE and arithmetic selection

The `gsu-O1-local` tools were preserved before these five changes. The new
`gsu-O1-selection` reports use the same workloads, model, timing harness and
Mesen binary. A strict comparison rejects changed fingerprints and regressions.

| Workload | CODE bytes local → selection | Fast GSU cycles local → selection |
| --- | ---: | ---: |
| triangle_fill | 388 → 383 | 2,000,130 → 1,997,705 |
| horizontal_span | 219 → 219 | 21,720 → 21,720 |
| rom_palette_plot | 444 → 444 | 117,725 → 117,725 |
| ram_palette_plot | 352 → 352 | 63,150 → 63,150 |
| memcpy | 537 → 537 | 121,700 → 121,700 |
| function_call | 365 → 363 | 52,380 → 51,740 |
| switch_dense | 699 → 695 | 14,125 → 13,945 |
| switch_sparse | 761 → 757 | 15,390 → 15,210 |
| arithmetic | 521 → 513 | 149,324 → 148,044 |

All nine independent results pass. Five cases improve and four remain equal;
stack accesses are unchanged in this suite. Fewer selector/copy instructions
need not eliminate a memory access, and these workloads do not exercise every
new constant-divisor pattern.

The unchanged rotating checkerboard, using the separate cache-enabled
21 MHz RAM profile described above, now measures:

| Rotation snapshot | Payload bytes | Phase 0 cycles | Mean cycles over 64 poses |
| --- | ---: | ---: | ---: |
| O1 local | 4,748 | 2,918,335 | 3,362,905.4375 |
| O1 selection | 4,721 | 2,853,506 | 3,300,426.375 |

That is 27 fewer payload bytes (0.6%) and 1.9% fewer mean cycles. The padded
ROM remains 65,536 bytes. All 64 full-frame pixel checks pass, all 64 screenshot
hashes match the local snapshot, and phase 0's repeat is identical. The
[selection rotation summary](../benchmarks/results/rotation-O1-selection-summary.json)
retains provenance and raw 65-sample evidence. No source algorithm, host clock
profile, CACHE request or safety check was changed to obtain these gains.

### Global O2: SSA, allocation and loops

The frozen O1 selection tools and the final O2 tools generate the same workload
sources in the same LoROM profile. The retained
[opcode/code report](../benchmarks/results/gsu-O2.json) and
[Mesen report](../benchmarks/results/gsu-O2-mesen.json) record tool hashes and
the matching workload/model/timing fingerprints. All nine independent result
checks pass; direct, compiler-assembly and final-assembly paths are covered by
the regression suite. O0 and O1 were separately rebuilt with both toolchains:
all nine payload hashes and functional counters remain identical at each level.

| Workload | CODE bytes O1 → O2 | Fast GSU cycles O1 → O2 | Cycle change |
| --- | ---: | ---: | ---: |
| triangle_fill | 383 → 395 | 1,997,705 → 872,075 | -56.35% |
| horizontal_span | 219 → 168 | 21,720 → 11,895 | -45.23% |
| rom_palette_plot | 444 → 388 | 117,725 → 104,700 | -11.06% |
| ram_palette_plot | 352 → 296 | 63,150 → 50,125 | -20.63% |
| memcpy | 537 → 534 | 121,700 → 119,710 | -1.64% |
| function_call | 363 → 325 | 51,740 → 46,450 | -10.22% |
| switch_dense | 695 → 558 | 13,945 → 11,225 | -19.51% |
| switch_sparse | 757 → 620 | 15,210 → 12,470 | -18.01% |
| arithmetic | 513 → 489 | 148,044 → 148,809 | +0.52% |

Triangle stack accesses fall from 39,294 to 1,367, the horizontal span from
392 to 2, and each palette span from 520 to 2. These counts include the same
startup, loop control and checked memory effects. The triangle uses 12 more
CODE bytes despite the large cycle reduction. Arithmetic saves 24 CODE bytes
and 102 stack accesses but loses 765 cycles: fewer instructions or spills do
not guarantee faster GSU execution. These tradeoffs remain visible rather than
being excluded from the comparison.

The separate unchanged checkerboard rotation was rebuilt with O2 at 21 MHz
in the RAM/CACHE profile described above:

| Rotation snapshot | Payload bytes | Phase 0 cycles | Mean cycles over 64 poses |
| --- | ---: | ---: | ---: |
| O1 selection | 4,721 | 2,853,506 | 3,300,426.375 |
| O2 | 4,766 | 2,813,404 | 3,279,943.078125 |

O2 adds 45 payload bytes (0.95%) and reduces mean cycles by 0.62%. Both padded
ROMs remain 65,536 bytes. The independent verifier checks all 49,152 pixels
per pose; all 64 screenshot hashes match O1, and phase 0's wrap sample repeats
its timing. The [O2 rotation summary](../benchmarks/results/rotation-O2-summary.json)
retains the raw 65 samples and provenance. This is emulated timing, not an FPS
or physical-hardware result, and is not interchangeable with the LoROM suite.

```sh
cmake --build --preset release --target benchmarks
cmake --build --preset release --target benchmarks-O1
cmake --build --preset release --target benchmarks-O2
# Configure external WLA-DX/Mesen tools first:
cmake --build --preset release --target benchmarks-mesen
cmake --build --preset release --target benchmarks-mesen-O1
cmake --build --preset release --target benchmarks-mesen-O2
```

## Pipeline and CACHE scheduling at O2

The post-allocation `GSUMachineScheduler` decodes complete physical instructions,
not operand bytes mistaken for opcodes. It remaps CODE symbols, relocation
patches and relative branch destinations together, after branch relaxation.
Its optional per-byte analysis is limited to one 64 KiB program bank.

- Fill eligible NOP slots with a one-byte INC/DEC, keeping branch register and
  flag dependencies intact. N/Z-dependent conditions cannot move INC/DEC.
  `LINK #4` retains its return address when the final stack adjustment moves
  into the call slot. JMP's address register cannot be modified there.
- BRA/Bcc preserve selectors: a register copy may leave WITH before the branch
  and complete TO in its slot. WITH/FROM completes MOVES only before BRA or a
  carry branch, because MOVES writes N/Z/V. One-byte arithmetic, shifts, byte
  extraction, multiplication and logical instructions may similarly retain
  selected registers and ALT across a branch whose tested NZCV bits they do
  not modify. COLOR/CMODE and PLOT preserve NZCV and may occupy conditional
  slots when moved from the predecessor: they still execute on both paths.
  These rules do **not** apply to transfers that reset selectors. A newly
  filled slot is never scheduled again.
- A private, uniquely reached taken-edge WITH/TO/FROM selector may replace a
  NOP when the untaken path immediately loads a literal fault code, STOPs and
  executes no selector-dependent operation first. The literal resets the
  speculative selector on that path. Labels are remapped to the continuation
  after the removed target prefix. Public aliases, relocation targets, multiple incoming branches,
  live ALT state and unknown fallthrough effects reject this transformation.
  This is not a license to move arbitrary target instructions into a slot.
- A PLOT may occupy LOOP's taken/final slot when it preserves R12/R13 and LOOP's
  flags. It still draws once per iteration and performs the only X increment.
  RPIX/flush, bank changes, loads/stores and potentially failing operations are
  never used as speculative slot fillers.
- Move independent fault-free register work before plain GETC or TO/GETB, after
  R14 has started the read. Keep GETB's complete destination prefix and result
  dependencies. Pure span-counter work can also move across the following PLOT,
  but neither a read nor a write of R1/R2 can cross that cursor effect. Memory,
  volatile accesses, entry labels, relocations and bank switches fence the proof.
- Keep natural loop blocks and PHI backedge copies contiguous before allocation;
  remap block/PHI/hardware-loop IDs and verify the result. Skip overlapping
  non-nested regions and optional layout analysis above 32 natural loops.
- Align function-entry CACHE's **next prefetched byte** to a 16-byte cache line.
  Padding is outside the entry symbol, so calls skip it. A private version-7
  layout hint aligns each object at its final linker origin, accounting for
  startup and preceding objects. Assembly round trips retain it. Decline padding
  if it would invalidate a relaxed short branch or exceed a program bank.

This follows MesenCE's [byte-prefetch implementation](https://github.com/nesdev-org/MesenCE/blob/master/Core/SNES/Coprocessors/GSU/Gsu.cpp)
and [instruction semantics](https://github.com/nesdev-org/MesenCE/blob/master/Core/SNES/Coprocessors/GSU/Gsu.Instructions.cpp).
A taken transfer retains only the old-flow slot opcode. If it is IWT or IBT,
subsequent operand bytes come from the destination. Independent model and Mesen
calibrations exercise that split form, but the compiler does **not** generate
it automatically: coordinated target data/layout and untaken paths need their
own implementation. Ordinary IWT/IBT still consume all their immediate bytes.

Alignment is not universally profitable. Padding every loop CACHE was tested
and rejected: the unchanged rotation averaged 3,304,528.09375 cycles, 0.75% more
than the previous O2 snapshot. Those NOPs executed on scanline entry. Local/loop
CACHE directives therefore retain their source position; hot-block layout,
not unconditional padding, handles their region.

### Pipeline measurements

The frozen pre-scheduler O2 executables were rerun with the current Mesen harness
so both sides have identical calibration/timing fingerprints. The
[remeasured baseline](../benchmarks/results/gsu-O2-before-pipeline-mesen.json),
[scheduled timing report](../benchmarks/results/gsu-O2-pipeline-mesen.json) and
[functional counters](../benchmarks/results/gsu-O2-pipeline.json) retain their
executable hashes. The earlier tables remain historical snapshots, not current
compiler output.

| Workload | CODE bytes before → after | Fast GSU cycles before → after |
| --- | ---: | ---: |
| triangle_fill | 395 → 393 | 872,075 → 870,625 |
| horizontal_span | 168 → 168 | 11,895 → 11,895 |
| rom_palette_plot | 388 → 388 | 104,700 → 104,700 |
| ram_palette_plot | 296 → 296 | 50,125 → 50,125 |
| memcpy | 534 → 533 | 119,710 → 119,705 |
| function_call | 325 → 324 | 46,450 → 46,130 |
| switch_dense | 558 → 548 | 11,225 → 11,135 |
| switch_sparse | 620 → 610 | 12,470 → 12,380 |
| arithmetic | 489 → 489 | 148,809 → 148,809 |

All nine results and final memories pass the independent checks. O0/O1 were
also compiled with the frozen and current toolchains: all nine payload hashes
and functional counters are unchanged at each level. The new scheduling is O2
only. A separate cache-resident GETB calibration moves independent IWT work
into the ROM wait: both programs return 149 with eight executed instructions,
but Mesen timing falls from 99 to 96 fast GSU cycles. This does not promise an
equivalent benefit for uncached ROM instruction fetches; the ROM palette suite
above is unchanged.

The unchanged cache-enabled rotation was separately measured at 21 MHz:

| Rotation snapshot | Payload bytes | Phase 0 cycles | Mean cycles over 64 poses |
| --- | ---: | ---: | ---: |
| O2 before scheduling | 4,766 | 2,813,404 | 3,279,943.078125 |
| O2 pipeline | 4,737 | 2,812,980 | 3,283,993.96875 |

It saves 29 bytes (0.61%), but the mean rises 0.12% even though phase 0 improves.
All 49,152 pixels per pose pass the independent verifier, all 64 screenshot
hashes match the frozen O2 output, and the wrap sample repeats phase 0. The
[pipeline rotation summary](../benchmarks/results/rotation-O2-pipeline-summary.json)
records raw samples and provenance. Layout/cache effects can outweigh fewer
instructions, so this is a code-size improvement, not a universal speedup.
The padded ROM remains 65,536 bytes. These are emulator measurements, not
physical-hardware timings or FPS.

## SCCP and CFG measurements

The frozen pipeline-O2 tools and the SCCP-O2 tools compile the same nine sources
with matching functional/timing fingerprints. The retained
[functional report](../benchmarks/results/gsu-O2-sccp.json) and
[Mesen report](../benchmarks/results/gsu-O2-sccp-mesen.json) record executable
provenance. The workload algorithm, inputs, safety checks and host profile are
unchanged. All nine independent result/memory checks pass.

| Workload | CODE bytes pipeline → SCCP | Fast GSU cycles pipeline → SCCP |
| --- | ---: | ---: |
| triangle_fill | 393 → 390 | 870,625 → 822,610 |
| horizontal_span | 168 → 167 | 11,895 → 11,255 |
| rom_palette_plot | 388 → 388 | 104,700 → 104,700 |
| ram_palette_plot | 296 → 295 | 50,125 → 49,485 |
| memcpy | 533 → 533 | 119,705 → 119,705 |
| function_call | 324 → 323 | 46,130 → 45,810 |
| switch_dense | 548 → 548 | 11,135 → 11,135 |
| switch_sparse | 610 → 610 | 12,380 → 12,380 |
| arithmetic | 489 → 489 | 148,809 → 148,809 |

Triangle instructions fall from 146,814 to 137,405 and emulated cycles by
5.51%; the span improves by 5.38%. Four cases improve and five remain equal
in bytes/cycles. Loads, stores, stack accesses and graphics-op counts stay
unchanged in all nine: this pass simplifies SSA/control flow, not unknown
memory. The switch inputs are runtime values, so SCCP does not turn them into
constant dispatches. O0/O1 were separately compiled with both toolchains; all
nine payload hashes and functional counters remain identical at each level.

The unchanged rotating checkerboard was rebuilt in its separate 21 MHz
RAM/CACHE profile, preserving its source, clock, CACHE requests and host:

| Rotation snapshot | Payload bytes | Phase 0 cycles | Mean cycles over 64 poses |
| --- | ---: | ---: | ---: |
| O2 pipeline | 4,737 | 2,812,980 | 3,283,993.96875 |
| O2 SCCP | 4,748 | 2,805,585 | 3,276,385.125 |

It trades 11 more payload bytes (0.23%) for 0.23% fewer mean cycles. The ROM
remains 65,536 bytes. All 49,152 pixels per pose pass the independent verifier,
all 64 screenshot hashes match the pipeline snapshot, and the wrap sample
repeats phase 0. The [rotation summary](../benchmarks/results/rotation-O2-sccp-summary.json)
retains provenance and all 65 raw samples. These are emulator measurements,
not physical-hardware timing or FPS; do not combine this profile with the
LoROM suite above.

## Proven-check elimination measurements

The frozen SCCP-O2 tools and this O2 snapshot compile unchanged sources under
matching profiles. The [functional report](../benchmarks/results/gsu-O2-guards.json)
and [Mesen report](../benchmarks/results/gsu-O2-guards-mesen.json) retain their
fingerprints. All nine results pass; loads, stores, stack accesses and graphics
operation counts are unchanged. Only checks justified by the proofs above are
omitted. The O0/O1 payload hashes and all functional counters remain identical
to the frozen tools across all nine workloads.

| Workload | CODE bytes SCCP → proven checks | Fast GSU cycles SCCP → proven checks |
| --- | ---: | ---: |
| triangle_fill | 390 → 340 | 822,610 → 822,440 |
| horizontal_span | 167 → 117 | 11,255 → 11,085 |
| rom_palette_plot | 388 → 338 | 104,700 → 104,530 |
| ram_palette_plot | 295 → 245 | 49,485 → 49,315 |
| memcpy | 533 → 457 | 119,705 → 119,445 |
| function_call | 323 → 223 | 45,810 → 39,800 |
| switch_dense | 548 → 460 | 11,135 → 9,795 |
| switch_sparse | 610 → 510 | 12,380 → 10,680 |
| arithmetic | 489 → 391 | 148,809 → 148,479 |

The unchanged rotating checkerboard uses the separate 21 MHz RAM/CACHE
profile described above, not the suite's cold LoROM profile:

| Rotation snapshot | Payload bytes | Phase 0 cycles | Mean cycles over 64 poses |
| --- | ---: | ---: | ---: |
| O2 SCCP | 4,748 | 2,805,585 | 3,276,385.125 |
| O2 proven checks | 3,453 | 2,697,248 | 3,158,312.75 |

That saves 1,295 payload bytes (27.3%) and 3.6% of mean emulated cycles.
The functions shrink from 1,232/504/3,000 to 482/478/2,481 CODE bytes
(`sine_q6`/`floor_div`/`main`); startup remains 12 bytes. The largest saving
comes from proving the sine table's initialization and masked/PHI indices
inside an already checked frame. Unknown edge/scanline indices still check.
All 49,152 pixels per pose pass, all 64 screenshots match the SCCP snapshot,
and phase wrap repeats phase 0's result/timing. The padded ROM is still
65,536 bytes. The [rotation summary](../benchmarks/results/rotation-O2-guards-summary.json)
links all 65 samples and provenance. These are emulator measurements, not FPS
or physical-hardware timing. Smaller layout also changes CACHE behavior;
bytes saved do not translate into a proportional cycle reduction.

## Divmod and shared-code measurements

The [functional report](../benchmarks/results/gsu-O2-size.json) and
[Mesen report](../benchmarks/results/gsu-O2-size-mesen.json) compare unchanged
sources with the frozen proven-check tools above. Every workload shrinks.
Eight keep identical instruction/memory/graphics counts and timing; their
savings are mainly unexecuted fail-stop code. `arithmetic` fuses its quotient
and remainder, reducing executed helpers and stack traffic. O0/O1 payload
hashes and all functional counters remain identical across all nine workloads.

| Workload | CODE bytes proven checks → shared code | Fast GSU cycles proven checks → shared code |
| --- | ---: | ---: |
| triangle_fill | 340 → 336 | 822,440 → 822,440 |
| horizontal_span | 117 → 113 | 11,085 → 11,085 |
| rom_palette_plot | 338 → 294 | 104,530 → 104,530 |
| ram_palette_plot | 245 → 225 | 49,315 → 49,315 |
| memcpy | 457 → 405 | 119,445 → 119,445 |
| function_call | 223 → 207 | 39,800 → 39,800 |
| switch_dense | 460 → 436 | 9,795 → 9,795 |
| switch_sparse | 510 → 474 | 10,680 → 10,680 |
| arithmetic | 391 → 296 | 148,479 → 81,229 |

For `arithmetic`, stack loads fall from 128 to 64 and stack stores from 226 to
99; executed opcodes fall from 25,528 to 14,060. Graphics operation counts stay
unchanged throughout. These are whole-workload improvements, not isolated
claims about one instruction.

The unchanged rotating checkerboard keeps its separate 21 MHz RAM/CACHE profile:

| Rotation snapshot | Payload bytes | Phase 0 cycles | Mean cycles over 64 poses |
| --- | ---: | ---: | ---: |
| O2 proven checks | 3,453 | 2,697,248 | 3,158,312.75 |
| O2 divmod/shared code | 3,126 | 2,659,799 | 3,045,929.8125 |

That saves 327 bytes (9.47%) and 3.56% of mean emulated cycles. Function sizes
become 473/324/2,317 bytes (`sine_q6`/`floor_div`/`main`), plus 12 startup bytes.
`floor_div` now uses a single division helper and a conditional remainder
projection. All 49,152 pixels per pose pass the verifier; all 64 screenshot
hashes match the preceding snapshot, and the phase-wrap sample repeats phase 0.
The padded ROM remains 65,536 bytes. The
[rotation summary](../benchmarks/results/rotation-O2-size-summary.json)
retains all 65 samples and provenance. CACHE/layout changes contribute to the
timing; these are emulator measurements, not FPS or physical-hardware timing.

## Scalar-value and hot-lifetime measurements

The [focused suite](../benchmarks/optimizer/README.md) remeasures the frozen
pre-change O2 tools and current tools using identical sources, inputs and the
same independent instruction runner. Direct and both assembly paths must
match byte for byte, and checked result/stack/volatile state must pass first.
Retained reports: [before](../benchmarks/results/gsu-O2-values-before.json),
[after](../benchmarks/results/gsu-O2-values-after.json).

| Workload | Payload bytes before → after | Executed opcodes before → after | Stack loads/stores before → after |
| --- | ---: | ---: | --- |
| gvn | 239 → 152 | 148 → 105 | 1/4 → 0/1 |
| recurrence | 185 → 193 | 2,230 → 1,494 | 64/66 → 64/35 |
| scalar_cells | 350 → 236 | 241 → 167 | 5/6 → 3/6 |
| known_bits | 132 → 96 | 88 → 66 | 0/1 → 0/1 |
| hot_lifetimes | 734 → 734 | 8,149 → 8,128 | 529/305 → 530/297 |

These are executed **opcodes, not cycles**. Recurrence costs eight extra bytes
for 33.0% fewer opcodes; lifetime partitioning saves seven net stack accesses,
not every reload. The original nine workloads remain byte/counter-identical
to the preceding O2 snapshot; all nine O0/O1 controls also remain identical.

The unchanged rotating checkerboard was measured separately with the same
21 MHz NTSC RAM/CACHE host and the public boundary/FPS probes:

| Rotation snapshot | Payload bytes | Phase 0 cycles | Mean GSU cycles, 64 poses | Completed-pose FPS |
| --- | ---: | ---: | ---: | ---: |
| Previous O2 | 3,126 | 2,659,799 | 3,045,929.8125 | 6.700912 |
| Scalar-value O2 | 3,114 | 2,658,089 | 3,044,338.3125 | 6.700912 |

That is 12 fewer payload bytes (0.384%) and 0.052% fewer mean emulated GSU
cycles, not a meaningful FPS improvement. VBlank publication still rounds
both versions to the same refresh intervals. All 64 captures match exactly,
the 65th sample confirms phase wrap, and the red failure ROM also passes.
The [summary](../benchmarks/results/rotation-O2-values-summary.json) records
provenance and raw cycle/FPS reports; the padded SNES ROM remains 65,536 bytes.
Initial outer-loop recurrences increased pressure/code size, so they were
rejected in favor of the conservative inner-loop scope. Complex dynamic-index
unrolling and a general split-location allocator remain future work.

## Profiling, cost model and allocation

The optional Mesen profiler identifies expensive **executed** code before
changing allocator heuristics. `PROFILE_GSU=ON` adds `profile.json` to each
benchmark directory; `benchmarks-profile-O2` supplies this option. Labels and
function ownership come from the byte-exact linked assembly, after scheduling
and relocation. It reports instruction counts and emulated master-clock
intervals by function, machine-label region, opcode category and PC. Regions
are **not** original IR basic blocks. Prefixes/delay opcodes count separately;
immediate operand bytes are fetch cost, not additional executed instructions.

The interval between opcode-entry hooks is attributed to the preceding opcode,
including prefetch, cache fills and waits. These are observed intervals, **not
isolated ISA latencies**. The calibrated STOP fetch completes the final interval;
host idle/polling is excluded. Every partition sums to the independent boundary
timing and functional opcode count. The rotating demo uses region-only timing
for its first pose: whole-emulator state is sampled only at region transitions,
and the observer is then removed. That mode deliberately omits per-opcode
timing rather than inventing it. All remaining image/FPS checks still execute.

`GSUCostModel` is a separate static pressure proxy:

- Fetch is byte-oriented: warm fast CACHE uses one clock per byte, uncached
  ROM/RAM five; a cold 16-byte line adds 80 fetch clocks in the fixed fast
  profile. Cache misses are measured, not predicted by allocation scores.
- IBT costs two bytes only when sign-extending its operand reproduces the
  required word (`0..127` or `$FF80..$FFFF`); otherwise use three-byte IWT.
  Unsigned `$0080` must not be mistaken for IBT's `$FF80`.
- WITH/TO copies cost two fetch bytes. Spills include displacement
  materialization and RAM transfer; a far pair transfers four bytes, not two.
  Caller-save push/pop costs are charged when a value is live through a call.
- `fetch_bytes * fetch_cost + ram_bytes * 5`, weighted by capped loop depth,
  ranks candidates. Buffered RAM operations overlap other work, so this is
  neither an exact cycle count nor a timing guarantee. Arithmetic saturates;
  candidate search and point-liveness work are bounded.

These fetch/ISA assumptions follow Mesen's
[GSU fetch/buffer implementation](https://github.com/SourMesen/Mesen2/blob/master/Core/SNES/Coprocessors/GSU/Gsu.cpp)
and [instruction implementation](https://github.com/SourMesen/Mesen2/blob/master/Core/SNES/Coprocessors/GSU/Gsu.Instructions.cpp).
The allocator compares its previous deterministic coloring with one cost-ranked
candidate, accepting only a lower warm score without a higher uncached score.
Hot/cold IR lifetime partitioning uses this same proxy instead of fixed generic
load/store weights. Static loop weights are **not** recorded execution counts;
there is no profile-input/PGO mode or globally optimal allocation claim.

The backend may retain a private spilled scalar's bits in a temporarily free
R5/R7/R8 register. Exact CFG point liveness includes operands, destinations,
PHI edge uses and hardware-loop backedges. Copies do not borrow R1/R2 or any
other special GSU register, and do not change the authoritative frame location.
Stores remain valid across edges/calls. Copies are revoked at register writes,
CFG/helper labels, calls and bank changes; only intrablock repeated consumers
can avoid reloads. This caches an already evaluated SSA snapshot, **not a fresh
RAM/ROM load**, and never merges volatile accesses. General split-location
allocation, spill-store removal and cross-edge spill-copy caching remain future
work. O0/O1 generation, ABI, object format and graphics semantics are unchanged.

The unchanged cache-enabled rotating triangle was checked against frozen
pre-cost O2 tools, not against v0.1.0 or a newly published release:

| Rotation snapshot | Payload bytes | Phase 0 cycles | Mean GSU cycles, 64 poses | Completed-pose FPS |
| --- | ---: | ---: | ---: | ---: |
| Pre-cost O2 | 3,114 | 2,658,089 | 3,044,338.3125 | 6.700912 |
| Cost-aware allocation | 3,008 | 2,562,178 | 2,914,672.734375 | 6.700913 |

This saves 106 payload bytes (3.40%) and 4.26% mean emulated GSU cycles.
All 64 captured images match, the wrap sample passes, and both red failure
ROMs pass. The padded ROM remains 65,536 bytes. Completed-pose FPS is
effectively unchanged: both use the same number of VBlank refresh intervals.
The first-pose profile locates the savings in `main`: 2,025,759 → 1,929,848
clocks; `floor_div` remains 627,320. It does not justify claiming a faster
division helper or a general FPS gain. The
[rotation summary](../benchmarks/results/rotation-O2-cost-summary.json) retains
timing/FPS evidence and fingerprints.

`triangle_fill` improves from 336 to 334 CODE bytes and 822,440 to 822,430
emulated cycles; the other eight original workloads keep their size/timing.
It removes 98 stack loads but adds 96 executed opcodes (137,381 → 137,477):
less RAM traffic does not automatically mean a meaningful speedup. The
[before](../benchmarks/results/gsu-O2-cost-before-mesen.json)/
[after](../benchmarks/results/gsu-O2-cost-after-mesen.json) reports preserve this
tradeoff and tool/model/harness fingerprints. Full PC profiles stay in local
build output instead of being duplicated in the retained timing reports.
The focused `hot_lifetimes` case changes from 734 to 715 payload bytes,
8,128 to 8,112 executed opcodes and 530/297 to 529/297 stack loads/stores.
Those microbenchmark counts are **not cycles**. All nine O0/O1 payload hashes
and counters remain identical to the frozen pre-cost controls. Profiling,
regressions and all compiler/assembler/final-assembly paths remain enabled.
The focused [before](../benchmarks/results/gsu-O2-cost-values-before.json)/
[after](../benchmarks/results/gsu-O2-cost-values-after.json) reports retain the
five programs' results, traffic and byte-round-trip evidence.

## Typed compaction, LOOP and automatic CACHE

O2/Os recognize repetition in verified typed IR, not by rewriting similar-looking
assembly text. This adds four related policies without changing language syntax,
the ABI, address spaces or the object format:

- **Keep eligible values in registers.** Nonvolatile, nonescaping near-pointer
  locals can become SSA values/PHIs. Their incoming types must agree, including
  pointer layers, qualifiers and address spaces. Far pairs and unknown aliases
  retain storage. Pointer-parameter loads also validate representations: keep
  their first entry-block load at its original effect position, or decline
  promotion. A failed pointer check cannot move before a volatile witness or
  into an untaken branch. Proven RAM accesses use allocated address/value
  registers directly; scalar constants can materialize in their destination.
- **Compact local initialization.** A bounded `memory.initialize declare` groups
  4–256 ordered constant byte/word/bool stores to one proven near-RAM array.
  Calculate its base once and advance the address between stores. Equal adjacent
  values reuse their materialization. Uniform runs of at least 32 elements use
  a saved/restored R12/R13 LOOP. No ROM table is substituted, and volatile stores,
  unknown pointer checks, intervening effects and out-of-object spans do not
  qualify. Initializer discovery has a one-million-step proof budget per function.
- **Reroll suitable ordinary loops.** Convert straight-line, single-entry/exit
  natural loops with an exact increasing word induction and 8–65,535 constant
  trips into the existing scoped hardware-loop IR. Preserve R12/R13; remove the
  induction only when the body does not use it. Os can instead retain a proven
  software countdown when the induction is unobserved and no other PHI is
  carried, avoiding R12/R13 scope overhead. Calls, nested hardware scopes,
  early exits, exit PHIs and loop-produced SSA values used outside remain
  conservative. Discovery is limited to 256 blocks/4,096 values, 16 conversions
  and a bounded proof budget. This is not arbitrary machine-code outlining.
- **Select CACHE after emission.** For generated loops, examine actual emitted
  hot-block extents and require at most 496 bytes, reserving 15 bytes for final
  placement alignment within the 512-byte window. A conservative cold-fill
  proxy must predict a fetch saving. Ordinary constant-trip word loops can
  reuse an existing entry/backedge delay-slot NOP; scoped hardware loops use
  their actual natural header, including split PHI-copy edges. If no probe
  wins, re-emit without optional probe bytes. Uniform initializer loops have
  a separately known small body. Function `@cache` and enclosing explicit
  loop windows suppress automatic rebasing inside their scope; a disjoint
  unhinted loop may use its own window. Calls, division kernels and nested
  hardware scopes conservatively fence this selection. O0/O1 do not gain
  automatic CACHE.
- **Avoid redundant explicit loop CACHE.** When the header begins with CACHE
  and no body operation can rebase it, internal backedges target a private
  label immediately after that opcode. Its physical byte and original CBR
  anchor remain unchanged; external entries still execute CACHE. PHI copies
  and R12/R13 save/restore are not skipped. Loops with calls, another CACHE,
  division, initialization kernels or nested hardware scopes retain the
  original repeated behavior. This is an entry-only execution optimization,
  not arbitrary motion of CACHE into a differently aligned preheader.

CACHE fills 16-byte lines on demand, not the entire window at once. Thus the
first iteration pays cold line fills, and already fetched lines can be reused;
there is no unconditional "first iteration uncached, second iteration cached"
rule. RAM instruction fetch can wait for pending RAM operations. The selector's
fetch-cost proxy is not a cycle oracle: use the calibrated Mesen reports to
measure the complete instruction/memory/pipeline behavior. See the existing
[Mesen GSU implementation](https://github.com/nesdev-org/MesenCE/blob/master/Core/SNES/Coprocessors/GSU/Gsu.cpp).

### Measured compaction tradeoffs

This historical snapshot uses frozen pre-compaction and post-compaction tools
to compile unchanged workloads with the
same model/harness fingerprints. The nine-workload suite executes from LoROM,
not RAM. O2 measurements include speed/size tradeoffs:

| Workload | Payload bytes before → after | Fast GSU cycles before → after |
| --- | ---: | ---: |
| triangle_fill | 334 → 329 | 822,430 → 822,405 |
| horizontal_span | 113 → 135 | 11,085 → 1,488 |
| rom_palette_plot | 294 → 340 | 104,530 → 98,940 |
| ram_palette_plot | 225 → 269 | 49,315 → 45,015 |
| memcpy | 405 → 400 | 119,445 → 119,420 |
| function_call | 207 → 202 | 39,800 → 39,775 |
| switch_dense | 436 → 434 | 9,795 → 9,785 |
| switch_sparse | 474 → 472 | 10,680 → 10,670 |
| arithmetic | 296 → 291 | 81,229 → 81,204 |

The O2 span trades 22 additional bytes for 86.58% fewer cycles using LOOP/CACHE.
Os instead selects a 111-byte software span (11,075 cycles), not the larger
speed candidate. Its ROM palette is 300 bytes/104,520 cycles versus the frozen
294/104,530: bounded candidate selection in that snapshot does not guarantee beating
every historical compiler output. Retained reports contain all nine cases:
[O2 before](../benchmarks/results/gsu-O2-compaction-before-mesen.json),
[O2 after](../benchmarks/results/gsu-O2-compaction-mesen.json),
[Os before](../benchmarks/results/gsu-Os-compaction-before-mesen.json),
[Os after](../benchmarks/results/gsu-Os-compaction-mesen.json).

The separate 21 MHz RAM/CACHE rotating triangle selects identical final O2/Os
payloads. It is not the nine-workload LoROM timing profile:

| Rotation snapshot | Payload bytes | Phase 0 cycles | Mean cycles, 64 poses |
| --- | ---: | ---: | ---: |
| Pre-compaction | 3,008 | 2,562,178 | 2,914,672.734375 |
| O2/Os compaction | 2,683 | 2,503,800 | 2,854,983.828125 |

That saves 325 payload bytes (10.80%) and 2.05% mean emulated GSU cycles.
All 64 screenshot hashes match the fresh control; phase wrap and deliberate
failure paths pass. The SNES ROM remains 65,536 bytes. Completed-pose throughput
stays approximately 6.70 FPS with the same 574 refresh intervals/510 repeats;
there is no measured FPS gain. The
[rotation summary](../benchmarks/results/rotation-compaction-summary.json)
retains fingerprints and both independent timing/presentation windows.

## Verification and remaining work

Existing O0 tests remain alongside O1 call, pointer/far, numeric, volatile,
global, library and graphics execution cases. Dedicated regressions cover
real register pressure, aliasing, IBT limits, comparison truth tables,
near-pointer scaling endpoints, config precedence and imported-unit builds.
SBK regressions check word read-modify-write/repeated stores, preserved
volatile counts, byte-adjacent sentinels, intervening loads/calls/far-bank
accesses, CFG joins and real spills under register pressure.
Local-IR unit tests verify cast boundaries, stable-ID compaction, idempotence
and memory-proof barriers, including overwritten stores and checked member
addresses. Execution regressions cover all 16 legal shift
counts and extracted bits across nine signed/unsigned boundary inputs, including
bit 9, plus aliasing calls, volatile accesses, continue and preserved fail-stops
for invalid shifts or division/remainder by zero.
The selection regression additionally checks direct allocated destinations and
144 constant-arithmetic results for each of thirteen signed/unsigned runtime
seeds in both modes, including negative power-of-two divisors and plot cursor
preservation. Signed masked bit 15 is distinguished from canonical 0/1 extraction.
Direct/assembler payloads and reassembled final linked exports match byte for
byte. See [testing](testing.md#gsu-optimization-regressions).

O1 does not promise globally optimal code. Its locals remain
memory-resident; far pairs and cross-block results still spill. Functions
containing implicit hardware-loop backedges keep conservative frame-backed
allocation until that liveness is modeled. There is no new automatic CACHE
insertion, speculative load elimination, global common-subexpression elimination,
delay-slot filling, calling-convention change or automatic unrolling of arbitrary
loops. O1 still uses existing checked software division and hardware-loop
selection. Further optimization requires measurements and separate correctness
coverage, not relaxation of memory or graphics rules.

O2 unit and execution checks additionally cover PHI swaps/cycles, exact
interference and shared spills, loop-carried call values, nested hardware
scopes, volatile/escaped/uninitialized locals, zero-trip fault ordering,
forward/backward address induction and malformed PHI/loop metadata. Existing
pointer, numeric, ABI, library, globals and graphics cases also run at O2.
Neither level changes the ABI or speculates unknown memory. O1 retains NOP
slots; O2 fills only the proven cases above, not arbitrary delay slots or split
immediate instructions. See [testing](testing.md#gsu-optimization-regressions).

SCCP adds executable-edge/late-backedge PHIs, mixed PHI-prefix ordering,
constant/default/partial switches, modular/bool/enum/cast boundaries, CFG
compaction and idempotence checks. Execution at all three levels preserves
volatile counts, short-circuit suppression, cursor effects and fail-stop
ordering through constant joins. SCCP itself does not add range analysis, GVN,
interprocedural SCCP, arbitrary CFG inlining or a new calling convention.

Checked-proof units and execution cases additionally protect wrapped/narrowed
intervals, unknown and cyclic pointer/index values, null/alignment/bank ends,
negative offsets, wider casts, resource fallback and stack-credit saturation.
Minimum frames, zero/nonzero stack floors, calls after a volatile witness,
untaken calls and mixed O0/O1/O2 callees retain results and fault ordering.
Divmod/size units and execution tests additionally cover signed/unsigned bounds,
remainder-first pairs, calls/branches/plot/hardware backedges, zero-divisor
effect ordering, bounded/idempotent fusion, exact deterministic PHI interference,
register/spill cycles and function-local fault/epilogue reach/relaxation.
GVN/scalar-cell/value tests additionally cover dominance siblings, identical
volatile reads, independent field PHIs, escaped/dynamic/uninitialized/packed
fallbacks, constant-index chains, signed/unsigned narrowing and wrap, known bits,
pointer-comparison exclusions, modular recurrences and hot lifetime interference.
The execution matrix uses 37 boundary/deterministic pairs at O0/O1/O2/Os and both
assembly paths, preserving volatile counts, cursor/PLOT/RPIX and packed-access
fail-stop ordering. It also checks exact final-assembly reconstruction.
Native Windows verification passes 242 tests in MSVC Release (including optional
Mesen integration), 234 in MSVC Debug and 234 in static MinGW Release. Os adds 28 existing language/backend
execution cases, nine functional benchmarks and mixed-level/kernel regressions.
Changed production code also passes GCC's C++14 syntax/warning checks. Linux,
macOS, sanitizers, DOS and physical hardware were not rerun for this snapshot.

## Flag-aware slots and exact IBT startup selection

The next pipeline snapshot extends the scheduler's one-byte selection by
instruction effects, rather than recognizing only the example WITH R10.
N/Z producers may precede carry/overflow branches; shifts that modify C may
precede overflow branches, but not carry branches. Instructions that supply a
branch's condition remain before it. Complete ALT/register-prefix state,
all incoming paths, special registers and relocations remain part of the proof.
Unproven slots retain NOP; no multi-byte immediate is automatically split.

The linker now applies the same exact-value IBT rule already used for optimized
compiler literals. `$0000-$007F` and `$FF80-$FFFF` need two bytes; other values
need IWT. This is about the resulting register bits, not the source type:
`ibt r10, #$FE` materializes `$FFFE`, while `$00FE` still needs IWT.
Patched entry addresses retain IWT. The no-globals startup is 11 bytes with
the default `$2000` stack or 10 bytes with `$FFFE`; global initialization uses
the same selection. These linker changes intentionally affect O0/O1 payloads
too when runtime initialization is requested.

Fresh MesenCE measurements use frozen pre-change executables, the same source,
host, reference images, clock profile and timing harness:

| Rotating checkerboard, cold CACHE, RAM execution | Before | O2 / Os |
|---|---:|---:|
| Payload bytes | 2,683 | 2,672 |
| First pose, fast GSU cycles | 2,503,800 | 2,494,487 |
| Mean cycles, 64 poses | 2,854,983.828125 | 2,844,363.546875 |
| Completed-pose FPS | 6.700912327557 | 6.700911412882 |

Payload reduction is **0.410%**, and mean GSU-cycle reduction is **0.372%**.
O2 and Os produce identical payloads here. Both match all 64 baseline PNGs and
pass per-pixel/bitplane, cursor, stack, bank, phase-wrap and deliberate failure
checks. Presentation still takes 574 refresh intervals (510 repeats), so there
is no meaningful FPS gain. The padded SNES ROM remains 65,536 bytes.
See the [rotation evidence summary](../benchmarks/results/rotation-prefix-summary.json).

All nine benchmarks reduce both code bytes and measured cycles at each level:
[O2 before](../benchmarks/results/gsu-O2-prefix-before-mesen.json),
[O2 after](../benchmarks/results/gsu-O2-prefix-mesen.json),
[Os before](../benchmarks/results/gsu-Os-prefix-before-mesen.json),
[Os after](../benchmarks/results/gsu-Os-prefix-mesen.json).
For example, O2 RAM-palette plotting changes from 269 bytes/45,015 cycles to
263 bytes/44,350 cycles; repeated function calls change from 202 bytes/39,775
cycles to 198 bytes/39,440 cycles. Measurements include calibrated STOP fetch,
but exclude host setup/DMA/display; the rotation FPS window includes publication.
These are emulator measurements, not physical-hardware timings or proof of
optimal scheduling. Native verification again passes 242 MSVC Release tests,
234 MSVC Debug tests and 234 static MinGW Release tests, with strict GCC C++14
production syntax/warnings. Linux, macOS, DOS and sanitizers were not rerun.

## Rotating-triangle bottleneck pass

This pass separates compiler improvements from changes to the example's
algorithm. It does not expand the language or reserve additional general
purpose registers: the allocator still uses R5/R7/R8.

- Scalar spill stores at O1/O2/Os form the frame address in R3 and store
  directly from R0, avoiding the old R6 round trip. O2/Os PHI copies can store
  an already allocated source directly. Far pointer pairs and cyclic parallel
  copies retain their existing preservation paths. Literal opcode tests check
  both R0 and non-R0 sources and preserve unrelated live values.
- Safe strict `<` word loops with invariant bounds can use counted GSU LOOP.
  Entry guards exclude zero/negative trip counts; taken paths have 1–65535
  iterations without wrapping the induction before completion. R12/R13 and
  live-out PHIs are preserved. Unknown or potentially wrapping inclusive
  bounds remain software loops. Tests exercise zero, signed cross-zero,
  unsigned 65535, live-out values, explicit CACHE and byte-exact assembly.
- Bounded/aligned induction and dominating comparisons can prove near-RAM
  addresses and checked frame subobjects. Hardware-loop proofs follow only
  bounded, single-predecessor, branch-only PHI edge chains. Unknown, null, odd,
  wrapping, out-of-bank, far/ROM or unsupported paths retain runtime guards.
  Analysis limits conservatively retain checks on large functions.
- Anonymous nonescaping, nonvolatile scalar temporaries join pruned SSA
  promotion, including short-circuit expression slots. Definite assignment,
  overlapping storage and escaped addresses block promotion. Source symbols
  and temporary identities remain distinct. Ordered call/write trace tests
  check evaluation order; existing div/mod fusion is retained rather than
  duplicated. Post-compaction canonicalization keeps the pipeline stable.
- O2 can insert CACHE for the bounded division kernel. Os can now reuse an
  existing guard delay-slot NOP for CACHE without growing the payload.
  An explicit function CACHE annotation suppresses an additional kernel
  CACHE. Callees may rebase CACHE: the ABI does not preserve the caller's cache
  base, so this is not a promise that caller hot code stays cached.
- The rotating example counts pixels once per span, derives black pixels
  after drawing, and uses guarded span-count LOOP. It initializes each edge's
  floor quotient/remainder and one-row delta once, then advances with sums
  and at most one remainder carry. There are at most six divisions per pose
  instead of 224–336. The independent full-image oracle is unchanged.

The [staged rotation workloads](../benchmarks/rotation/README.md) preserve the
original and counter-only sources. `TRIANGLE_SOURCE` selects a variant for the
same complete-SNES build/timing harness. This makes compiler-only gains
reproducible without attributing an algorithm rewrite to register allocation.

This historical bottleneck snapshot uses the frozen pre-bottleneck executables and the
same host, timing profile and full-image/FPS harness:

| Workload / toolchain | Payload bytes | Mean fast-GSU cycles, 64 poses | Completed poses/s |
|---|---:|---:|---:|
| Frozen original, previous O2 | 2,672 | 2,844,363.546875 | 6.700911412882 |
| Same original source, post-bottleneck O2 | 2,285 | 1,910,470.984375 | 10.068910245328 |
| Counter/span rewrite only, post-bottleneck O2 | 2,217 | 1,538,819.671875 | 12.095359151697 |
| Incremental edges, post-bottleneck O2 | 2,819 | 1,159,469.625 | 15.024708464127 |
| Incremental edges, post-bottleneck Os | 2,818 | 1,167,535.71875 | 15.024702551842 |

Compiler-only gains are **14.484% fewer payload bytes** and **32.833% fewer
mean cycles**. The complete O2 renderer uses **59.236% fewer mean cycles** and
presents **2.242x** as many completed poses, but grows **5.501%** versus the
original payload: incremental edge initialization/static state trades setup
code for less executed work. The padded SNES ROM remains 65,536 bytes. All 64
captured images are byte-identical at every stage; phase wrap, pixel/bitplane,
mailbox, cursor, bank, stack, STOP and deliberate red-ROM checks pass.

On the same incremental workload, explicit whole-helper CACHE takes 2,826
bytes and 1,161,763.28125 mean cycles, versus 2,819 bytes and 1,159,469.625 for
the bounded kernel. The kernel wins this measured comparison; caller-cache
preservation is not assumed. The final post-compaction LICM/idempotence fix
leaves all nine O2/Os benchmark payloads/opcode counts and all three O2 rotation
payloads byte-identical to those measured.

The [rotation evidence](../benchmarks/results/rotation-bottleneck-summary.json)
retains source/tool/payload hashes, full 65-sample timing/FPS reports,
first-pose function/opcode totals and screenshot comparison counts. Phase 0
drops from 946,672 to 384,619 executed opcodes; its division helper drops from
625,000 to 4,827 emulated cycles. These are emulator measurements, not
physical-hardware timings.

All nine unchanged benchmarks improve or preserve measured cycles:
[O2 before](../benchmarks/results/gsu-bottleneck-O2-before-mesen.json),
[O2 after](../benchmarks/results/gsu-bottleneck-O2-after-mesen.json),
[Os before](../benchmarks/results/gsu-bottleneck-Os-before-mesen.json),
[Os after](../benchmarks/results/gsu-bottleneck-Os-after-mesen.json).
O2 `memcpy` grows 14 CODE bytes but reduces cycles from 119,400 to 89,535 and
stack accesses from 906 to 270. O2 arithmetic grows four bytes but changes
81,189 cycles to 25,652. Os improves or preserves both bytes and cycles in all
nine cases. The comparator enforces matching workload/model/harness/emulator
fingerprints; the O2 speed comparison explicitly permits these disclosed
size tradeoffs rather than weakening functional assertions.

Final verification passes **242 MSVC Release tests**, **234 MSVC Debug tests**
and **234 static MinGW Release tests**. Changed production sources pass strict
GCC C++14 syntax checks with warnings as errors. Both final rotation policies
pass all 64 poses plus wrap, byte-exact assembly and deliberate failure
verification in Mesen. Linux, macOS, DOS, sanitizers and physical hardware were
not rerun for this pass.
