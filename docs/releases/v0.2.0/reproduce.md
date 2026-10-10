# Reproduce the v0.2.0 source-candidate validation

Run from the repository root in a **clean checkout** of
`d967c3ef45a948ff457d81052448732da3bd73dc`. Use a separate checkout if necessary;
do not discard an existing working tree to reproduce these results. The source
must remain unchanged until measurements finish. Add reports/docs afterward,
without changing their recorded source revision.

These PowerShell commands describe the executed Windows workflow. A fresh
clone needs an external Mesen build with the recorded Lua/state APIs, its core,
WLA W65816 and WLALINK. Set their paths below; actual local paths, versions and
hashes are recorded in [environment.json](environment.json). OpenSNES is not
needed. Use **Developer PowerShell for Visual Studio 2022 (x64)**, CMake 3.21+
and Ninja. Native builds require C++23; this is not the C++14 DOS workflow.

## Freeze and build

```powershell
$revision = 'd967c3ef45a948ff457d81052448732da3bd73dc'
if ((git rev-parse HEAD) -ne $revision -or (git status --porcelain)) {
    throw 'Use the clean frozen source revision'
}
$root = (Get-Location).Path
$candidate = Join-Path $root 'build/release-candidate/v0.2.0-rc.1'
$mesen = (Resolve-Path 'build/mesence test/Mesen.exe').Path
$wla = (Resolve-Path 'build/mesence test/SNES Test/WLA-DX/wla-65816.exe').Path
$wlalink = (Resolve-Path 'build/mesence test/SNES Test/WLA-DX/wlalink.exe').Path
$tools = Join-Path $candidate 'release/bin'
$external = @("-DMESEN=$mesen", "-DWLA_65816=$wla", "-DWLALINK=$wlalink")

cmake --preset release -B "$candidate/release" `
    '-DDISCO_TEST_MESEN_BENCHMARKS=ON' '-DDISCO_TEST_SNES_INTEGRATION=ON' `
    "-DDISCO_MESEN=$mesen" "-DDISCO_WLA_65816=$wla" "-DDISCO_WLALINK=$wlalink"
if ($LASTEXITCODE) { throw 'Release configure failed' }
cmake --build "$candidate/release" --parallel 4
if ($LASTEXITCODE) { throw 'Release build failed' }
ctest --test-dir "$candidate/release" --output-on-failure --parallel 4 `
    --output-log "$candidate/release/ctest.log" --output-junit "$candidate/release/ctest.xml"
if ($LASTEXITCODE) { throw 'Release tests failed' }

cmake --preset debug -B "$candidate/debug" `
    '-DDISCO_TEST_MESEN_BENCHMARKS=OFF' '-DDISCO_TEST_SNES_INTEGRATION=OFF'
if ($LASTEXITCODE) { throw 'Debug configure failed' }
cmake --build "$candidate/debug" --parallel 4
if ($LASTEXITCODE) { throw 'Debug build failed' }
ctest --test-dir "$candidate/debug" --output-on-failure --parallel 4 `
    --output-log "$candidate/debug/ctest.log" --output-junit "$candidate/debug/ctest.xml"
