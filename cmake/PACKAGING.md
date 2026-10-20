# Native binary distributions

This recipe creates a build artifact, not a tag, GitHub release or publication.
Use a clean checkout at the exact source commit, including this recipe. Choose a
fresh build directory below `build/`; existing directories are never overwritten
or deleted. The CI Release jobs use the documented preset directories.

For example, from a clean checkout with no existing `build/release`:

```sh
cmake -DACTION=prepare -DBUILD_DIR=build/release -P cmake/package-tools.cmake
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake -DACTION=package -DBUILD_DIR=build/release -P cmake/package-tools.cmake
```

For MinGW use `build/release-mingw` and the `release-mingw` preset instead. Keep
the actual configured compiler installed until packaging finishes. For an
alternate fresh directory, configure with `cmake -S . -B build/<name>` and the
equivalent Release/toolchain options, build, and run the complete existing tests.
The source stamp must be created **before** configuration/building. It binds a
fresh build to the clean HEAD and is checked again before packaging; it is not a
claim that untrusted build tools or deliberately replaced binaries are attested.

The package contains precisely three built tools in `bin/`, plus the complete
`git archive` source tree (including documentation, tests, examples, library
modules and LICENSE). Ignored/untracked files are never copied. A source commit
containing private development files or an existing raw `bin/` is rejected.
`BUILD-INFO.json`, `DEPENDENCIES.txt` and `PACKAGE.txt` describe the build and
runtime requirements. MinGW notices are required from the actual compiler
prefix: `crt/COPYING`, `crt/COPYING.MinGW-w64-runtime.txt`,
`libstdc++/{COPYING3,COPYING.RUNTIME}` (or the current `gcc-libs` directory), and
`libwinpthread/COPYING`; their complete directories and hashes accompany the
tools. Missing evidence is an error, never silently omitted.

Packaging needs Git and CMake 3.21+, plus the native inspection tools already on
the CI hosts: Linux `file`, `ldd`, `readelf`; macOS `file`, `otool`; MSVC
`dumpbin`; MinGW `objdump` beside its configured compiler. Linux records actual
GLIBC/GLIBCXX requirements; macOS records Mach-O architecture and deployment
targets; MSVC requires the matching Visual C++ v14 Redistributable and Universal
CRT (not bundled). MinGW rejects dynamic GCC/MinGW runtime imports.

Windows produces ZIP; Linux/macOS produce TAR.GZ with executable tool modes.
The commit date is used for archive entries, but byte-for-byte reproducible
cross-toolchain builds are **not** promised. The exact archive is extracted to a
fresh path containing spaces. The inventory and every file hash are compared,
Unix execute bits are checked, then **only extracted tools** run the existing
`backend_equivalence` (nine pairs) and `diagnostics` cases, the multifile manifest
build, and `final.s` assembly/relink byte comparison. No GSU runner/emulator is
needed. MinGW smoke uses only Windows system directories on PATH.

`build/<name>/package/artifacts/` holds the already-smoked archive and separate
build/dependency/smoke receipts and command log. The archive is never repacked
after smoke. CI uploads distributions and receipts separately from its existing
raw build/test artifacts; existing tests are not replaced by package smoke.

DOS remains an experimental cross-build-only CI artifact, not a native package
from this recipe. DOS runtime execution is unverified. Before distributing it,
collect `copying.dj`, `copying`, `copying.lib` and GCC/libstdc++ notices from the
actual pinned DJGPP toolchain, record its origin/hash and requirements, and label
it cross-build-only/unverified. No emulator is installed or inferred here.
