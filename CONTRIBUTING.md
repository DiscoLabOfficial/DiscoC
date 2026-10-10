# Contributing to DiscoC

Bug reports, independent testing, documentation improvements, and code
contributions are welcome. DiscoC is experimental compiler infrastructure:
correctness, explicit semantics, and reproducible evidence come before speed.

Please follow the [Code of Conduct](CODE_OF_CONDUCT.md). Its private reporting
contact is pending; do not put sensitive conduct reports or personal
information in public issues or pull requests.

## Report a Bug or Suggest a Change

Use the [issue templates](https://github.com/DiscoLabOfficial/DiscoC/issues/new/choose).
For a bug, include a minimal source/manifest, toolchain revision, platform,
build method, exact commands, expected behavior, and observed output.
For generated-code problems, include final assembly/disassembly when applicable,
execution origin, ROM/RAM setup, optimization level, and emulator/hardware
configuration. Remove private information and proprietary assets.

For a substantial feature or a language/ABI change, discuss the contract before
implementing it. The [language specification](docs/language-spec.md) is the
Baseline 0.1 source of truth; the [roadmap](ROADMAP.MD) describes planned work.

## Prepare a Focused Pull Request

1. Keep unrelated changes separate and explain the behavior being changed.
2. Add a regression that reproduces a correctness bug. Include valid and
   invalid cases when changing syntax, types, or diagnostics.
3. Update the relevant guide and specification when observable behavior changes.
4. Preserve attribution when integrating someone else's work; keep independent
   correctness fixes identifiable.
5. Summarize the change, verification commands/results, and remaining limits.

Follow the configured C++ standard: native builds use C++23 and the
experimental DOS configuration retains C++14 compatibility. Avoid new
dependencies or portability assumptions without discussing their impact.

## Verify Changes

The [build guide](docs/building.md) covers platform prerequisites and presets.
For the native Release path:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

Use `release-mingw` on Windows MinGW. For compiler/backend changes, also run
the corresponding Debug preset and relevant regressions; document unavailable
platforms or external tools rather than claiming they passed.

Optimizer changes should preserve functional outputs, volatile ordering,
checked failures, graphics side effects, and assembly/binary equivalence.
Use the [benchmark workflow](benchmarks/README.md) to report bytes and
execution counters; use calibrated Mesen runs for emulated cycles.
Keep cycles, presentation time, and FPS separate, and record the tested
revision and environment. See [Testing](docs/testing.md).

Documentation-only changes normally need link/path checks, `git diff --check`,
and verification of any changed runnable examples rather than a full rebuild.

## Credit and Licensing

The README's [contributors section](README.MD#contributors-and-community)
recognizes code and independent testing contributions. Use public names/handles;
do not add personal contact details without consent.

Contributions must be yours to share, with sources and licenses identified.
The project's code is [GPL v3 or later](LICENSE); the
[Code of Conduct](CODE_OF_CONDUCT.md#attribution) is separately licensed.
