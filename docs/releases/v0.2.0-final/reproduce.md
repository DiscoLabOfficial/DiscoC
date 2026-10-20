# Reproduce the v0.2.0 final-candidate checks

This is a procedure, not a passing report. Use a **clean checkout of the exact
candidate commit** after the final metadata is committed. Do not substitute the
RC revision or assume a later merge has the same identity. Keep the source
unchanged through builds, measurements and packaging. Use a separate checkout
if needed; do not discard unrelated work.

Run the following blocks in one Developer PowerShell for Visual Studio 2022
(x64) session at the repository root, with C++23, CMake 3.21+ and Ninja available.
Set the external paths to a GSU-capable Mesen/MesenCE build exposing the required
Lua/state APIs, its matching core, and WLA-DX's W65816/WLALINK. Nothing downloads
or bundles these tools. OpenSNES is not needed. MinGW and other platform checks
are separate runs using the [documented presets](../../building.md).

## Freeze the source and select fresh directories

```powershell
$ErrorActionPreference = 'Stop'
# Optionally set DISCO_SOURCE_REVISION to the expected full candidate SHA.
$actualRevision = git rev-parse HEAD
if ($LASTEXITCODE) { throw 'Cannot identify source revision' }
$revision = if ($env:DISCO_SOURCE_REVISION) { $env:DISCO_SOURCE_REVISION } else { $actualRevision }
if ($revision -notmatch '^[0-9a-f]{40}$') { throw 'Expected a full source SHA' }
if ($actualRevision -ne $revision) { throw 'Wrong source revision' }
$sourceState = @(git status --porcelain --untracked-files=all)
if ($LASTEXITCODE -or $sourceState.Count) { throw 'Use a clean frozen checkout' }
$root = (Get-Location).Path
$final = Join-Path $root "build/stable-final/$revision"
if (Test-Path -LiteralPath $final) { throw 'Choose a fresh checkout/output tree; preserve prior evidence' }
New-Item -ItemType Directory -Path $final | Out-Null

$mesen = (Resolve-Path 'C:/external/Mesen/Mesen.exe').Path
$mesenCore = (Resolve-Path 'C:/external/Mesen/MesenCore.dll').Path
$wla = (Resolve-Path 'C:/external/WLA-DX/wla-65816.exe').Path
$wlalink = (Resolve-Path 'C:/external/WLA-DX/wlalink.exe').Path
$external = @("-DMESEN=$mesen", "-DWLA_65816=$wla", "-DWLALINK=$wlalink")
$tools = Join-Path $final 'release/bin'
Start-Transcript -Path "$final/reproduce.log"
$revision | Out-File -Encoding ascii "$final/source-sha.txt"
cmake --version
if ($LASTEXITCODE) { throw 'CMake unavailable' }
ninja --version
if ($LASTEXITCODE) { throw 'Ninja unavailable' }
Get-Item -LiteralPath $mesen,$mesenCore,$wla,$wlalink |
    Select-Object FullName,@{Name='FileVersion';Expression={$_.VersionInfo.FileVersion}} |
    ConvertTo-Json | Out-File -Encoding utf8 "$final/external-versions.json"
Get-FileHash -Algorithm SHA256 -LiteralPath $mesen,$mesenCore,$wla,$wlalink |
    ConvertTo-Json | Out-File -Encoding utf8 "$final/external-hashes.json"
```

Replace the example external paths before running. Record external tool versions
from their build metadata as well when file-version fields are unavailable.
The output root is ignored by Git; no report needs to embed the SHA of a commit
that also contains that report. Never reuse an earlier RC or final output folder.

## Clean Release and Debug, with actual test inventories

```powershell
cmake --preset release -B "$final/release" '-DDISCO_VERSION_PRERELEASE=' `
    '-DDISCO_TEST_MESEN_BENCHMARKS=ON' '-DDISCO_TEST_SNES_INTEGRATION=ON' `
    "-DDISCO_MESEN=$mesen" "-DDISCO_WLA_65816=$wla" "-DDISCO_WLALINK=$wlalink"
if ($LASTEXITCODE) { throw 'Release configure failed' }
cmake --build "$final/release" --parallel 4
if ($LASTEXITCODE) { throw 'Release build failed' }

cmake --preset debug -B "$final/debug" '-DDISCO_VERSION_PRERELEASE=' `
    '-DDISCO_TEST_MESEN_BENCHMARKS=OFF' '-DDISCO_TEST_SNES_INTEGRATION=OFF'
if ($LASTEXITCODE) { throw 'Debug configure failed' }
cmake --build "$final/debug" --parallel 4
if ($LASTEXITCODE) { throw 'Debug build failed' }

