<#
.SYNOPSIS
    Saves a PNG screenshot of a top-level window (default: the HDR Hint main window).

.DESCRIPTION
    Finds the window by class name (and optional title) with FindWindowW, or takes -Hwnd directly,
    then renders it with PrintWindow(PW_RENDERFULLCONTENT). That flag asks DWM for the composed
    surface, so DirectComposition / layered windows come out right even when partly covered.
    When PrintWindow fails, or returns an empty bitmap, the window rectangle is copied from the
    screen instead; that path needs the window to be visible and unobstructed.

    The script switches itself to Per-Monitor-V2 DPI awareness first, so GetWindowRect returns
    physical pixels and the capture is 1:1 on 125 % / 150 % displays.

.PARAMETER WindowClass
    Window class to look for. Default HdrHint.MainWindow.

.PARAMETER Title
    Optional exact window title to disambiguate several windows of the same class.

.PARAMETER Out
    PNG path. Default %TEMP%\HdrHint_<timestamp>.png. The parent folder is created when missing.

.PARAMETER Hwnd
    Capture this window handle instead of searching by class.

.EXAMPLE
    pwsh scripts\screenshot.ps1 -Out .\shot.png

.EXAMPLE
    pwsh scripts\screenshot.ps1 -WindowClass "Adobe Media Encoder 2026" -Out ame.png

.NOTES
    Output is plain ASCII so it is safe in any Windows console.
    Exit codes: 0 saved, 1 window not found or capture failed, 2 System.Drawing unavailable.
#>
[CmdletBinding()]
param(
    [string]$WindowClass = 'HdrHint.MainWindow',
    [string]$Title = '',
    [string]$Out = (Join-Path $env:TEMP ('HdrHint_{0}.png' -f (Get-Date).ToString('yyyyMMdd_HHmmss'))),
    [long]$Hwnd = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Win32 interop. Compiled once per session; the type check avoids "type already exists" on re-runs.
# ---------------------------------------------------------------------------
$interop = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class HhShot
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

    public const uint PW_RENDERFULLCONTENT = 0x00000002;
    public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr FindWindowW(string lpClassName, string lpWindowName);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr value);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr hWnd, StringBuilder buffer, int maxCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder buffer, int maxCount);

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out RECT value, int size);
}
'@

if (-not ('HhShot' -as [type])) {
    Add-Type -TypeDefinition $interop -ErrorAction Stop
}
try {
    Add-Type -AssemblyName System.Drawing -ErrorAction Stop
} catch {
    Write-Host 'System.Drawing is not available in this PowerShell host; run this under pwsh 7 on Windows.'
    exit 2
}

# Per-Monitor-V2 (-4). Fails harmlessly when the host process already fixed its awareness.
[void][HhShot]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Get-WindowClassName {
    param([IntPtr]$Handle)
    $sb = New-Object System.Text.StringBuilder 256
    [void][HhShot]::GetClassNameW($Handle, $sb, $sb.Capacity)
    return $sb.ToString()
}

function Get-WindowTitle {
    param([IntPtr]$Handle)
    $sb = New-Object System.Text.StringBuilder 512
    [void][HhShot]::GetWindowTextW($Handle, $sb, $sb.Capacity)
    return $sb.ToString()
}

function Test-HasContent {
    <#
    .SYNOPSIS  Samples an 8x8 grid; true when at least one pixel is non-transparent and not all are identical.
               PrintWindow can report success and still hand back a blank surface for some windows.
    #>
    param([System.Drawing.Bitmap]$Bitmap)

    if ($null -eq $Bitmap -or $Bitmap.Width -lt 1 -or $Bitmap.Height -lt 1) { return $false }
    $first = $null
    $distinct = 0
    $opaque = 0
    for ($y = 0; $y -lt 8; $y++) {
        for ($x = 0; $x -lt 8; $x++) {
            $px = $Bitmap.GetPixel([int](($Bitmap.Width - 1) * $x / 7), [int](($Bitmap.Height - 1) * $y / 7))
            if ($px.A -gt 0) { $opaque++ }
            if ($null -eq $first) { $first = $px.ToArgb() }
            elseif ($px.ToArgb() -ne $first) { $distinct++ }
        }
    }
    return ($opaque -gt 0 -and $distinct -gt 0)
}

function New-ArgbBitmap {
    param([int]$Width, [int]$Height)
    return New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
}

