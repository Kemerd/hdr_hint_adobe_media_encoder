<#
.SYNOPSIS
    Replays Adobe Media Encoder's on-disk export sequence and encoding-log block without running AME.

.DESCRIPTION
    Drives HDR Hint through Encoding -> Ready -> Muxing -> Done from a sandbox folder, so the engine
    can be exercised end to end on any machine. Per export the script does what AME 26.x was observed
    to do, in this order:

      1. <Sandbox>\26.0\AMEEncodingLog.txt is created as UTF-16LE with BOM containing the
         "Log File Created" header and rule (only when it does not exist yet). With -Truncate the
         file is rewritten from scratch while the sidecars are still growing, like AME recreating
         its log mid-session.
      2. Sidecars <Sandbox>\out\<stem>.<pid>.<tid>.m4v and .aac appear and grow by 1 MB every
         500 ms for about 5 s. HDR Hint should show the job as Encoding.
      3. <Sandbox>\out\<stem>.mp4 is created with an EXCLUSIVE handle (FileShare.None), the first
         24 bytes of the clip (the ftyp box) are written, 3 s pause, then the rest streams in 1 MB
         chunks over about 5 s; the sidecars are deleted; the handle is closed.
      4. Unless -NoLog, the block is appended with FileShare.ReadWrite:
           - Source File / Output File / Preset Used / Video / Audio / Bitrate / Encoding Time
           <stamp> : File Successfully Encoded        (or, with -Fail, "Encoding Failed" followed by
                                                       the 37-hyphen reason block, which AME also
                                                       writes to AMEEncodingErrorLog.txt)
           blank lines, then <stamp> : Queue Stopped
         A "<stamp> : Queue Started" line precedes the export, as in the real log.
      5. -Twice repeats steps 2-4 for the same stem. HDR Hint should create generation 2 and a
         " (2)" hint file.

    Finally the settings.ini lines that point HDR Hint at the sandbox are printed, together with an
    mkvmerge -J command that verifies the result.

.PARAMETER Sandbox
    Sandbox root. Default %TEMP%\hdrhint_sim. Created when missing.

.PARAMETER Clip
    An .mp4 to replay, for example one produced by make_test_hdr_mp4.ps1. Required.

.PARAMETER Suffix
    Hint-file suffix HDR Hint is configured with (only used for the printed paths). Default _REC709_HINT.

.PARAMETER Fail
    Write an "Encoding Failed" block instead of a success block.

.PARAMETER Truncate
    Recreate the log from scratch while the sidecars are still growing.

.PARAMETER Twice
    Run the export sequence twice for the same stem.

.PARAMETER NoLog
    Never append a log block (a Premiere direct export never logs).

.PARAMETER Pid
    Process id embedded in the sidecar names. Default: the pid of a running "Adobe Media Encoder",
    else 4242. Stored as $AmePid because PowerShell reserves $PID.

.PARAMETER Tid
    Thread id embedded in the sidecar names. Default: random.

.PARAMETER ColorSpace
    Colour token written into the Video line. Default "Rec. 2100 PQ"; use "Rec. 2100 HLG" or
    "Rec. 709" to replay those exports.

.EXAMPLE
    pwsh scripts\simulate_ame_export.ps1 -Sandbox $env:TEMP\hdrhint_sim -Clip $env:TEMP\hdrhint_tests\test_pq_noSEI.mp4

.EXAMPLE
    pwsh scripts\simulate_ame_export.ps1 -Clip .\test_pq_noSEI.mp4 -Fail

.EXAMPLE
    pwsh scripts\simulate_ame_export.ps1 -Clip .\test_pq_noSEI.mp4 -Twice -Truncate

.NOTES
    Exit codes: 0 ok, 1 a step failed, 2 bad arguments (clip missing or too small).
