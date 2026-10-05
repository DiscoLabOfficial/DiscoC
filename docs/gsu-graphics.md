# Stateful SuperFX graphics

DiscoC exposes SuperFX plotting as persistent hardware state, not a sequence
of independent drawing calls. These operations require the GSU graphics
capability; SPC700 rejects them during analysis and IR verification.

## Cursor and pixels

```c
void main() {
    plot {
        options;
        color 5;
        cursor.x = 10;
        cursor.y = 20;
        pixel; // Draw (10,20); X becomes 11.
        pixel; // Draw (11,20); X becomes 12.
        byte c = read_pixel at (10, 20);
        flush;
    }
}
```

`cursor.x` and `cursor.y` are word-sized R1/R2 accesses. Their addresses cannot
be taken. PLOT uses the low eight coordinate bits, writes the pixel caches, and
increments the full R1 register once, even for a transparent pixel. Y does not
change. There is no software increment or implicit reload after `pixel;`.
A cursor value assigned to an ordinary variable is a snapshot, not a live alias.

`at (x, y);` assigns X then Y, evaluating each expression once. Coordinates
can be arbitrary expressions. `draw at (x, y);` adds one pixel; its optional
`with color expr` clause evaluates color before either coordinate assignment.

For a scanline, initialize X once and let PLOT advance it:

```c
plot {
    color 3;
    at (0, y);
    for (word i = 0; i < width; i++) { pixel; }
    flush;
}
```

Blocks own lexical scope, not a saved/restored hardware-state transaction.
Nested plot blocks are rejected. Branches, loops, switch, break, continue and
return are supported. Ordinary calls and software arithmetic preserve the
caller's plotting cursor. Entering/leaving a block does not reset COLR/POR,
clear the framebuffer, or implicitly flush pending pixels.
It does not initialize R1/R2 either; assign the required state before use.
Cursor prefix/postfix/compound updates follow normal word expression and
single-evaluation rules. A callee's own plot operations may change COLR/POR;
the ABI's cursor preservation is not a color/options preservation promise.

## One color operation, two hardware paths

`color expr;` updates COLR after evaluating its scalar integer expression.
The backend selects the instruction; source code does not choose COLOR/GETC.

- Immediate, register, computed, function-result and RAM values use COLOR.
- A direct nonvolatile ROM byte read can use GETC. The backend computes the
  address, selects ROMB, writes R14 to start the ROM-buffer fetch, and emits
  GETC. GETC synchronizes that buffer; it has no address operand. Far access
  restores the configured near ROM bank afterward.
- Arithmetic, explicit casts, volatile reads and values copied to local/RAM
  storage retain normal expression evaluation followed by COLOR.

```c
rom const byte palette[] = { 1, 2, 3, 4 };
// Inside a plot block:
color palette[i];       // Eligible for GETC.
color palette[i] + 1;   // ROM read, arithmetic, then COLOR.
color working[i];       // RAM read, then COLOR, even if copied from palette.
```

Both COLOR and GETC apply the same POR color transformation. With
`high_nibble`, the input's high nibble becomes the new low nibble and the old
COLR high nibble is retained. With `freeze_high`, only the input's low nibble
changes; if both are set, high-nibble selection wins.

ROM-qualified data must really reside in ROM. The flat linker cannot separately
place ROM constants while executing the same payload from RAM. Copying a
ROM palette into RAM changes the access path, not the original declaration.

## Symbolic options

`options` replaces the complete POR state. Unknown or duplicate names are errors.

| Name | Effect |
| --- | --- |
| `transparent` | Skip transparent zero pixels; clear POR bit 0. |
| `dither` | Alternate COLR nibbles by X/Y parity in 2/4bpp modes. |
| `high_nibble` | Select the input's high nibble during COLOR/GETC. |
| `freeze_high` | Preserve the previous COLR high nibble. |
| `object` | Use OBJ character addressing during pixel access. |

`options;` means none of the named options: opaque zero, POR bit 0 set.
CMODE is omitted only for a known redundant setting in a straight-line block;
calls and CFG joins invalidate that knowledge. `object` changes POR plotting
layout, not the host's SCMR/SCBR or allocated framebuffer reservation.

## Reading and flushing

`read_pixel` yields a byte containing the decoded logical color index, not a
packed framebuffer byte. It executes RPIX at the current cursor, flushes both
pixel caches, and does not increment X. The optional `at (x, y)` form assigns
the cursor first. Normal byte signedness/conversion rules still apply; use an
explicit `u8` cast when interpreting indices above 127 as unsigned integers.

