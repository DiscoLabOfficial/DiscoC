# Multi-file RAM result buffer

This example writes `42` to a host-agreed word at RAM offset `$0400`, rather
than relying on a compiler-private local variable's stack offset.

From the repository root, with the tools on PATH:

```sh
discc examples/ram_result/main.dc -o main.o
discc examples/ram_result/math.dc -o math.o
discld main.o math.o --origin 0x706000 --init-runtime \
  --ram-bank 0 --stack-pointer 0x2000 --emit-asm final.s -o ram-result.bin
```

Store the binary wherever convenient in the SNES ROM, copy it to cartridge
RAM `$70:6000`, grant GSU RAM access, and start `PBR=$70`, `R15=$6000`.
The startup selects data/stack bank `$70` and initializes R10. Reserve writable
RAM for the descending stack around/below `$2000`, keep it away from code and
the result buffer, and do not start at `main` directly (that skips startup).
After STOP and CPU-access restoration, the word at `$70:0400` is `42`. The
buffer leaves room for checked-address code when the example is also linked
at RAM offset zero; regression tests enforce that the payload ends before it.

`--ram-bank 1` instead selects `$71:0400` for the result and `$71:2000` for the
initial stack word; it does not change PBR or the code origin. Only use bank 1
on cartridges that provide it. This is a fixed-origin example, not PIC.

See [GSU loading and startup](../../docs/gsu-loading.md) for the complete contract.
