# DiscoC project manifests

`discoc.toml` stores build, placement, runtime, and output configuration outside
the source language. It does not replace object files, `.dci` interfaces, or a
SNES host program. The compiler and linker can still be used separately.

## Build a project

Build the toolchain first with `cmake --preset release` and
`cmake --build --preset release`; its executables are in `build/release/bin`.
Windows MinGW uses `release-mingw` and `build/release-mingw/bin`. Add the chosen
directory to the current shell's `PATH`, or invoke `discc` by its full path.
See [building](building.md); this compiler output location is independent of
a user's manifest-selected project output directory.

With `discc` on `PATH`, run this in the directory containing `discoc.toml`:

```sh
discc build
```

From another directory, select the manifest explicitly:

```sh
discc build --config examples/multifile/discoc.toml
discc --project examples/multifile/discoc.toml
```

The two explicit forms are equivalent. The manifest selector is optional only
for `build`; `--project` requires a filename and must be the first argument.
There is no automatic config discovery for ordinary one-source compilation or
object linking. Only the current directory is searched for `discc build`.

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
entry = "main"

[output]
directory = "build"
binary = "multifile.bin"
assembly = "final.s"
```

This builds `build/0-main.o`, `build/1-math.o`, `build/multifile.bin`, and
`build/final.s`, relative to the manifest's directory. Indexed object names
preserve source order and prevent collisions between equal basenames. Final
assembly includes startup and resolved addresses and reassembles byte-for-byte
to the linked payload; it is DiscoC assembly, not WLA-DX syntax.

Compilation and linking run inside `discc`, using the same drivers as the
separate tools. No shell commands or sibling executables are invoked. Source
order is link order; put the entry unit first when using host-owned startup.
With runtime initialization, `entry` selects the exported CODE symbol to enter.

The result is still fixed-origin: copy the first byte to `$70:0900` and enter
with `PBR=$70`, `R15=$0900`. The host must configure GSU bus access and other
SNES registers. A manifest does not make the payload position-independent or
produce a complete ROM. See [GSU loading](gsu-loading.md).

## Precedence and paths

Precedence is **built-in defaults, then manifest, then explicit CLI options**.
Among repeated CLI options, the last one wins. The position of `--config` does
not affect precedence. Malformed or out-of-range manifest values are rejected
even when a CLI option would override them.

Repeatable import-path options are a list, not a last-value setting: any explicit
CLI import paths replace `compiler.import_paths`, retaining their supplied order.

```sh
discc build --origin 0x701000 --config discoc.toml --ram-bank 1 \
  --stack-pointer 0x3000 --output-dir experiment
```

All relative manifest paths resolve from the manifest's directory. Relative
`output.binary`/`output.assembly` resolve inside `output.directory`. Absolute
paths remain absolute. CLI paths (`--output-dir`, `-o`, `--emit-asm`) resolve
from the terminal's working directory. `--output-dir` also moves relative
manifest-named binary/assembly files; explicit `-o`/`--emit-asm` paths win.
Use forward slashes or TOML literal strings for Windows paths; basic strings
require escaped backslashes. Drive-relative paths such as `C:build` are rejected.

`--no-init-runtime`, `--no-host-initialized-globals`, and `--no-emit-asm` override
enabled manifest settings. Disabling startup omits manifest-only `entry` and
`stack_pointer` settings; supplying either explicitly on the CLI still requires
`--init-runtime`. Disabling assembly does not delete an existing assembly file.

## Use existing tools with a manifest

```sh
discc --config discoc.toml main.dc -o main.o
discc --config discoc.toml math.dc -o math.o
discld main.o math.o --config discoc.toml
```

Ordinary `discc --config` uses target/placement/import-path defaults, not `project.sources`
or linked output names. Its input and `-o` still name one compilation unit.
`discld --config` uses target, placement, runtime, and output defaults but does
not compile sources. It checks that target/mapping agree with the objects;
`origin` rebases all objects using the normal relocation pipeline. It does not
silently change an object's cartridge mapping. `discas` does not read manifests;
the compiler's assembly metadata already preserves target/placement.

In ordinary compilation, `--emit-asm` is a switch selecting relocatable assembly
instead of an object. In **project build mode**, `--emit-asm <file>` selects the
final linked assembly output. `discc build --check` analyzes and verifies each
source without generating objects, creating output directories, or linking.
It cannot diagnose unresolved cross-unit symbols or link-time layout failures.

## Source dependencies and import paths

```toml
[project]
sources = ["src/main.dc"]

