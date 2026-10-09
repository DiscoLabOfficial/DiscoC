#!/usr/bin/env bash
# Bash 3.2 compatible, including the version shipped with macOS.
# Bash 3.2 treats empty arrays as unset with nounset; all options are initialized
# explicitly, and missing option arguments are checked before accessing $2.
set -eo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
backend=cmake
configuration=Release
build_dir=
compiler=${CXX:-}
generator=
jobs=4
run_tests=0
static_link=0
toolchain=
dos=0

fail() { printf 'Build failed: %s\n' "$*" >&2; exit 1; }
require() { command -v "$1" >/dev/null 2>&1 || fail "Required tool '$1' not found. See docs/building.md."; }
value() { [[ $# -ge 2 && -n $2 ]] || fail "Missing value for $1"; }
while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend) value "$@"; backend=$2; shift 2 ;;
        --config) value "$@"; configuration=$2; shift 2 ;;
        --build-dir) value "$@"; build_dir=$2; shift 2 ;;
        --compiler) value "$@"; compiler=$2; shift 2 ;;
        --generator) value "$@"; generator=$2; shift 2 ;;
        --jobs) value "$@"; jobs=$2; shift 2 ;;
        --toolchain) value "$@"; toolchain=$2; shift 2 ;;
        --test) run_tests=1; shift ;;
        --static) static_link=1; shift ;;
        --dos) dos=1; shift ;;
        --help|-h)
            printf '%s\n' 'DiscoC build helper (Linux/macOS; Bash 3.2+)' \
                '  bash build.sh [--backend cmake|make|ninja|direct] [--test]' \
                '                [--compiler g++|clang++|path] [--config Release|Debug|RelWithDebInfo]' \
                '                [--build-dir path] [--jobs N] [--generator name]' \
                '                [--static] [--toolchain path] [--dos]' \
                'Direct builds need GCC/Clang; --static is direct-only and unavailable on macOS.' \
                'Direct --test needs CMake to run the existing regression scripts.' \
                'DOS cross-builds require --toolchain; no host tests for cross-builds.' \
                'No automatic dependency installation or directory deletion.'
            exit 0 ;;
        *) fail "Unknown option: $1 (use --help)" ;;
    esac
done
case "$backend" in cmake|make|ninja|direct) ;; *) fail "Invalid backend: $backend" ;; esac
case "$configuration" in Debug|Release|RelWithDebInfo) ;; *) fail "Invalid configuration: $configuration" ;; esac
[[ $jobs =~ ^[1-9][0-9]*$ && ${#jobs} -le 3 && $jobs -le 256 ]] || fail '--jobs must be between 1 and 256'
[[ $static_link == 0 || $backend == direct ]] || fail '--static requires --backend direct'
[[ $run_tests == 0 || ( $dos == 0 && -z $toolchain ) ]] || fail 'Host tests cannot run with --dos or --toolchain'
[[ $dos == 0 || -n $toolchain ]] || fail '--dos requires a cross-compilation --toolchain file'
[[ $backend != direct || ( -z $generator && -z $toolchain && $dos == 0 ) ]] || fail 'Direct builds do not accept --generator, --toolchain, or --dos'
[[ -z $generator || ( $backend != make && $backend != ninja ) ]] || fail 'Use --generator with --backend cmake'
system=$(uname -s)
[[ $static_link == 0 || $system != Darwin ]] || fail 'Fully static linking is not supported on macOS'
if [[ -z $build_dir ]]; then build_dir="$root/build/$backend-$configuration"; fi
mkdir -p -- "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
[[ $build_dir != "$root" && $build_dir != / ]] || fail 'Use a separate build directory, not the repository or filesystem root'

if [[ $backend != direct ]]; then
    require cmake
    if [[ $run_tests == 1 ]]; then require ctest; fi
    case "$backend" in
        make)
            case "$system" in
                MINGW*|MSYS*|CYGWIN*)
                    # Native Windows CMake and MSYS CMake expose different generators.
                    case "$(cmake --help)" in
                        *'MinGW Makefiles'*) require mingw32-make; generator='MinGW Makefiles' ;;
                        *) require make; generator='Unix Makefiles' ;;
                    esac ;;
                *) require make; generator='Unix Makefiles' ;;
            esac ;;
        ninja) require ninja; generator=Ninja ;;
    esac
    configure=(-S "$root" -B "$build_dir" "-DCMAKE_BUILD_TYPE=$configuration" -DBUILD_TESTING=ON)
    if [[ -n $generator ]]; then configure+=(-G "$generator"); fi
    if [[ -n $compiler ]]; then require "$compiler"; configure+=("-DCMAKE_CXX_COMPILER=$(command -v "$compiler")"); fi
    if [[ -n $toolchain ]]; then
        [[ -f $toolchain ]] || fail "Toolchain file not found: $toolchain"
        toolchain="$(cd -- "$(dirname -- "$toolchain")" && pwd)/$(basename -- "$toolchain")"
        configure+=("-DCMAKE_TOOLCHAIN_FILE=$toolchain")
    fi
    if [[ $dos == 1 ]]; then configure+=(-DDISCO_DOS_BUILD=ON); fi
    cmake "${configure[@]}"
    cmake --build "$build_dir" --config "$configuration" --parallel "$jobs"
    if [[ $run_tests == 1 ]]; then
        ctest --test-dir "$build_dir" -C "$configuration" --output-on-failure --no-tests=error
    fi
