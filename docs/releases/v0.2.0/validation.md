# v0.2.0 candidate validation

Validation date: **2026-10-09**. Candidate name: **v0.2.0-rc.1** (not published).

All compiler builds and measurements below used the clean, merged source commit
`d967c3ef45a948ff457d81052448732da3bd73dc`, tree
`1435ea92f18ff8229b4051fa8e35e12d89947315`. Compiler, test, benchmark and demo
sources were not changed during this pass. Documentation/evidence files were
added **after** execution. This records validation of that source SHA, not a
claim that a later documentation commit or release tag has been validated.

## Gates completed

| Gate | Fresh result |
| --- | --- |
| Clean Windows x64 MSVC Release build | Passed, 94 build steps |
| Complete Release CTest suite | **256/256 passed**, 0 failures/disabled, 194.04 seconds |
| Clean Windows x64 MSVC Debug build | Passed, 94 build steps |
| Complete Debug CTest suite | **248/248 passed**, 0 failures/disabled, 273.69 seconds |
| Post-merge hosted CMake CI | **9/9 jobs succeeded** on the same SHA |
| Post-merge CodeQL | **2/2 analysis jobs succeeded** on the same SHA |
| Nine official workloads at O0/O1/O2/Os | **36/36** functional and independent Mesen checks passed |
| Report compatibility | Seven suite/model/profile/result comparisons passed |
| Cross-level observable outputs | All 36 result/memory fingerprints match their O0 reference |
| RAM rotating triangle, O2 and Os | 64 poses plus phase wrap, positive/negative image and ABI checks passed |
| RAM OBJ interactive shapes, O2 and Os | Controller, framebuffer/PPU and negative checks passed |
| Official SNES triangle | Full positive/negative checks passed, including explicit NTSC/100% rerun |

Release enables seven optional Mesen tests and one SNES triangle integration
test; Debug disables those eight optional entries. Both run all 248 default
tests, including language conformance, contributor loop/switch regressions,
SSA/PHIs, volatile ordering, ROM/GETC, PLOT/RPIX, register allocation, ABI,
linker/object/runtime hardening, mixed optimization levels and assembly
equivalence. The optional Release tests also calibrate the timing observer,
reject stale/no-STOP evidence and execute the division-size fixture.

Native environment: Windows `10.0.26200.0`, MSVC **19.44.35229.0**, x64 C++23,
CMake **4.4.3**, Ninja generator. Builds used separate new directories under
`build/release-candidate/v0.2.0-rc.1/{release,debug}`. Build/test times are host
wall times, **not GSU timing measurements**.

### Hosted CI

