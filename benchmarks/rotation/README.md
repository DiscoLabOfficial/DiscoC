# Rotating-triangle bottleneck comparisons

Keep compiler gains separate from changes to the workload algorithm:

1. `reference/triangle.dc` freezes the renderer before counter/edge changes.
2. `counters/triangle.dc` accumulates the pixel count once per span, derives
   the black count at the end, and uses a guarded span-count loop. It retains
   the original per-row edge divisions.
3. `../../tests/graphics/rotating_triangle/triangle.dc` is the current example:
   exact incremental edge quotient/remainder updates replace per-row divisions.

All variants use the same host, 64 phases, Q6 geometry, checker colors,
framebuffer/mailbox layout, runtime origin, stack and independent image oracle.
The frozen sources are benchmark references, not separate language APIs.

Use the existing end-to-end builder's source override, for example from the
repository root:

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -DTRIANGLE_SOURCE=benchmarks/rotation/reference/triangle.dc -DOUTPUT_DIR=build/rotation-reference -DOPTIMIZATION=2 -DVERIFY_MESEN=ON -DMEASURE_GSU_TIMING=ON -DPROFILE_GSU=ON -P tests/graphics/rotating_triangle/build-snes.cmake
```

WLA-DX and Mesen are external prerequisites; their executable paths can be
provided as `WLA_65816`, `WLALINK` and `MESEN`. Omit `TRIANGLE_SOURCE` to measure
the current renderer, and use distinct output directories. Both assembly
paths must reproduce the direct payload byte for byte before building a ROM.
The full per-pixel, RAM/VRAM, stack, bank, cursor, CACHE, STOP, phase-wrap and
deliberate-failure checks remain enabled.

Timing counts emulated fast-GSU cycles at the fixed NTSC CLSR=1/GSU=100%
profile, from the first real opcode to STOP completion. It excludes CPU
setup/copy/poll/DMA/display. `fps.json` instead measures completed-image
publication intervals, including host work, VBlank DMA and repeated refreshes.
Neither is physical-hardware evidence or GIF playback speed.

For compiler-only comparisons, run **the same reference source** through
frozen and current executables. Compare counters and incremental variants
using **the same current executables**. Retain source/tool/payload hashes and
the reports, not just a rounded percentage. An optimized renderer can trade
more setup code/static storage for fewer executed instructions; disclose that
tradeoff instead of attributing its entire gain to register allocation.
