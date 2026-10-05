# DiscoC 0.1 Stabilization

## Scope and freeze

v0.1.0 is the first functional, reproducible **SuperFX baseline**, not an
optimization milestone or an executable SPC700 release. The existing language
is frozen as **DiscoC Language Baseline 0.1**. Bug fixes, specified semantic
corrections and stronger diagnostics/tests remain allowed; new keywords, types
and major features wait until this stabilization step is closed.

[language-spec.md](language-spec.md) is normative. Parser, analyzer, verified
IR, backend, library interfaces and examples must agree with it. In particular,
the graphics interface is `cursor.x/y`, `color`, `pixel`, `read_pixel` and `flush`
inside lexical `plot` blocks. Old spellings are migration errors, not aliases.

This document identifies release-candidate acceptance. It does not create a
GitHub release/tag or promise a stable ABI across future toolchain versions.

## Ordered acceptance

1. Freeze the documented current language; diagnose obsolete graphics and
   source-level placement syntax instead of retaining conflicting semantics.
2. Specify types, signedness, modular overflow, casts, address spaces,
   const/volatile, near/far banks/alignment, aggregates, globals/startup,
   visibility, imports, control flow, graphics/bitmap and target capabilities.
3. Run positive/negative [conformance](../tests/language/README.md) separately
   from opcode/execution oracles. Both target models share portable semantics;
   hardware-specific operations receive explicit capability diagnostics.
4. Execute cursor/COLR/POR and ROM/RAM color paths, PLOT auto-increment,
   result/discard RPIX, both pixel caches, every bitmap/OBJ depth and size,
   layout boundaries and metadata round trips. Reject invalid profiles and
   framebuffer/payload/static/stack conflicts before output.
5. Build the [official triangle](../examples/snes/triangle/README.md) from
   `discoc.toml` through payload and linked assembly to a complete WLA-DX SNES
   ROM. Independently verify geometry/bitplanes, host startup and ownership,
   result after STOP, VRAM/tilemap/PPU display and the deliberate failure path
   with Mesen's compatible Lua test runner.
6. Consolidate the CMake preset and direct/helper paths on Windows/MinGW,
   Linux and macOS, with tools in `<build-dir>/bin` and manifest-driven
   `discc build` documented separately from the C++ toolchain build.
7. Validate object/version bounds, relocations, effective origin and metadata,
   RAM/bitmap/static/stack placement and known frame requirements before output;
   retain byte-exact multi-file and dynamic runtime-guard regressions.
8. Keep an explicit [internal v0.1.0-rc checklist](release-readiness-0.1.md),
   separating verified local results from candidate CI/review/publication gates.

The public example and `graphics_triangle` use one canonical source and
manifest. The host reads generated origin/SCBR/SCMR definitions from the linked
export; it does not ask users to duplicate bitmap metadata manually.

## Reproduce the acceptance checks

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
ctest --preset release -L conformance
```

Native Debug/Release, direct GCC/Clang, Ubuntu sanitizers and the experimental
DOS cross-build remain in CI. For the optional complete-ROM check, install
WLA-DX and a Mesen/MesenCE build supporting the Lua test-runner API, then run:

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -DVERIFY_MESEN=ON -P examples/snes/triangle/build-snes.cmake
```

MinGW presets use `build/release-mingw/bin`; manual and multi-configuration
builds use `<build-dir>/bin`. Explicit executable paths and output directories are
supported; see the example README. The default native suite needs neither
external assembler nor emulator. Optional `DISCO_TEST_SNES_INTEGRATION=ON`
registers the complete-ROM test with CTest and requires those dependencies.

Expected triangle evidence: 9,409 PLOT operations, 97 computed COLOR operations,
zero GETC, one final RPIX, correct R0/RAM result, normal fault status R6=0,
RAMBR=0, PBR=$70 and final R10=$FFFA. Mesen checks all 49,152 pixels and 1,024
tilemap entries, including the negative ROM's solid-red outcome. ROM-color GETC
is covered separately because RAM execution with independently placed ROM
constants is not supported yet.

## Candidate verification

The canonical dated verification record and outstanding platform gates live in
[release-readiness-0.1.md](release-readiness-0.1.md). Keep CI results tied to the
exact candidate commit; local native/emulator evidence does not replace
Ubuntu/macOS/sanitizer runs or establish physical-hardware/cycle correctness.

## Compatibility and non-goals

- Rebuild all tools together, then rebuild `.o` files and relink when upgrading.
  Current emission is little-endian object format v7; documented legacy readers
  are compatibility aids, not a promise that older tools accept new objects.
- Payloads are fixed-origin and must fit one accessible program bank. `.incbin`
  storage and copying do not relocate embedded addresses. Relink for a new origin.
- Host bus ownership, real cartridge capacity, code copying and SNES/PPU setup
  remain explicit. Runtime initialization is optional outside the example.
- No interbank code-call ABI, aggregate-by-value ABI, function pointers, unions,
  opaque inline ASM, interrupt ABI, separate RAM-code/ROM-constant placement,
  native WLA GSU export or executable SPC700 backend is implied by 0.1.
- The GSU oracle models functional instruction/graphics/cache behavior, not
  cycle accuracy, complete SNES hardware or bus/interrupt timing. The Mesen
  integration adds independent complete-ROM evidence for this example only.
  Physical hardware has not been verified.
- Fuzzing entry points and a sanitizer CI job are not evidence of exhaustive
  fuzz campaigns. Project outputs are input-protected, not fully transactional
  under I/O failure. Further hardening, performance and SPC700 follow this baseline.

Publishing a release, selecting a tag and uploading release artifacts remain
maintainer actions after the candidate's CI/review checks have passed.
