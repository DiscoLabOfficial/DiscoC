# DiscoC v0.2.0 — published release validation

[v0.2.0 was published on 2026-10-21](https://github.com/DiscoLabOfficial/DiscoC/releases/tag/v0.2.0)
as a normal, non-prerelease release with 11 assets. This page indexes existing
records; it does not report new tests of later documentation changes.
The compiler remains experimental and pre-1.0; see the [release limitations](release-notes.md#limitations-and-pre-10-status).

## Exact source identity

| Identity | Revision |
| --- | --- |
| Annotated `v0.2.0` tag target | [`2ff90df8e11410537ebabfc0bbd51ad5919cadd4`](https://github.com/DiscoLabOfficial/DiscoC/commit/2ff90df8e11410537ebabfc0bbd51ad5919cadd4) |
| Measured and packaged source | [`be39ef96037d1762814f8445f4059ff557790dd5`](https://github.com/DiscoLabOfficial/DiscoC/commit/be39ef96037d1762814f8445f4059ff557790dd5) |
| Identical Git tree for both commits | `53390aa43de224eabfc011cfd28134b9c3b5c87a` |

The commit identities differ, but their tracked source contents are identical.
The packages and local measurement records identify `be39ef9`; post-merge CI
identifies `2ff90df`. This equivalence does not extend to future commits.
The explicit source ZIP is the recorded package source, not a renamed RC archive.

## Downloads and records

Use the [release downloads](https://github.com/DiscoLabOfficial/DiscoC/releases/tag/v0.2.0)
or [README platform links](../../../README.MD#download-v020).

- [SHA256SUMS.txt](https://github.com/DiscoLabOfficial/DiscoC/releases/download/v0.2.0/SHA256SUMS.txt):
  hashes of the other ten assets. At publication, all 11 uploaded names, sizes
  and GitHub API SHA256 digests matched the accepted files.
- [BUILD-INFO.json](https://github.com/DiscoLabOfficial/DiscoC/releases/download/v0.2.0/BUILD-INFO.json):
  source/tree identity, source-archive verification, package hashes, native
  requirements and reproducibility limits.
- [discoc-v0.2.0-evidence.zip](https://github.com/DiscoLabOfficial/DiscoC/releases/download/v0.2.0/discoc-v0.2.0-evidence.zip):
  start with `EVIDENCE-INDEX.json`, `validation.json` and `demo-results.json`.
  Final benchmark metrics are at
  `inputs/build/sv/be39ef96037d-r1/evidence/benchmark-results.json` and
  `inputs/build/sv/be39ef96037d-r1/evidence/benchmark-results.csv` inside the archive.
  `ci/hosted-ci-receipt.json` records exact-source CI;
  `ci/native-packages/<platform>/` contains build/dependency/smoke records.
  The index maps raw reports, commands, tool identities and retained failures.
- [FINAL-NOTES.md](https://github.com/DiscoLabOfficial/DiscoC/releases/download/v0.2.0/FINAL-NOTES.md):
  frozen validation addendum and qualitative preparation notes, not updated
  publication status. Its pre-merge CodeQL limitation is historical; see CI below.

The prepared wording in the released metadata is intentionally unchanged.
SHA256 anchors for the manifest and evidence archive are:

```text
08e8aa174c6efcd5aaf2a9088a0fe00f3514c3b3d8d0d11ff54207221c8e59a0  SHA256SUMS.txt
a10ef96d6b3bc18bd3384f15771a0a0496e83f132d3d1c18bc7dba9261f7a111  discoc-v0.2.0-evidence.zip
```

## Platform requirements

Read each archive's `DEPENDENCIES.txt` before running the tools.

- Windows MSVC AMD64 requires Universal CRT and a compatible architecture-matched
  Microsoft Visual C++ v14 Redistributable; Microsoft runtime DLLs are not bundled.
- Windows MinGW AMD64 requires Universal CRT and the listed Windows system DLLs;
  GCC/libstdc++/winpthread runtimes are statically linked.
- Linux x86_64 requires at least GLIBC 2.38 and GLIBCXX 3.4.32. It is a dynamic
  build, not a static binary supporting all distributions.
- macOS arm64 requires macOS 26.0 or later; it is neither universal,
  Developer ID-signed nor notarized.
- DOS i586 is an experimental cross-build, **runtime unverified**.

The four native downloads are the original, previously extracted-and-smoke-tested
CI archives, not repacked binaries. A CI smoke pass does not establish that a
clean end-user machine has every required runtime installed.

## Recorded results

The released evidence records the following for `be39ef9`:

| Scope | Result |
| --- | --- |
| Fresh native Release, optional Mesen/SNES tests enabled | 395/395 passed; zero skipped/disabled |
| Fresh native Debug, optional Mesen/SNES tests disabled | 387/387 passed; zero skipped/disabled |
| Nine workloads × O0/O1/O2/Os | 36 functional-model, 36 independent Mesen, 36 assembly-identity and 36 observable-RAM cross-policy checks passed |
| Complete-ROM configurations | Nine positive and nine deliberate-negative runtime gates passed |

Post-merge CI on `2ff90df` passed all 13 jobs:
[CMake (9)](https://github.com/DiscoLabOfficial/DiscoC/actions/runs/39534623951),
[Static Analysis (2)](https://github.com/DiscoLabOfficial/DiscoC/actions/runs/39534623928),
and [CodeQL (2)](https://github.com/DiscoLabOfficial/DiscoC/actions/runs/39534623452).
These are separate from the packaged exact-source CI receipt, not new local runs.

All 36 strict RC comparison rows are incompatible because the model identity
changed; no like-for-like RC speedup is claimed. Instruction-model counters,
Mesen GSU cycles and completed-image FPS are distinct. Official benchmark code
runs in ROM with cold caches/buffers; interactive demos use their recorded RAM profiles.
Neither emulator checks nor the demo ROMs establish physical-hardware performance.

Retained failures and warnings remain in the evidence. `bundle/REFERENCE-CLOSURE.json`
identifies six historical diagnostic references to four unavailable script hashes;
those old scripts cannot be replayed. Definitive executed commands have their
recorded source snapshots; complete historical-harness-source closure is not claimed.

## Reproduce independently

Use a separate clean checkout; preserve existing work. To match the package records:

```sh
git clone https://github.com/DiscoLabOfficial/DiscoC.git DiscoC-v0.2.0
cd DiscoC-v0.2.0
git checkout --detach be39ef96037d1762814f8445f4059ff557790dd5
git rev-parse HEAD
git rev-parse "HEAD^{tree}"
```

Compare those identities with the table above. With the documented C++23/CMake/Ninja
prerequisites, the ordinary native check is:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

This default check alone does not reproduce the optional emulator gates.
For Release/Debug inventories, external Mesen/WLA-DX identities, all workload
policies and complete-ROM positive/negative checks, follow the
[procedure pinned to the measured source](https://github.com/DiscoLabOfficial/DiscoC/blob/be39ef96037d1762814f8445f4059ff557790dd5/docs/releases/v0.2.0-final/reproduce.md).
The recorded Windows output root was `build/sv/be39ef96037d-r1`; the full source
SHA remains recorded in the evidence. The original long-path reporting/replay
failure is retained alongside the same-binary short-path control and recovery.
On Windows, a short fresh output prefix may be needed instead of the procedure's
long SHA-based default. General Windows long-path support is not established.
Use fresh output directories, record actual results, and run Mesen workflows
serially. Missing tools or skipped tests are not passes.

The [older RC records](../v0.2.0/validation.md) remain historical evidence for
their own revisions, not measurements of this final release.
