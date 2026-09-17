<#
.SYNOPSIS
    Installs (or updates) the HDR Hint CEP panel for Adobe Media Encoder.

.DESCRIPTION
    Does the same eight steps as the in-app "Install AME panel" button:

      1. Copies the panel folder to %APPDATA%\Adobe\CEP\extensions\com.everett.hdrhint
      2. Writes config.json (exe path, installed version, pipe name) into it
      3. Sets HKCU\Software\Adobe\CSXS.9 .. CSXS.14  PlayerDebugMode = "1"
         (unpacked, unsigned extensions only load with this flag)
      4. Writes HKCU\Software\HdrHint\ExePath so the panel can find the exe
      5. Prints what to do next

    The script is idempotent: run it again after a rebuild to update in place.
    Nothing is downloaded; nothing outside HKCU and %APPDATA% is touched.

.PARAMETER Source
    The panel folder to install. Defaults to ..\cep\com.everett.hdrhint
    relative to this script.

.PARAMETER ExePath
    Full path of HdrHint.exe recorded in config.json and the registry.
    Defaults to ..\build\Release\HdrHint.exe relative to this script.

.PARAMETER Destination
    Override the CEP extensions folder (testing only).

.EXAMPLE
    pwsh scripts\install_panel.ps1
    pwsh scripts\install_panel.ps1 -ExePath "C:\Tools\HdrHint\HdrHint.exe"
    pwsh scripts\install_panel.ps1 -WhatIf
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$Source = "",
    [string]$ExePath = "",
    [string]$Destination = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---- constants ---------------------------------------------------------------
$ExtensionId   = "com.everett.hdrhint"
$PipeName      = "\\.\pipe\HdrHint"
$AppRegKey     = "HKCU:\Software\HdrHint"
$CsxsMajors    = 9..14
$Utf8NoBom     = [System.Text.UTF8Encoding]::new($false)

# ---- helpers -----------------------------------------------------------------
function Write-Step {
    param([string]$Text)
    Write-Host ("[install] " + $Text)
}

function Write-Warn {
    param([string]$Text)
    Write-Warning ("[install] " + $Text)
}

# Resolves a possibly-relative path against the repo root (hdr_hint\) without
# requiring the target to exist yet.
function Resolve-RepoPath {
    param([string]$Path, [string]$Root)
    if ([string]::IsNullOrWhiteSpace($Path)) { return "" }
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path $Root $Path))
}

# Reads ExtensionBundleVersion from a manifest; empty when unreadable.
function Get-ManifestVersion {
    param([string]$ManifestPath)
    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) { return "" }
    try {
        $xml = [xml](Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8)
        $root = $xml.DocumentElement
        if ($null -eq $root) { return "" }
        $attr = $root.GetAttribute("ExtensionBundleVersion")
        if ([string]::IsNullOrWhiteSpace($attr)) { return "" }
        return $attr.Trim()
    } catch {
        return ""
    }
}

# Sets a REG_SZ value, creating the key when needed. Honours -WhatIf.
function Set-RegString {
    param([string]$Key, [string]$Name, [string]$Value)
    if (-not (Test-Path -LiteralPath $Key)) {
        if ($PSCmdlet.ShouldProcess($Key, "Create registry key")) {
            New-Item -Path $Key -Force | Out-Null
        }
    }
    if ($PSCmdlet.ShouldProcess("$Key\$Name", "Set REG_SZ to '$Value'")) {
        New-ItemProperty -Path $Key -Name $Name -Value $Value -PropertyType String -Force | Out-Null
    }
}

