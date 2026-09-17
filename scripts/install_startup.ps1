<#
.SYNOPSIS
    Installs (or removes) the HDR Hint startup script inside Adobe Media Encoder.

.DESCRIPTION
    Media Encoder evaluates every .jsx in its "Scripts\Startup" folder while it
    launches. Dropping HdrHintLauncher.jsx there makes AME start HDR Hint
    itself, so nothing has to run with Windows and no service is involved.

    The folder lives under Program Files, so this script needs an elevated
    PowerShell. It stamps the executable's path next to the script as a
    fallback for the case where %APPDATA%\HdrHint\launcher.json is missing.

.PARAMETER AmeRoot
    Media Encoder's install folder. Autodetected when omitted; pass it for a
    non-standard install or a version this script does not know about.

.PARAMETER Exe
    HdrHint.exe to launch. Defaults to the build output next to this script.

.PARAMETER Uninstall
    Removes the startup script and the stamped path instead of installing.

.EXAMPLE
    pwsh -File scripts\install_startup.ps1
    pwsh -File scripts\install_startup.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string] $AmeRoot,
    [string] $Exe,
    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
# Windows console compatibility: force UTF-8 so the output is never mangled.
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$scriptName = 'HdrHintLauncher.jsx'
$stampName  = 'HdrHintExePath.txt'
$repoRoot   = Split-Path -Parent $PSScriptRoot

function Find-AmeRoot {
    <# All Media Encoder installs that have a Startup folder, newest first. #>
    $roots = @()
    foreach ($base in @("$env:ProgramFiles\Adobe", "${env:ProgramFiles(x86)}\Adobe")) {
        if (-not (Test-Path -LiteralPath $base)) { continue }
        Get-ChildItem -LiteralPath $base -Directory -Filter 'Adobe Media Encoder*' -ErrorAction SilentlyContinue |
            ForEach-Object {
                $startup = Join-Path $_.FullName 'Scripts\Startup'
                if (Test-Path -LiteralPath $startup) { $roots += $_.FullName }
            }
    }
    # "2026" sorts above "2025", so a plain descending sort picks the newest.
    return $roots | Sort-Object -Descending
}

# ---- resolve Media Encoder --------------------------------------------------
$targets = @()
if ($AmeRoot) {
    if (-not (Test-Path -LiteralPath $AmeRoot)) { throw "Media Encoder folder not found: $AmeRoot" }
    $targets = @($AmeRoot)
} else {
    $targets = @(Find-AmeRoot)
    if ($targets.Count -eq 0) { throw 'No Adobe Media Encoder install found. Pass -AmeRoot explicitly.' }
}

# ---- resolve the executable (install only) ----------------------------------
if (-not $Uninstall) {
    if (-not $Exe) { $Exe = Join-Path $repoRoot 'build\Release\HdrHint.exe' }
    if (-not (Test-Path -LiteralPath $Exe)) {
        throw "HdrHint.exe not found at '$Exe'. Build it first or pass -Exe."
    }
    $Exe = (Resolve-Path -LiteralPath $Exe).Path

    $source = Join-Path $repoRoot 'cep\startup\HdrHintLauncher.jsx'
    if (-not (Test-Path -LiteralPath $source)) { throw "Startup script missing: $source" }
}

$failed = 0
foreach ($root in $targets) {
    $startup = Join-Path $root 'Scripts\Startup'
    $target  = Join-Path $startup $scriptName
    $stamp   = Join-Path $startup $stampName

    try {
        if ($Uninstall) {
            foreach ($path in @($target, $stamp)) {
                if (Test-Path -LiteralPath $path) {
                    Remove-Item -LiteralPath $path -Force
                    Write-Host "Removed $path"
                } else {
                    Write-Host "Not present: $path"
                }
            }
            continue
        }

        Copy-Item -LiteralPath $source -Destination $target -Force
        # UTF-8 without BOM: the .jsx reader strips one anyway, but a clean
        # file is easier to inspect by hand.
        [IO.File]::WriteAllText($stamp, $Exe, (New-Object Text.UTF8Encoding($false)))
        Write-Host "Installed $target"
        Write-Host "  launches $Exe"
    } catch {
        $failed++
        Write-Warning "$root : $($_.Exception.Message)"
        if ($_.Exception -is [UnauthorizedAccessException]) {
            Write-Warning '  Run this script from an elevated PowerShell (Program Files is protected).'
        }
    }
}

if ($failed -gt 0) { exit 1 }

# ---- Media Encoder now owns the launching, so the Windows Run entry is
# ---- redundant: turn the setting off and remove the entry it wrote.
if (-not $Uninstall) {
    $ini = Join-Path $env:APPDATA 'HdrHint\settings.ini'
    if (Test-Path -LiteralPath $ini) {
        $text = Get-Content -LiteralPath $ini -Raw -Encoding UTF8
        if ($text -match '(?m)^\s*start_with_windows\s*=\s*true\s*$') {
            $text = $text -replace '(?m)^(\s*start_with_windows\s*=\s*)true\s*$', '${1}false'
            [IO.File]::WriteAllText($ini, $text, (New-Object Text.UTF8Encoding($false)))
            Write-Host 'Turned off "Start with Windows" (Media Encoder starts HDR Hint now).'
        }
    }
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    if (Get-ItemProperty -Path $runKey -Name 'HdrHint' -ErrorAction SilentlyContinue) {
        Remove-ItemProperty -Path $runKey -Name 'HdrHint' -ErrorAction SilentlyContinue
        Write-Host 'Removed the Windows Run entry.'
    }
}

if ($Uninstall) {
    Write-Host ''
    Write-Host 'Media Encoder will no longer start HDR Hint.'
} else {
    Write-Host ''
    Write-Host 'Restart Media Encoder. HDR Hint starts with it, in the tray, and docks'
    Write-Host 'onto the HDR Hint panel when you open Window > Extensions > HDR Hint.'
}
