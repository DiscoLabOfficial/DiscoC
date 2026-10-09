# Interactive SuperFX cube and low-poly sphere

DiscoC calculates and rasterizes a 3D cube and an **icosahedral sphere**:
12 sphere vertices, 30 unique edges and 20 triangular faces. This is a solid
low-poly body, not a planar circle or ellipse. The SNES CPU handles controller
input, checks STOP/results, and presents completed images through two actual
64x64 PPU sprites. There are no pre-rendered geometry frames.

## Build and controls

Build DiscoC, put `wla-65816` and `wlalink` on PATH, then run from the repository
root with CMake 3.20 or newer:

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -P tests/graphics/interactive_shapes/build-snes.cmake
```

Open `build/interactive-shapes/interactive-shapes.sfc` in a SuperFX-capable
SNES emulator. Map its player-one SNES controller normally:

| Control | Action |
| --- | --- |
| Left / Right | Rotate both objects around the vertical axis |
| Up / Down | Rotate around the horizontal axis |
| A press | Toggle both objects between wireframe and filled colored faces |
| Select + Up / Down | Increase / decrease scale, without vertical rotation |

A held down toggles **once**, not every frame. Opposing directions cancel.
Angles wrap through 64 positions; cube half-size is clamped to 6..16 and sphere
radius is `2*size-2`. Input is read in NMI every NTSC refresh, even during a long
GSU render. A coherent snapshot is supplied to each render. Unchanged poses do
not trigger further rendering; missed refreshes retain the previous complete
image. This does not promise 60 newly rendered poses per second.

The default is **O2, OBJ/4bpp, CLSR=1, normal multiply timing**, approximately
21.477 MHz on NTSC, without emulator overclocking. `-DOPTIMIZATION=s` builds the
size-oriented variant. This particular bank70 RAM host requires O2 or Os:
O0/O1 exceed its code reservation. That is a placement limitation, not a new
language rule. Native O1 regressions use the GSU's full-width ROM mirror bank;
O0 tests explicitly verify oversized-program rejection.

Explicit tool locations are supported. Quote paths containing spaces:

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -DWLA_65816=/path/to/wla-65816 -DWLALINK=/path/to/wlalink -DOUTPUT_DIR=build/my-shapes -P tests/graphics/interactive_shapes/build-snes.cmake
```

The [manifest](discoc.toml) also supports standalone payload building:

```sh
discc build --config tests/graphics/interactive_shapes/discoc.toml
```

It produces `interactive-shapes.bin` and byte-exact `interactive-shapes.s`,
**not a complete SNES ROM**. The ROM build additionally assembles the original
WLA-DX host. Temporary `triangle*` files belong to the shared round-trip harness.

## Renderer and memory contract

Projection is orthographic: yaw first, then pitch, with screen Y negated.
All arithmetic is bounded signed integer Q6. Rounding is symmetric, including
negative midpoint ties. The cube uses five shared projection multiplications;
the sphere uses ten. Raw products are combined before rounding, preserving the
same vertices as the full per-vertex transform. Antipodal vertices are reused.

Wire edges use canonical increasing major-axis midpoint rasterization.
Horizontal-major lines exploit PLOT's hardware X increment; vertical-major
lines undo only unwanted X steps. Cursor registers are not reloaded per point.
Filled faces use the inclusive scanline envelope of their rasterized edges.
The contour collector batches shallow-edge pixels into one interval per row;
only `minY..maxY` is initialized and scanned. Negative projected face area selects
visible outward faces. Face colors are flat palette colors, not lighting.

Opaque zero-color cached PLOT spans clear both panels before any new geometry,
so arbitrary angle, size and mode changes leave no trails. `flush;` drains the
pixel caches before STOP. Pixel loops use PLOT's X increment, not a manual one.
Checked pointer operations remain enabled; no runtime safety checks were removed.