`flush;` is RPIX with a discarded result. It is allowed after the plot block
as well as inside it. Unused `read_pixel` expressions and `flush` remain
observable operations and cannot be removed by dead-code elimination.

## Bitmap declarations are a host contract

```c
bitmap main_screen {
    mode bitmap;
    size 256x192;
    depth 4bpp;
    base 0x0000;
}
bitmap sprites {
    mode obj;
    depth 4bpp;
    base 0x4000;
}
// Select one of these profiles in the program:
// use bitmap main_screen;
```

Bitmap sizes are 256x128, 256x160 and 256x192. OBJ has a fixed 256x256
character-addressing domain and forbids `size`. Both modes support 2bpp,
4bpp and 8bpp. Base is an offset from `$70:0000`, must be aligned to 1024
bytes, and the full framebuffer must fit the supported 128 KiB cartridge RAM
window. Actual cartridge capacity remains the host's responsibility.

`use bitmap name;` selects compile-time metadata, not a runtime mode switch.
At most one compatible profile can be selected per compilation/link; unused
declarations do not reserve memory. Multiple selected objects must agree.
The linker rejects overlap with static RAM, RAM payloads and a generated
startup's initial stack word, and incorporates the framebuffer into downward
stack-floor checks. Arbitrary pointer writes, out-of-range coordinates and
host-owned resources are not automatically reserved or clipped.

The exported assembly retains this metadata, without adding GSU instructions:

```asm
.define __DISCO_BITMAP_SCBR $00
.define __DISCO_BITMAP_SCMR $21
```

The SNES host must write SCBR and combine these SCMR mode bits with the desired
RON/RAN ownership bits before GSU execution. These are not PPU setup, ROM
headers, automatic framebuffer clearing, or runtime initialization instructions.
Selecting POR `object` with a normal bitmap does not enlarge its reservation;
declare/select OBJ mode when its full addressing domain is needed.

Build the [triangle example](../examples/plot.dc) from the repository root:

```sh
discc examples/plot.dc -o plot.o
discld plot.o --init-runtime --stack-pointer 0x8000 --emit-asm final.s -o plot.bin
```

This is a ROM payload at `$00:8000`; its 4bpp framebuffer occupies
`$70:0000-$70:5FFF`. Use SCBR `$00`, SCMR mode bits `$21`, and the required bus
ownership bits. The explicit stack is above the framebuffer. Linker startup
initializes the RAM bank and stack; the host still loads/starts the GSU and
configures graphics and bus access. See [GSU loading](gsu-loading.md).

## IR and regression contract

IR retains cursor reads/writes, `pixel`, `color`, `cmode`, and result-producing
or discarded `rpix`. Hardware effects explicitly identify R1/R2, COLR/POR,
framebuffer/pixel caches and the ROM buffer. The verifier checks context,
capabilities and color address-space types. Allocation cannot treat R1 as
unchanged across PLOT or rematerialize an observable RPIX read.

Graphics regressions execute direct and assembled payloads, compare bytes,
and inspect cursor advancement, COLOR/GETC selection, transformations,
bitplane layout, cache flushing and bitmap metadata. The independent
instruction-level oracle is not a cycle-accurate SuperFX or full SNES emulator.
ROM-buffer timing and CPU/GSU bus contention still require emulator/hardware
integration testing. Hardware checks were cross-referenced with the primary
[Ares GSU instruction implementation](https://github.com/ares-emulator/ares/blob/master/ares/component/processor/gsu/instructions.cpp)
and [SuperFX graphics implementation](https://github.com/ares-emulator/ares/blob/master/ares/sfc/coprocessor/superfx/core.cpp).

Baseline 0.1 separately validates the complete
[SNES triangle example](../examples/snes/triangle/README.md) in Mesen. Its host
consumes linked origin/SCBR/SCMR metadata, checks CPU-read results after STOP,
and displays the actual computed bitplanes. Native regressions also check the
final pixel/bitplane byte for all twelve bitmap/OBJ profiles. Neither evidence
implies exhaustive timing or physical-hardware validation.

## Migration

Replace `plot_begin`/`plot_end` with one lexical block, `plot.x/y` with
`cursor.x/y`, `plot(x,y)` with `draw at (x,y)`, `set_color(expr)` with
`color expr;`, raw `set_plot_options` masks with named `options`, and
`flush_pixels()` with `flush;`. Removed spellings produce diagnostics rather
than silently retaining conflicting behavior. `plot_x`/`plot_y` are ordinary
user identifiers, not hidden register aliases.