#>
[CmdletBinding()]
param(
    [string]$Sandbox = (Join-Path $env:TEMP 'hdrhint_sim'),
    [Parameter(Mandatory = $true)]
    [string]$Clip,
    [string]$Suffix = '_REC709_HINT',
    [switch]$Fail,
    [switch]$Truncate,
    [switch]$Twice,
    [switch]$NoLog,
    [Alias('Pid')]
    [int]$AmePid = 0,
    [int]$Tid = 0,
    [string]$ColorSpace = 'Rec. 2100 PQ'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Constants that mirror the real log and the observed timing.
# ---------------------------------------------------------------------------
$HeaderRule      = '-' * 42          # under "Log File Created"
$FailureRule     = '-' * 37          # around the failure reason
$ChunkBytes      = 1MB
$SidecarSeconds  = 5
$LockHoldSeconds = 3
$StreamSeconds   = 5

# UTF-16LE encoder that never emits a BOM by itself: the BOM is written once, by hand, at creation.
$Utf16     = New-Object System.Text.UnicodeEncoding($false, $false)
$Invariant = [System.Globalization.CultureInfo]::InvariantCulture

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Get-Stamp {
    <# .SYNOPSIS  Timestamp in the log's "M/d/yyyy h:mm:ss tt" shape (AM/PM from the invariant culture). #>
    return (Get-Date).ToString('M/d/yyyy h:mm:ss tt', $Invariant)
}

function Write-Step {
    <# .SYNOPSIS  Console line with a wall-clock prefix so the timing can be matched against HDR Hint's log. #>
    param([string]$Text)
    Write-Host ('[{0}] {1}' -f (Get-Date).ToString('HH:mm:ss.fff'), $Text)
}

function New-AmeLog {
    <# .SYNOPSIS  Creates (or truncates) a log with BOM, empty line, "Log File Created", rule, two blank lines. #>
    param([string]$Path)

    $text = "`r`nLog File Created: $(Get-Stamp)`r`n$HeaderRule`r`n`r`n`r`n"
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
    try {
        $fs.Write([byte[]](0xFF, 0xFE), 0, 2)
        $bytes = $Utf16.GetBytes($text)
        $fs.Write($bytes, 0, $bytes.Length)
    } finally { $fs.Dispose() }
}

function Add-AmeLogText {
    <# .SYNOPSIS  Appends UTF-16LE text with the sharing AME uses (readers and writers allowed). #>
    param([string]$Path, [string]$Text)

    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
    try {
        $bytes = $Utf16.GetBytes($Text)
        $fs.Write($bytes, 0, $bytes.Length)
    } finally { $fs.Dispose() }
}

function Add-Bytes {
    <# .SYNOPSIS  Appends raw bytes to a sidecar (share Read so directory listings and probes still work). #>
    param([string]$Path, [byte[]]$Bytes)

    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
    try { $fs.Write($Bytes, 0, $Bytes.Length) } finally { $fs.Dispose() }
}

function Open-Exclusive {
    <#
    .SYNOPSIS  Opens the output with FileShare.None like AME's finaliser; retries while something else holds it.
    #>
    param([string]$Path)

    $deadline = (Get-Date).AddSeconds(120)
    while ($true) {
        try {
            return [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        } catch [System.IO.IOException] {
            if ((Get-Date) -gt $deadline) { throw }
            Write-Step "output busy ($($_.Exception.Message.Trim())), retrying in 500 ms"
            Start-Sleep -Milliseconds 500
        }
    }
}

# ---------------------------------------------------------------------------
# Validate inputs and lay out the sandbox
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $Clip -PathType Leaf)) {
    Write-Host "Clip not found: $Clip"
    exit 2
}
$Clip = (Resolve-Path -LiteralPath $Clip).Path
$stem = [System.IO.Path]::GetFileNameWithoutExtension($Clip)
if ([string]::IsNullOrWhiteSpace($stem)) {
    Write-Host "Clip has no usable file name: $Clip"
    exit 2
}

$clipBytes = [System.IO.File]::ReadAllBytes($Clip)
if ($clipBytes.Length -lt 24) {
    Write-Host "Clip is smaller than 24 bytes; nothing to replay: $Clip"
    exit 2
}

if ($AmePid -le 0) {
    $ame = Get-Process -Name 'Adobe Media Encoder' -ErrorAction SilentlyContinue | Select-Object -First 1
    $AmePid = if ($ame) { $ame.Id } else { 4242 }
}
if ($Tid -le 0) { $Tid = Get-Random -Minimum 1000 -Maximum 60000 }