| Cartridge RAM | Reservation |
| --- | --- |
| `$70:0000..7FFF` | Full 32-KiB OBJ/4bpp addressing domain |
| `$70:8000..DFFF` | Fixed-origin payload reservation |
| Above code, below `$70:EFFE` | Checked descending stack; initial R10=`$EFFE` |
| `$70:F000` | Primitive count, also returned in R0 |
| `$70:F002/F004/F006/F008` | Input yaw, pitch, size and filled flag |
| `$70:F010/F012/F014/F016` | Normalized input echoes |

The sprite panels occupy `(32..95,64..127)` and `(160..223,64..127)`. OBJ metadata
is SCBR=`$00`, SCMR mode bits=`$25`; the host uses SCMR=`$2D` for RAM ownership
and POR object mode. OBSEL=`$40` selects 64x64 sprites, with tiles `$84` in the
first and second character tables. Sixteen 256-byte transfers upload **4096
bytes per changed pose** during VBlank, using register-held source/destination
addresses. The host checks the beam position and displays red on failure.
The ROM declares 64 KiB cartridge RAM and does not access bank71.

## Verification and performance

With MesenCE available, run full image/control checks and real GSU timing:

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -DMESEN=/path/to/Mesen -DVERIFY_MESEN=ON -DBENCHMARK=ON -P tests/graphics/interactive_shapes/build-snes.cmake
```

The verifier injects actual player-one buttons, not writes to input mailboxes.
It checks press/hold/release/repress, both axes, opposite directions, Select
resize, minimum/maximum scale, all 32,768 framebuffer and VRAM bytes, the full
visible PPU image, sprites/palette, ABI/STOP, CACHE and VBlank completion. Its
edge oracle uses a direct rational formula, not the production accumulator.
A separate deliberately wrong-result ROM must show a solid red backdrop.

The compiler-only [loop-state comparison](../../../docs/optimization.md#phase-2-loop-state-refinements-and-demo-measurements)
preserves this source and host. At yaw 8, pitch 5, size 12, O2 shrinks the
payload from 13,794 to 12,987 bytes, reduces wireframe GSU cycles from 469,556
to 407,584, and steady-state filled cycles from 1,901,192 to 1,739,658.
Os selects 12,978 bytes with a different speed tradeoff. These are emulator
GSU timings excluding host/DMA/presentation, not measured interactive FPS or
final-release evidence. Both policies pass the complete image/control oracle.

Native regressions execute 17 geometric/input cases through direct objects,
compiler assembly and final linked assembly under O1/O2/Os. They include
cardinal/diagonal/near-edge-on poses, signed input boundaries, exact PLOT counts,
mailboxes, color/options, bank/stack initialization, clearing poisoned panel
bytes, and preservation of unrelated RAM. O0 source/assembly handling and
oversized RAM/ROM rejection are tested separately.

`benchmark.json` records four wire and four filled samples at the **same pose**
`yaw=8,pitch=5,size=12`, from the first GSU opcode through normal STOP including
calibrated STOP completion. It excludes CPU work, controller polling, VBlank
waiting and DMA; these numbers are **not displayed FPS**. The separately built
benchmark host intentionally repeats unchanged poses, unlike the interactive
ROM. Cold CACHE and the 21 MHz profile are asserted.

Measured in local MesenCE, O2, comparing the straightforward icosahedron renderer
with this optimized renderer (not comparing against the discarded planar disk):

| Same geometry/pose | Payload bytes | Wire GSU cycles | Filled steady GSU cycles |
| --- | ---: | ---: | ---: |
| Per-vertex baseline, indexed RAM clear, generic edge helper | 11,010 | 1,331,814 | 4,221,443 |
| Shared projection, PLOT clear, specialized cursor/contour loops | 13,879 | 465,656 | 1,910,364 |

That is about **65.0% fewer wire cycles** and **54.7% fewer filled cycles**, at
the cost of a larger payload. The first filled sample is slightly cheaper;
the table uses the repeated steady sample. Re-run measurements after compiler
changes. This is emulator evidence, not a physical-hardware guarantee.

Owned-screen BMP captures and verification logs are written alongside the ROM.
Any GIF assembled from those captures is a preview, not a timing measurement.

**The ROM contains no Nintendo proprietary code, commercial game assets or
Nintendo SDK components.** Host code and generated geometry are original.
Physical hardware has not been tested.
