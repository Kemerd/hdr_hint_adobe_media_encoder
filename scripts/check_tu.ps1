<#
.SYNOPSIS
    Compile a single HdrHint translation unit (no link) with the project's
    warning level and defines. Used for fast, conflict-free syntax checks while
    several people (or agents) edit different files at once.

.EXAMPLE
    pwsh scripts/check_tu.ps1 -File src/core/AmeLogParser.cpp
    pwsh scripts/check_tu.ps1 -File src/core/AmeLogParser.h    # header-only check (wrapped in a .cpp)
#>
param(
    [Parameter(Mandatory = $true)][string]$File,
    [string]$Std = "c++20"
)

$ErrorActionPreference = "Stop"

# Locate Visual Studio through vswhere so the script survives VS updates.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
if (-not $vs) { Write-Error "Visual Studio not found"; exit 2 }
$devCmd = Join-Path $vs "Common7\Tools\VsDevCmd.bat"

# Resolve project paths relative to this script (scripts/ -> hdr_hint/).
$root = Split-Path -Parent $PSScriptRoot
$file = (Resolve-Path $File).Path

# Headers are compiled through a throwaway .cpp that just includes them.
$outDir = Join-Path $env:TEMP "hdrhint_tu"
New-Item -ItemType Directory -Force $outDir | Out-Null
$leaf = Split-Path -Leaf $file
if ($file -match '\.h$') {
    $wrapper = Join-Path $outDir ($leaf + ".check.cpp")
    Set-Content -Path $wrapper -Value ("#include `"" + $file.Replace('\', '/') + "`"`nint hh_header_check_symbol = 0;`n") -Encoding utf8
    $file = $wrapper
    $leaf = Split-Path -Leaf $wrapper
}
$obj = Join-Path $outDir ($leaf + ".obj")

$flags = @(
    "/nologo", "/c", "/std:$Std", "/W4", "/permissive-", "/utf-8", "/EHsc",
    "/Zc:preprocessor", "/Zc:__cplusplus", "/Zc:inline", "/bigobj", "/wd4324", "/wd4458",
    "/DUNICODE", "/D_UNICODE", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/DSTRICT",
    "/D_CRT_SECURE_NO_WARNINGS", "/D_WIN32_WINNT=0x0A00", "/DWINVER=0x0A00", "/DNTDDI_VERSION=0x0A000006",
    "/I`"$root\src`"", "/I`"$root\third_party`"", "/I`"$root\resources`""
) -join " "

# One cmd invocation: set up the environment, then compile.
$cmd = "`"$devCmd`" -arch=amd64 -host_arch=amd64 -no_logo >nul 2>&1 && cl $flags `"$file`" /Fo`"$obj`""
cmd /c $cmd
exit $LASTEXITCODE
