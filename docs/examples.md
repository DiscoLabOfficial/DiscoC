# Examples and Freestanding Libraries

Build the tools using [Building DiscoC](building.md), then add the chosen
`build/<preset>/bin` directory to `PATH`. Unless noted otherwise, commands
below run from the repository root.

## Complete SNES Graphics Demos

These demos calculate their geometry in DiscoC. The SNES host initializes
hardware, runs the GSU, and transfers completed graphics to the PPU.
WLA-DX (`wla-65816` and `wlalink`) is required to assemble a complete ROM;
Mesen is an additional dependency for independent emulator checks.

### Filled, Banded Triangle

The [official triangle](../examples/snes/triangle/README.md) includes
DiscoC source, `discoc.toml`, original host assembly, portable build script,
and full framebuffer/display checks.

```sh
# Fixed-origin GSU payload and final assembly:
discc build --config examples/snes/triangle/discoc.toml
# Complete SNES ROM:
cmake -P examples/snes/triangle/build-snes.cmake
# ROM plus independent positive/negative Mesen checks:
cmake -DVERIFY_MESEN=ON -P examples/snes/triangle/build-snes.cmake
```

ROM outputs go to `build/snes-triangle`. The host consumes linked bitmap/origin
metadata. The [triangle test wrapper](../tests/graphics/triangle/README.md)
points to this canonical example.

### Rotating Checkerboard Triangle

The [rotation demo](../tests/graphics/rotating_triangle/README.md) uses
integer fixed-point math, incremental edge/span calculations, a local sine
table, cached pixel loops, and hardware cursor increments.

```sh
cmake -DOPTIMIZATION=2 -P tests/graphics/rotating_triangle/build-snes.cmake
```

Open `build/plot-rotating-triangle/rotating-triangle.sfc`.
The demo guide documents full-image checks at all 64 angles, byte-exact
assembly, GSU timing, and completed-pose FPS. The README's GIF is a preview
sequence, not a timing measurement.

### Interactive OBJ Cube and Sphere

The [interactive demo](../tests/graphics/interactive_shapes/README.md) renders
a cube and an icosahedral sphere in OBJ/4bpp at approximately 21 MHz.
D-Pad rotates, A toggles wireframe/filled faces, and Select + Up/Down resizes.

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -P tests/graphics/interactive_shapes/build-snes.cmake
```

For a different preset, replace `DISCO_TOOLS_DIR` with its tool directory.
Open `build/interactive-shapes/interactive-shapes.sfc`. This RAM host requires
O2 or Os to fit its code reservation. Optional Mesen checks exercise actual
controller input and compare complete framebuffer, VRAM, and visible images.

The [scaling triangle](../tests/graphics/scaling_triangle/README.md) is another
integer-only animation example.

The example ROMs contain **no Nintendo proprietary code, commercial game
assets, or Nintendo SDK components**. Emulator verification is not
physical-hardware validation. See [SuperFX graphics](gsu-graphics.md) for the
plot/bitmap contract and [GSU loading](gsu-loading.md) before reusing a payload.

## Language and Integration Examples

- [Multi-file project](../examples/multifile/README.md): prototypes, linking,
  manifests, and fixed execution origins.
- [Two-file getting-started tutorial](getting-started.md): source imports and
  host-visible RAM output.
- [Language core](../examples/language_core/README.md): qualifiers, globals,
  numeric operators, and lexical plotting.
- [Module interfaces](../examples/language_modules/README.md): shared types
  and declaration-only interfaces.
- [Source imports](../examples/source_imports/README.md): dependency discovery,
  shared imports, and search paths.
- [RAM result](../examples/ram_result/README.md): startup and a host-visible
  output contract rather than inspecting arbitrary registers after STOP.
- [Stateful plotting source](../examples/plot.dc): ROM-colored scanline drawing;
  its host requirements are explained in [SuperFX graphics](gsu-graphics.md).

## Libraries

`lib/core` provides near-RAM byte copy/move/fill routines and signed Q8.8/Q12.4
helpers. Fixed-point aliases are transparent integers with explicit scaling,
not new primitive types or floating-point support. `lib/targets/gsu` contains
target-specific graphics wrappers.

Import a library's `.dci` interface and explicitly compile/link its matching
`.dc` implementation, or import the `.dc` directly and let a project build
discover it. Importing an interface alone does not link a library.

See the [fixed-point example](../examples/fixed_point/README.md) and
[freestanding library contract](freestanding-library.md) for APIs, rounding,
overflow, and memory-access limits.
