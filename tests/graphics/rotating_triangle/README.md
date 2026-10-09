# Stateful PLOT: a rotating checkerboard triangle

DiscoC calculates a black-and-white checkerboard triangle at 64 angles, using
integer Q6 rotation about `(128,96)`. The checkerboard is sampled in the
triangle's local coordinates, so it rotates with the geometry. No frame is a
pre-rendered asset; the SNES CPU only supplies the phase and presents GSU output.
The original [colored triangle](../triangle/README.md) remains a separate test.

![64 real Mesen captures, replayed at an accelerated rate](../../../docs/images/superfx-rotating-triangle-mesen.gif)

The preview replays captured poses at **8 fps**; that playback rate is not a
measurement of the ROM. The host no longer holds each image for six VBlanks:
it starts the next render immediately after uploading a completed pose. If the
GSU misses a refresh, VRAM repeats the last complete image, never partial work.
The [unthrottled O2 measurement](../../../benchmarks/results/rotation-unthrottled-summary.json)
records newly presented poses separately from repeated refreshes. Physical
hardware has not been tested.

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
cmake -DDISCO_TOOLS_DIR=build/release/bin -DOUTPUT_DIR=build/my-rotation -P tests/graphics/rotating_triangle/build-snes.cmake
```

The script also accepts `-DWLA_65816=/path/to/wla-65816` and
`-DWLALINK=/path/to/wlalink`. Quote complete `-D...=...` arguments containing
spaces. CMake 3.15 or newer is required; external tools are not bundled.
Compiler assembly and final linked assembly are both reassembled and compared
byte-for-byte with the direct payload before either ROM is built.

Select optimization explicitly; omitting it preserves the compiler default:

```sh
cmake -DOPTIMIZATION=2 -P tests/graphics/rotating_triangle/build-snes.cmake
cmake -DOPTIMIZATION=s -DOUTPUT_DIR=build/rotation-Os -P tests/graphics/rotating_triangle/build-snes.cmake
```

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
sets the cursor once, and walks the accepted pixels. Each non-horizontal edge
initializes an exact Euclidean quotient/remainder for the first row and its
one-row delta. Subsequent rows use additions and at most one remainder carry,
including empty rows. This preserves the original floor rounding while doing
at most six divisions per pose rather than 224–336. Horizontal edges never
divide by zero. The independent pixel oracle still uses the original
half-plane equations, not this recurrence.

The total pixel count is accumulated once per span; the white count uses the
checker bit and the black count is derived after drawing. All three mailbox
checks remain. The renderer increments local
checker coordinates, but **only PLOT advances `cursor.x`**. Colors 1 and 2
use COLOR; there are no ROM palette reads or GETC instructions. A local RAM
sine table avoids ROM-buffer access while the SNES CPU owns the ROM bus.
`flush;` drains the GSU pixel caches before returning and STOP.

`@cache` marks the inner clearing and pixel loops. O2/Os can lower the guarded,
positive span count to the GSU's counted `LOOP`; zero or negative counts never
enter its do-while body. R12/R13 are preserved, and PLOT supplies the physical
X increment. O2 also caches the bounded division kernel; Os avoids that
speed-oriented cache padding. A callee may rebase CACHE: it is not a
callee-preserved ABI resource. Code outside a 512-byte cache window still
executes from RAM, so an attribute does not promise that an entire function
fits. The host records CBR after STOP, and the Mesen checks require an aligned
base inside the payload.
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
There is no post-presentation delay: every completed pose goes to the next
VBlank's upload, followed immediately by the next render. A slow render repeats
display refreshes instead of exposing an incomplete framebuffer. This does not
skip angular poses or claim 60 new images per second. VRAM DMA still runs during
VBlank; removing that hardware constraint would not be a valid FPS comparison.

The bitmap declaration is metadata, not automatic SCMR/SCBR/PPU initialization.
Memory placements and tile-upload bounds are specific to this demo. Changing
the framebuffer, rotation center, vertices or origin requires updating the
host and independent references too.

## Automated verification

The native `graphics_rotation` regression needs no external assembler/emulator:

```sh
ctest --preset release -R '^graphics_rotation$'
```

Ten input vectors cover representative poses and phase masking, executed through
direct, compiler-assembly and final-assembly payloads. Checks include counts,
bitplane landmarks, clearing poisoned RAM, untouched memory outside the bitmap,
startup with incorrect RAMBR/stack state, one RPIX, no GETC, and byte-exact
reassembly. Both explicit CACHE requests must remain in final assembly, and
execution checks their exact count and final CBR, accounting for hardware
LOOP and optional divider caching. The normal GSU execution budget is unchanged.
Hand-encoded model self-tests independently cover selector reset, cache-base
alignment, preserving/rebasing cached lines, RAM fallback and payload bounds.
These model checks do not claim cycle-accurate cache timing.

For the separate complete-SNES test, add Mesen to `PATH` and run:

```sh
cmake -DOPTIMIZATION=2 -DVERIFY_MESEN=ON -P tests/graphics/rotating_triangle/build-snes.cmake
```

An explicit `-DMESEN=/path/to/Mesen` is also accepted. The independent Lua
reference tests every one of the **49,152 logical pixels at all 64 angles**,
then checks phase wrap. It compares all framebuffer/VRAM bytes and tilemap
entries, CPU-read results, stack/bank/CACHE/STOP state, palette/display state and
VBlank DMA completion. Comparing the complete image catches trails from the
previous pose. The negative test checks the deliberately failed result and
visible red backdrop.

RAM/VRAM/mailbox checks run synchronously on the host's publication write,
before the next GSU render reuses RAM. Screenshots and display checks wait for
the completed scanout and Mesen's screen-buffer update; they do not insert a
delay into the ROM. The bounded capture queue also supports one new pose per
refresh, independently checked using an accelerated-GSU scheduling stress.

## Measuring animation FPS

The same verified run writes `fps.json` beside the ROM. It timestamps 65
completed-image publications after VRAM DMA, covering 64 intervals and phase
wrap. The window excludes boot and the first render, but includes CPU setup,
GSU work, presentation waits and DMA between subsequent images. Time comes
from emulated SNES master clocks, not the speed at which Mesen runs on the PC.
NTSC, CLSR=1 (~21.48 MHz), GSU=100%, no added PPU scanlines and disabled emulator
frame skipping are selected explicitly; a GSU clock-multiplier mismatch fails
the measurement. Emulator frame skipping is distinct from the ROM repeating an
image when the next render is not ready.

`displayed_pose_fps = 64 * 21,477,270 / elapsed_master_clocks`. The report also
contains individual publication clocks, SNES refresh counts and
`repeated_refreshes`. This is actual completed-pose throughput, not the inverse
of GSU-only render time, the GIF playback rate or a physical-hardware result.
VBlank presentation still quantizes intervals, so retain the separate GSU-cycle
reports when comparing small compiler improvements.

To record GSU-only timing in the same verified run:

```sh
cmake -DOPTIMIZATION=2 -DVERIFY_MESEN=ON -DMEASURE_GSU_TIMING=ON -P tests/graphics/rotating_triangle/build-snes.cmake
```

The optional [boundary probe](../../../benchmarks/mesen/measure-rotation.lua)
writes `timing.json` for all 64 poses plus phase wrap. It samples first real
opcode entry and normal STOP entry, then adds the separately calibrated Mesen
STOP fetch cost (1/5/81 fast ticks depending on cache state). It excludes CPU
setup, synthetic prefetch, copying, polling after STOP and DMA/display. Bank,
clock, cold-cache, bus, stack and fault states are checked; unsupported/missing
timing evidence fails, not silently estimates. The normal full-image/FPS oracle
and deliberate red-ROM check remain enabled. This is a fixed demo/emulator
profile, not physical hardware or a general timer for arbitrary payloads.

To profile the first pose's functions and linked machine-code regions:

```sh
cmake -DOPTIMIZATION=2 -DVERIFY_MESEN=ON -DMEASURE_GSU_TIMING=ON -DPROFILE_GSU=ON -P tests/graphics/rotating_triangle/build-snes.cmake
```

`profile.json` includes function/region clocks and per-PC counts. It records
region transitions rather than individual opcode timings to bound observer
overhead, and removes its observer after the first publication. Region totals
must match phase 0's boundary timing. All subsequent pose/FPS/negative tests
remain enabled; measured simulated cycles do not depend on host emulator speed.

The [bottleneck comparison](../../../benchmarks/results/rotation-bottleneck-summary.json)
separates compiler gains from the renderer changes:

| Workload / toolchain | Payload bytes | Mean cycles, 64 poses | Completed poses/s |
|---|---:|---:|---:|
| Original renderer, frozen previous O2 | 2,672 | 2,844,363.546875 | 6.70 |
| Same source, current O2 | 2,285 | 1,910,470.984375 | 10.07 |
| Counter/span rewrite, current O2 | 2,217 | 1,538,819.671875 | 12.10 |
| Incremental edges, current O2 | 2,819 | 1,159,469.625 | 15.02 |
| Incremental edges, current Os | 2,818 | 1,167,535.71875 | 15.02 |

The complete O2 renderer removes 59.24% of mean GSU cycles, but adds 147
payload bytes versus the original: it trades more edge setup/state for less
runtime work. The compiler-only comparison shrinks the unchanged source by
387 bytes. All 64 screenshots match, and the padded SNES ROM remains 65,536
bytes. Use `OPTIMIZATION=s` and a separate output directory to compare Os.
The [frozen staged sources](../../../benchmarks/rotation/README.md) and
`TRIANGLE_SOURCE` override reproduce the separation with the same host/oracle.

The verifier also checks all 49,152 visible pixels in an owned screen-buffer
snapshot before capture. [Screenshot decoding is asynchronous](https://github.com/nesdev-org/MesenCE/blob/master/Core/Shared/Video/VideoDecoder.cpp): a two-refresh
delay occasionally saved the preceding pose even for byte-identical ROMs.
Capture now waits three refreshes and the complete visible reference, with
bounded failure if it never appears. This does not stall the SNES host or
change GSU/FPS sample windows; the final verifier exits one refresh later.
The raw-buffer contract is this fixed NTSC profile (256x239, seven top-border
rows); saved screenshots remain 256x224. Unsupported dimensions fail visibly.

Logs and `rotating-frame-00.png` through `rotating-frame-63.png` are saved next
to the ROMs. Each run removes stale capture/log evidence first and fails on
missing artifacts. Lua file access and disabled frame skipping affect only
these subprocesses; `--doNotSaveSettings` preserves Mesen's configuration.
