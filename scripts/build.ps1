<#
.SYNOPSIS
    Configure, build and test HDR Hint with the Visual Studio generator.

.EXAMPLE
    pwsh scripts/build.ps1                 # Release build + tests
    pwsh scripts/build.ps1 -Config Debug
    pwsh scripts/build.ps1 -Target hdrhint_tests -NoTest
#>
param(
    [ValidateSet("Release", "Debug")][string]$Config = "Release",
    [string]$Target = "",
    [switch]$NoTest,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

if ($Clean -and (Test-Path $build)) {
    Remove-Item -Recurse -Force $build
}

# Configure once; CMake reuses the cache afterwards.
if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$targetArgs = @()
if ($Target) { $targetArgs = @("--target", $Target) }
cmake --build $build --config $Config @targetArgs -- /m /nologo /verbosity:minimal
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $NoTest -and (-not $Target -or $Target -eq "hdrhint_tests")) {
    ctest --test-dir $build -C $Config --output-on-failure
    exit $LASTEXITCODE
}