# ---- resolve paths -----------------------------------------------------------
$repoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($Source)) { $Source = Join-Path $repoRoot ("cep\" + $ExtensionId) }
$Source = Resolve-RepoPath -Path $Source -Root $repoRoot

if ([string]::IsNullOrWhiteSpace($ExePath)) { $ExePath = Join-Path $repoRoot "build\Release\HdrHint.exe" }
$ExePath = Resolve-RepoPath -Path $ExePath -Root $repoRoot

if ([string]::IsNullOrWhiteSpace($Destination)) {
    $appData = $env:APPDATA
    if ([string]::IsNullOrWhiteSpace($appData)) { throw "APPDATA is not set; cannot locate the CEP extensions folder." }
    $Destination = Join-Path $appData ("Adobe\CEP\extensions\" + $ExtensionId)
}
$Destination = [System.IO.Path]::GetFullPath($Destination)

# ---- validate the source -----------------------------------------------------
$sourceManifest = Join-Path $Source "CSXS\manifest.xml"
if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
    throw "Panel source folder not found: $Source"
}
if (-not (Test-Path -LiteralPath $sourceManifest -PathType Leaf)) {
    throw "Not a CEP extension (missing CSXS\manifest.xml): $Source"
}
foreach ($required in @("index.html", "js\panel.js", "js\CSInterface.js", "jsx\host.jsx")) {
    if (-not (Test-Path -LiteralPath (Join-Path $Source $required) -PathType Leaf)) {
        throw "Panel source is incomplete (missing $required): $Source"
    }
}

$bundledVersion = Get-ManifestVersion -ManifestPath $sourceManifest
if ([string]::IsNullOrWhiteSpace($bundledVersion)) { $bundledVersion = "unknown" }

# The exe may not be built yet; that is a warning, not an error, because the
# panel's Locate... button can fix it later.
if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
    Write-Warn "HdrHint.exe not found at $ExePath - config.json will still point there. Build the app or pass -ExePath."
}

# ---- install vs update -------------------------------------------------------
$installedVersion = Get-ManifestVersion -ManifestPath (Join-Path $Destination "CSXS\manifest.xml")
if ([string]::IsNullOrWhiteSpace($installedVersion)) {
    Write-Step "Installing HDR Hint panel $bundledVersion"
} else {
    Write-Step "Updating HDR Hint panel $installedVersion -> $bundledVersion"
}
Write-Step "Source:      $Source"
Write-Step "Destination: $Destination"
Write-Step "Exe:         $ExePath"

# ---- step 1: copy the panel folder ------------------------------------------
if ($PSCmdlet.ShouldProcess($Destination, "Copy panel files")) {
    $parent = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    if (-not (Test-Path -LiteralPath $Destination -PathType Container)) {
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    }
    try {
        # Copy-Item with a trailing "\*" copies the contents (not the folder itself).
        Copy-Item -Path (Join-Path $Source "*") -Destination $Destination -Recurse -Force
    } catch {
        throw "Copy failed ($($_.Exception.Message)). If Adobe Media Encoder is running, close it and retry."
    }
    Write-Step "Copied panel files."
}

# ---- step 2: config.json -----------------------------------------------------
$config = [ordered]@{
    exePath          = $ExePath
    installedVersion = $bundledVersion
    pipeName         = $PipeName
}
$configJson = ($config | ConvertTo-Json -Depth 3)
$configPath = Join-Path $Destination "config.json"
if ($PSCmdlet.ShouldProcess($configPath, "Write config.json")) {
    [System.IO.File]::WriteAllText($configPath, $configJson + "`n", $Utf8NoBom)
    Write-Step "Wrote config.json"
}

# ---- step 3: PlayerDebugMode for every CEP major we might meet --------------
# Never lower an existing value: "1" stays "1"; anything else becomes "1".
foreach ($major in $CsxsMajors) {
    $key = "HKCU:\Software\Adobe\CSXS.$major"
    $current = $null
    if (Test-Path -LiteralPath $key) {
        try { $current = (Get-ItemProperty -LiteralPath $key -Name "PlayerDebugMode" -ErrorAction Stop).PlayerDebugMode } catch { $current = $null }
    }
    if ($null -ne $current -and [string]$current -eq "1") {
        continue
    }
    Set-RegString -Key $key -Name "PlayerDebugMode" -Value "1"
    Write-Step "CSXS.$major PlayerDebugMode = 1"
}

# ---- step 4: app registry key ------------------------------------------------
Set-RegString -Key $AppRegKey -Name "ExePath" -Value $ExePath
Set-RegString -Key $AppRegKey -Name "PanelPath" -Value $Destination
Set-RegString -Key $AppRegKey -Name "PanelVersion" -Value $bundledVersion
Write-Step "Registry: $AppRegKey\ExePath = $ExePath"

# ---- step 5: next steps ------------------------------------------------------
$ameRunning = $false
try {
    $ameRunning = $null -ne (Get-Process -Name "Adobe Media Encoder" -ErrorAction SilentlyContinue)
} catch {
    $ameRunning = $false
}

Write-Host ""
Write-Host "Done. Next steps:"
if ($ameRunning) {
    Write-Host "  1. Adobe Media Encoder is running: quit and restart it so it re-scans extensions."
} else {
    Write-Host "  1. Start Adobe Media Encoder."
}
Write-Host "  2. Open Window > Extensions > HDR Hint (the menu label may differ by version)."
Write-Host "  3. The panel starts HdrHint.exe automatically; use Launch or Locate... if it does not."
Write-Host ""
Write-Host "If the panel is not listed, check the CEP logs:"
Write-Host ("  " + (Join-Path $env:TEMP "CEP12-AME.log"))
Write-Host ("  " + (Join-Path $env:TEMP ("CEPHtmlEngine12-AME-<version>-" + $ExtensionId + ".log")) + "  (+ -renderer.log)")
Write-Host "Common causes: malformed manifest.xml, PlayerDebugMode missing for the CEP major in use,"
Write-Host "or a manifest Version newer than the installed CEP runtime."
Write-Host "Remote DevTools while the panel is open: http://localhost:8092"
