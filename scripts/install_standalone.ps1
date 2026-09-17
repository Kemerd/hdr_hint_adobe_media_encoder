<#
.SYNOPSIS
    Installs HDR Hint as a standalone watch-folder tool, with no Adobe software
    involved.

.DESCRIPTION
    Copies the built application (exe, LUTs, guide, CEP panel) out of the build
    tree into a real install folder, so rebuilding or moving the repository
    later never breaks the install. Optionally starts it with Windows, in the
    tray, watching the folders you configure in Settings.

    Nothing is registered as a service and nothing is written outside the
    current user: the install folder, one Start Menu shortcut and (with
    -StartWithWindows) one HKCU Run entry.

    HDR Hint works the same whether Media Encoder exists or not. It watches the
    folders listed in Settings > Watch folders and processes any new video that
    appears. When Media Encoder is present it also reads AME's own log, and a
    file seen by both paths is still only processed once.

.PARAMETER Destination
    Where to install. Defaults to %LOCALAPPDATA%\Programs\HdrHint.

.PARAMETER Source
    The build output to copy from. Defaults to build\Release next to this repo.

.PARAMETER WatchFolder
    One or more folders to watch. They are written into settings.ini, so the
    app is ready to use on first launch. Repeat the switch for several folders.

.PARAMETER StartWithWindows
    Adds an HKCU Run entry that starts HDR Hint into the tray at logon.

.PARAMETER NoShortcut
    Skips the Start Menu shortcut.

.PARAMETER Launch
    Starts HDR Hint once the install finishes.

.PARAMETER Uninstall
    Removes the Run entry, the shortcut and the install folder. Settings and
    logs in %APPDATA%\HdrHint and %LOCALAPPDATA%\HdrHint are left alone.

.EXAMPLE
    pwsh -File scripts\install_standalone.ps1 -WatchFolder "D:\Renders" -StartWithWindows -Launch

.EXAMPLE
    pwsh -File scripts\install_standalone.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]   $Destination = "$env:LOCALAPPDATA\Programs\HdrHint",
    [string]   $Source,
    [string[]] $WatchFolder,
    [switch]   $StartWithWindows,
    [switch]   $NoShortcut,
    [switch]   $Launch,
    [switch]   $Uninstall
)

$ErrorActionPreference = 'Stop'
# Windows console compatibility: force UTF-8 so nothing is mangled.
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$repoRoot    = Split-Path -Parent $PSScriptRoot
$runKey      = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValue    = 'HdrHint'
$startMenu   = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\HDR Hint.lnk'
$settingsIni = Join-Path $env:APPDATA 'HdrHint\settings.ini'

# ---- uninstall ---------------------------------------------------------------
if ($Uninstall) {
    if (Get-ItemProperty -Path $runKey -Name $runValue -ErrorAction SilentlyContinue) {
        Remove-ItemProperty -Path $runKey -Name $runValue
        Write-Host 'Removed the Windows Run entry.'
    }
    if (Test-Path -LiteralPath $startMenu) {
        Remove-Item -LiteralPath $startMenu -Force
        Write-Host 'Removed the Start Menu shortcut.'
    }
    # Stop a running copy so the folder is not locked.
    Get-Process -Name 'HdrHint' -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Path -and $_.Path.StartsWith($Destination, [StringComparison]::OrdinalIgnoreCase)) {
            $_ | Stop-Process -Force
            Start-Sleep -Milliseconds 500
        }
    }
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
        Write-Host "Removed $Destination"
    }
    Write-Host ''
    Write-Host 'Settings and logs were kept. Delete these by hand to remove every trace:'
    Write-Host "  $env:APPDATA\HdrHint"
    Write-Host "  $env:LOCALAPPDATA\HdrHint"
    exit 0
}

# ---- locate the build --------------------------------------------------------
if (-not $Source) { $Source = Join-Path $repoRoot 'build\Release' }
$exeSource = Join-Path $Source 'HdrHint.exe'
if (-not (Test-Path -LiteralPath $exeSource)) {
    throw "HdrHint.exe not found in '$Source'. Build it first (pwsh scripts/build.ps1) or pass -Source."
}

