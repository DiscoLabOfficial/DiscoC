# Imported interfaces and aligned static storage

Compile from the repository root:

```sh
discc examples/language_modules/main.dc -o main.o
discc examples/language_modules/math.dc -o math.o
discld main.o math.o --init-runtime --ram-origin 0x402 -o modules.bin
```

The common math.dci interface provides prototypes, an extern RAM struct,
constexpr and enum definitions. Implementations are still compiled independently.
The linker aligns both 8- and 16-byte objects even though the selected RAM
origin is only word-aligned. The model checks 149 at near RAM $0100 and 16 at
$0102. Like other examples, this is a fixed-origin GSU payload, not a SNES ROM.

Use `--check --target gsu` or `--check --target spc700` to check portable
frontend/IR rules without machine output. SPC700 machine emission is not yet
implemented. For RAM code loading, link with an explicit `--origin`; source
files do not contain placement directives.
