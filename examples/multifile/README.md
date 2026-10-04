# Multi-file example

This example separates a function declaration from its definition. The
prototype in `main.dc` is checked by the compiler but does not emit a second
function body; the definition in `math.dc` provides the exported symbol that
the linker resolves.

With `discc` on `PATH`, run from this example's directory:

```sh
discc build
```

The included `discoc.toml` builds both units, links for `$70:0900`, initializes
RAMBR/R10, and writes `build/0-main.o`, `build/1-math.o`,
`build/multifile.bin`, and `build/final.s`. Copy the payload to `$70:0900` and
enter at that address; the host still owns SNES/GSU bus setup. The assembly is
the exact linked payload, not the compiler's relocatable export.

From the repository root, use `discc build --config examples/multifile/discoc.toml`.
Relative paths stay relative to the manifest, regardless of the terminal's
working directory. CLI flags can override it without source edits. See
[project manifests](../../docs/project-manifest.md). The commands below show
the independent-tool workflow with its original defaults.

From a build directory containing `discc` and `discld`:

```sh
discc ../examples/multifile/main.dc -o main.o
discc ../examples/multifile/math.dc -o math.o
discld main.o math.o -o multifile.bin
```

The resulting file is a linked GSU payload. It still needs to be integrated
into a valid SNES ROM image before it can run on an emulator or console.

The default link origin is `$00:8000`. Place the first payload byte at that GSU
execution address, set `PBR` to `$00`, and start `R15` at `$8000`; the linker patches the
`add` call to `$8000` plus its offset in the concatenated code. These are
absolute addresses within the current GSU program bank, not offsets relative
to wherever the payload happens to be loaded. The linker does not prepend
padding or create a ROM header.

For optional RAM execution, the linker can choose the origin directly:

```sh
discld main.o math.o --origin 0x706000 --init-runtime --ram-bank 0 \
  --stack-pointer 0x2000 --emit-asm multifile-final.s -o multifile.bin
```

Copy the first byte to `$70:6000`, start with `PBR=$70` and `R15=$6000`, and
grant GSU RAM access. The bootstrap selects data/stack bank `$70` and sets R10.
The binary remains fixed-origin; `.incbin` ROM storage does not relocate it.
For host-owned initialization, omit `--init-runtime` and initialize RAMBR/R10
before entering generated code. Final assembly is in DiscoC's dialect, not
WLA-DX. See [GSU loading](../../docs/gsu-loading.md).

Alternatively, compile both units with `--execution-memory ram`.
LoROM remains the default mapping and RAM's default origin is `$70:8000`.
`--origin 0x710000` overrides it with `$71:0000`. Placement survives both direct
and assembly workflows. Source-level `set` directives are no longer accepted.
The linker
rejects inaccessible origins and payloads that cross a program-bank boundary.
RAM execution requires copying the payload into cartridge RAM first. Configure the SNES
host code to use the corresponding program bank and PC, permit GSU access to
the required ROM/RAM, select the RAM bank, and initialize `R10` to an even
address in writable GSU RAM with sufficient room for the descending stack.
Keep stack storage separate from the payload when executing code from RAM.

`main` ends with `STOP` rather than returning to a GSU caller. With this
example, its saved entry registers occupy four bytes below the initial stack
pointer (the empty-descending stack first stores at the initial pointer).
`result` is a local stack variable, **not** a public return buffer: with an
initial R10 of `$2000`, it is at RAM offset `$1FFA`. For a stable host-visible
result address, use the [RAM-result example](../ram_result/README.md).
The automated multi-file test executes the linked payload and checks
that `result` is `42` and that the stack returns to the entry frame.