# ---- copy ---------------------------------------------------------------------
# Stop a running copy from the destination first, or the files are locked.
Get-Process -Name 'HdrHint' -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.Path -and $_.Path.StartsWith($Destination, [StringComparison]::OrdinalIgnoreCase)) {
        Write-Host 'Stopping the running copy...'
        $_ | Stop-Process -Force
        Start-Sleep -Milliseconds 700
    }
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

# Only the files a user needs: no .pdb, .lib, .exp or the test runner.
$files = @('HdrHint.exe', 'hdrhint_cli.exe', 'GUIDE.md')
foreach ($name in $files) {
    $from = Join-Path $Source $name
    if (Test-Path -LiteralPath $from) {
        Copy-Item -LiteralPath $from -Destination (Join-Path $Destination $name) -Force
    }
}
foreach ($dir in @('luts', 'cep')) {
    $from = Join-Path $Source $dir
    if (Test-Path -LiteralPath $from) {
        $to = Join-Path $Destination $dir
        if (Test-Path -LiteralPath $to) { Remove-Item -LiteralPath $to -Recurse -Force }
        Copy-Item -LiteralPath $from -Destination $to -Recurse -Force
    }
}
$exe = Join-Path $Destination 'HdrHint.exe'
Write-Host "Installed to $Destination"

# ---- watch folders ------------------------------------------------------------
# Written straight into settings.ini so the app is useful on first launch. The
# app rewrites this file on exit, so only touch it while it is not running.
if ($WatchFolder) {
    $valid = @()
    foreach ($folder in $WatchFolder) {
        if (Test-Path -LiteralPath $folder) {
            $valid += (Resolve-Path -LiteralPath $folder).Path
        } else {
            Write-Warning "Watch folder does not exist, skipped: $folder"
        }
    }
    if ($valid.Count -gt 0) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $settingsIni) | Out-Null
        # The app stores lists pipe-separated under [ame].
        $joined = ($valid -join '|')
        if (Test-Path -LiteralPath $settingsIni) {
            $text = Get-Content -LiteralPath $settingsIni -Raw -Encoding UTF8
            if ($text -match '(?m)^\s*extra_watch_folders\s*=') {
                $text = $text -replace '(?m)^(\s*extra_watch_folders\s*=).*$', "`${1} $joined"
            } elseif ($text -match '(?m)^\[ame\]') {
                $text = $text -replace '(?m)^(\[ame\])', "`${1}`r`nextra_watch_folders = $joined"
            } else {
                $text += "`r`n[ame]`r`nextra_watch_folders = $joined`r`n"
            }
            [IO.File]::WriteAllText($settingsIni, $text, (New-Object Text.UTF8Encoding($false)))
        } else {
            $seed = "[ame]`r`nextra_watch_folders = $joined`r`n"
            [IO.File]::WriteAllText($settingsIni, $seed, (New-Object Text.UTF8Encoding($false)))
        }
        Write-Host "Watching: $joined"
    }
}

# ---- Start Menu shortcut -------------------------------------------------------
if (-not $NoShortcut) {
    try {
        $shell = New-Object -ComObject WScript.Shell
        $link = $shell.CreateShortcut($startMenu)
        $link.TargetPath       = $exe
        $link.WorkingDirectory = $Destination
        $link.Description      = 'HDR Hint - HDR10 metadata and SDR-hint LUTs for finished exports'
        $link.Save()
        Write-Host 'Added a Start Menu shortcut.'
    } catch {
        Write-Warning "Could not create the Start Menu shortcut: $($_.Exception.Message)"
    }
}

# ---- start with Windows --------------------------------------------------------
if ($StartWithWindows) {
    Set-ItemProperty -Path $runKey -Name $runValue -Value "`"$exe`" --tray"
    Write-Host 'HDR Hint will start with Windows, hidden in the tray.'
} elseif (Get-ItemProperty -Path $runKey -Name $runValue -ErrorAction SilentlyContinue) {
    # An earlier install may have added it; leave it, but say so.
    Write-Host 'Note: an existing "start with Windows" entry was left in place.'
}

Write-Host ''
Write-Host 'Done. HDR Hint runs on its own: it watches the folders in Settings and'
Write-Host 'processes new videos with mkvmerge. Media Encoder is not required.'
if ($Launch) {
    Start-Process -FilePath $exe
    Write-Host 'Started.'
} else {
    Write-Host "Run it with: $exe"
}