# ---------------------------------------------------------------------------
# Find the window
# ---------------------------------------------------------------------------
$handle = [IntPtr]::Zero
if ($Hwnd -ne 0) {
    $handle = [IntPtr]$Hwnd
} else {
    # PowerShell converts $null to "" for a .NET string parameter, which would make FindWindowW
    # look for a window with an EMPTY title. [NullString]::Value is the only way to pass a real NULL.
    $titleArg = if ($Title) { $Title } else { [NullString]::Value }
    $handle = [HhShot]::FindWindowW($WindowClass, $titleArg)
}

if ($handle -eq [IntPtr]::Zero -or -not [HhShot]::IsWindow($handle)) {
    $what = if ($Hwnd -ne 0) { "hwnd 0x$($Hwnd.ToString('X'))" } else { "class '$WindowClass'" + $(if ($Title) { ", title '$Title'" } else { '' }) }
    Write-Host "Window not found ($what). Is HdrHint.exe running? Use -WindowClass / -Title / -Hwnd to pick another window."
    exit 1
}

$className = Get-WindowClassName -Handle $handle
$windowTitle = Get-WindowTitle -Handle $handle

$rect = New-Object HhShot+RECT
if (-not [HhShot]::GetWindowRect($handle, [ref]$rect)) {
    Write-Host "GetWindowRect failed for hwnd 0x$($handle.ToInt64().ToString('X'))."
    exit 1
}
$width  = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) {
    Write-Host "Window has an empty rectangle ($width x $height); nothing to capture."
    exit 1
}
if ([HhShot]::IsIconic($handle)) {
    Write-Host 'Note: the window is minimised; PrintWindow may return an empty image and the screen fallback cannot see it.'
}
if (-not [HhShot]::IsWindowVisible($handle)) {
    Write-Host 'Note: the window is hidden; only PrintWindow can capture it, and it may return nothing.'
}

# ---------------------------------------------------------------------------
# Capture: PrintWindow(PW_RENDERFULLCONTENT) first
# ---------------------------------------------------------------------------
$method = 'PrintWindow'
$bitmap = New-ArgbBitmap -Width $width -Height $height
$captured = $false
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
try {
    $hdc = $graphics.GetHdc()
    try {
        $captured = [HhShot]::PrintWindow($handle, $hdc, [HhShot]::PW_RENDERFULLCONTENT)
    } finally {
        $graphics.ReleaseHdc($hdc)
    }
} finally {
    $graphics.Dispose()
}
if ($captured -and -not (Test-HasContent -Bitmap $bitmap)) {
    Write-Host 'PrintWindow returned a blank surface; falling back to a screen copy.'
    $captured = $false
} elseif (-not $captured) {
    $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host "PrintWindow failed (Win32 error $err); falling back to a screen copy."
}

# ---------------------------------------------------------------------------
# Fallback: copy the visible frame bounds from the screen
# ---------------------------------------------------------------------------
if (-not $captured) {
    $method = 'CopyFromScreen'
    $bitmap.Dispose()

    # The DWM frame bounds exclude the invisible resize borders of framed windows.
    $visible = $rect
    $frame = New-Object HhShot+RECT
    $hr = [HhShot]::DwmGetWindowAttribute($handle, [HhShot]::DWMWA_EXTENDED_FRAME_BOUNDS, [ref]$frame, 16)
    if ($hr -eq 0 -and ($frame.Right - $frame.Left) -gt 0 -and ($frame.Bottom - $frame.Top) -gt 0) { $visible = $frame }

    $width  = $visible.Right - $visible.Left
    $height = $visible.Bottom - $visible.Top
    $bitmap = New-ArgbBitmap -Width $width -Height $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($visible.Left, $visible.Top, 0, 0, (New-Object System.Drawing.Size($width, $height)))
        $captured = $true
    } catch {
        Write-Host "CopyFromScreen failed: $($_.Exception.Message)"
        $captured = $false
    } finally {
        $graphics.Dispose()
    }
}

if (-not $captured) {
    $bitmap.Dispose()
    Write-Host 'Capture failed by both methods.'
    exit 1
}

# ---------------------------------------------------------------------------
# Save
# ---------------------------------------------------------------------------
$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}
try {
    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
} catch {
    Write-Host "Saving '$Out' failed: $($_.Exception.Message)"
    $bitmap.Dispose()
    exit 1
} finally {
    $bitmap.Dispose()
}

$savedPath = (Resolve-Path -LiteralPath $Out).Path
$bytes = (Get-Item -LiteralPath $savedPath).Length
Write-Host ("Saved {0}" -f $savedPath)
Write-Host ("  {0}x{1} px, {2:N0} bytes, method {3}, hwnd 0x{4}, class '{5}', title '{6}'" -f $width, $height, $bytes, $method, $handle.ToInt64().ToString('X'), $className, $windowTitle)
exit 0
