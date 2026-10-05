# Building DiscoC

The build helpers support Windows, Linux, and macOS. CMake remains the primary
build system; the direct backend builds the same tools using GCC or Clang
without configuring a CMake project. All native paths use C++23. The experimental
DOS cross-build keeps its C++14 configuration.

The helpers do not install dependencies, delete build directories, change your
working directory, or modify persistent shell/security settings. Every failed
compile, link, or test stops the build and returns a nonzero exit code.

These scripts build the **toolchain's C++ executables**. To build a user's
DiscoC program after that, use `discc build` with a `discoc.toml`
[project manifest](project-manifest.md). The manifest driver calls the compiler
and linker internally and does not require CMake, Make, or `discld` on `PATH`.

## Prerequisites

The documented presets require CMake 3.21+ and Ninja (`release`/`debug`), or
`mingw32-make` (`release-mingw`/`debug-mingw`). The non-preset/manual build and
native helpers require CMake 3.20+ for C++23; DOS/manual C++14 and script-only
regression/integration workflows retain CMake 3.15. CMake's
[C++23 standard setting](https://cmake.org/cmake/help/latest/prop_tgt/CXX_STANDARD.html)
was added in 3.20; the project diagnoses older native configurations explicitly.
Presets do not download a compiler or select a different compiler in an existing
cache.

- **Windows / MinGW:** a C++23-capable MinGW-w64 GCC installation and
  `mingw32-make` on `PATH`, plus CMake for the CMake backend or regression tests.
- **Windows / MSVC:** Visual Studio Build Tools with the C++ workload and CMake.
  For Ninja, use a Developer PowerShell/Command Prompt with `cl` available.
  Direct builds support GCC/Clang, not the MSVC command-line syntax.
- **Linux:** a C++23-capable GCC or Clang installation. CMake builds also need
  Make or Ninja, depending on the selected backend.
- **macOS:** a C++23-capable Apple Clang/Clang installation and its SDK (normally
  supplied by Xcode Command Line Tools). Add CMake and Make/Ninja as needed.
  The shell helper defaults to `clang++` for direct macOS builds.

Use PowerShell 5.1+ on Windows and Bash 3.2+ on Linux/macOS. GNU Make is optional
for the convenience Makefile. Compiler selection is an executable name or path,
not a command containing flags. `CXX` is respected when no compiler is specified.

## CMake happy path

Run from the repository root on Linux/macOS, or a Windows MSVC Developer shell
with Ninja and `cl` on `PATH`:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Windows MinGW-w64 users with `g++` and `mingw32-make` on `PATH` use:

```sh
cmake --preset release-mingw
cmake --build --preset release-mingw
ctest --preset release-mingw
```

The MinGW presets enable `DISCO_MINGW_STATIC`: tools/test executables do not
need MinGW/GCC runtime DLLs. Windows system DLLs remain normal dependencies.
This option is not a portable request for fully static Linux/macOS binaries.

`debug` and `debug-mingw` use the same commands with their preset names. The
Linux-only `sanitizers` preset runs ASan/UBSan in Debug without suppressing
sanitizer failures. `cmake --list-presets=all` lists available choices.
Preset build directories are `build/<preset>`; every executable is in `bin/`.
Personal overrides belong in ignored `CMakeUserPresets.json`, not the tracked
shared presets. Use a separate directory when changing compiler or generator.

Manual CMake remains available, including Visual Studio/multi-configuration
generators and CMake 3.20 installations without preset support:

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --config Release --parallel
ctest --test-dir build/native -C Release --output-on-failure
```

These tools are also in `build/native/bin`, not `Release/` or `Debug/`.
An explicit `CMAKE_RUNTIME_OUTPUT_DIRECTORY` still overrides the default.

## Use the built toolchain

Either invoke the tool by its full path, or add its `bin` directory to `PATH`
for the current shell. For example, from the repository root:

```powershell
$env:PATH = (Resolve-Path .\build\release-mingw\bin).Path + ';' + $env:PATH
discc build --config examples/multifile/discoc.toml
```

```sh
export PATH="$PWD/build/release/bin:$PATH"
discc build --config examples/multifile/discoc.toml
```

Inside a user's project containing `discoc.toml`, the command is simply
`discc build`. The project driver does not need a separate `discld` on `PATH`.
See [project manifests](project-manifest.md) for configuration and output rules.
Toolchain `bin/` output does not change a project's manifest-selected outputs.

For an optional local install, use
`cmake --install build/release --prefix <local-directory>` (add `--config Release`
for multi-configuration builds). Only `discc`, `discas` and `discld` are installed
into `<local-directory>/bin`; no global install or release publication is needed.

## Windows

From PowerShell or Command Prompt, the launcher configures, builds, and optionally
runs the full test suite:

```powershell
.\build.cmd -Test
.\build.cmd -Backend Make -Compiler g++ -Test
.\build.cmd -Backend Ninja -Test
.\build.cmd -Backend Direct -Compiler g++
.\build.cmd -Backend Direct -Compiler g++ -Test
.\build.cmd -Backend Direct -Compiler g++ -Static
```

`build.cmd` launches `build.ps1` using an execution-policy bypass scoped to that
one PowerShell process. It does not change the machine/user policy; organization
policies can still prohibit execution. Where script execution is already allowed,
you can invoke `./build.ps1` directly with the same arguments.

The default CMake backend selects MinGW Makefiles when both `g++` and
`mingw32-make` are available; otherwise CMake chooses its default generator.
Select a generator explicitly when using another Windows toolchain:

```powershell
.\build.cmd -Backend CMake -Generator "Visual Studio 17 2022" -Test
.\build.cmd -Backend CMake -Generator Ninja -Compiler clang++ -Test
.\build.cmd -Configuration Debug -Test -Jobs 4
.\build.cmd -Help
```

The Visual Studio generator name must match your installed version. CMake handles
MSVC's compiler flags. `-Backend Make` specifically selects MinGW Makefiles;
`-Backend Ninja` requires Ninja on `PATH`.

## Linux and macOS

The same Bash script works on both platforms; invoke it with Bash so no executable
permission adjustment is required:

```bash
bash build.sh --test
bash build.sh --backend make --test
bash build.sh --backend ninja --test
bash build.sh --backend direct --compiler g++
bash build.sh --backend direct --compiler clang++ --test
bash build.sh --config Debug --test --jobs 4
bash build.sh --help
```

Direct builds use `g++` by default on Linux and `clang++` on macOS. Release uses
`-O3 -DNDEBUG`; Debug uses `-O0 -g`; RelWithDebInfo uses `-O2 -g -DNDEBUG`.
Warnings are enabled in all configurations. `--static` is optional for direct
GCC/Clang builds on platforms with the required static libraries; it is rejected
on macOS, where fully static executables are not supported by this helper.

## GNU Make shortcuts

```bash
make
make test
make CONFIG=Debug JOBS=4 test
make BACKEND=ninja test
make direct CXX=g++
make direct-test CXX=clang++
make help
```

These targets delegate to `build.sh`: Make/Ninja incrementality comes from CMake,
not a second hand-maintained dependency graph. Direct builds recompile each
required translation unit once per invocation, then link the tools (and test
executables with `--test`). Shared sources are not recompiled for every tool.
On Windows, prefer `build.cmd`; the Makefile additionally works from a configured
MSYS2/Git Bash environment with GNU Make and MinGW build tools available.
The shell helper checks which Makefile generators the selected CMake executable
supports, so MSYS CMake and native Windows CMake can use their respective tools.

## Output directories and tests

Default output is `build/<backend>-<configuration>`, for example
`build/cmake-Release` or `build/direct-Debug`. CMake, Visual Studio and direct
builds all place `discc`, `discas`, `discld` and test executables in that
directory's `bin/` subdirectory (with `.exe` on Windows). Switching Visual Studio
configuration reuses that location; use separate build directories if both
Debug and Release executables must remain available simultaneously.

Override the directory with `-BuildDir` / `--build-dir`. Relative directories are
relative to the caller's current directory; source paths are relative to the
helper itself, so it also works when invoked from elsewhere. Use a separate build
directory when changing compilers, generators, toolchains, or static-link mode.
The helpers never erase an existing directory to resolve a configuration conflict.

`-Test` / `--test` runs the full native suite. For direct builds, **CMake is still
needed to run the existing regression scripts**, but no CMake project is
configured and no Make/Ninja installation is needed. Without the test option,
direct builds require only a compiler and the platform shell utilities.
The shared source manifest is `cmake/build-sources.txt`; the shared regression
registry is `tests/toolchain-cases.cmake`. Add new core source files and regression
cases there so all build paths stay synchronized.

`AssemblyGenerator.cpp` belongs to the shared object group: both `discc` and
`discld` use it for canonical/final assembly export. Hand-maintained `g++`
commands must include it when linking either tool; the supplied helpers read
the manifest automatically.

GSU execution regressions verify payload results and stack behavior using a
bounded instruction model, not a complete SNES emulator. See [testing.md](testing.md).

## Experimental DOS cross-build

Use CMake and an installed cross-toolchain; host-native tests cannot run DOS
executables, and the direct backend intentionally does not implement DOS builds:

```bash
bash build.sh --backend ninja --dos \
  --toolchain cmake/dos-djgpp.cmake --build-dir build/dos-Release
```

```powershell
.\build.cmd -Backend Ninja -Dos -Toolchain cmake/dos-djgpp.cmake -BuildDir build/dos-Release
```

The supplied toolchain describes the Linux-hosted DJGPP binaries under `.djgpp`.
With `DISCO_DOS_BUILD=ON`, DJGPP uses GNU C++14: its libc hides required
filesystem APIs such as `getcwd` and `stat` in strict ISO mode. Native builds
remain strict C++23, and DOS builds do not build or run host-native tests.
The PowerShell example requires a toolchain file appropriate for the Windows host;
the helper does not download or translate cross-compilers. Other cross-builds
can supply a toolchain without enabling the DOS configuration.
