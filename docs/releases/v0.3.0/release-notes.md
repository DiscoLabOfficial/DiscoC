# DiscoC v0.3.0 — release notes

Released on **2026-11-17** as [v0.3.0](https://github.com/DiscoLabOfficial/DiscoC/releases/tag/v0.3.0),
with native tools, exact source, SNES demos, checksums and validation evidence.
The release build manifest records the tag/package source and archive hashes.
The [validation record](validation.md) preserves the dated preparation checks;
earlier 0.2.0 evidence retains its original identity.

DiscoC remains an experimental pre-1.0 toolchain implementing **Language
Baseline 0.1**, not ISO C. The native toolchain still uses C++23; the
experimental DOS cross-build retains C++14.

## Changes since 0.2.0

- **Checked GSU address proofs.** Bounded facts cover accesses inside known
  static RAM objects, validated frames and same-object ROM tables. Proven
  accesses avoid redundant guards; unknown, out-of-range and unsupported
  cases retain checks. Pointer arithmetic, fault ordering and volatile
  observations remain part of the correctness contract.
- **Stack and frame planning.** Eligible functions can use static frames with
  the runtime-entry proof and pre-existing static RAM; eligible private
  functions can receive register arguments without changing the public call ABI.
  `--static-stack-proof` lets O2/Os prove stack guards unnecessary when the
  runtime exclusively enters `main`; the linker checks the recorded bound
  against the actual initial stack and placement. This is an explicit entry
  contract, not permission to skip runtime initialization.
- **Optional short RAM.** `--short-ram <bytes>` with the stack-proof option,
  or project `target.gsu.short_ram = true` with eligible runtime startup,
  places static-frame word slots in `$0002..$01FF` for `LMS`/`SMS`.
  The compiler, assembler, linker and runtime carry the section, relocations,
  initialization and overlap checks together. The host must reserve the
  window and keep the framebuffer clear of it.
- **Optimization and scheduling.** Bounded inlining and dead internal-function
  removal, pointer-load and register-transfer improvements, nested-loop CACHE
  selection, division-kernel window handling, software-loop regional values
  and cache-aware Os candidate exchange reduce selected code or execution
  work. Machine peepholes and delay-slot scheduling preserve selector, flag,
  entry-point, relocation and ABI constraints. These are workload-dependent
  improvements, not a guarantee of minimal code or cycles.
- **Revised SNES demos.** Gem chess and interactive shapes use the short RAM
  layout and rewritten renderers. The rotating checkerboard uses a two-pass
  renderer and a bounded queue for asynchronous image capture. Its SNES host
  initializes the status mailbox before multi-frame setup, preventing the
  verifier from reading power-on RAM as a host failure. The scaling host uses
  the same early initialization. The guides now
  distinguish current memory/capture contracts from historical measurements.
- **Regression coverage.** Expanded proof, scheduler, compaction, optimizer,
  object/linker, project and graphics checks cover the new behavior and
  expected failures, including assembly round-trips.

See [architecture](../../architecture.md), [optimization](../../optimization.md)
and [project configuration](../../project-manifest.md) for the detailed rules.

## Compatibility and upgrading

Rebuild **discc, discas and discld together**, then rebuild application objects.
The default writer remains object format **7**. Static-RAM proofs require at
least **8**, static stack bounds at least **9**, and short RAM **10**; the writer
selects the highest version required by an object's features. The current reader
accepts versions **3–10** with version-specific
validation; older tools cannot consume the new variants. Legacy readability
does not guarantee ABI compatibility. See the [object contract](../../object-format.md).

`-O0` remains the default; `-O1`/`-O`, `-O2` and `-Os` keep their explicit
selection. A CMake Release build optimizes the native tools and does not choose
an optimization level for DiscoC input. Optimization policies remain linkable
under the current public ABI, while private conventions and proof metadata
must be handled by the matching toolchain.

Short RAM changes the demo memory layouts: gem chess starts its framebuffer
at `$0400` and payload at `$70:6400`; interactive shapes reserves the full
OBJ domain through `$83FF` and starts its payload at `$70:8400`. Both place
ordinary statics at `$D000`. Keep each manifest, SNES host, display code and
verifier together when rebuilding.

## Validation and measurement scope

The [fresh functional benchmark record](optimization-results.md) covers all
nine workloads at O0/O1/O2/Os. It reports bytes and executed opcodes with
source/model/tool/payload identities; it does not infer GSU cycles or FPS.
The [validation record](validation.md) lists the actual native, sanitizer and
complete-ROM checks performed during preparation. Final package and CI receipts
are retained with the [release downloads](https://github.com/DiscoLabOfficial/DiscoC/releases/tag/v0.3.0).

Earlier before/after tables in the optimization and demo guides remain dated
development evidence. They must not be relabeled as measurements of this
0.3.0 preparation or the release tag. Package-specific results identify their
own clean source revision in the release build manifest.

## Remaining limits

- A linked `.bin` is a fixed-origin GSU payload, not a complete SNES ROM or
  position-independent program. The host owns bus/PPU setup and loading.
- SPC700 remains frontend/capability/verified-IR support without an executable
  backend. General source inline assembly, interbank code calls, aggregate
  by-value ABI and independently placed RAM code/ROM constants remain outside
  the supported backend contract.
- The gem and interactive-shapes RAM hosts accept O2/Os. Large balanced
  switches can still exceed a diagnosed local-branch range limit.
- No physical-hardware validation, exhaustive fuzzing or universal performance
  claim is made. Current platform/emulator results and any failures are listed
  in the validation record; historical successful runs do not replace them.
- No new Windows, macOS, DOS runtime or hosted CI result is implied by the
  local Linux preparation. Published package receipts separately identify
  their platform checks and requirements, using the
  [documented clean-checkout workflow](../../../cmake/PACKAGING.md).

The SNES demos contain no proprietary Nintendo code, commercial game assets
or Nintendo SDK components. Existing contributor attribution is retained in
the [project credits](../../../README.MD#contributors-and-community).
