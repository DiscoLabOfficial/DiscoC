# DiscoC: A Systems Language and Cross-Compilation Toolchain

[![CMake](https://github.com/DiscoLabOfficial/DiscoC/actions/workflows/cmake.yml/badge.svg)](https://github.com/DiscoLabOfficial/DiscoC/actions/workflows/cmake.yml)

DiscoC is a freestanding systems programming language and compiler, assembler,
and linker for specialized hardware. Its C-like syntax is paired with explicit
rules for small integers, banked memory, hardware access, and target capabilities.
It is not an ISO C implementation.

The toolchain is written in C++23, with a C++14-compatible experimental DOS
cross-build. The executable backend targets the SNES SuperFX (GSU); SPC700
currently provides a frontend and IR foundation.

> **Status: Pre-Alpha / Experimental**
> Correctness and explicit language contracts take priority over speed. GSU
> compilation, assembly, linking, and runtime regressions are implemented, but
> the language and object format can still change. A linked `.bin` is a GSU
> payload, not a complete SNES ROM.

## Supported Targets

| Target | Current support |
| --- | --- |
| `gsu` / `superfx` | Source compilation, assembly, relocatable objects, linking, and ROM/RAM execution payloads. |
| `spc700` | Language analysis, target-capability diagnostics, and verified IR inspection; no machine-code emission yet. |

Select the target through the CLI or `discoc.toml`. Hardware-specific features
are capability-checked rather than treated as portable language operations.

## Language

The refactored language has a documented [language contract](docs/language-spec.md):

- **Small, predictable values:** signed-by-default `byte` and `word`,
  `unsigned` variants, byte-sized `bool`, and transparent `type` aliases
  including `i8`, `u8`, `i16`, and `u16`.
- **Deliberate numeric rules:** modular overflow for signed and unsigned integers,
  explicit narrowing and signedness conversions, boolean comparisons,
  short-circuit logic, bitwise operators, checked shifts, and software `/` and
  `%` on GSU.
- **Independent access properties:** `rom`/`ram` specify address space,
  `const` restricts modification, and `volatile` preserves observable memory
  accesses through the IR and backend.
- **Bank-aware data pointers:** 16-bit near pointers and four-byte far
  descriptors, pointer arithmetic, indexing, `->`, `null`, and checked casts.
  Far accesses restore the configured near data bank. Typed word accesses
  require two-byte alignment.
- **Memory-resident aggregates:** structs, arrays of structs/pointers, initializer
  lists, and character/string literals. Unsupported aggregate-by-value
  operations are diagnosed instead of silently using an incompatible ABI.
- **Static storage and visibility:** mutable RAM globals, ROM data, explicit
  initialization, and `internal`/`export`/`extern` linkage.
- **Compile-time tools:** `constexpr`, enums, constant-expression array sizes
  and case labels, `sizeof`, `alignof`, `offsetof`, and `static_assert`.
- **Structured control flow:** compound assignments, `++`/`--`, real
  `for` loops with correct `continue` behavior, and explicit
  `fallthrough;` annotations.
- **Explicit hardware context:** lexical `plot { ... }` blocks with arbitrary
  coordinate expressions, `plot.x`/`plot.y`, and GSU drawing operations.
- **Attributes and interfaces:** `@cache`, `@packed`, `@align(N)`,
  `@target(...)`, and top-level `@cfg(gsu)`/`@cfg(spc700)` selection.
  Relative `.dci` module interfaces share declarations and types without a C
  textual preprocessor.

Warnings cover shadowing, unused declarations, uninitialized reads, unreachable
code, and implicit fallthrough. `-Wall` also enables expensive-helper warnings;
`-Werror` turns enabled warnings into errors.

## Toolchain

- `discc` compiles one source file or builds a multi-file project from
  `discoc.toml`. `--check` checks the frontend/IR without emitting objects;
  `--emit-ir` prints the verified IR.
- `discas` assembles DiscoC's ca65-like assembly dialect into relocatable
  objects.
- `discld` resolves symbols, validates placement and relocations, and produces
  a fixed-origin payload with optional runtime initialization and final
  assembly export.

The canonical backend uses typed IR and a control-flow graph, stable resolved
symbol IDs, a dedicated IR verifier, and linear-scan register allocation.
GSU lowering includes hardware-loop selection, switch dispatch optimization,
and branch relaxation. The ABI uses `R9` as frame pointer and `R10` as stack
pointer, with regression coverage for nested calls and stack restoration.

Objects use a versioned little-endian format with CODE/DATA/RAM images,
visibility, relocations, and alignment metadata. The reader and linker reject
malformed records, invalid relocation spans, and unsupported bank crossings.
Rebuild the tools together when the object format changes.

## Building from Source

Install a C++23-capable compiler. CMake is the primary build system; the helpers
also support direct GCC/Clang builds.

```powershell
# Windows: build the tools and run the tests
.\build.cmd -Test

# Direct GCC build, without configuring a CMake project
.\build.cmd -Backend Direct -Compiler g++
```

```bash
# Linux/macOS: build the tools and run the tests
bash build.sh --test

# Alternatives
make test
bash build.sh --backend direct --compiler clang++
```

The default helper output is `build/<backend>-<configuration>`, such as
`build/cmake-Release`. Multi-configuration CMake generators add a configuration
subdirectory. Add that executable directory to `PATH`, or invoke the tools
by their full paths in the examples below.

Manual CMake builds remain supported:

```bash
git clone https://github.com/DiscoLabOfficial/DiscoC.git
cd DiscoC
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --config Release --parallel
ctest --test-dir build/native -C Release --output-on-failure
```

See [Building DiscoC](docs/building.md) for prerequisites, MinGW/MSVC selection,
static linking, direct-build tests, and the retained experimental DOS
cross-build. Use a fresh build directory when changing compilers or generators.

## Quick Start: A Multi-File Project

Create these two source files. This example writes 42 to a host-agreed RAM
address rather than treating registers after `STOP` as a result API.

```c
// main.dc
word add(word a, word b);

void main() {
    volatile word* output = (volatile word*)0x0100;
    *output = add(30, 12);
}
```

```c
// math.dc
word add(word a, word b) {
    return a + b;
}
```

Place `discoc.toml` alongside them:

```toml
[project]
name = "multifile"
target = "gsu"
sources = ["main.dc", "math.dc"]

[target.gsu]
memory_mapping = "lorom"
execution_memory = "ram"
origin = 0x700900
ram_bank = 0x00
stack_pointer = 0x2000

[runtime]
initialize = true

[output]
directory = "build"
binary = "multifile.bin"
assembly = "final.s"
```

Then run:

```bash
discc build
# Equivalent explicit selection:
discc --project discoc.toml
```

This produces `build/0-main.o`, `build/1-math.o`,
`build/multifile.bin`, and `build/final.s`. The result is stored at
`$70:0100`; load the payload at `$70:0900` and enter there with the host setup
described below.

To build the repository's original multi-file example from the repository root:

```bash
discc build --config examples/multifile/discoc.toml
```

Configuration precedence is **built-in defaults → manifest → explicit CLI
options**. Manifest paths resolve relative to the manifest, not the terminal's
working directory. Sources, libraries, and outputs are explicit; this is not a
package manager or an incremental build system.

See [Project manifests](docs/project-manifest.md) for the supported TOML subset,
schema, output-path rules, CLI overrides, and frontend-only project checks.

## Individual Commands and Assembly Export

The separate compile/link workflow remains available for the same source files:

```bash
discc main.dc -o main.o
discc math.dc -o math.o
discld main.o math.o --origin 0x700900 --init-runtime \
  --ram-bank 0 --stack-pointer 0x2000 --emit-asm final.s -o multifile.bin
```

There are two distinct assembly exports:

```bash
# Relocatable assembly for one compilation unit
discc main.dc --emit-asm -o main.s
discas main.s -o main.o

# Inspect target-independent IR, or check language rules only
discc main.dc --emit-ir
discc --check -Wall -Werror main.dc

# SPC700 currently supports frontend/IR workflows only
discc --target spc700 --check main.dc
```

Compiler assembly preserves symbols and relocations. Without edits, assembling
and linking it with the same options produces the same bytes as direct object
compilation.

`discld --emit-asm final.s`, or `discc build --emit-asm final.s`, exports the
**final linked payload**, including startup and resolved addresses. Reassemble
it without adding startup or changing its origin:

```bash
discas final.s -o reconstructed.o
discld reconstructed.o -o reconstructed.bin
```

`reconstructed.bin` matches the linked payload byte for byte. This is DiscoC
assembly, not native WLA-DX syntax; `.byte` directives preserve encodings when
needed.

## GSU Placement and Host Integration

LoROM with ROM execution at `$00:8000` is the default. HiROM remains an explicit
`--memory-mapping hirom` choice, independent of RAM/ROM execution.
Placement belongs in the CLI or manifest, not source-level `set` directives.

For the quick-start RAM payload, the host must copy it to `$70:0900`, set
`PBR=$70` and `R15=$0900`, grant the required bus access, and configure the
other SNES/GSU registers. The bootstrap selects near RAM/stack bank `$70`,
initializes `R10`, and enters `main`. After normal completion, the host can
read the word at `$70:0100`.

The binary is fixed-origin, **not position-independent**. Storing it with
WLA-DX `.incbin` does not relocate it. Relink to change its execution address;
the complete payload must fit one accessible program bank.

Mutable globals begin at near RAM offset `$0400` by default; `--ram-origin`
changes that placement. Use `--init-runtime` for static initialization, or
explicitly accept the host-owned contract with `--host-initialized-globals`.
Without runtime initialization, the host also owns the initial stack/data-bank
setup.

Far data accesses do not implement interbank code calls. RAM execution does not
yet provide separate ROM placement for ROM-qualified constants. The host remains
responsible for real cartridge capacity, memory reservations, stack space, and
valid ROM integration. See [GSU loading and startup](docs/gsu-loading.md).

## Freestanding Libraries and Examples

`lib/core` provides near-RAM byte copy/move/fill routines and signed Q8.8/Q12.4
fixed-point helpers. Fixed-point aliases are transparent integer types with
explicit scaling helpers, not new primitive types or floating-point support.
`lib/targets/gsu` contains target-specific graphics wrappers.

Import a library's `.dci` interface and explicitly compile/link its matching
`.dc` implementation, or list it in `project.sources`. Importing an interface
does not automatically add an implementation to the build.

Useful examples:

- [Language core](examples/language_core/README.md): qualifiers, globals,
  numeric operators, and lexical plotting.
- [Module interfaces](examples/language_modules/README.md): shared types,
  declarations, and multi-file linking.
- [Fixed-point and memory helpers](examples/fixed_point/README.md): portable
  core modules and explicit library linking.
- [RAM result](examples/ram_result/README.md): host-visible output and startup.

See the [freestanding library contract](docs/freestanding-library.md) for API,
rounding, overflow, and memory-access limits.

## Validation and Current Limits

CTest covers language conformance, negative diagnostics, parser/object
hardening, IR verification, multi-file linking, placement boundaries, project
manifests, assembly/binary equivalence, and GSU execution regressions.
Optional sanitizer and fuzzing configurations are documented in
[Testing](docs/testing.md).

Execution tests use an instruction-level GSU model, not a complete SNES
emulator or physical hardware. Test a complete ROM in its deployment environment.

SPC700 machine emission and DSP libraries, interbank calls, aggregate-by-value
ABI support, function pointers, and unions remain future work. Inline assembly
and interrupt/naked/section/calling-convention attributes have
[design contracts](docs/inline-assembly-contract.md), not implemented source
semantics. BF16/FP16 and wider fixed-point arithmetic are also deferred.

See [ROADMAP.MD](ROADMAP.MD) for completed milestones and future work.

## Technical Documentation

- [Language specification](docs/language-spec.md): supported semantics and
  compatibility limits.
- [Project manifests](docs/project-manifest.md): TOML configuration and builds.
- [Architecture](docs/architecture.md): compiler pipeline, ABI, and ownership.
- [IR and CFG](docs/ir-cfg.md): intermediate representation and verification.
- [Object format](docs/object-format.md): serialization and relocation contracts.
- [Assembler](docs/assembler.md): supported assembly syntax and workflows.
- [GSU loading](docs/gsu-loading.md): execution origins and host initialization.
- [SPC700 foundation](docs/spc700-target.md): target model and planned backend.
- [Building](docs/building.md) and [Testing](docs/testing.md): build paths and
  regression tooling.

## License

DiscoC is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.**

This program is distributed in the hope that it will be a useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