[compiler]
import_paths = ["src", "lib"]
```

`import "math.dc";` exposes public declarations without inserting function
bodies or defining storage in the importing unit. `project.sources` lists
roots: the build discovers their transitive `.dc` imports and compiles every
implementation once into a separate object. Shared dependencies and sources
already listed as roots are not compiled twice. The build keeps a bounded
dependency graph, diagnoses cycles, and analyzes/compiles dependency-first.

Explicit root order remains link order. Automatically discovered sources follow
the roots in deterministic dependency order; indexed object names use that link
order, not compilation order. Runtime `entry` selects the entry symbol normally.

Imports search the importing file's directory first, then `compiler.import_paths`
in order. Manifest directories resolve relative to the manifest. The first
existing file wins: invalid local source is an error, not a reason to try another
directory. Transitive imports use their own file's directory. Paths can contain
spaces; imports must name relative `.dc` or `.dci` files, not logical module names.

```sh
discc build --config discoc.toml --import-path experiment --import-path shared
discc --check -I shared main.dc
discc --check -Ishared main.dc
```

CLI import directories resolve from the terminal's working directory and replace
the manifest list, regardless of `--config` position. Local-file precedence still
applies. There are at most 64 configured directories. `--check` follows the same
graph without producing objects or linking; SPC700 remains frontend/IR-only.

An ordinary one-source command also discovers/checks imports, but emits only
that unit's object or assembly. Compile/link dependencies explicitly or use
project build mode. `.dci` imports remain declaration-only: they do not infer or
automatically compile a matching implementation file. See
[language module rules](language-spec.md#13-modules-and-interfaces) and the
[source-import example](../examples/source_imports/README.md).

## Schema

All keys are optional except a nonempty `project.sources` for project builds.
Unknown tables/keys and duplicate definitions are errors.

| Table | Key | Default / contract |
| --- | --- | --- |
| `project` | `name` | `new`; 1–64 ASCII letters, digits, `_`, `-` |
| `project` | `target` | `gsu`; `superfx` alias, or `spc700` |
| `project` | `sources` | Ordered nonempty array of up to 128 `.dc` roots; imports discover additional sources |
| `compiler` | `import_paths` | `[]`; ordered array of up to 64 manifest-relative search directories |
| `target.gsu` | `memory_mapping` | `lorom`; explicit `hirom` retained |
| `target.gsu` | `execution_memory` | Inferred from origin; optional `rom` or `ram` constraint |
| `target.gsu` | `origin` | `$00:8000`; `$40:8000` for HiROM, `$70:8000` for explicit RAM execution |
| `target.gsu` | `ram_bank` | `0` (`$70`); `1` selects `$71` for near data/stack |
| `target.gsu` | `ram_origin` | `0x0400`; even 16-bit static-storage origin |
| `target.gsu` | `rom_bank` | Execution ROM bank, or `0` for RAM execution; accessible ROM bank |
| `target.gsu` | `stack_pointer` | `0x2000`; even value in `0x0008..0xFFFE`, used with startup |
| `target.gsu` | `initialize_runtime` | Alias for `runtime.initialize`; do not specify both |
| `target.spc700` | `origin` | Optional 16-bit future-linker setting; not applied by frontend-only checks |
| `runtime` | `initialize` | `false`; initialize data/stack and enter `entry` |
| `runtime` | `entry` | `main`; exported CODE symbol, used with startup |
| `runtime` | `host_initialized_globals` | `false`; host supplies global initialization instead of startup |
| `output` | `directory` | `build`, relative to manifest |
| `output` | `binary` | `<project.name>.bin`, or `new.bin` |
| `output` | `assembly` | Omitted; final linked assembly when specified |

`[target.superfx]` aliases `[target.gsu]`; specifying both is a duplicate-table
error. A CLI `--target` selects that target's table without leaking another
target's placement settings. Common runtime/output configuration remains shared.
GSU address windows, alignment, relocation spans, stack/payload overlaps, and
bank boundaries are checked by the existing compiler/linker rules.

`spc700` currently supports project `--check` and one-source IR inspection only;
project emission fails before creating outputs because there is no executable
SPC700 backend. Its stack/runtime configuration is intentionally not invented.

## TOML support and limits

The dependency-free, C++14-compatible reader implements a documented subset of
[TOML 1.0](https://toml.io/en/v1.0.0) sufficient for this schema:

- Bare keys and the table headers above; whitespace and `#` comments.
- Single-line basic (`"..."`) and literal (`'...'`) UTF-8 strings. Basic strings
  decode standard escapes and Unicode scalars; paths reject control characters.
- Signed 64-bit decimal integers, unsigned-form hexadecimal/octal/binary
  integers, valid numeric underscores, and lowercase `true`/`false`. Schema
  integers must additionally fit the nonnegative range of their setting.
- String arrays, including multiple lines, comments, and trailing commas.
- LF/CRLF input; diagnostics identify manifest, line, and byte column.

Unsupported TOML constructs are rejected, not silently ignored: quoted/dotted
assignment keys, multiline strings, inline tables, arrays of tables, non-string
arrays, floats, and dates/times. The reader is bounded at 64 KiB per file,
4,096 bytes per string/path, 128 source entries, 64 import directories,
128 bytes per key/table name, 64 keys, and 8 tables. It does not recurse over
input-controlled structures.

The module graph is separately bounded at 128 files, 32 import levels,
16 MiB per file and 32 MiB total source, counting both roots and dependencies.

## Failure behavior and scope

Project builds reject duplicate sources, output collisions, and outputs aliasing
the manifest, sources, or imported source/interface dependencies before object writes. Existing
file identity and normalized paths are checked; native C++17+ builds additionally
canonicalize symlink paths. Syntax-only input discovery precedes compilation so
an output cannot destroy an imported file used by a later unit. Any compilation
failure stops the project before linking, including when old objects exist.
Diagnostic failures leave an existing final payload untouched.

This is a full rebuild, not an incremental build system or a file-write
transaction: successful earlier objects may remain when a later unit fails,
and an I/O failure during final writing can leave incomplete output. It does
not run arbitrary commands, expand shell variables/globs, download packages,
provide optimization profiles, or support section/region linker scripts.
Future detailed memory layouts and SPC700 runtime settings need separate
contracts; unknown future settings are errors today.
