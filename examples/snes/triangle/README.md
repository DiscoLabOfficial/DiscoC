# DiscoC 0.1 end-to-end SNES triangle

This is the official Baseline 0.1 example pictured in DiscoC's README.
It includes the DiscoC program, a minimal WLA-DX 65816 host, and independent
Mesen checks. WLA-DX and Mesen are optional external tools, not bundled binaries.
It is a correctness example, not a performance benchmark or a hardware-tested
engine. There are no pre-rendered pixels and no source-level placement directives.

`main.dc -> discc -> object -> discld -> payload/final assembly -> host.asm -> SNES ROM -> Mesen`

`discoc.toml` is the deployment configuration. `host.asm` owns CPU/GSU bus
access and PPU setup. `triangle-config.inc` is generated from linked metadata,
so host SCBR/SCMR and the load/entry address cannot silently drift from the
selected bitmap and linker origin. The regression suite consumes this same
`main.dc` and manifest; it does not keep a second copy of the triangle.

![DiscoC triangle rendered by the SNES PPU in Mesen](../../../docs/images/superfx-triangle-mesen.png)

## Build just the GSU payload

From the repository root, with the current DiscoC tools on `PATH`:

```sh
discc build --config examples/snes/triangle/discoc.toml
```

`triangle.bin` is a fixed-origin GSU payload, **not a SNES ROM**. Copy it to
`$70:6000` and start at its first byte, with `PBR=$70` and `R15=$6000`.
The bootstrap initializes RAMBR and R10. The host must still configure bus
ownership, SCBR/SCMR, and display the resulting framebuffer.
Outputs are `build/snes-triangle/0-main.o`, `triangle.bin` and `triangle.s`.
The manifest's relative paths work from any terminal directory. Inside this
example directory, `discc build` is equivalent. Rebuild the toolchain first
using the root [build instructions](../../../docs/building.md).

The program calculates the edges of an isosceles triangle with vertices
`(128,48)`, `(32,144)`, and `(224,144)`. Its 97 scanlines have widths
`1,3,...,193`, totaling **9,409 pixels**. Each row uses a calculated color
index, `1 + ((y - 48) >> 3)`, then repeatedly executes `pixel;`. Hardware PLOT
advances `cursor.x`; the source and backend do not add a second increment.
`flush;` commits the final pixel-cache entries before the CPU reads RAM.

This RAM-executed demo intentionally uses computed colors, not a ROM palette:
separate placement of RAM code and ROM-qualified constants is not implemented.

## Build the complete SNES ROM

Install WLA-DX's `wla-65816` and `wlalink` and put them on `PATH`. With CMake
3.15 or newer, the same script runs on Windows, Linux, and macOS:

```sh
cmake -P examples/snes/triangle/build-snes.cmake
```

If the DiscoC tools are not on `PATH`, select their executable directory:

```sh
cmake -DDISCO_TOOLS_DIR=build/release/bin -P examples/snes/triangle/build-snes.cmake
```

MinGW presets use `build/release-mingw/bin`; helpers use their build directory's
`bin/` subdirectory, as do multi-configuration builds. Optional
`-DWLA_65816=/path/to/wla-65816` and `-DWLALINK=/path/to/wlalink` select specific
executables; quote each complete `-D...=...` argument when its path has spaces.
All generated artifacts go to `build/snes-triangle` by default. Use
`-DOUTPUT_DIR=build/my-triangle` to choose another build directory.

Open `build/snes-triangle/triangle-test.sfc` in a SuperFX-capable SNES emulator
(Mesen/MesenCE with GSU support).
Success displays the banded triangle; a failed result check displays solid red.
`triangle-test-negative.sfc` deliberately expects 9408 instead of 9409 and
must display red. Both compiler assembly and final linked assembly are
reassembled and byte-compared with the direct payload before ROM generation.
The manifest-built payload is also byte-compared with standalone compiler/linker
invocations. Missing tools or a failed comparison stop the script; verification
is never silently skipped when requested.

## Host contract