$logDir     = Join-Path $Sandbox '26.0'
$outDir     = Join-Path $Sandbox 'out'
$logPath    = Join-Path $logDir 'AMEEncodingLog.txt'
$errLogPath = Join-Path $logDir 'AMEEncodingErrorLog.txt'
New-Item -ItemType Directory -Force -Path $logDir, $outDir | Out-Null
$Sandbox    = (Resolve-Path -LiteralPath $Sandbox).Path
$logDir     = Join-Path $Sandbox '26.0'
$outDir     = Join-Path $Sandbox 'out'
$logPath    = Join-Path $logDir 'AMEEncodingLog.txt'
$errLogPath = Join-Path $logDir 'AMEEncodingErrorLog.txt'

$mp4Path  = Join-Path $outDir "$stem.mp4"
$hintPath = Join-Path $outDir "$stem$Suffix.mkv"
$m4vPath  = Join-Path $outDir "$stem.$AmePid.$Tid.m4v"
$aacPath  = Join-Path $outDir "$stem.$AmePid.$Tid.aac"

Write-Host "Sandbox : $Sandbox"
Write-Host "Clip    : $Clip ($($clipBytes.Length) bytes)"
Write-Host "Log     : $logPath"
Write-Host "Output  : $mp4Path"
Write-Host "Sidecars: $stem.$AmePid.$Tid.m4v / .aac"
Write-Host ("Options : Fail={0} Truncate={1} Twice={2} NoLog={3} ColorSpace='{4}'" -f $Fail.IsPresent, $Truncate.IsPresent, $Twice.IsPresent, $NoLog.IsPresent, $ColorSpace)
Write-Host ''

# Deterministic filler for the sidecars: random-looking bytes so nothing mistakes them for a real mp4.
$chunk = New-Object byte[] $ChunkBytes
(New-Object System.Random(20260916)).NextBytes($chunk)

# ---------------------------------------------------------------------------
# Step 1: the log file
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $logPath)) {
    New-AmeLog -Path $logPath
    Write-Step "created $logPath (UTF-16LE + BOM)"
} else {
    Write-Step "log exists, appending to $logPath"
}

# ---------------------------------------------------------------------------
# One export = steps 2-4
# ---------------------------------------------------------------------------
function Invoke-Export {
    param([int]$Run)

    # -- step 2: sidecars grow ---------------------------------------------------------------
    if (-not $NoLog) { Add-AmeLogText -Path $logPath -Text "$(Get-Stamp) : Queue Started`r`n`r`n`r`n" }
    foreach ($p in @($m4vPath, $aacPath)) {
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
    }
    $iterations = [int]($SidecarSeconds * 2)
    $truncateAt = [int][Math]::Max(1, [Math]::Floor($iterations / 2))
    Write-Step "step 2: sidecars growing ($iterations x 1 MB, 500 ms apart)"
    for ($i = 1; $i -le $iterations; $i++) {
        Add-Bytes -Path $m4vPath -Bytes $chunk
        Add-Bytes -Path $aacPath -Bytes $chunk
        if ($Truncate -and $i -eq $truncateAt) {
            New-AmeLog -Path $logPath
            Write-Step 'step 1b: log truncated and recreated mid-run (-Truncate)'
        }
        Start-Sleep -Milliseconds 500
    }

    # -- step 3: exclusive finalise ----------------------------------------------------------
    Write-Step 'step 3: creating output with an exclusive handle'
    $fs = Open-Exclusive -Path $mp4Path
    try {
        $fs.Write($clipBytes, 0, 24)
        $fs.Flush($true)
        Write-Step "wrote the 24-byte ftyp, holding the lock for $LockHoldSeconds s"
        Start-Sleep -Seconds $LockHoldSeconds

        $remaining = $clipBytes.Length - 24
        $chunks    = [int][Math]::Max(1, [Math]::Ceiling($remaining / $ChunkBytes))
        $delayMs   = [int][Math]::Max(50, ($StreamSeconds * 1000) / $chunks)
        $pos = 24
        while ($pos -lt $clipBytes.Length) {
            $n = [int][Math]::Min($ChunkBytes, $clipBytes.Length - $pos)
            $fs.Write($clipBytes, $pos, $n)
            $fs.Flush($true)
            $pos += $n
            Start-Sleep -Milliseconds $delayMs
        }
        Remove-Item -LiteralPath $m4vPath, $aacPath -Force -ErrorAction SilentlyContinue
        Write-Step "streamed $($clipBytes.Length) bytes in $chunks chunk(s), sidecars deleted, releasing the lock"
    } finally {
        $fs.Dispose()
    }

    # -- step 4: the log block ---------------------------------------------------------------
    if ($NoLog) {
        Write-Step 'step 4 skipped (-NoLog): no block appended'
        return
    }
    $stamp = Get-Stamp
    $block = @(
        " - Source File: $Clip",
        " - Output File: $mp4Path",
        ' - Preset Used: Custom',
        " - Video: 1920x1080 (1.0), 30 fps, $ColorSpace, 203 (75% HLG, 58% PQ), Hardware Encoding, Nvidia Codec, 00:00:03:00",
        ' - Audio: AAC, 128 kbps, 48 kHz, Stereo',
        ' - Bitrate: VBR, 1 pass, Target 10.00 Mbps',
        ' - Encoding Time: 00:00:05'
    )
    if ($Fail) {
        $block += "$stamp : Encoding Failed"
        $block += $FailureRule
        $block += 'The Operation was interrupted by user'
        $block += $FailureRule
    } else {
        $block += "$stamp : File Successfully Encoded"
    }
    $block += ''
    $block += ''
    $blockText = ($block -join "`r`n") + "`r`n"
    $queueText = "$stamp : Queue Stopped`r`n`r`n`r`n"

    Add-AmeLogText -Path $logPath -Text ($blockText + $queueText)
    Write-Step ("step 4: appended '{0}' block + Queue Stopped" -f $(if ($Fail) { 'Encoding Failed' } else { 'File Successfully Encoded' }))

    # AME writes failed blocks to the error log as well; the tailer must dedupe the pair.
    if ($Fail) {
        if (-not (Test-Path -LiteralPath $errLogPath)) { New-AmeLog -Path $errLogPath }
        Add-AmeLogText -Path $errLogPath -Text $blockText
        Write-Step "        and the same block to $errLogPath"
    }
}

