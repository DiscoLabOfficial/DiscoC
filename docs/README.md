# DiscoC Documentation

The [repository README](../README.MD) is the short project overview. Start here
for detailed workflows and contracts.

## Build and Use

- [Building DiscoC](building.md): prerequisites, Windows/MinGW/MSVC, Linux,
  macOS, CMake presets, direct GCC/Clang, Make helpers, and experimental DOS.
- [Getting started](getting-started.md): a two-file project, host-visible RAM
  output, optimization selection, and assembly round-trips.
- [Project manifests](project-manifest.md): `discoc.toml`, imports, paths,
  CLI precedence, schema, and supported TOML subset.
- [GSU loading and startup](gsu-loading.md): origins, RAM/ROM placement,
  initialization, globals, stack, and the host's responsibilities.
- [Assembler](assembler.md): DiscoC's dialect and object-generation workflow.
- [Examples and libraries](examples.md): runnable demos and language examples.
- [Freestanding library](freestanding-library.md): memory and fixed-point APIs.

## Language and Targets

- [Language specification](language-spec.md): authoritative Baseline 0.1
  semantics, including types, numeric rules, qualifiers, pointers, aggregates,
  imports, visibility, control flow, and capabilities.
- [Baseline 0.1 contract](baseline-0.1.md): scope and compatibility boundaries.
- [SuperFX graphics](gsu-graphics.md): cursor, COLOR/GETC, PLOT/RPIX, options,
  bitmap metadata, and migration from the old API.
- [SPC700 foundation](spc700-target.md): current frontend/IR support and limits.
- [Inline assembly design](inline-assembly-contract.md): a design contract,
  not implemented source syntax.
- [Roadmap](../ROADMAP.MD): milestones and deferred work.

## Compiler Internals and Verification

- [Architecture](architecture.md): pipeline, ownership, and GSU ABI.
- [IR and CFG](ir-cfg.md): intermediate representation and verifier invariants.
- [Object format](object-format.md): versions, sections, serialization, and relocations.
- [Optimization](optimization.md): O0/O1/O2/Os contracts and development measurements.
- [Testing](testing.md): native regressions, sanitizers, fuzzing, and SNES integration.
- [Language conformance suite](../tests/language/README.md): rule-to-test index.
- [Benchmarks](../benchmarks/README.md): workloads, counters, timing, and reproduction.

## Releases and Evidence

- [v0.2.0 final-candidate notes](releases/v0.2.0-final/release-notes.md) and
  [reproduction](releases/v0.2.0-final/reproduce.md): prepared, not published.
  Exact-source validation records and package manifests are generated separately
  under `build/stable-final/<SHA>/`; historical RC results are not final evidence.
- [Published v0.2.0-rc.1](https://github.com/DiscoLabOfficial/DiscoC/releases/tag/v0.2.0-rc.1):
  the current download, release notes, and evidence for the tagged RC revision.
- [Historical v0.2.0 RC candidate records](releases/v0.2.0/validation.md),
  [optimization results](releases/v0.2.0/optimization-results.md),
  [reproduction](releases/v0.2.0/reproduce.md), and
  [release-note preparation](releases/v0.2.0/release-notes.md).
  These checked-in records identify their own tested source revision; they are
  not silently relabeled as measurements of a later commit.
- [v0.1.0 release notes](release-notes-0.1.0.md) and
  [readiness record](release-readiness-0.1.md).

Instruction-model counters, emulated GSU cycles, presentation time, and FPS
are different measurements. Historical optimization snapshots are not final
release evidence or physical-hardware performance claims.

## Community

See [contributors](../README.MD#contributors-and-community),
[Contributing](../CONTRIBUTING.md), and the
[Code of Conduct](../CODE_OF_CONDUCT.md). The private reporting contact is
pending; no private reporting channel is currently advertised.