foreach ($configuration in @('release','debug')) {
    $directory = "$final/$configuration"
    $inventoryText = ctest --test-dir $directory --show-only=json-v1
    if ($LASTEXITCODE) { throw "$configuration inventory failed" }
    $inventory = ($inventoryText -join "`n") | ConvertFrom-Json
    if (@($inventory.tests).Count -eq 0) { throw 'Empty test inventory' }
    $inventoryText | Out-File -Encoding utf8 "$directory/test-inventory.json"
    Write-Host "$configuration registered tests: $(@($inventory.tests).Count)"
    ctest --test-dir $directory --output-on-failure --parallel 4 `
        --output-log "$directory/ctest.log" --output-junit "$directory/ctest.xml"
    if ($LASTEXITCODE) { throw "$configuration tests failed" }
    [xml]$junit = Get-Content -Raw -LiteralPath "$directory/ctest.xml"
    $suite = $junit.DocumentElement
    if ($suite.LocalName -ne 'testsuite' -or
        @($suite.SelectNodes('testcase')).Count -ne @($inventory.tests).Count -or
        @($suite.SelectNodes('testcase/failure|testcase/error|testcase/skipped')).Count) {
        throw "$configuration has missing, failed, or skipped test results"
    }
}
Get-ChildItem -LiteralPath $tools -Filter '*.exe' | Get-FileHash -Algorithm SHA256 |
    ConvertTo-Json | Out-File -Encoding utf8 "$final/release-tool-hashes.json"
```

Keep configure/build output, compiler identity/version and flags from the fresh
CMake trees along with inventories and JUnit/logs. Report the actual registered
and executed totals for each configuration; do not carry forward RC totals.
Release enables optional emulator tests, Debug does not. Run no other Mesen
workflow concurrently with Release CTest or the blocks below. Missing tools,
timeouts, skipped/not-run tests and incomplete artifacts are failures, not passes.
Do not raise timeouts, frame caps or resource bounds to obtain a result.

The default configure message must be exactly `DiscoC version: 0.2.0`, with no
trailing hyphen. Future prereleases can use
`-DDISCO_VERSION_PRERELEASE=rc.1` to obtain `0.2.0-rc.1`; do not use that override
for final packages.

## All nine workloads at O0, O1, O2 and Os

```powershell
foreach ($level in @('0','1','2','s')) {
    cmake "-DTOOLS_DIR=$tools" "-DOPTIMIZATION=$level" `
        "-DTOOLCHAIN_LABEL=v0.2.0-O$level" "-DTOOLCHAIN_REVISION=$revision" `
        "-DOUTPUT_DIR=$final/opcodes/O$level" -P benchmarks/run.cmake
    if ($LASTEXITCODE) { throw "Instruction-model O$level failed" }
}
foreach ($level in @('0','1','2','s')) {
    cmake "-DTOOLS_DIR=$tools" @external "-DOPTIMIZATION=$level" `
        "-DTOOLCHAIN_LABEL=v0.2.0-O$level" "-DTOOLCHAIN_REVISION=$revision" `
        "-DOUTPUT_DIR=$final/mesen/O$level" -P benchmarks/run-mesen.cmake
    if ($LASTEXITCODE) { throw "Mesen O$level failed" }
}
foreach ($kind in @('opcodes','mesen')) {
    foreach ($level in @('1','2','s')) {
        cmake "-DBASELINE=$final/$kind/O0/report.json" `
            "-DCURRENT=$final/$kind/O$level/report.json" `
            "-DOUTPUT_DIR=$final/comparisons/$kind/O0-O$level" -P benchmarks/compare.cmake
        if ($LASTEXITCODE) { throw 'Incompatible reports' }
    }
}
cmake "-DBASELINE=$final/mesen/O2/report.json" "-DCURRENT=$final/mesen/Os/report.json" `
    "-DOUTPUT_DIR=$final/comparisons/mesen/O2-Os" -P benchmarks/compare.cmake
if ($LASTEXITCODE) { throw 'Incompatible O2/Os reports' }
```

Omitting `CASE` selects all nine versioned workloads: `triangle_fill`,
`horizontal_span`, `rom_palette_plot`, `ram_palette_plot`, `memcpy`,
`function_call`, `switch_dense`, `switch_sparse` and `arithmetic`. The expected
coverage is nine times four independent executions per measurement path, not a
claim that they have passed. Mesen recalibrates, runs its own functional report,
checks result/memory/stack state and requires matching executed-opcode counts.
Compare the explicit model reports with Mesen's nested `opcodes/report.json`
for identical payload hashes and counters. Preserve raw JSON/CSV and per-case
artifacts, including tool/workload/harness/model/profile fingerprints.

The fixed benchmark profile is cold LoROM execution at `$00:8000`, NTSC,
100% GSU speed, CLSR=1 and normal multiplication. See the
[benchmark contract](../../../benchmarks/README.md) for calibrated STOP fetch
cost and exclusions. Do not mix this with RAM demo timing, presentation FPS or
physical-hardware claims. Cross-level comparisons cover observable results,
framebuffers or copy boundaries, not equality of dead stack bytes/registers.

## All complete-ROM demos, in series

Rotation, scaling, interactive shapes and gem-chess run at O2 and Os. Keep both
positive and deliberate-failure checks, assembly round-trips, framebuffer/PPU,
controller and timing evidence. The gem-chess and interactive-shapes hosts
intentionally do not support O0/O1. Never replace a failed full-scene run with
a compile-only claim.

```powershell
foreach ($level in @('2','s')) {
    foreach ($demo in @('rotating_triangle','scaling_triangle')) {
        cmake "-DDISCO_TOOLS_DIR=$tools" @external "-DOPTIMIZATION=$level" `
            '-DVERIFY_MESEN=ON' '-DMEASURE_GSU_TIMING=ON' `
            "-DOUTPUT_DIR=$final/demos/$demo-O$level" `
            -P "tests/graphics/$demo/build-snes.cmake"
        if ($LASTEXITCODE) { throw "$demo O$level failed" }
    }
    cmake "-DDISCO_TOOLS_DIR=$tools" @external "-DOPTIMIZATION=$level" `
        '-DVERIFY_MESEN=ON' '-DBENCHMARK=ON' `
        "-DOUTPUT_DIR=$final/demos/shapes-O$level" `
        -P tests/graphics/interactive_shapes/build-snes.cmake
    if ($LASTEXITCODE) { throw "Shapes O$level failed" }
    cmake "-DDISCO_TOOLS_DIR=$tools" @external "-DOPTIMIZATION=$level" `
        '-DVERIFY_MESEN=ON' "-DOUTPUT_DIR=$final/demos/gem-O$level" `
        -P examples/snes/gem_chess/build-snes.cmake
    if ($LASTEXITCODE) { throw "Gem-chess O$level failed" }
}

# The public triangle builder does not itself force region/GSU clock settings.
# Build once, then run both Lua oracles explicitly at the recorded fixed profile.
$triangle = "$final/demos/triangle"
cmake "-DDISCO_TOOLS_DIR=$tools" @external '-DVERIFY_MESEN=OFF' `
    "-DOUTPUT_DIR=$triangle" -P examples/snes/triangle/build-snes.cmake
if ($LASTEXITCODE) { throw 'Triangle build failed' }
foreach ($suffix in @('','-negative')) {
    $log = "$triangle/triangle$suffix-verification.log"
    $preview = "$triangle/triangle$suffix-preview.png"
    if ((Test-Path -LiteralPath $log) -or (Test-Path -LiteralPath $preview)) {
        throw 'Unexpected stale triangle evidence'
    }
    $arguments = @(
        '--testrunner', '--timeout=15', '--doNotSaveSettings',
        '--snes.region=Ntsc', '--snes.gsuClockSpeed=100',
        '--snes.enableRandomPowerOnState=false',
        '--snes.ppuExtraScanlinesBeforeNmi=0', '--snes.ppuExtraScanlinesAfterNmi=0',
        '--snes.disableFrameSkipping=true', '--debug.scriptWindow.allowIoOsAccess=true',
        ('"' + $root + '/examples/snes/triangle/verify-triangle' + $suffix + '.lua"'),
        ('"' + $triangle + '/triangle-test' + $suffix + '.sfc"')
    )
    $process = Start-Process -FilePath $mesen -ArgumentList $arguments `
        -WorkingDirectory $triangle -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $log) -or
        -not (Test-Path -LiteralPath $preview)) { throw 'Triangle execution/evidence failed' }
    if ((Get-Content -Raw -LiteralPath $log) -notmatch '^PASS triangle') {
        throw 'Triangle oracle failed'
    }
}
```

Rotation/scaling require every phase plus wrap and completed-image evidence;
shapes includes controlled input and wire/filled samples. Gem-chess validates
camera/controller publications, geometry, both buffers, framebuffer/VRAM/PPU,
STOP/ABI, reset, zoom/elevation and VBlank DMA, with ten named view captures and
a deliberate red failure screen. Preserve all generated logs and images; use
the verifiers' evidence, not a screenshot alone. The example SNES ROMs contain
no proprietary Nintendo code, assets or SDK components.

## Evidence and package handoff

```powershell
$endingRevision = git rev-parse HEAD
if ($LASTEXITCODE -or $endingRevision -ne $revision) { throw 'Source revision changed' }
$endingState = @(git status --porcelain --untracked-files=all)
if ($LASTEXITCODE -or $endingState.Count) { throw 'Source changed during validation' }
Stop-Transcript
```

Generate the validation summary, aggregate JSON/CSV, package `BUILD-INFO` and
SHA256 manifests from this run under the same ignored exact-SHA root. Include
the actual counts, failures, warnings, unrun scopes and tool/core hashes. Keep
original raw reports intact and use invariant decimal dots in derived CSV.
Each distributable must contain the tested tools, required licensing material,
exact source/build identity and dependency information; do not silently rebuild
different tools for packaging or claim standalone runtime support without checks.

Local MSVC/model/Mesen results do not stand in for exact-SHA hosted CI, other
platforms, sanitizers or review. Record those separately with the source SHA and
run URLs. Package preparation and these commands do **not** authorize a tag,
release, release-asset upload or merge. Publication remains a separate maintainer decision.
