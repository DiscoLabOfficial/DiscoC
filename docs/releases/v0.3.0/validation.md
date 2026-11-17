# v0.3.0 validation records

Release date: **2026-11-17**. The project version is `0.3.0` with an empty
prerelease suffix. The [release](https://github.com/DiscoLabOfficial/DiscoC/releases/tag/v0.3.0)
provides final package/source identities, checksums and CI receipts in its
`BUILD-INFO.json` and evidence archive.

The records below describe the **2026-11-16 preparation pass**. No tag or
package was created during that measurement pass; the later publication
does not relabel its historical source or test results.

## Source and tool identity

The base commit is `60e83ce`, but it does **not** identify the tested source:
the preparation includes uncommitted compiler, test, demo and documentation
changes. No final release SHA had been assigned during that pass. Benchmark
reports deliberately retain `toolchain.revision = "unknown"`; the published
package source is recorded separately in the release build manifest.

[evidence.json](evidence.json) records the full base SHA, environment, tool
hashes, build/test receipts and measurement scope.
[source-inputs.sha256](source-inputs.sha256) identifies the exact on-disk
build/test inputs, including the new `src/GSUStaticStackProof.hpp`. It excludes
prose, images, generated files and release evidence, so it is not a complete
Git tree or an exact-source archive. The benchmark reports separately retain
normalized workload/model identities and payload hashes. See
[reproduction](reproduce.md) for commands and manifest verification.

The fresh native Release build used **Linux x86_64, GCC 16.2.0, CMake 4.3.4
and Ninja 1.13.2**, C++23, in `build/release-v0.3.0-preparation`.
All 102 build steps completed without a C++ compiler warning. That warning
statement concerns this native build, not historical builds or expected
DiscoC diagnostics inside tests. Compiler and native-test sources were
unchanged during measurement. The subsequent SNES mailbox correction was
checked in both complete-ROM configurations, as described below.

## Recorded checks

| Check | Result |
| --- | --- |
| Fresh Linux Release build, project version 0.3.0 | Passed, 102 steps |
| Complete default Release CTest suite | **387/387 passed**, no failures, skips or disabled tests; 48.18 s |
| Functional benchmark matrix, nine workloads at four levels | **36/36 passed**, fresh raw JSON/CSV retained |
| Complete SNES demo builds and compiler/final-assembly round-trips | **6/6 passed**, three demos at O2/Os |
| Mesen positive and deliberate-failure ROM checks | **6/6 configurations passed**, including both paths |
| Focused Debug ASan/UBSan regressions | **14/14 passed**, no failures/skips; 28.25 s |

Debug ASan/UBSan coverage is recorded separately in [evidence.json](evidence.json).
It uses the existing incremental Debug directory `build/linux-asan`,
reconfigured to version 0.3.0. The full sanitizer suite is **incomplete**;
the completed partial run and focused relevant regressions have separate
records and are not counted as a 387-test sanitizer pass. The interrupted retry
completed **21 passing tests**; the focused run added **14 distinct passing
tests**, for 35 completed tests under ASan/UBSan. The focused set covers
object/linker/export, optimizer/scheduler/size policy, project configuration,
proof execution and strict-allocation checks; exact names are in the evidence.

The first sanitizer attempt encountered LeakSanitizer's fatal
`does not work under ptrace` environment restriction and was interrupted.
The retry uses `ASAN_OPTIONS=detect_leaks=0` and
`UBSAN_OPTIONS=halt_on_error=1`: address and undefined-behavior checks remain
enabled, but no LeakSanitizer result is claimed. The broad retry was stopped
to complete the relevant regressions separately; its completed tests and
unfinished scope remain recorded rather than being reported as a full pass.

## Functional benchmarks

The [results table](optimization-results.md), [36-row CSV](benchmark-results.csv)
and four pairs of [raw reports](raw/) retain all nine workloads at O0/O1/O2/Os.
The existing runner independently checks scalar results, graphics bitplanes,
copy bytes/sentinels, stack/bank state and payload/object agreement as
applicable. The compile/link/export/reassembly workflow is checked at every
level. All four levels share the same workload, model and execution profile.

These are **functional-model opcodes and byte counts**, not GSU cycles or FPS.
No fresh nine-workload Mesen timing matrix or strict comparison against
0.2.0 is claimed. The model changed since earlier reports; older tools must
be remeasured with the matching runner before asserting a strict improvement.

## Complete-ROM evidence

All six final runs used the same hashed Linux Mesen executable and
`MesenCore.so`, plus WLA-DX. [demo-results.json](demo-results.json) retains
tool/source/payload/ROM hashes, verification-log text, the initial failures,
functional round-trip identities and measured demo samples. ROMs, captures
and native test/build logs stay under ignored `build/`; they are not packages.

The explicit emulator profile is NTSC, GSU=100%, CLSR=1, normal multiply,
deterministic power-on, no extra PPU scanlines, frame skipping disabled and
`--doNotSaveSettings`. Emulator runs were serial and retained the builders'
120-second limits. These results do not validate physical hardware or other
memory/clock/emulator profiles.

| Demo | O2 payload | Os payload | Independent checks |
| --- | ---: | ---: | --- |
| Rotating checkerboard | 2,328 bytes | 2,158 bytes | 64 poses plus phase wrap, 49,152 reference pixels per pose, RAM/VRAM/PPU, ABI/STOP/CACHE, VBlank and negative ROM |
| Interactive shapes | 5,456 bytes | 5,201 bytes | 61/59 full OBJ framebuffers respectively, 255 controller polls each, geometry, controls, PPU/DMA and negative ROM |
| Gem chess | 13,940 bytes | 13,440 bytes | Ten captured views, 18 publications, framebuffer/VRAM/PPU, input/ABI/STOP and negative ROM |

### Rotation startup correction

The initial O2 and Os complete-ROM attempts built and round-tripped but
failed before the first publication, reporting status 38 and 208. The verifier
reads the status mailbox at every end-of-frame callback; the host initialized
it only after framebuffer clearing, payload copying and display setup, which
can span multiple refreshes. It was therefore reading uninitialized work RAM.

The host now stores status zero immediately after its data-bank bootstrap,
before multi-frame setup. **Lua checks, emulator flags, deadlines and GSU
source were not changed.** Both O2/Os then passed the complete positive and
negative checks. GSU payload hashes were identical before and after the host
fix. This fixes the observed startup failure; it does not weaken image,
publication, ABI or fault validation.

The two final rotation runs also retained 65 publication samples: **64 completed
image intervals after DMA**, excluding boot and first render. O2 measured
30.049414957492 new poses/s; Os measured 30.049392622202. These include the
host/presentation cadence and are **not isolated GSU cycle measurements**.

### Other demo measurements

Shapes measured wireframe/steady-filled GSU cycles of **196,698 / 347,215** at
O2 and **203,949 / 402,806** at Os, at yaw 8, pitch 5 and size 12. The four
wire samples, first-filled sample and three steady-filled samples are retained
in the embedded benchmark records. These exclude presentation and are not FPS.

Gem chess recorded 18 publications, including 16 filled samples with the final
reset and two wireframe samples. The raw [O2 timings](raw/gem-chess-O2-timings.csv)
and [Os timings](raw/gem-chess-Os-timings.csv) retain GSU cycles separately from
host publication clocks. Payloads or cycles that differ from the dated demo
tables are fresh results for this preparation; historical tables are preserved.

## Publication and measurement boundary

The [release notes](release-notes.md) describe scope and compatibility, including
object formats 7–10 and the short RAM host contract. Documentation links and
whitespace are checked separately. Previous 0.1/0.2 release/CI evidence remains
historical and is not relabeled with this preparation's identities.

Published archives use a clean checkout and the existing
[packaging procedure](../../../cmake/PACKAGING.md). Their exact SHA, platform
checks and extracted-package smoke results are recorded in the release assets.
The local preparation measurements above do not imply new hosted CI,
Windows/macOS/DOS execution, physical-hardware checks or exhaustive fuzzing;
those scopes remain separate from the final package receipts.