if ($LASTEXITCODE) { throw 'Debug tests failed' }
```

Release/Debug builds ran in parallel in separate directories during the
recorded pass. Optional Mesen tests were enabled only in Release; avoid running
another emulator workflow concurrently. No tests were skipped to obtain the
248 default / 256 optional-enabled counts. Use an unused output directory for
a new run; the scripts reject stale emulator evidence.

## All nine workloads, all four levels

```powershell
foreach ($level in @('0','1','2','s')) {
    cmake "-DTOOLS_DIR=$tools" "-DOPTIMIZATION=$level" `
        "-DTOOLCHAIN_LABEL=v0.2.0-rc.1-O$level" "-DTOOLCHAIN_REVISION=$revision" `
        "-DOUTPUT_DIR=$candidate/opcodes/O$level" -P benchmarks/run.cmake
    if ($LASTEXITCODE) { throw "Functional O$level failed" }
}
foreach ($level in @('0','1','2','s')) {
    cmake "-DTOOLS_DIR=$tools" @external "-DOPTIMIZATION=$level" `
        "-DTOOLCHAIN_LABEL=v0.2.0-rc.1-O$level" "-DTOOLCHAIN_REVISION=$revision" `
        "-DOUTPUT_DIR=$candidate/mesen/O$level" -P benchmarks/run-mesen.cmake
    if ($LASTEXITCODE) { throw "Mesen O$level failed" }
}
```

No `CASE` means all nine workloads. Both scripts publish JSON/CSV only after
all cases pass. The Mesen script independently recalibrates, regenerates its
functional reports, builds complete SNES hosts, verifies RAM/register dumps and
requires matching executed-opcode counts. The explicit local functional run
and Mesen's nested functional run must agree on every counter/payload hash.

The underlying compiler/linker commands for each workload are:

```text
discc -O<level> --target gsu --memory-mapping lorom --execution-memory rom --origin 0x008000 <source.dc> -o <unit.o>
discld <objects> --origin 0x008000 --init-runtime --ram-bank 0 --ram-origin 0x0400 --stack-pointer 0xFFFE --entry main --emit-asm <final.s> -o <payload.bin>
discas <final.s> -o <linked.o>
disco_gsu_benchmarks <case> <payload.bin> <linked.o>
```

Mesen is invoked by `benchmarks/mesen/profile.cmake` with:

```text
Mesen.exe --testrunner --timeout=30 --doNotSaveSettings --snes.region=Ntsc --snes.gsuClockSpeed=100 --snes.enableRandomPowerOnState=false --snes.ppuExtraScanlinesBeforeNmi=0 --snes.ppuExtraScanlinesAfterNmi=0 --snes.disableFrameSkipping=true --debug.scriptWindow.allowIoOsAccess=true benchmarks/mesen/measure.lua <host.sfc>
```

Actual script/ROM paths are absolute and working directories are the isolated
per-case output folders. Settings are not saved. Benchmark host registers,
origin, cold-entry state and calibration are defined by the versioned source
scripts, not by ambient UI settings.

## Report comparisons

```powershell
foreach ($kind in @('opcodes','mesen')) {
    foreach ($level in @('1','2','s')) {
        cmake "-DBASELINE=$candidate/$kind/O0/report.json" `
            "-DCURRENT=$candidate/$kind/O$level/report.json" `
            "-DOUTPUT_DIR=$candidate/comparisons/$kind/O0-O$level" `
            -P benchmarks/compare.cmake
        if ($LASTEXITCODE) { throw 'Incompatible benchmark reports' }
    }
}
cmake "-DBASELINE=$candidate/mesen/O2/report.json" `
    "-DCURRENT=$candidate/mesen/Os/report.json" `
    "-DOUTPUT_DIR=$candidate/comparisons/mesen/O2-Os" -P benchmarks/compare.cmake
if ($LASTEXITCODE) { throw 'Incompatible O2/Os reports' }
```

Comparisons enforce suite/workload/model/profile identity and per-case results;
Mesen comparisons also enforce emulator/tool/harness identity and timing
contracts. Size/cycle deltas remain visible rather than forcing Os to meet an
O2 speed contract. Each independent execution already checks its expected
memory output. For additional cross-level hashes, compare the result word
(RAM offset `$0120`, two bytes), graphics framebuffer (offset `$6000`, 24,576
bytes), or memcpy source/destination/sentinels (`$0400..$0580` inclusive).
Do not demand equality of dead stack bytes or all register snapshots.

## RAM demos and host/PPU validation

Run in series after Release CTest and the official benchmark emulator runs:

```powershell
foreach ($level in @('2','s')) {
    cmake "-DDISCO_TOOLS_DIR=$tools" @external "-DOPTIMIZATION=$level" `
        '-DVERIFY_MESEN=ON' '-DMEASURE_GSU_TIMING=ON' `
        "-DOUTPUT_DIR=$candidate/rotation-O$level" `
        -P tests/graphics/rotating_triangle/build-snes.cmake
    if ($LASTEXITCODE) { throw 'Rotation validation failed' }
    cmake "-DDISCO_TOOLS_DIR=$tools" @external "-DOPTIMIZATION=$level" `
        '-DVERIFY_MESEN=ON' '-DBENCHMARK=ON' `
        "-DOUTPUT_DIR=$candidate/shapes-O$level" `
        -P tests/graphics/interactive_shapes/build-snes.cmake
    if ($LASTEXITCODE) { throw 'Shapes validation failed' }
}
cmake "-DDISCO_TOOLS_DIR=$tools" @external '-DVERIFY_MESEN=ON' `
    "-DOUTPUT_DIR=$candidate/triangle" -P examples/snes/triangle/build-snes.cmake
if ($LASTEXITCODE) { throw 'Official triangle validation failed' }
```

Rotation writes 65 GSU timing samples, 64 publication intervals, 64 screenshots
and positive/negative logs. Shapes writes four wire/four filled timing samples,
controller/image assertions and negative logs. Both scripts explicitly force
NTSC/100% and deterministic power-on. The public triangle builder does not
force region/clock percentage itself, so the recorded pass additionally reran
both generated ROMs explicitly:

```powershell
foreach ($suffix in @('','-negative')) {
    $log = "$candidate/triangle/triangle$suffix-verification.log"
    $preview = "$candidate/triangle/triangle$suffix-preview.png"
    foreach ($artifact in @($log, $preview)) {
        if (Test-Path -LiteralPath $artifact) { Remove-Item -LiteralPath $artifact }
    }
    $arguments = @(
        '--testrunner', '--timeout=15', '--doNotSaveSettings',
        '--snes.region=Ntsc', '--snes.gsuClockSpeed=100',
        '--snes.enableRandomPowerOnState=false',
        '--snes.ppuExtraScanlinesBeforeNmi=0', '--snes.ppuExtraScanlinesAfterNmi=0',
        '--snes.disableFrameSkipping=true', '--debug.scriptWindow.allowIoOsAccess=true',
        ('"' + $root + '/examples/snes/triangle/verify-triangle' + $suffix + '.lua"'),
        ('"' + $candidate + '/triangle/triangle-test' + $suffix + '.sfc"')
    )
    $process = Start-Process -FilePath $mesen -ArgumentList $arguments `
        -WorkingDirectory "$candidate/triangle" -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw 'Explicit-profile triangle run failed' }
    foreach ($artifact in @($log, $preview)) {
        if (-not (Test-Path -LiteralPath $artifact)) { throw "No fresh artifact: $artifact" }
    }
    $evidence = Get-Content -Raw -LiteralPath $log
    if ($evidence -notmatch '^PASS triangle') { throw $evidence }
}
```

Waiting for the GUI executable explicitly avoids reading an old PASS before
Mesen has finished. Only disposable verification logs/previews are removed;
the ROMs and source remain unchanged. Check fresh PASS artifacts and exit codes,
not screenshots alone. Record the
MesenCore DLL as well as executable hashes, tool versions and full source SHA.
Preserve raw report contents; these copies normalize CRLF to LF and remove
trailing JSON formatting whitespace, so their byte hashes need not match the
original files. Aggregate numeric CSV columns using invariant
decimal dots. No tag/release publication is part of these commands.

The hosted post-merge checks can be inspected read-only with:

```powershell
gh run view 37895498955 --repo DiscoLabOfficial/DiscoC --json headSha,status,conclusion,jobs,url
gh run view 37895498688 --repo DiscoLabOfficial/DiscoC --json headSha,status,conclusion,jobs,url
```
