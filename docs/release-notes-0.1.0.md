# DiscoC 0.1.0

DiscoC 0.1.0 establishes the first documented and tested language/toolchain
baseline, with an end-to-end functional SuperFX target.

This is the first experimental baseline release, not a declaration that the
compiler is production-ready or that generated code is optimal.

## Included

- `discc`: compile source or build a project from `discoc.toml`.
- `discas`: assemble DiscoC assembly into relocatable GSU objects.
- `discld`: link a fixed-origin GSU payload, validate placement, optionally
  initialize runtime state and export byte-exact linked assembly.
- The Baseline 0.1 language specification, toolchain/ABI/loading documentation,
  freestanding library modules, examples and a reproducible SNES triangle host.

Language foundations include defined small-integer semantics, `bool`,
`const`/`volatile`, ROM/RAM and near/far data pointers, memory-resident aggregates,
mutable globals, linkage, imports and compile-time layout tools. The canonical
backend uses verified typed IR, a CFG and linear-scan scalar allocation.

SuperFX plotting is stateful: `cursor.x/y` map to R1/R2, `pixel` uses PLOT's
hardware X increment, and `read_pixel`/`flush` use observable RPIX. Direct ROM
byte colors can use GETC; RAM/computed colors use a load/evaluation followed by
COLOR. Bitmap metadata is separate from host-side SNES setup.

## Start here

Extract the package for your operating system and add its `bin` directory to
`PATH`. The source build happy path, with a suitable C++23 compiler and Ninja, is:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Windows MinGW users can substitute the `release-mingw` preset. To build a
DiscoC project, run `discc build` in the directory containing `discoc.toml`.
See [building](building.md), the [manifest guide](project-manifest.md),
[GSU loading](gsu-loading.md) and the [SNES triangle](../examples/snes/triangle/README.md).

## Verification

The baseline has 91 native CTest entries, including 22 language-conformance
categories and GSU graphics, ABI, object/linker, runtime, malformed-input and
byte-equivalence regressions. Windows MSVC/MinGW, Ubuntu and macOS builds/tests
and Ubuntu ASan/UBSan passed in hosted CI. The experimental C++14 DJGPP tools
cross-compile; DOS execution is not verified.

A separate local WLA-DX + Mesen complete-ROM check verifies the SNES triangle's
pixels/bitplanes, startup, STOP/result, VRAM/tilemap and deliberate failure path.
This is independent emulator evidence for that example, not physical-hardware
validation or a complete timing/bus oracle. See the dated
[readiness record](release-readiness-0.1.md).

## Pre-1.0 guarantees and limitations

- Language syntax may still evolve.
- ABI and object format may change. Rebuild all tools and objects together
  when upgrading; current emission is little-endian object format v7.
- The optimizer is immature; generated code is not yet expected to be optimal.
- SPC700 provides frontend/IR foundations, not a fully supported executable target.
- A `.bin` is a GSU payload, not a complete SNES ROM. Payloads are fixed-origin
  and single-program-bank; moving one to a new execution address requires relinking.
- The host owns SNES/PPU setup, copying and CPU/GSU memory access coordination.
  Without optional runtime initialization it also owns RAMBR, stack and static
  initialization. Separately placed RAM code and ROM constants are not supported.
- Aggregate-by-value and interbank code-call ABIs are not implemented.
- Physical hardware, complete timing and exhaustive fuzzing are unverified.

Further correctness hardening, deployment visibility, optimization and an
executable SPC700 backend follow this baseline; they are not prerequisites
for the first SuperFX release.
