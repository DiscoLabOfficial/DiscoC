# DiscoC v0.2.0 — release notes

**Published on 2026-10-21:** [DiscoC v0.2.0](https://github.com/DiscoLabOfficial/DiscoC/releases/tag/v0.2.0),
without the `-rc.1` suffix, with native tools, exact source, SNES demos,
checksums and validation evidence. DOS remains an experimental cross-build
whose runtime is unverified.
Stable-channel naming does not make this experimental pre-1.0 compiler
production-ready.

The [final validation record](validation.md) identifies the published tag,
package/test source, their identical Git tree, recorded results and post-merge
CI. It links the released evidence and [reproduction procedure](reproduce.md).
The released `FINAL-NOTES.md` and `BUILD-INFO.json` retain their pre-publication
snapshot wording unchanged; that wording describes preparation, not the current
publication status. These qualitative notes do not replace the raw records.

## Scope and relationship to the RC

DiscoC 0.2.0 builds on the documented Language Baseline 0.1 with explicit
optimization policies and correctness-oriented SuperFX code generation. The
release also includes the gem-chess scene and expanded backend,
differential, raster and build-path regression coverage developed after the RC.
The [gem-chess guide](../../../examples/snes/gem_chess/README.md) describes the
scene viewer and its limits; it is not a chess rules engine or a real-time game.

The published RC and the older checked-in
[candidate records](../v0.2.0/validation.md) identify different historical source
revisions. Neither is relabeled as evidence for the final release. In
particular, old test totals, payload sizes, cycles and FPS are not final-source
measurements. The [final-source checks](validation.md#recorded-results) cover
all nine official workloads at all four optimization levels and nine complete-ROM
configurations. The RC comparison has 36 incompatible strict signatures, so no
apples-to-apples speedup against the RC is claimed.

## Optimization and compatibility contract

- `-O0` remains the default. `-O1` (also `-O`) selects local optimization;
  `-O2` selects the speed-oriented global policy; `-Os` selects size-oriented
  candidates. A native CMake Release build does not implicitly optimize source
  programs. See the [optimization contract](../../optimization.md).
- TOML `compiler.optimization_level = 0/1/2/"s"` selects the same policies;
  `compiler.optimize = true` selects O1. Do not set both manifest keys.
- Mixed-level objects retain the same current call ABI and object format.
  Stateful graphics retain cursor R1/R2, PLOT's X increment, observable
  RPIX/flush, ROM-buffer GETC and RAM/computed COLOR semantics.
- Functional instruction counters, Mesen GSU cycles and completed-image FPS
  are separate measurements. Smaller output may be slower; measure the actual
  workload instead of treating Os as a speed promise.

## Limitations and pre-1.0 status

- The language remains **DiscoC Language Baseline 0.1**, not ISO C. Syntax,
  ABI and object compatibility may change before 1.0. Current emission remains
  explicitly little-endian **object format v7**. Rebuild the tools and objects
  together when upgrading; legacy object readers are not an ABI guarantee.
- Optimization is bounded and immature, not globally optimal. Large balanced
  switches can still exceed local branch range. Fixed workloads do not establish
  safety or performance for arbitrary programs, memory/clock/cache profiles,
  or physical hardware, nor constitute exhaustive fuzzing.
- A linked `.bin` is a fixed-origin GSU payload, **not a complete SNES ROM** or
  position-independent code. Code must fit its accessible program bank. The
  SNES host owns copying, bus ownership and PPU configuration; runtime
  initialization is optional outside the supplied workflows.
- Interbank code calls, aggregate-by-value ABI, independently placed RAM code
  and ROM constants, general inline assembly, native WLA-SuperFX export and
  profile-guided optimization are not supplied by this finalization.
- **SPC700 remains a frontend/capability/verified-IR foundation, with no
  executable backend.** OpenSNES is neither bundled nor required.
- The gem and interactive-shapes RAM hosts accept O2/Os, not O0/O1. Source
  regression checks, complete-ROM execution and image/timing evidence have
  different scopes; a successful build alone does not validate display output.

See the [language baseline](../../baseline-0.1.md),
[object format](../../object-format.md), [loading contract](../../gsu-loading.md)
and [testing guide](../../testing.md) for the detailed boundaries.

## Contributor credits

Thanks to **[k0b3n4irb](https://github.com/k0b3n4irb)** for independently authored
correctness fixes and regression tests retained in this release line:

- [Hardware-loop R13 correction, `57d98b0`](https://github.com/k0b3n4irb/DiscoC/commit/57d98b0492bd577a6e5e86c67f1cda78afda6324):
  load R13 with the loop-body address rather than jumping to it; integrated as
  `4cc42c2c8004a4502307b18407d696ddfdd7da83`.
- [Exhaustive switch-return correction, `dc6200f`](https://github.com/k0b3n4irb/DiscoC/commit/dc6200f12a4c45f1ee930d4acf2fe941072d1e2f):
  correct exhaustive-return analysis and IR; integrated as
  `7d6b0c3ea6e59f77ea9a574d2b312c94776a7517`.

Original authorship and cherry-pick provenance remain intact. The contributor's
OpenSNES benchmark branch was deferred, not merged wholesale. See the
[project contributor list](../../../README.MD#contributors-and-community).

## SNES demo disclaimer

The included SNES demo ROMs **do not use proprietary Nintendo code**. Their
hosts and generated payloads contain no Nintendo SDK code, retail-game ROM
code or proprietary Nintendo assets. Nintendo and Super Nintendo are trademarks
of their respective owner. This independent project is not affiliated with or
endorsed by Nintendo; emulator validation is not physical-hardware validation.
