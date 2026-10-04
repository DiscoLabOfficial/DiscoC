# Stateful PLOT: a rotating checkerboard triangle

DiscoC calculates a black-and-white checkerboard triangle at 64 angles, using
integer Q6 rotation about `(128,96)`. The checkerboard is sampled in the
triangle's local coordinates, so it rotates with the geometry. No frame is a
pre-rendered asset; the SNES CPU only supplies the phase and presents GSU output.
The original [colored triangle](../triangle/README.md) remains a separate test.

![64 real Mesen captures, replayed at an accelerated rate](../../../docs/images/superfx-rotating-triangle-mesen.gif)

The preview replays captured poses at **8 fps**, faster than the ROM runs.
This is a correctness demo, not a smooth animation benchmark: the tested NTSC
Mesen run with `@cache` took 3,606 SNES frames to render and check 65 poses,
including phase wrap. Software division and checked memory accesses currently
dominate the cost.
Physical hardware has not been tested.

## Build and run

With current `discc`, `discas`, `discld`, WLA-DX's `wla-65816` and `wlalink` on
`PATH`, run from the repository root:

```sh
cmake -P tests/graphics/rotating_triangle/build-snes.cmake
```

Open `build/plot-rotating-triangle/rotating-triangle.sfc` in a SuperFX-capable
SNES emulator. It rotates continuously, retaining the last completed frame
while the GSU renders the next. A failed host check displays solid red.
`rotating-triangle-negative.sfc` deliberately corrupts the result comparison
and must display red.

Select executable locations and output directories when needed:

```sh
cmake -DDISCO_TOOLS_DIR=build/cmake-Release -DOUTPUT_DIR=build/my-rotation -P tests/graphics/rotating_triangle/build-snes.cmake
```

The script also accepts `-DWLA_65816=/path/to/wla-65816` and
`-DWLALINK=/path/to/wlalink`. Quote complete `-D...=...` arguments containing
spaces. CMake 3.15 or newer is required; external tools are not bundled.
Compiler assembly and final linked assembly are both reassembled and compared
byte-for-byte with the direct payload before either ROM is built.

To build only the fixed-origin GSU payload:

```sh
cmake -E make_directory build/plot-rotating-triangle
discc --target gsu --execution-memory ram tests/graphics/rotating_triangle/triangle.dc -o build/plot-rotating-triangle/triangle.o
discld build/plot-rotating-triangle/triangle.o --origin 0x706000 --init-runtime --ram-bank 0 --stack-pointer 0xFFFE --emit-asm build/plot-rotating-triangle/triangle.s -o build/plot-rotating-triangle/triangle.bin
```

The `.bin` is not a SNES ROM and is not position-independent. Copy it to
`$70:6000`; the host must configure the bitmap/bus, write the phase, and start
with `PBR=$70` and `R15=$6000`. The bootstrap initializes RAMBR and the stack.

## Rendering and host contract

At phase zero the local vertices `(0,-48)`, `(40,32)`, and `(-40,32)` produce
screen vertices `(128,48)`, `(168,128)`, and `(88,128)`. Each phase advances
5.625 degrees clockwise in screen coordinates. Integer rounding causes small
changes in rasterized area between poses; phase zero draws 3,281 pixels.

The renderer clips three transformed half-planes to find each scanline span,
sets the cursor once, and walks the accepted pixels. It increments local
checker coordinates, but **only PLOT advances `cursor.x`**. Colors 1 and 2
use COLOR; there are no ROM palette reads or GETC instructions. A local RAM
sine table avoids ROM-buffer access while the SNES CPU owns the ROM bus.
`flush;` drains the GSU pixel caches before returning and STOP.

`@cache` marks the inner clearing and pixel loops. The emitted clearing loop
occupies 477 bytes from its aligned cache base and fits the 512-byte instruction
window. The pixel loop occupies 1,011 bytes: its initial portion uses the cache,
and code outside the window still executes from RAM. These are measured emitted
sizes, not a promise that the entire function is cached. The host records CBR
after STOP, and the Mesen checks require an aligned base inside the payload.
The cache window and RAM fallback match
[Mesen's GSU instruction-fetch implementation](https://github.com/SourMesen/Mesen2/blob/master/Core/SNES/Coprocessors/GSU/Gsu.cpp).

| Resource | Location / configuration |
| --- | --- |
| GSU framebuffer | `$70:0000-$70:2FFF`, 256x192, 2bpp, SCBR `$00` |
| GSU payload | `$70:6000`, startup R10 `$FFFE`, RAMBR `$00` |
| Phase supplied by host | word at `$70:F002`, masked to 0..63 |
| Pixel / white / black counts | words at `$70:F000`, `$70:F004`, `$70:F006` |
| Completed phase | word at `$70:F008` |
| SCMR while running | `$28`: 192 lines, 2bpp, CPU ROM / GSU RAM |
| PPU | mode 0, BG1 tilemap at VRAM byte `$7000`, BG1SC `$38` |
| Palette | dark-gray backdrop, black index 1, white index 2 |

The 65816 host uses `.incbin` in a `SUPERFREE` ROM section and copies the
payload once. For every pose it waits for STOP, reclaims RAM, validates the
result/mailbox/fault state, and uploads 4,096 bytes of dirty tiles during
VBlank. The GSU clears the previous pose before drawing; the column-major
bitmap becomes a normal SNES background through the tilemap. VRAM retains
the completed image during the next render. No CPU drawing is involved.

The bitmap declaration is metadata, not automatic SCMR/SCBR/PPU initialization.
Memory placements and tile-upload bounds are specific to this demo. Changing
the framebuffer, rotation center, vertices or origin requires updating the
host and independent references too.

## Automated verification

The native `graphics_rotation` regression needs no external assembler/emulator:

```sh
ctest --test-dir build/cmake-Release -R '^graphics_rotation$' --output-on-failure
```

Ten input vectors cover representative poses and phase masking, executed through
direct, compiler-assembly and final-assembly payloads. Checks include counts,
bitplane landmarks, clearing poisoned RAM, untouched memory outside the bitmap,
startup with incorrect RAMBR/stack state, one RPIX, no GETC, and byte-exact
reassembly. Both CACHE requests must remain in final assembly, and execution
checks their count and final CBR. The normal GSU execution budget is unchanged.
Hand-encoded model self-tests independently cover selector reset, cache-base
alignment, preserving/rebasing cached lines, RAM fallback and payload bounds.
These model checks do not claim cycle-accurate cache timing.

For the separate complete-SNES test, add Mesen to `PATH` and run:

```sh
cmake -DVERIFY_MESEN=ON -P tests/graphics/rotating_triangle/build-snes.cmake
```

An explicit `-DMESEN=/path/to/Mesen` is also accepted. The independent Lua
reference tests every one of the **49,152 logical pixels at all 64 angles**,
then checks phase wrap. It compares all framebuffer/VRAM bytes and tilemap
entries, CPU-read results, stack/bank/CACHE/STOP state, palette/display state and
VBlank DMA completion. Comparing the complete image catches trails from the
previous pose. The negative test checks the deliberately failed result and
visible red backdrop.

Logs and `rotating-frame-00.png` through `rotating-frame-63.png` are saved next
to the ROMs. Each run removes stale capture/log evidence first and fails on
missing artifacts. Lua file access and disabled frame skipping affect only
these subprocesses; `--doNotSaveSettings` preserves Mesen's configuration.
