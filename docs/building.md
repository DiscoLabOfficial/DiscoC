# Building DiscoC

The build helpers support Windows, Linux, and macOS. CMake remains the primary
build system; the direct backend builds the same tools using GCC or Clang
without configuring a CMake project. All native paths use C++23. The experimental
DOS cross-build keeps its C++14 configuration.

The helpers do not install dependencies, delete build directories, change your
working directory, or modify persistent shell/security settings. Every failed
compile, link, or test stops the build and returns a nonzero exit code.

## Prerequisites

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
`build/cmake-Release` or `build/direct-Debug`. CMake multi-configuration generators
such as Visual Studio place executables in a configuration subdirectory; direct
builds place `discc`, `discas`, and `discld` directly in the selected build folder
(with `.exe` on Windows).

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
The PowerShell example requires a toolchain file appropriate for the Windows host;
the helper does not download or translate cross-compilers. Other cross-builds
can supply a toolchain without enabling the DOS configuration.
