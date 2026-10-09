# Stateful PLOT: a scaling rainbow triangle

The GSU calculates a colored triangle that grows and shrinks through 64 phases,
without rotation or pre-rendered frames. The SNES CPU only advances the phase,
checks the result and uploads completed tiles during VBlank. The existing
colored static triangle and rotating checkerboard remain separate examples.

## Build and run

With current DiscoC tools, WLA-DX's `wla-65816` and `wlalink` on `PATH`, run from
the repository root (CMake 3.20 or newer):

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -P tests/graphics/scaling_triangle/build-snes.cmake
```

Open `build/plot-scaling-triangle/scaling-triangle.sfc` in MesenCE or another
SuperFX-capable SNES emulator. This demo defaults to **O2**, `CLSR=1` and normal
GSU speed, approximately 21 MHz. It publishes at most one new image per NTSC
refresh, with no extra presentation hold. If rendering misses a refresh, the
last complete image remains visible. The deliberate-failure ROM
`scaling-triangle-negative.sfc` must show a solid red screen.

Explicit executable locations and output directories are supported:

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -DWLA_65816=/path/to/wla-65816 -DWLALINK=/path/to/wlalink -DOUTPUT_DIR=build/my-scaling-test -P tests/graphics/scaling_triangle/build-snes.cmake
```

Quote complete `-D...=...` arguments containing spaces. External tools are not
bundled. Select `-DOPTIMIZATION=0`, `1`, `2` or `s` to compare compiler modes.
Both compiler assembly and final linked assembly must reassemble byte for byte
before the host ROMs are built.

For just the payload, the included manifest selects O2 and the same runtime:

```sh
discc build --config tests/graphics/scaling_triangle/discoc.toml
```

The `.bin` is a fixed-origin GSU payload, **not a complete SNES ROM**. The host
loads it at `$70:6000` with `PBR=$70`, `R15=$6000`, `RAMBR=0` and initial
`R10=$FFFE`. Bitmap metadata is SCBR=0, SCMR=$21; the host adds RAM bus ownership
to select SCMR=$29 and retains CPU ROM ownership.

## Efficient drawing and its contract

For masked phase `p`, height is `24 + min(p, 64-p)`. The fixed base midpoint is
`(128,128)`. At row `r=0..height`, the span is `128-r..128+r`, starting at
`y=128-height`. Thus the triangle scales uniformly about its base midpoint;
its centroid is not fixed. Minimum and maximum sizes are 49x25 and 113x57
pixels, with 625 and 3,249 plotted foreground pixels respectively.

- Edges use subtraction/addition; span widths advance `1,3,5,...`. There are
  no divisions, trigonometry or per-pixel coordinate calculations.
- `cursor.x` is initialized once per drawn scanline. The inner loop consists
  of `pixel;`: **PLOT itself increments R1**, never a manual X increment.
- Eight colors stretch with the triangle using a row-only error accumulator.
  The reference color is `1 + floor(8*r/(height+1))`. COLOR changes only eight
  times per pose, plus a single zero-color selection on shrinking poses.
- `@cache` marks the edge-erasure and pixel loops. O2/Os select guarded GSU
  `LOOP` instructions; no inline assembly is needed.
- `flush;` drains the pixel caches before the host reads RAM. The foreground
  pixel count is `(height+1)^2`, calculated once outside the drawing loops.

**The incremental clearing contract is important:** the host clears the whole
bitmap once, starts at phase zero and advances by exactly one modulo 64.
Growth overwrites the entire previous, smaller triangle. Shrink erases just
the old triangle's two edges with opaque color zero, including its apex
(erased twice harmlessly). Only 52–114 erasure PLOTs are needed instead of
clearing 3,249 pixels or 3,840 bytes every frame. Phase zero also handles the
63-to-0 shrink. Restarting the host clears the bitmap again. Arbitrarily
jumping phases on a dirty bitmap is **not supported by this optimized demo**;
clear the bitmap before starting a new sequence.

All reachable pixels fit tile columns 9–23 and rows 9–16. The host transfers
15 spans of 256 bytes, **3,840 bytes per published frame**, and fails visibly
if DMA overruns VBlank. The small maximum size deliberately keeps a complete
4bpp update within one NTSC VBlank without double buffering or partial frames.

| Cartridge RAM address | Meaning |
| --- | --- |
| `$70:F000` | Foreground pixel count; also returned in R0 |
| `$70:F002` | Host input phase, masked to 0–63 |
| `$70:F004` | Height |
| `$70:F006` | Scanline count, height + 1 |
| `$70:F008` | Completed phase |

## Verification and measurements

With MesenCE available, verify normal and failure ROMs and record cycles/FPS:

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -DVERIFY_MESEN=ON -DMEASURE_GSU_TIMING=ON -P tests/graphics/scaling_triangle/build-snes.cmake
```

Supply `-DMESEN=/path/to/Mesen` if needed. The independent oracle checks all
49,152 logical pixels and framebuffer/VRAM bytes at every phase, all eight
colors, tilemap/palette, STOP/stack/banks/CACHE, DMA completion within VBlank,
64 phases plus wrap, and all 33 visible sizes. Display checks use the actual
screen buffer, allowing the emulator's asynchronous presentation latency
without delaying the ROM. It also verifies the solid-red failure path.

Outputs include `scaling-minimum.bmp`, `scaling-maximum.bmp`, 33 owned-screen
captures (`scaling-height-24.bmp` through `scaling-height-56.bmp`), verification
logs, `fps.json` and, when requested, `timing.json`. Timing records 65 cold-CACHE
entry-to-STOP samples; FPS counts 64 completed-image publication intervals,
including host work, DMA and refresh synchronization. They are not the same
measurement. Captures encode the exact buffer already checked by the display
oracle; they do not depend on the separately delayed screenshot API.

Measured locally in MesenCE with NTSC, CLSR=1, CFGR.MSO=0 and GSU=100%:

| O2 clearing strategy, same image | Payload bytes | Mean GSU cycles, 64 phases |
| --- | ---: | ---: |
| Clear bounding tiles through indexed RAM stores | 624 | 104,345.33 |
| Clear the maximum triangle with PLOT | 601 | 82,845.33 |
| Erase only old edges on shrink (this demo) | 650 | 41,913.53 |

The selected version favors runtime efficiency: about 59.8% fewer GSU cycles
than the indexed-clear variant, for 26 additional payload bytes. Its measured
range is 23,183–64,213 GSU cycles. Completed-image throughput was approximately
**60.10 poses/s**, with zero repeated refreshes over 64 intervals. This is an
emulator measurement, not a physical-hardware or universal speed guarantee.
Re-run the measurement after compiler changes rather than treating these
numbers as immutable golden expectations.

`graphics_scaling`, `optimized_graphics_scaling`, `o2_graphics_scaling` and
`os_graphics_scaling` run without Mesen: they check direct/compiler-assembly/
linked-assembly byte equivalence, 12 phase inputs including word-width masking,
analytic four-plane landmarks, foreground/erasure PLOT counts, COLOR counts,
RPIX, result mailboxes and preservation of an unrelated RAM byte.

The ROM uses original DiscoC/SNES host code and generated geometry. It does not
include Nintendo proprietary code, commercial game assets or a Nintendo SDK.
Physical hardware has not been tested.