# ---------------------------------------------------------------------------
# Run once or twice
# ---------------------------------------------------------------------------
$runs = if ($Twice) { 2 } else { 1 }
$failed = $false
for ($run = 1; $run -le $runs; $run++) {
    Write-Step "=== export $run of $runs : $stem ==="
    try {
        Invoke-Export -Run $run
    } catch {
        Write-Step "FAILED: $($_.Exception.Message)"
        $failed = $true
        break
    }
    if ($run -lt $runs) {
        Write-Step 'pausing 2 s before the second export'
        Start-Sleep -Seconds 2
    }
}

# ---------------------------------------------------------------------------
# What to do next
# ---------------------------------------------------------------------------
$mkvmerge = 'C:\Program Files\MKVToolNix\mkvmerge.exe'
if (-not (Test-Path -LiteralPath $mkvmerge)) {
    $found = Get-Command mkvmerge -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $mkvmerge = if ($found) { $found.Source } else { 'mkvmerge' }
}
$secondHint = Join-Path $outDir ("{0}{1} (2).mkv" -f $stem, $Suffix)

Write-Host ''
Write-Host 'Point HDR Hint at the sandbox: edit %APPDATA%\HdrHint\settings.ini, then restart HDR Hint'
Write-Host '(or re-run this script while it is running):'
Write-Host ''
Write-Host '  [ame]'
Write-Host "  log_path_override = $logPath"
Write-Host "  extra_watch_folders = $outDir          (append to the existing list if there is one)"
Write-Host ''
Write-Host "Then watch for: $hintPath"
if ($Twice) { Write-Host "  and, for the second export: $secondHint" }
if ($Fail)  { Write-Host '  (with -Fail the job must end as Failed and no .mkv may appear)' }
if ($NoLog) { Write-Host '  (with -NoLog the job is confirmed by the 20 s readiness timeout instead of the log)' }
Write-Host ''
Write-Host 'Verify the result:'
Write-Host ("  & `"{0}`" -J `"{1}`"" -f $mkvmerge, $hintPath)
Write-Host '  (video track: color_transfer_characteristics 16 or 18, color_primaries 9; attachments: one application/x-cube)'

if ($failed) { exit 1 }
exit 0
