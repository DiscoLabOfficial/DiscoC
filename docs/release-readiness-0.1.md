# DiscoC v0.1.0 release readiness

This record separates baseline acceptance from release publication. The
maintainer explicitly authorized publishing v0.1.0 on 2026-10-05 after the
merged baseline passed platform CI. Creating the tag and uploading artifacts
remain separate release operations.

The candidate is the frozen [Language Baseline 0.1](baseline-0.1.md), with
correctness and deployment fixes only. It does not require further optimization
or an executable SPC700 backend. Local results and exact-commit CI evidence
are recorded separately below. Any code/configuration change invalidates
affected checks and requires proportionate retesting. Release preparation
changes only documentation, not the tested compiler or build configuration.

## Checklist

- [x] Normative [language-spec.md](language-spec.md) agrees with the frozen
  parser/analyzer/IR/backend contract and documented limitations.
- [x] README describes current syntax, fixed-origin payloads and toolchain UX.
- [x] Tracked examples use current syntax; obsolete spellings remain only in
  migration documentation and expected-negative fixtures.
- [x] Positive/negative conformance suite passes on both frontend target models.
- [x] GSU instruction, graphics, ABI and byte-equivalence regressions pass.
- [x] Linker/object/runtime tests pass, including rejected dangerous layouts
  preserving existing binary/assembly outputs.
- [x] Fresh Windows MSVC Debug/Release and MinGW Release preset builds succeed.
- [x] Fresh Ubuntu Debug/Release builds and tests pass for the merged baseline.
- [x] Fresh macOS Debug/Release builds and tests pass for the merged baseline.
- [x] Ubuntu ASan/UBSan tests pass for the merged baseline.
- [x] The official SNES triangle builds from its manifest and passes the complete
  Mesen ROM test, including deliberate red-screen failure.
- [x] No unresolved silent-codegen defect is currently known in the verified
  baseline scope; this is not proof of correctness for every possible program.
- [x] CMake happy path, compiler prerequisites and executable locations are
  documented in [building.md](building.md).
- [x] `discc build`, TOML schema, path/precedence and runtime contracts are
  documented in [project-manifest.md](project-manifest.md).
- [x] Review the exact final diff and merged baseline CI evidence; confirm no stale
  results, skipped required tests or newly reported correctness regressions.
- [x] Obtain explicit maintainer permission before creating a tag/publishing.

“Fresh build” means configuration in a new build directory, not a claim that
every compiler emits zero warnings. Existing MSVC warnings remain visible;
they have not been disabled globally to obtain a green build.

## Verification record — 2026-10-04

Local toolchains: MSVC 19.44, MinGW GCC 16.1, CMake 4.4.3, and DJGPP GCC 12.2.
The native baseline has 91 CTest entries, including 22 language-conformance
categories, the new in-process linker hardening suite and independent GSU
execution/graphics models. External SNES integration is optional and adds one
entry when explicitly enabled.

| Check | Evidence / boundary |
| --- | --- |
| Windows MSVC Debug | `debug` preset: 91/91 native tests passed. |
| Windows MSVC Release | `release` preset: 92/92 tests passed, including all 91 native entries and the optional complete-ROM integration. |
| Windows MinGW Release | `release-mingw` preset: 91/91 native tests passed; standalone MinGW runtime linking enabled. |
| Direct GCC helper | Fresh Windows `build.ps1 -Backend Direct -Static -Test`: the complete prebuilt registry passes, including linker hardening; tools/tests are in the selected build directory's `bin/`. |
| Manifest happy path | `project_build` executes bare `discc build` from a directory containing `discoc.toml`, compares manual/project payloads and final assembly, then executes the result. |
| Local tool install | `cmake --install` installs just the three tools into a separate `bin/`; installed `discc build` builds the multi-file manifest without CMake or sibling-tool subprocesses. |
| Linker/object/runtime | Golden per-object relocation bases, all relocation types, malformed objects, compatibility, placement/overlap and stack checks pass; invalid layouts preserve binary/assembly sentinels. |
| DOS compatibility | All three tools cross-compile under GNU C++14; DOS execution is not tested. |
| SNES integration | WLA-DX + Mesen 2.2.1 verify all 49,152 pixels, 1,024 tilemap entries, startup/result/STOP and both positive/negative ROM outcomes. |
| Linux/macOS/sanitizers | Not executed locally; verified separately in exact-commit hosted CI below. |

