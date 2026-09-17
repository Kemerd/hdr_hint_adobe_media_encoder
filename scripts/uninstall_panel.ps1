<#
.SYNOPSIS
    Removes the HDR Hint CEP panel from Adobe Media Encoder.

.DESCRIPTION
    Deletes %APPDATA%\Adobe\CEP\extensions\com.everett.hdrhint and the
    HKCU\Software\HdrHint registry key. The CSXS PlayerDebugMode values are
    left alone: other unpacked extensions may depend on them.

.PARAMETER Destination
    Override the installed panel folder (testing only).

.EXAMPLE
    pwsh scripts\uninstall_panel.ps1
    pwsh scripts\uninstall_panel.ps1 -WhatIf
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$Destination = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExtensionId = "com.everett.hdrhint"
$AppRegKey   = "HKCU:\Software\HdrHint"

function Write-Step {
    param([string]$Text)
    Write-Host ("[uninstall] " + $Text)
}

# ---- resolve the installed folder -------------------------------------------
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $appData = $env:APPDATA
    if ([string]::IsNullOrWhiteSpace($appData)) { throw "APPDATA is not set; cannot locate the CEP extensions folder." }
    $Destination = Join-Path $appData ("Adobe\CEP\extensions\" + $ExtensionId)
}
$Destination = [System.IO.Path]::GetFullPath($Destination)

# Refuse to delete anything that does not look like our extension: the folder
# name must end with the extension id so a mistyped -Destination cannot wipe
# an unrelated directory.
if (-not $Destination.TrimEnd('\').ToLowerInvariant().EndsWith($ExtensionId)) {
    throw "Refusing to remove '$Destination': the folder name does not end with $ExtensionId."
}

# ---- warn when AME is running -----------------------------------------------
$ameRunning = $false
try {
    $ameRunning = $null -ne (Get-Process -Name "Adobe Media Encoder" -ErrorAction SilentlyContinue)
} catch {
    $ameRunning = $false
}
if ($ameRunning) {
    Write-Warning "[uninstall] Adobe Media Encoder is running. The panel stays loaded until AME restarts; file removal may partially fail if the panel is open."
}

# ---- remove the panel folder -------------------------------------------------
if (Test-Path -LiteralPath $Destination -PathType Container) {
    if ($PSCmdlet.ShouldProcess($Destination, "Remove panel folder")) {
        try {
            Remove-Item -LiteralPath $Destination -Recurse -Force
            Write-Step "Removed $Destination"
        } catch {
            throw "Could not remove $Destination ($($_.Exception.Message)). Close Adobe Media Encoder and retry."
        }
    }
} else {
    Write-Step "Panel folder not present: $Destination"
}

# ---- remove the app registry key --------------------------------------------
if (Test-Path -LiteralPath $AppRegKey) {
    if ($PSCmdlet.ShouldProcess($AppRegKey, "Remove registry key")) {
        Remove-Item -LiteralPath $AppRegKey -Recurse -Force
        Write-Step "Removed $AppRegKey"
    }
} else {
    Write-Step "Registry key not present: $AppRegKey"
}

Write-Host ""
Write-Host "Done. HKCU\Software\Adobe\CSXS.*\PlayerDebugMode was left unchanged."
if ($ameRunning) {
    Write-Host "Restart Adobe Media Encoder to unload the panel."
}
