[CmdletBinding()]
param(
    [ValidateSet('CMake', 'Make', 'Ninja', 'Direct')]
    [string]$Backend = 'CMake',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$BuildDir,
    [string]$Compiler = $env:CXX,
    [string]$Generator,
    [ValidateRange(1, 256)][int]$Jobs = [Math]::Min(8, [Environment]::ProcessorCount),
    [switch]$Test,
    [switch]$Static,
    [string]$Toolchain,
    [switch]$Dos,
    [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue)) {
        throw "Required tool '$Name' was not found. Install it and add it to PATH. See docs/building.md."
    }
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "'$Program' failed with exit code $LASTEXITCODE."
    }
}

try {
    if ($Help) {
        Write-Output @'
DiscoC build helper (PowerShell 5.1+)
  .\build.ps1 [-Backend CMake|Make|Ninja|Direct] [-Test]
              [-Compiler g++|clang++|path] [-Configuration Release|Debug|RelWithDebInfo]
              [-BuildDir path] [-Jobs N] [-Generator name]
              [-Static] [-Toolchain path] [-Dos]
CMake is the default. Direct needs GCC/Clang; -Static is direct-only.
Direct -Test needs CMake only to run the existing regression scripts.
DOS cross-builds require CMake and -Toolchain; host tests are not run for DOS.
No tools are installed automatically and no build directories are deleted.
'@
        exit 0
    }
    if ($Static -and $Backend -ne 'Direct') { throw '-Static is supported only with -Backend Direct.' }
    if ($Test -and ($Dos -or $Toolchain)) { throw 'Host tests cannot run with -Dos or -Toolchain.' }
    if ($Dos -and -not $Toolchain) { throw '-Dos requires a cross-compilation -Toolchain file.' }
    if ($Backend -eq 'Direct' -and ($Generator -or $Dos -or $Toolchain)) {
        throw 'Direct builds do not accept -Generator, -Dos, or -Toolchain.'
    }
    if ($Generator -and $Backend -in @('Make', 'Ninja')) {
        throw 'Use -Generator with -Backend CMake, or select -Backend Make/Ninja without it.'
    }
    if (-not $BuildDir) { $BuildDir = "build/$($Backend.ToLowerInvariant())-$Configuration" }
    # Resolve paths without changing the caller's working directory.
    if ([IO.Path]::IsPathRooted($BuildDir)) {
        $buildPath = [IO.Path]::GetFullPath($BuildDir)
    } else {
        $buildPath = [IO.Path]::GetFullPath((Join-Path $PWD $BuildDir))
    }
    if ($buildPath.TrimEnd('\', '/') -eq $PSScriptRoot.TrimEnd('\', '/') -or
        $buildPath.TrimEnd('\', '/') -eq ([IO.Path]::GetPathRoot($buildPath)).TrimEnd('\', '/')) {
        throw 'Use a separate build directory, not the repository or filesystem root.'
    }

    if ($Backend -ne 'Direct') {
        Require-Command cmake
        if ($Test) { Require-Command ctest }
        if ($Backend -eq 'Make') {
            Require-Command mingw32-make
            $Generator = 'MinGW Makefiles'
            if (-not $Compiler) { $Compiler = 'g++' }
        } elseif ($Backend -eq 'Ninja') {
            Require-Command ninja
            $Generator = 'Ninja'
        } elseif (-not $Generator -and -not $Toolchain -and
                  (Get-Command g++ -ErrorAction SilentlyContinue) -and
                  (Get-Command mingw32-make -ErrorAction SilentlyContinue)) {
            # Prefer the available MinGW toolchain over an unconfigured MSVC environment.
            $Generator = 'MinGW Makefiles'
            if (-not $Compiler) { $Compiler = 'g++' }
        }
        $configureArgs = @('-S', $PSScriptRoot, '-B', $buildPath,
            "-DCMAKE_BUILD_TYPE=$Configuration", '-DBUILD_TESTING=ON')
        if ($Generator) { $configureArgs += @('-G', $Generator) }
        if ($Compiler) {
            Require-Command $Compiler
            # Application discovery can return every installed compiler. Keep
            # PATH precedence instead of interpolating all paths into one value.
            $compilerPath = (Get-Command $Compiler -CommandType Application | Select-Object -First 1).Source
            $configureArgs += "-DCMAKE_CXX_COMPILER=$compilerPath"
        }
        if ($Toolchain) {
            $toolchainPath = (Resolve-Path -LiteralPath $Toolchain).Path
            $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchainPath"
        }
        if ($Dos) { $configureArgs += '-DDISCO_DOS_BUILD=ON' }
        Invoke-Checked cmake $configureArgs
        Invoke-Checked cmake @('--build', $buildPath, '--config', $Configuration, '--parallel', "$Jobs")
        if ($Test) {
            Invoke-Checked ctest @('--test-dir', $buildPath, '-C', $Configuration, '--output-on-failure', '--no-tests=error')
        }
    } else {
        if (-not $Compiler) { $Compiler = 'g++' }
        Require-Command $Compiler
        if ($Test) { Require-Command cmake }
        $groups = @{}
        foreach ($line in Get-Content -LiteralPath "$PSScriptRoot/cmake/build-sources.txt") {
            if (-not $line.Trim() -or $line.StartsWith('#')) { continue }
            $fields = $line -split '\s+'
            $sources = @()
            foreach ($field in $fields[1..($fields.Length - 1)]) {
                if ($field.StartsWith('@')) {
                    $dependency = $field.Substring(1)
                    if (-not $groups.ContainsKey($dependency)) { throw "Unknown source group: $dependency" }
                    $sources += $groups[$dependency]
                } else { $sources += $field }
            }
            $groups[$fields[0]] = $sources
        }
        $targets = @('discc', 'discld', 'discas')
        if ($Test) {
            $targets += @('disco_object_tests', 'disco_ir_verifier_tests', 'disco_linear_scan_tests',
                'disco_target_foundation_tests', 'disco_gsu_execution_tests', 'disco_gsu_mapping_tests',
                'disco_assembly_export_tests', 'disco_language_contract_tests', 'disco_project_manifest_tests',
                'disco_module_loader_tests', 'disco_linker_hardening_tests')
        }
        $flags = @('-std=c++23', '-Wall', '-Wextra', '-Wpedantic', '-I', "$PSScriptRoot/src")
        switch ($Configuration) {
            Debug { $flags += @('-O0', '-g') }
            Release { $flags += @('-O3', '-DNDEBUG') }
            RelWithDebInfo { $flags += @('-O2', '-g', '-DNDEBUG') }
        }
        $linkFlags = @()
        if ($Static) { $linkFlags += '-static' }
        $binaryPath = Join-Path $buildPath 'bin'
        New-Item -ItemType Directory -Force -Path $binaryPath | Out-Null
        $objectDir = Join-Path $buildPath 'obj'
        New-Item -ItemType Directory -Force -Path $objectDir | Out-Null
        $compiled = @{}
        foreach ($target in $targets) {
            $objects = @()
            foreach ($source in $groups[$target]) {
                $object = Join-Path $objectDir ($source.Replace('/', '_') + '.o')
                if (-not $compiled.ContainsKey($source)) {
                    Write-Host "Compiling $source"
                    Invoke-Checked $Compiler ($flags + @('-c', "$PSScriptRoot/$source", '-o', $object))
                    $compiled[$source] = $true
                }
                $objects += $object
            }
            Write-Host "Linking $target.exe"
            Invoke-Checked $Compiler ($objects + $linkFlags + @('-o', "$binaryPath/$target.exe"))
        }
        if ($Test) {
            Invoke-Checked cmake @("-DBIN_DIR=$binaryPath", "-DROOT_DIR=$PSScriptRoot", '-DEXE_SUFFIX=.exe',
                '-P', "$PSScriptRoot/tests/run_prebuilt_tests.cmake")
        }
    }
    Write-Host "Build completed: $buildPath"
    Write-Host "Executables: $(Join-Path $buildPath 'bin')"
} catch {
    [Console]::Error.WriteLine("Build failed: $($_.Exception.Message)")
    exit 1
}