The Windows local GCC installation uses MinGW-w64; the hosted MinGW CI job uses
UCRT64. That additional environment requires its own candidate CI result.
Local Docker startup was attempted with permission but did not make the Linux
engine available; no Ubuntu/container or sanitizer result is claimed from it.
Complete-ROM checks add independent evidence for this example, not physical
hardware or complete GSU/SNES timing validation. Fuzzer entry points and a
sanitizer configuration are not evidence of a long fuzzing campaign.

## Hosted CI record — 2026-10-05

PR [#13](https://github.com/DiscoLabOfficial/DiscoC/pull/13) merged as
`8f38d40536ddce28d285929a66936a43b4b0dad9`. The
[main build/test run](https://github.com/DiscoLabOfficial/DiscoC/actions/runs/37259096534)
completed successfully at that exact SHA:

- Windows MSVC, Ubuntu and macOS Debug/Release: all native tests passed.
- Windows UCRT64 MinGW static Release: preset and helper builds/tests passed,
  including compiler-selection regressions in PowerShell 5.1 and 7.
- Ubuntu Release and macOS Release: direct GCC/Clang helper builds/tests passed.
- Ubuntu ASan/UBSan: all native tests passed.
- DOS DJGPP Release: cross-compilation succeeded; DOS execution is unverified.

The [CodeQL run](https://github.com/DiscoLabOfficial/DiscoC/actions/runs/37259096724)
also completed successfully for the same SHA. Hosted CI does not run the optional
external Mesen integration; its evidence remains the local record above.

Release binaries must come from a successful run of the tagged source, or an
explicitly recorded compiler-identical baseline differing only in release
documentation. Packages must identify their source/run, retain the license,
include documentation/examples and supply SHA-256 checksums. No locally ignored
reference files, development-agent files or test executables belong in them.

## Reproduce

From the repository root, with the compiler and Ninja on `PATH` (MSVC requires
a Developer shell):

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
ctest --preset release -L conformance
ctest --preset release -L 'linker|object'
```

Use `debug` for MSVC/GCC/Clang Debug or `release-mingw`/`debug-mingw` on Windows
with `g++` and `mingw32-make`. Each preset writes tools into `build/<preset>/bin`.
On Ubuntu, separately run:

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

For the complete-ROM gate, install WLA-DX and a compatible Mesen/MesenCE Lua
test runner, then:

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -DVERIFY_MESEN=ON -P examples/snes/triangle/build-snes.cmake
```

Explicit executable paths are supported; quote whole `-D...=...` arguments with
spaces. Alternatively configure `DISCO_TEST_SNES_INTEGRATION=ON` and the three
`DISCO_WLA_65816`/`DISCO_WLALINK`/`DISCO_MESEN` paths, then run the CTest integration
label. Missing dependencies or stale/missing PASS artifacts fail that enabled
test instead of producing a successful skip.

The [workflow](../.github/workflows/cmake.yml) runs native presets on Windows,
Ubuntu and macOS, a MinGW static preset/helper job, direct GCC/Clang checks,
Ubuntu sanitizers and DOS cross-compilation. Build artifacts from CI are not
release artifacts and do not authorize publication.

## Limits that must remain public

- Object v7 remains implementation-owned. Legacy container readability does
  not establish old ABI compatibility; rebuild all tools/objects together.
- Payloads are fixed-origin and single-program-bank. No arbitrary-address
  loading, interbank code ABI or independently placed RAM-code/ROM-constant
  section layout is promised.
- Linker checks use known sections, selected bitmap metadata and startup
  configuration, not arbitrary host RAM reservations or dynamic pointer intent.
  Cross-unit type signatures and CODE instruction boundaries are not stored
  in the object format. Recursion depth is not proved at link time.
- Runtime startup is optional; host-owned startup requires an explicit valid
  bank/stack/global-initialization and memory-ownership contract.
- Input protection and pre-write validation are not transactional replacement
  after disk/permission/write/close failures. Output atomicity is future work.
- Physical hardware, complete bus/cache timing and exhaustive fuzzing remain
  unverified; SPC700 remains frontend/IR-only.

Current decision: **baseline readiness gates passed; maintainer-authorized
v0.1.0 publication may proceed, with the limitations above kept public.**
