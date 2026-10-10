# DiscoC v0.2.0-rc.1 — draft release notes

**Prepared, not published.** These notes describe source candidate
`d967c3ef45a948ff457d81052448732da3bd73dc`; no release tag or package was created
by this validation pass. The preparation PR updates CMake's numeric version to
`0.2.0` with prerelease `rc.1`; the measurements remain attributed to the source
SHA above. Final packaging and the eventual post-merge tag revision still
require review, validation and explicit publication approval.

DiscoC 0.2.0 focuses on verified SuperFX code generation, explicit optimization
policies and reproducible measurements, building on the documented 0.1 language
and toolchain baseline. It does not declare the compiler production-ready.

## Correctness first

- Integrate **k0b3n4irb's** hardware-loop fix: load R13 with the loop-body address
  instead of accidentally jumping to it; preserve LOOP backedge/delay semantics
  through CFG, liveness, allocation and scheduling.
- Integrate **k0b3n4irb's** exhaustive switch-return analysis/IR correction.
- Retain contributor regressions across optimization modes and reconcile the
  optimizer without bypassing the fixes or changing language semantics.
- Restore C++14/DJGPP linking and direct-build test coverage, and verify build
  target parity so a listed test executable cannot silently disappear.

## Optimization policies

- **O0** remains the default generation policy, with correctness fixes applied.
- **O1** adds local optimization, ISA-aware immediate/bit/shift selection,
  register/spill-copy reductions and safe local load/store reuse, including SBK
  only when its implicit-address semantics are established.
- **O2** adds verified CFG/liveness/SSA/PHIs, SCCP/CFG simplification, pure GVN,
  scalar-cell promotion, known-bit/range proofs, guarded loop/induction and
  inlining transformations, divmod fusion, shared tails, register allocation
  improvements, proved check reuse and pipeline/CACHE-aware code generation.
- **Os** reuses verified infrastructure but selects bounded candidates by real
  emitted CODE/DATA bytes, limits speed-only growth, pools profitable repeated
  division kernels and shares exact tails. Smaller can be substantially slower.

Select `-O0`, `-O1` (or `-O`), `-O2`, `-Os`, or TOML
`compiler.optimization_level = 0/1/2/"s"`. `compiler.optimize = true` selects O1;
do not set both TOML keys. A native CMake Release build does not implicitly
enable source optimization. Mixed-level objects retain the same call ABI and
object format. See the [optimization contract](../../optimization.md).

Stateful SuperFX graphics semantics remain intact: cursor R1/R2, PLOT's X
increment, observable RPIX/flush, ROM-buffer GETC and RAM/computed COLOR. No
unchecked-memory mode, register-agnostic inline ASM or executable SPC700
backend is introduced.

## Fresh validation and measurements

- Clean Windows MSVC builds: **256/256 Release**, **248/248 Debug** tests passed.
- Post-merge CI: native Windows/Linux/macOS Debug and Release, MinGW static,
  DJGPP cross-build, Ubuntu ASan/UBSan and both CodeQL analyses succeeded.
- All nine official benchmarks rerun at all four levels, with **36/36**
  independent Mesen result/memory/stack checks, exact assembly reconstruction,
  matching opcode counts and cross-level observable-output fingerprints.
- Triangle CODE/cycles: O0 **1,198 / 7,359,320**, O1 **381 / 1,997,695**,
  O2 **268 / 243,130**, Os **231 / 1,920,665** under the fixed cold LoROM profile.
- RAM rotating triangle: both levels use **2,733 payload bytes**, all 64 poses
  plus wrap pass; roughly **20.03 completed-image FPS** under the recorded host
  profile. FPS includes presentation, not only GSU execution.
- RAM OBJ/4bpp interactive shapes: controller/image and negative tests pass
  in O2 and Os; fixed-pose wire/filled cycle samples are recorded separately.

The complete [validation record](validation.md), [results](optimization-results.md),
[JSON](benchmark-results.json), [CSV](benchmark-results.csv) and
[reproduction commands](reproduce.md) identify the exact source, tools and
profiles. Figures are emulator measurements, not physical hardware guarantees.
No benchmark source rewrite was used for this four-level comparison.

## Limitations and pre-1.0 compatibility

- Syntax, ABI and object format may evolve before 1.0. Current objects remain
  little-endian format v7; rebuild tools and objects together when upgrading.
- Optimization is immature and bounded, not globally optimal. Os's triangle
  is **7.90x slower** than O2 despite saving 37 bytes; RAM-palette Os also has
  a large cycle cost. Measure before choosing a size policy for hot rendering.
- Local clean builds still emit 23 MSVC warning occurrences per configuration;
  this is not a warning-free release candidate.
- Large balanced switches can still be rejected for local branch range.
  Physical hardware, exhaustive fuzzing and arbitrary memory/clock/cache
  configurations are not validated by the fixed workloads.
- `.bin` files are fixed-origin, single-program-bank GSU payloads, not complete
  SNES ROMs or position-independent code. Hosts own copying, PPU setup and bus
  ownership; runtime initialization is optional. Separate RAM code and ROM
  constants, interbank code calls and aggregate-by-value ABI remain unsupported.
- SPC700 is a target/frontend/IR foundation, not a fully supported executable
  target. OpenSNES integration is neither included nor required for this RC.
- Native WLA-SuperFX assembly export, general split-location allocation and
  profile-guided optimization remain future work.

## Contributor credits

Thanks to **[k0b3n4irb](https://github.com/k0b3n4irb)** for the independently
authored correctness fixes and regression tests:

- [Hardware-loop R13 correction](https://github.com/k0b3n4irb/DiscoC/commit/57d98b0492bd577a6e5e86c67f1cda78afda6324), integrated as `4cc42c2c8004a4502307b18407d696ddfdd7da83`.
- [Exhaustive switch-return correction](https://github.com/k0b3n4irb/DiscoC/commit/dc6200f12a4c45f1ee930d4acf2fe941072d1e2f), integrated as `7d6b0c3ea6e59f77ea9a574d2b312c94776a7517`.

Original authorship and cherry-pick provenance are preserved; the optimizer
implementation remains separately attributable. The contributor's OpenSNES
benchmark branch was deferred, not merged wholesale.

## SNES demo disclaimer

The included SNES demo ROMs **do not use proprietary Nintendo code**. Their
hosts and generated payloads do not include Nintendo SDK code, retail-game ROM
code or proprietary Nintendo assets. Nintendo and Super Nintendo are trademarks
of their respective owner; this independent project is not affiliated with or
endorsed by Nintendo.
