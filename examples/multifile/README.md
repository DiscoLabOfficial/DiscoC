# Multi-file example

This example separates a function declaration from its definition. The
prototype in `main.dc` is checked by the compiler but does not emit a second
function body; the definition in `math.dc` provides the exported symbol that
the linker resolves.

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

For optional RAM execution, put `set execution_memory = ram;` in both `.dc`
files. LoROM remains the mapping default and the execution origin becomes
`$70:8000`. An explicit `set code_start_address = 0x710000;` can override this
with `$71:0000`. Placement is preserved in both direct and assembly workflows.
The linker
rejects inaccessible origins and payloads that cross a program-bank boundary.
RAM execution requires copying the payload into cartridge RAM first. Configure the SNES
host code to use the corresponding program bank and PC, permit GSU access to
the required ROM/RAM, select the RAM bank, and initialize `R10` to an even
address in writable GSU RAM with sufficient room for the descending stack.
Keep stack storage separate from the payload when executing code from RAM.

`main` ends with `STOP` rather than returning to a GSU caller. With this
example, its saved entry registers occupy four bytes below the initial stack
pointer. The automated multi-file test executes the linked payload and checks
that `result` is `42` and that the stack returns to the entry frame.