| Resource | Location / configuration |
| --- | --- |
| GSU framebuffer | `$70:0000-$70:5FFF`, 256x192, 4bpp, SCBR `$00` |
| GSU payload | `$70:6000`, PBR `$70`, initial R15 `$6000` |
| GSU startup | RAMBR `$00`, initial R10 `$FFFE` |
| Diagnostic word | `$70:F000`, expected `$24C1` / 9409 |
| SCMR while running | `$29`: 192-line bitmap, 4bpp, CPU ROM access, GSU RAM access |
| PPU tile bytes | VRAM byte `$0000-$5FFF` |
| PPU blank padding tile | VRAM byte `$6000-$601F`, tile 768 |
| PPU BG1 tilemap | VRAM byte `$7000-$77FF`, BG1SC `$38` |

The 65816 host stores the payload in a `SUPERFREE` ROM section with `.incbin`,
computes its length from surrounding labels, clears the framebuffer, copies
the payload to cartridge RAM, and starts the GSU. After STOP, it reclaims RAM,
checks the `word main()` result in R0, the RAM diagnostic word, and R6's runtime
fault status, then DMA-copies the framebuffer to VRAM. The GSU's column-major
tile order is mapped to the PPU's row-major order through BG1's tilemap.
PPU mode 1 and a 16-color CGRAM palette make the image visible.

The generated bitmap definitions are host configuration metadata: they do **not**
initialize SCMR, SCBR, or the SNES PPU itself. These host settings must agree
with the source and linker. This is a single-frame demo, not a double-buffered
engine. Changing vertices/colors is safe within the 256x192 framebuffer;
changing memory placement requires updating the host and link options too.
The script rejects any bitmap other than this host's base-zero 256x192 4bpp
profile. Other modes need a matching PPU transfer and tilemap, not just a change
to `depth`. Code must fit below the mailbox at `$70:F000` and remain separate
from the framebuffer and active stack.

## Automated checks

The ordinary `graphics_triangle` regression runs without WLA-DX or Mesen:

```sh
ctest --preset release -R '^graphics_triangle$'
```

It executes direct and assembled payloads in the instruction-level GSU model,
checks exactly 9409 PLOT, 97 COLOR, zero GETC and one RPIX, verifies the result,
stack/bank state and representative bitplane bytes, and checks byte-exact
reassembly of the final export. The startup test begins with deliberately
incorrect RAMBR and R10 values.

For the separate full SNES integration test, install Mesen with its Lua API
and add it to `PATH`, then run:

```sh
cmake -DVERIFY_MESEN=ON -P examples/snes/triangle/build-snes.cmake
```

Use `-DMESEN=/path/to/Mesen` and `-DDISCO_TOOLS_DIR=...` if needed. On Windows,
an explicit executable path can include the `.exe` extension.

The Mesen scripts check all **49,152 framebuffer pixels**, every transferred
framebuffer byte, all **1,024 tilemap entries**, the blank tile, CPU-read result,
and stack/bank state. They also check that the PPU displays the triangle and
that the intentionally incorrect expectation reaches the red-screen path.
Logs and screenshots are saved beside the generated ROMs. Temporary Lua file
I/O and disabled frame skipping apply only to these test subprocesses;
`--doNotSaveSettings` preserves the user's Mesen configuration.

Expected successful logs start with `PASS triangle: all 49152 pixels and 1024
tilemap entries match` and `PASS triangle negative: status=2 R0=9409 RAM=9409`.
An old screenshot alone is not evidence: the script removes stale verification
outputs and requires new PASS logs and captures from each emulator subprocess.

The complete integration can also be registered with CTest by configuring
`-DDISCO_TEST_SNES_INTEGRATION=ON` (see [testing](../../../docs/testing.md)).
WLA-DX/Mesen are deliberately not required for the normal compiler tests or DOS
cross-build. Use an emulator exposing the documented Mesen Lua/testrunner API;
the ROM can be opened manually in other SuperFX emulators without Lua checks.

The README screenshot is a real Mesen capture of this test. Physical hardware
has not been tested. See [SuperFX graphics](../../../docs/gsu-graphics.md) for
the language and hardware-state contract.
