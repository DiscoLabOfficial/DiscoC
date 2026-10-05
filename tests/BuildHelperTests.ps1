[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:buildHelper = Join-Path (Split-Path $PSScriptRoot -Parent) 'build.ps1'
$fixtureParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$fixtureRoot = Join-Path $fixtureParent ('discoc-build-helper-' + [Guid]::NewGuid().ToString('N'))
$originalPath = $env:PATH
$originalCxx = $env:CXX
$fixtureCreated = $false
$cmakeInvocations = New-Object 'System.Collections.Generic.List[object]'
$script:cmakeInvocations = $cmakeInvocations

# Keep real application discovery, but capture the complete helper's native
# argument boundaries without configuring or compiling a project.
Set-Item -LiteralPath Function:cmake -Value {
    $cmakeInvocations.Add([string[]]$args)
    $global:LASTEXITCODE = 0
}.GetNewClosure()

function Assert-CompilerSelection([string]$Label, [hashtable]$HelperArguments, [string]$ExpectedPath) {
    $script:cmakeInvocations.Clear()
    $output = & $script:buildHelper @HelperArguments *>&1
    if ($LASTEXITCODE -ne 0) {
        throw "${Label}: helper failed: $($output | Out-String)"
    }
    if ($script:cmakeInvocations.Count -ne 2) {
        throw "${Label}: expected one configure and one build invocation."
    }
    $configure = $script:cmakeInvocations[0]
    $compilerArguments = @($configure | Where-Object { $_ -like '-DCMAKE_CXX_COMPILER=*' })
    if ($compilerArguments.Count -ne 1 -or
        $compilerArguments[0] -cne "-DCMAKE_CXX_COMPILER=$ExpectedPath") {
        throw "${Label}: expected one compiler path '$ExpectedPath'; received: $($compilerArguments -join ', ')"
    }
    if ($script:cmakeInvocations[1][0] -ne '--build') {
        throw "${Label}: expected configure before build."
    }
    Write-Host "PASS $Label"
}

try {
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    $fixtureCreated = $true
    $toolDirectories = @('first toolchain', 'second toolchain', 'third toolchain') |
        ForEach-Object { Join-Path $fixtureRoot $_ }
    # Copies supply genuine ApplicationInfo entries. These fixture executables
    # are never run; only the stubbed cmake command is invoked by the helper.
    $application = (Get-Command cmd.exe -CommandType Application | Select-Object -First 1).Source
    foreach ($directory in $toolDirectories) {
        New-Item -ItemType Directory -Path $directory | Out-Null
        Copy-Item -LiteralPath $application -Destination (Join-Path $directory 'g++.exe')
    }
    foreach ($tool in @('cmake.exe', 'mingw32-make.exe', 'ninja.exe')) {
        Copy-Item -LiteralPath $application -Destination (Join-Path $toolDirectories[0] $tool)
    }
    $env:PATH = ($toolDirectories -join ';') + ';' + $originalPath
    $env:CXX = $null
    $compilerMatches = @(Get-Command g++ -CommandType Application)
    if ($compilerMatches.Count -lt 3) {
        throw 'Fixture did not reproduce multiple compiler matches.'
    }
    $firstCompiler = Join-Path $toolDirectories[0] 'g++.exe'
    $fixtureBuild = Join-Path $fixtureRoot 'build output'
    Assert-CompilerSelection 'Make chooses first PATH match' @{ Backend = 'Make'; Compiler = 'g++'; BuildDir = $fixtureBuild } $firstCompiler
    Assert-CompilerSelection 'Ninja chooses first PATH match' @{ Backend = 'Ninja'; Compiler = 'g++'; BuildDir = $fixtureBuild } $firstCompiler
    Assert-CompilerSelection 'Default MinGW selection' @{ BuildDir = $fixtureBuild } $firstCompiler
    Assert-CompilerSelection 'Executable name with extension' @{ Backend = 'Make'; Compiler = 'g++.exe'; BuildDir = $fixtureBuild } $firstCompiler
    $explicitCompiler = Join-Path $toolDirectories[1] 'g++.exe'
    Assert-CompilerSelection 'Explicit path with spaces' @{ Backend = 'Make'; Compiler = $explicitCompiler; BuildDir = $fixtureBuild } $explicitCompiler
    $env:CXX = Join-Path $toolDirectories[2] 'g++.exe'
    Assert-CompilerSelection 'CXX path with spaces' @{ Backend = 'Make'; BuildDir = $fixtureBuild } $env:CXX

    $script:cmakeInvocations.Clear()
    $missingCompiler = 'discoc-missing-' + [Guid]::NewGuid().ToString('N')
    $originalErrorWriter = [Console]::Error
    $expectedDiagnostic = New-Object IO.StringWriter
    try {
        [Console]::SetError($expectedDiagnostic)
        & $script:buildHelper -Backend Make -Compiler $missingCompiler -BuildDir (Join-Path $fixtureRoot 'missing compiler')
        if ($LASTEXITCODE -ne 1 -or $script:cmakeInvocations.Count -ne 0) {
            throw 'Missing compiler must fail before configuring or building.'
        }
        if (-not $expectedDiagnostic.ToString().Contains("Required tool '$missingCompiler' was not found.")) {
            throw 'Missing compiler must report an actionable diagnostic.'
        }
    } finally {
        [Console]::SetError($originalErrorWriter)
        $expectedDiagnostic.Dispose()
    }
    Write-Host 'PASS missing compiler fails before CMake'
} finally {
    $env:PATH = $originalPath
    $env:CXX = $originalCxx
    # Remove only the uniquely owned fixture directory, never its parent.
    $resolvedRoot = [IO.Path]::GetFullPath($fixtureRoot)
    if ([IO.Path]::GetDirectoryName($resolvedRoot) -ne $fixtureParent -or
        [IO.Path]::GetFileName($resolvedRoot) -notmatch '^discoc-build-helper-[0-9a-f]{32}$') {
        throw 'Refusing to remove a fixture outside its intended temporary directory.'
    }
    if ($fixtureCreated -and (Test-Path -LiteralPath $resolvedRoot)) {
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}
Write-Host 'All Windows build-helper regression tests passed'
exit 0
