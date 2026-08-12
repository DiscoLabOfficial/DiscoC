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