The [post-merge CMake run](https://github.com/DiscoLabOfficial/DiscoC/actions/runs/37895498955)
passed Ubuntu, Windows and macOS in Debug and Release, Windows MinGW static
Release, DJGPP Release cross-compilation and Ubuntu ASan/UBSan. Linux/macOS
Release also built and tested the direct GCC/Clang helper path.
The [post-merge CodeQL run](https://github.com/DiscoLabOfficial/DiscoC/actions/runs/37895498688)
passed its Actions and C/C++ analyses. See [ci.json](ci.json) for per-job links,
SHA and completion times. Hosted success is not a claim of local macOS/Linux
execution or DOS runtime validation.

### Warnings remain visible

Each clean local build emitted **23 MSVC warning occurrences**: 18 C4456
shadowing, three C4244 narrowing and two C4127 constant-condition warnings.
They are retained in [environment.json](environment.json), not suppressed or
counted as build errors. The narrowing sites include literal parsing for
hardware-loop recognition in `Optimizer.cpp` and branch-opcode table
construction in `Assembler.cpp`.
This is **not a warning-free build**; warning cleanup remains separate work.
No source fixes were mixed into the frozen validation pass.

## Independent emulator identity and configuration

Installed Mesen file version **2.2.1**, product version
`2.2.1+20ba206cef5ba207c21203176d02cb9f43dda9fb`.
WLA W65816 assembler **10.7**; WLALINK **5.22**.
Executable hashes, the separate **MesenCore.dll hash**, and Release/Debug
DiscoC executable hashes are recorded in [environment.json](environment.json).
The core DLL is included because an executable-only hash does not identify an
external emulator core. These external tools are not bundled by this report.

| Validation | Program memory / origin | Graphics | Runtime and timing profile |
| --- | --- | --- | --- |
| Nine workloads, all four levels | LoROM `$00:8000` | 4bpp 256x192 for graphics; framebuffer `$70:6000`, SCBR `$18`, host SCMR `$39` | NTSC, GSU 100%, CLSR=1, CFGR=0; cold instruction/pixel caches and ROM buffer; RAMBR starts 1 and SP `$1234`, startup establishes RAMBR 0 / SP `$FFFE` |
| Rotating triangle, O2/Os | RAM `$70:6000` | 2bpp 256x192, SCBR 0, host SCMR `$28` | NTSC, GSU 100%, CLSR=1, CFGR `$80` (normal multiply, IRQ disabled); RAMBR 0, initial SP `$FFFE`, cold instruction CACHE on every pose entry |
| Interactive shapes, O2/Os | RAM `$70:8000` | OBJ/4bpp, SCBR 0, host SCMR `$2D` | NTSC, GSU 100%, CLSR=1, CFGR `$80`; RAMBR 0, initial SP `$EFFE`, cold instruction CACHE on entry |
| Official filled triangle | RAM `$70:6000` | 4bpp 256x192, SCBR 0, host SCMR `$29` | Explicit NTSC/100% rerun, CLSR=1, CFGR=0, RAMBR 0, initial SP `$FFFE` |

Fast NTSC GSU is nominally **21.477270 MHz**, often described as 21 MHz.
No overclock was used. Generated CACHE instructions remain enabled: cold entry
does not mean that execution stays uncached.

The nine-workload timing window starts at first opcode entry and ends at STOP
completion. Reports preserve the sampled interval and the separately guarded,
calibrated STOP fetch cost (1/5/81 clocks). Initial pipeline prefetch and SNES
CPU copying, DMA and display time are excluded. `gsu_cycles == master_clocks`
only for this recorded fast-clock profile. These are **emulator measurements**,
not physical hardware measurements or isolated opcode latencies.

The rotation additionally measures 64 completed-image publication intervals
after VRAM DMA, excluding boot/first render. Those FPS figures include host and
presentation work and must not be substituted for isolated GSU cycle counts.
Interactive-shapes timings are fixed-pose GSU samples, **not interactive FPS**.

## Functional evidence

Every workload checks R0 and the host-visible result word, RAMBR, program bank
and restored stack. Graphics cases independently check all 24,576 framebuffer
bytes against expected bitplanes; memcpy checks 128 source/destination bytes
and boundary sentinels. Final assembly is reassembled and checked against the
payload. Mesen's observed opcode count must equal the instruction model's count.

The consolidated report also compares cross-level SHA256 fingerprints of the
observable result word and graphics framebuffer or memcpy source/destination
region. Dead registers and stack/spill bytes are deliberately **not** required
to match across optimization levels.

Both rotation runs pass all 49,152 pixels per pose, 64 captured images, phase
wrap, mailbox/bank/stack/CACHE/STOP, VBlank DMA and deliberate red-screen checks.
All 64 O2/Os PNG hashes match. Each shapes run checks **35 complete OBJ
framebuffers and 255 controller polls**, including A edge/hold/repress, both
rotation axes, Select resize/clamps, opposing directions, PPU sprites and the
negative path. The official triangle checks 49,152 pixels, 1,024 tilemap entries,
framebuffer-to-VRAM transfer, count 9409 and displayed output.

## Evidence files and reproduction

- [optimization-results.md](optimization-results.md): readable comparisons and tradeoffs.
- [benchmark-results.json](benchmark-results.json) / [CSV](benchmark-results.csv): 36 rows, all counters, bytes, timing and observable-output fingerprints.
- [raw/](raw/): eight content-preserving runner report copies, each in JSON and CSV (functional and Mesen, four levels). CRLF is normalized to LF and trailing JSON formatting whitespace is removed; byte hashes may therefore differ from the original files.
- [demo-results.json](demo-results.json) / [CSV](demo-results.csv): rotation samples/FPS/image hashes, shapes samples and positive/negative PASS evidence.
- [tests.json](tests.json): actual counts and hashes/locations of local CTest logs and JUnit output.
- [environment.json](environment.json), [ci.json](ci.json): tool identity, warnings and hosted checks.
- [reproduce.md](reproduce.md): build, benchmark and emulator commands.

Raw build logs, CTest logs/JUnit, generated objects/payloads/ROMs, emulator RAM
snapshots and screenshots remain under the ignored candidate build directory;
they are not release binaries. The original JUnit invocations used relative
output paths, so those XML files are nested under each build directory as
recorded in `tests.json`; reproduction commands use absolute paths instead.

## RC PR preparation (2026-10-10)

The follow-up preparation updates CMake's numeric project version to `0.2.0`
and records `DISCO_VERSION_PRERELEASE=rc.1` and
`DISCO_VERSION_STRING=0.2.0-rc.1`. The README identifies the unpublished
candidate. Language Baseline 0.1, object format v7, compiler behavior and the
measured workloads are unchanged. Existing reports retain their tested
`d967c3ef45a948ff457d81052448732da3bd73dc` revision; this metadata/documentation
PR must not be confused with final tag-revision validation.

Preparation checks on the metadata/documentation working tree also passed:
both configurations report `0.2.0-rc.1`, the rebuilds succeed, and CTest passes
**256/256 Release** (179.87 seconds) and **248/248 Debug** (268.58 seconds).
The completed rerun follows an intentionally interrupted attempt. Its logs and
JUnit receipts are under `build/rc-pr-validation/{release,debug}`. These checks
do not relabel the frozen benchmark reports or validate a future merge/tag SHA.

## Remaining release actions and limitations

- The measurement pass created no tag, GitHub release or binary package.
  The follow-up PR commits/pushes only release metadata, documentation and
  evidence; merging and publication remain separate maintainer actions.
- Review and merge the preparation PR, then freeze the actual tag revision and
  rerun its gates. Do not relabel these results with another SHA. Packaging and
  publication require separate approval.
- Os deliberately trades speed for size; triangle and RAM-palette penalties
  remain significant. The choice is bounded, not proven globally optimal.
- Larger balanced switch dispatch can still produce a diagnosed local-branch
  range error; the official dense/sparse cases do not establish arbitrary-size
  switch support.
- Physical hardware, exhaustive fuzzing, general warm-entry timing and all
  deployment memory/clock configurations remain unverified. No complete timing
  oracle or universally optimal allocator/cache placement is claimed.
- SPC700 is still a frontend/IR foundation, not an executable backend.
- No OpenSNES SDK/toolchain/cache benchmark branch was integrated or required.

Credits: **k0b3n4irb** contributed the hardware-loop R13 correction and exhaustive
switch-return correction, integrated with original authorship preserved. See
the [release notes](release-notes.md) for original commit links.
