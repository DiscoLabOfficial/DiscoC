# Getting Started with DiscoC

This tutorial builds a two-file GSU program that writes 42 to an agreed RAM
address. A payload is not a complete SNES ROM: the host must load it and
configure the GSU before execution.

## Build the Tools

Follow [Building DiscoC](building.md). With CMake 3.21+, Ninja, and a C++23
compiler, the Linux/macOS and Windows MSVC Developer-shell path is:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Tools are in `build/release/bin`. Windows MinGW users select `release-mingw`
instead and use `build/release-mingw/bin`. Add the chosen directory to your
current shell's `PATH`; all following commands assume the tools are available.

To try an existing project from the repository root:

```sh
discc build --config examples/multifile/discoc.toml
```

That original example demonstrates prototypes and a local result. The tutorial
below instead uses source imports and an explicit host-visible RAM result.

## Create Two Source Files

Place `main.dc` and `math.dc` in a new project directory:

```c
// main.dc
import "math.dc";

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

`import` exposes declarations without pasting definitions. Project builds
discover the imported `.dc` implementation and compile it once into a separate
object. Symbols are public by default; `export` makes that explicit, while
`internal` hides implementation details. Declaration-only `.dci` interfaces
do not automatically add a matching implementation.

## Add a Manifest

Save `discoc.toml` alongside the source files:

```toml
[project]
name = "multifile"
target = "gsu"
sources = ["main.dc"]

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

From that project directory:

```sh
discc build
# Equivalent explicit selection:
discc --project discoc.toml
```

Outputs are `build/0-main.o`, `build/1-math.o`, `build/multifile.bin`,
and `build/final.s`. Manifest paths resolve relative to the manifest.
Configuration precedence is built-in defaults, then manifest, then explicit CLI
options. This is a full project rebuild, not a package manager or incremental
build system.

Imports search the importing file's directory first, then configured
`[compiler] import_paths`. See [project manifests](project-manifest.md)
for graph limits, paths, CLI overrides, and the complete schema.

## Load and Execute

For this manifest, the SNES host must:

1. Copy the payload's first byte to `$70:0900`.
2. Configure GSU access and the required SNES/GSU registers, and enter with
   `PBR=$70`, `R15=$0900`.
3. Wait for normal STOP, restore the appropriate CPU RAM access, and read the
   word at `$70:0100`; it should be 42.

The bootstrap initializes RAMBR and R10 before entering `main`. It does not
configure the host, display, bus ownership, or every GSU register.
The host must reserve payload, result, and stack memory without overlap.

This binary is **fixed-origin, not position-independent**. WLA-DX `.incbin`
can store it in a ROM section, but copying it elsewhere does not relocate its
embedded addresses. Relink to change its execution address. The complete
payload must fit one supported GSU program bank.
See [GSU loading and startup](gsu-loading.md) for placement and safety limits.

For a complete, reproducible SNES ROM rather than a bare payload, follow
the [official triangle example](../examples/snes/triangle/README.md).

## Choose an Optimization Policy

```sh
discc build -O0
discc build -O1
discc build -O2
discc build -Os
```

O0 is the default baseline; `-O` aliases O1. O1 performs local optimization,
O2 adds global SSA/CFG and speed-oriented backend work, and Os prioritizes
emitted size. Smaller does not necessarily mean faster.

To select a project default:

```toml
[compiler]
optimization_level = 2
```

Use 0, 1, 2, or the string `"s"`. The legacy `optimize = true` selects O1;
do not specify both keys. Explicit CLI settings win. See
[optimization](optimization.md) for preserved semantics and tradeoffs, and
[benchmarks](../benchmarks/README.md) for reproducible measurements.

## Use the Tools Separately

Run these commands in the tutorial project directory. The separate compiler
checks imports but emits only the selected unit, so compile both:

```sh
discc main.dc -o main.o
discc math.dc -o math.o
discld main.o math.o --origin 0x700900 --init-runtime \
  --ram-bank 0 --stack-pointer 0x2000 --emit-asm final.s -o multifile.bin
```

The examples use POSIX shell line continuations; in PowerShell, put the linker
command on one line.

There are two different assembly exports. Compiler export is relocatable
assembly for one unit:

```sh
discc main.dc --emit-asm -o main.s
discas main.s -o main.o
```

Linker `--emit-asm final.s` exports the entire linked payload, including startup
and resolved addresses. Reassemble that final export without adding startup or
changing the origin:

```sh
discas final.s -o reconstructed.o
discld reconstructed.o -o reconstructed.bin
```

`reconstructed.bin` matches the linked payload byte for byte.
This is **DiscoC assembly, not native WLA-DX syntax**. See
[the assembler guide](assembler.md) and [final assembly export](gsu-loading.md).

## Inspect Without Emitting

```sh
discc --check -Wall -Werror main.dc
discc main.dc --emit-ir
discc --target spc700 --check main.dc
```

SPC700 currently supports frontend/IR workflows only, not executable output.
For language rules, consult the [specification](language-spec.md).