else
    if [[ -z $compiler ]]; then
        if [[ $system == Darwin ]]; then compiler=clang++; else compiler=g++; fi
    fi
    require "$compiler"
    if [[ $run_tests == 1 ]]; then require cmake; fi
    flags=(-std=c++23 -Wall -Wextra -Wpedantic -I "$root/src")
    case "$configuration" in
        Debug) flags+=(-O0 -g) ;;
        Release) flags+=(-O3 -DNDEBUG) ;;
        RelWithDebInfo) flags+=(-O2 -g -DNDEBUG) ;;
    esac
    link_flags=()
    if [[ $static_link == 1 ]]; then link_flags+=(-static); fi
    targets=(discc discld discas)
    if [[ $run_tests == 1 ]]; then
        targets+=(disco_object_tests disco_ir_verifier_tests disco_linear_scan_tests
                  disco_target_foundation_tests disco_gsu_execution_tests disco_gsu_mapping_tests
                  disco_assembly_export_tests disco_language_contract_tests disco_project_manifest_tests
                  disco_module_loader_tests disco_linker_hardening_tests disco_gsu_benchmarks
                  disco_ir_local_optimizer_tests disco_ir_global_optimizer_tests disco_ir_value_optimizer_tests disco_gsu_cost_tests disco_ir_conditional_optimizer_tests disco_gsu_scheduler_tests disco_gsu_checked_proof_tests disco_gsu_size_optimization_tests)
    fi
    # Recursive lookup uses only Bash arrays, not Bash 4 associative arrays or eval.
    resolve_sources() {
        local wanted=$1 name rest field found=0
        local fields=()
        while read -r name rest; do
            [[ $name == "$wanted" ]] || continue
            found=1
            read -r -a fields <<< "$rest"
            for field in "${fields[@]}"; do
                if [[ $field == @* ]]; then resolve_sources "${field#@}"
                else resolved_sources+=("$field"); fi
            done
            break
        done < "$root/cmake/build-sources.txt"
        [[ $found == 1 ]] || fail "Unknown source group: $wanted"
    }
    mkdir -p -- "$build_dir/obj" "$build_dir/bin"
    compiled_sources=()
    suffix=
    case "$system" in MINGW*|MSYS*|CYGWIN*) suffix=.exe ;; esac
    for target in "${targets[@]}"; do
        resolved_sources=()
        resolve_sources "$target"
        objects=()
        for source in "${resolved_sources[@]}"; do
            object="$build_dir/obj/${source//\//_}.o"
            compiled=0
            for previous in "${compiled_sources[@]}"; do
                if [[ $previous == "$source" ]]; then compiled=1; break; fi
            done
            if [[ $compiled == 0 ]]; then
                printf 'Compiling %s\n' "$source"
                "$compiler" "${flags[@]}" -c "$root/$source" -o "$object"
                compiled_sources+=("$source")
            fi
            objects+=("$object")
        done
        printf 'Linking %s\n' "$target$suffix"
        "$compiler" "${objects[@]}" "${link_flags[@]}" -o "$build_dir/bin/$target$suffix"
    done
    if [[ $run_tests == 1 ]]; then
        cmake "-DBIN_DIR=$build_dir/bin" "-DROOT_DIR=$root" "-DEXE_SUFFIX=$suffix" \
            -P "$root/tests/run_prebuilt_tests.cmake"
    fi
fi
printf 'Build completed: %s\n' "$build_dir"
printf 'Executables: %s/bin\n' "$build_dir"
