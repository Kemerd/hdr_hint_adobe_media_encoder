<#
.SYNOPSIS
    Generates small HDR / SDR HEVC test clips for HDR Hint with ffmpeg and verifies them with ffprobe.

.DESCRIPTION
    Writes the following files into -OutDir (default %TEMP%\hdrhint_tests):

      test_pq_noSEI.mp4        PQ (SMPTE ST 2084), BT.2020, 10-bit, NO mastering-display / CLL SEI.
                               This is what AME writes with "Include HDR10 Metadata" OFF.
      test_pq_SEI.mp4          Same, plus in-band mastering display (BT.2020 primaries, D65,
                               1000 / 0.0001 cd/m2) and MaxCLL 1000 / MaxFALL 400. What AME writes
                               with the box ON (software encode).
      test_hlg.mp4             HLG (ARIB STD-B67), BT.2020, 10-bit.
      test_sdr.mp4             BT.709 primaries / transfer / matrix, 8-bit.
      test_ünïcode 日本.mp4     Byte copy of test_pq_noSEI.mp4 with a non-ASCII file name.
      longpath\...\test_pq_noSEI_longpath.mp4
                               Byte copy nested so deep that the full path exceeds 260 characters.

    Every clip is 1920x1080, 30 fps, testsrc2 video plus a 440 Hz sine (AAC 128 kbps, 48 kHz),
    libx265 ultrafast, hvc1 tag, faststart (moov before mdat) like an AME export.

    Each file is checked with ffprobe (video stream colour tags, first-frame side data for the SEI,
    audio stream) and the two copies additionally by SHA-256 against their source. A summary table
    is printed at the end.

.PARAMETER OutDir
    Destination folder. Created when missing. Default: %TEMP%\hdrhint_tests

.PARAMETER Ffmpeg
    Path to ffmpeg.exe. Default: C:\ffmpeg\bin\ffmpeg.exe, then ffmpeg on PATH.
    ffprobe.exe is looked up next to ffmpeg first, then on PATH.

.PARAMETER Seconds
    Clip length in seconds (1..60). Default 3.

.EXAMPLE
    pwsh scripts\make_test_hdr_mp4.ps1

.EXAMPLE
    pwsh scripts\make_test_hdr_mp4.ps1 -OutDir D:\scratch\hh -Ffmpeg C:\tools\ffmpeg\bin\ffmpeg.exe

.NOTES
    This file is saved as UTF-8 WITH BOM on purpose. Windows PowerShell 5.1 assumes the ANSI code
    page for BOM-less scripts and would mangle the non-ASCII file name literal below.

    Exit codes: 0 every file generated and verified, 1 at least one file failed, 2 ffmpeg or ffprobe missing.
#>
[CmdletBinding()]
param(
    [string]$OutDir = (Join-Path $env:TEMP 'hdrhint_tests'),
    [string]$Ffmpeg = '',
    [ValidateRange(1, 60)]
    [int]$Seconds = 3
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# A failing ffmpeg must not abort the script through PowerShell 7.4's native-command error
# preference; exit codes are inspected by hand below.
$PSNativeCommandUseErrorActionPreference = $false

# ---------------------------------------------------------------------------
# Console: UTF-8 so the non-ASCII file name renders correctly in conhost and Windows Terminal.
# ---------------------------------------------------------------------------
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Resolve-Tool {
    <#
    .SYNOPSIS  Finds an executable: explicit path, then the conventional install folder, then PATH.
    #>
    param([string]$Explicit, [string]$Conventional, [string]$Name)

    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit -PathType Leaf) { return (Resolve-Path -LiteralPath $Explicit).Path }
        return $null
    }
    if ($Conventional -and (Test-Path -LiteralPath $Conventional -PathType Leaf)) { return $Conventional }

    $cmd = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    return $null
}

function Get-Prop {
    <#
    .SYNOPSIS  Strict-mode-safe property read on objects produced by ConvertFrom-Json.
    #>
    param($Object, [string]$Name, $Default = $null)

    if ($null -eq $Object) { return $Default }
    $p = $Object.PSObject.Properties[$Name]
    if ($null -eq $p -or $null -eq $p.Value) { return $Default }
    return $p.Value
}

function Invoke-Native {
    <#
    .SYNOPSIS  Runs a native exe and returns @{ ExitCode; Output } with stdout and stderr merged as text.
    #>
    param([string]$Exe, [string[]]$Arguments)

    # stderr lines arrive as ErrorRecords when merged; 'Continue' keeps them from terminating the call.
    $ErrorActionPreference = 'Continue'
    $lines = & $Exe @Arguments 2>&1 | ForEach-Object { $_.ToString() }
    $code = $LASTEXITCODE
    return @{ ExitCode = $code; Output = @($lines) }
}

function Get-Sha256Hex {
    <#
    .SYNOPSIS  SHA-256 of a file as hex; falls back to the \\?\ form for paths beyond MAX_PATH.
    #>
    param([string]$Path)

    foreach ($candidate in @($Path, ('\\?\' + $Path))) {
        try {
            $stream = [System.IO.File]::OpenRead($candidate)
            try {
                $sha = [System.Security.Cryptography.SHA256]::Create()
                try { return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
                finally { $sha.Dispose() }
            } finally { $stream.Dispose() }
        } catch { }
    }
    return ''
}

function Copy-ByteExact {
    <#
    .SYNOPSIS  Copies a file, creating the parent folder; retries with the \\?\ prefix for long paths.
    #>
    param([string]$Source, [string]$Destination)

    $dir = Split-Path -Parent $Destination
    foreach ($prefix in @('', '\\?\')) {
        try {
            [System.IO.Directory]::CreateDirectory($prefix + $dir) | Out-Null
            [System.IO.File]::Copy($Source, $prefix + $Destination, $true)
            return $true
        } catch {
            $lastError = $_.Exception.Message
        }
    }
    Write-Warning "copy to '$Destination' failed: $lastError"
    return $false
}

function New-TestClip {
    <#
    .SYNOPSIS  Encodes one synthetic clip with ffmpeg. Returns the Invoke-Native result.
    #>
    param(
        [string]$Path,
        [string]$X265Params,
        [string[]]$ColorArgs,
        [string]$PixFmt
    )

    # Two lavfi inputs (picture + tone), explicit maps so nothing surprising sneaks in, then the
    # same container flags AME uses: hvc1 sample entry and moov first.
    $ffArgs = @(
        '-y', '-hide_banner', '-nostdin', '-loglevel', 'error',
        '-f', 'lavfi', '-i', 'testsrc2=size=1920x1080:rate=30',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
        '-t', [string]$Seconds,
        '-map', '0:v:0', '-map', '1:a:0',
        '-c:v', 'libx265', '-preset', 'ultrafast', '-pix_fmt', $PixFmt, '-tag:v', 'hvc1',
        '-x265-params', $X265Params
    ) + $ColorArgs + @(
        '-c:a', 'aac', '-b:a', '128k', '-ar', '48000', '-ac', '2',
        '-movflags', '+faststart',
        $Path
    )
    return (Invoke-Native -Exe $script:ffmpeg -Arguments $ffArgs)
}

function Get-ClipInfo {
    <#
    .SYNOPSIS  Probes a clip with ffprobe. Tries the \\?\ form when the plain path cannot be opened.
    #>
    param([string]$Path)

    $info = [ordered]@{
        Ok = $false; Error = ''; ProbedPath = $Path
        Codec = ''; Tag = ''; PixFmt = ''; Width = 0; Height = 0
        Primaries = ''; Transfer = ''; Matrix = ''; Range = ''
        Audio = ''; SideData = @(); Hdr10Sei = $false
    }

    # -- stream level ---------------------------------------------------------
    $streamEntries = 'stream=index,codec_type,codec_name,codec_tag_string,pix_fmt,width,height,' +
                     'color_primaries,color_transfer,color_space,color_range'
    $probe = $null
    foreach ($candidate in @($Path, ('\\?\' + $Path))) {
        $probe = Invoke-Native -Exe $script:ffprobe -Arguments @('-v', 'error', '-show_entries', $streamEntries, '-of', 'json', $candidate)
        if ($probe.ExitCode -eq 0) { $info.ProbedPath = $candidate; break }
    }
    if ($null -eq $probe -or $probe.ExitCode -ne 0) {
        $info.Error = "ffprobe exit $($probe.ExitCode): $(($probe.Output -join ' ').Trim())"
        return $info
    }

    $json = $null
    try { $json = ($probe.Output -join "`n") | ConvertFrom-Json }
    catch { $info.Error = "ffprobe JSON unreadable: $($_.Exception.Message)"; return $info }

    $streams = @(Get-Prop $json 'streams' @())
    $video = $streams | Where-Object { (Get-Prop $_ 'codec_type' '') -eq 'video' } | Select-Object -First 1
    $audio = $streams | Where-Object { (Get-Prop $_ 'codec_type' '') -eq 'audio' } | Select-Object -First 1
    if ($null -eq $video) { $info.Error = 'no video stream'; return $info }

    $info.Codec     = [string](Get-Prop $video 'codec_name' '')
    $info.Tag       = [string](Get-Prop $video 'codec_tag_string' '')
    $info.PixFmt    = [string](Get-Prop $video 'pix_fmt' '')
    $info.Width     = [int](Get-Prop $video 'width' 0)
    $info.Height    = [int](Get-Prop $video 'height' 0)
    $info.Primaries = [string](Get-Prop $video 'color_primaries' '')
    $info.Transfer  = [string](Get-Prop $video 'color_transfer' '')
    $info.Matrix    = [string](Get-Prop $video 'color_space' '')
    $info.Range     = [string](Get-Prop $video 'color_range' '')
    $info.Audio     = if ($audio) { [string](Get-Prop $audio 'codec_name' '') } else { '' }

    # -- frame level: mastering display / content light level SEI show up as frame side data ------
    # Decode the first few packets; HEVC reorder delay means packet 1 alone may yield no frame.
    # The entries filter must name the frame_side_data section itself: asking for
    # "frame=side_data_list" yields empty objects on current ffprobe builds.
    $frames = Invoke-Native -Exe $script:ffprobe -Arguments @(
        '-v', 'error', '-select_streams', 'v:0', '-read_intervals', '%+#8',
        '-show_entries', 'frame_side_data=side_data_type', '-of', 'json', $info.ProbedPath)
    if ($frames.ExitCode -eq 0) {
        try {
            $fj = ($frames.Output -join "`n") | ConvertFrom-Json
            $types = @()
            foreach ($frame in @(Get-Prop $fj 'frames' @())) {
                foreach ($sd in @(Get-Prop $frame 'side_data_list' @())) {
                    $t = [string](Get-Prop $sd 'side_data_type' '')
                    if ($t -and ($types -notcontains $t)) { $types += $t }
                }
            }
            $info.SideData = $types
        } catch { }
    }
    # x265 always emits a "User Data Unregistered" SEI with its version string; only the two
    # HDR10 messages count as in-band HDR metadata.
    $info.Hdr10Sei = ($info.SideData -contains 'Mastering display metadata') -or
                     ($info.SideData -contains 'Content light level metadata')

    $info.Ok = $true
    return $info
}

function Test-Expectation {
    <#
    .SYNOPSIS  Compares probe results with what the clip must contain. Returns a list of problems.
    #>
    param($Info, $Expect)

    if (-not $Info.Ok) { return @($Info.Error) }
    $problems = @()
    if ($Info.Codec -ne 'hevc')           { $problems += "codec '$($Info.Codec)' != hevc" }
    if ($Info.Tag -ne 'hvc1')             { $problems += "tag '$($Info.Tag)' != hvc1" }
    if ($Info.Width -ne 1920 -or $Info.Height -ne 1080) { $problems += "size $($Info.Width)x$($Info.Height) != 1920x1080" }
    if ($Info.PixFmt -ne $Expect.PixFmt)  { $problems += "pix_fmt '$($Info.PixFmt)' != $($Expect.PixFmt)" }
    if ($Info.Primaries -ne $Expect.Primaries) { $problems += "primaries '$($Info.Primaries)' != $($Expect.Primaries)" }
    if ($Info.Transfer -ne $Expect.Transfer)   { $problems += "transfer '$($Info.Transfer)' != $($Expect.Transfer)" }
    if ($Info.Matrix -ne $Expect.Matrix)       { $problems += "matrix '$($Info.Matrix)' != $($Expect.Matrix)" }
    if ($Info.Range -ne $Expect.Range)         { $problems += "range '$($Info.Range)' != $($Expect.Range)" }
    if ($Info.Audio -ne 'aac')            { $problems += "audio '$($Info.Audio)' != aac" }

    $hasMastering = ($Info.SideData -contains 'Mastering display metadata')
    $hasCll       = ($Info.SideData -contains 'Content light level metadata')
    if ($Expect.Sei) {
        if (-not $hasMastering) { $problems += 'mastering display SEI missing' }
        if (-not $hasCll)       { $problems += 'content light level SEI missing' }
    } elseif ($hasMastering -or $hasCll) {
        $problems += 'unexpected HDR10 SEI present'
    }
    return $problems
}

# ---------------------------------------------------------------------------
# Locate the tools
# ---------------------------------------------------------------------------
$script:ffmpeg = Resolve-Tool -Explicit $Ffmpeg -Conventional 'C:\ffmpeg\bin\ffmpeg.exe' -Name 'ffmpeg'
if (-not $script:ffmpeg) {
    Write-Host 'ffmpeg.exe not found. Pass -Ffmpeg <path> or install it to C:\ffmpeg\bin.'
    exit 2
}
$probeNextToFfmpeg = Join-Path (Split-Path -Parent $script:ffmpeg) 'ffprobe.exe'
$script:ffprobe = Resolve-Tool -Explicit '' -Conventional $probeNextToFfmpeg -Name 'ffprobe'
if (-not $script:ffprobe) {
    Write-Host "ffprobe.exe not found next to '$($script:ffmpeg)' or on PATH."
    exit 2
}

$ffVersion = (Invoke-Native -Exe $script:ffmpeg -Arguments @('-version')).Output | Select-Object -First 1
Write-Host "ffmpeg : $($script:ffmpeg)"
Write-Host "ffprobe: $($script:ffprobe)"
Write-Host "         $ffVersion"

# ---------------------------------------------------------------------------
# Output folder
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
Write-Host "Output : $OutDir"
Write-Host ''

# ---------------------------------------------------------------------------
# The four encodes. x265 gets the VUI values through -x265-params (what lands in the bitstream);
# the -color_* flags tag the container / stream side so both agree, like an AME export.
# ---------------------------------------------------------------------------
$pqVui  = 'colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited'
$hlgVui = 'colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc:range=limited'
$sdrVui = 'colorprim=bt709:transfer=bt709:colormatrix=bt709:range=limited'
$hdr10Sei = ':master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400'

$pqColor  = @('-color_primaries', 'bt2020', '-color_trc', 'smpte2084',    '-colorspace', 'bt2020nc', '-color_range', 'tv')
$hlgColor = @('-color_primaries', 'bt2020', '-color_trc', 'arib-std-b67', '-colorspace', 'bt2020nc', '-color_range', 'tv')
$sdrColor = @('-color_primaries', 'bt709',  '-color_trc', 'bt709',        '-colorspace', 'bt709',    '-color_range', 'tv')

$clips = @(
    @{ Name = 'test_pq_noSEI.mp4'; X265 = $pqVui;             Color = $pqColor;  PixFmt = 'yuv420p10le'
       Expect = @{ PixFmt = 'yuv420p10le'; Primaries = 'bt2020'; Transfer = 'smpte2084';    Matrix = 'bt2020nc'; Range = 'tv'; Sei = $false } },
    @{ Name = 'test_pq_SEI.mp4';   X265 = $pqVui + $hdr10Sei; Color = $pqColor;  PixFmt = 'yuv420p10le'
       Expect = @{ PixFmt = 'yuv420p10le'; Primaries = 'bt2020'; Transfer = 'smpte2084';    Matrix = 'bt2020nc'; Range = 'tv'; Sei = $true } },
    @{ Name = 'test_hlg.mp4';      X265 = $hlgVui;            Color = $hlgColor; PixFmt = 'yuv420p10le'
       Expect = @{ PixFmt = 'yuv420p10le'; Primaries = 'bt2020'; Transfer = 'arib-std-b67'; Matrix = 'bt2020nc'; Range = 'tv'; Sei = $false } },
    @{ Name = 'test_sdr.mp4';      X265 = $sdrVui;            Color = $sdrColor; PixFmt = 'yuv420p'
       Expect = @{ PixFmt = 'yuv420p';     Primaries = 'bt709';  Transfer = 'bt709';        Matrix = 'bt709';    Range = 'tv'; Sei = $false } }
)

$rows = @()
$allOk = $true

foreach ($clip in $clips) {
    $path = Join-Path $OutDir $clip.Name
    Write-Host ("Encoding {0} ..." -f $clip.Name)
    $result = New-TestClip -Path $path -X265Params $clip.X265 -ColorArgs $clip.Color -PixFmt $clip.PixFmt

    $status = 'OK'
    $detail = ''
    $info = $null
    if ($result.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $path)) {
        $status = 'FAIL'
        $detail = "ffmpeg exit $($result.ExitCode): $(($result.Output -join ' ').Trim())"
    } else {
        $info = Get-ClipInfo -Path $path
        $problems = @(Test-Expectation -Info $info -Expect $clip.Expect)
        if ($problems.Count -gt 0) { $status = 'FAIL'; $detail = $problems -join '; ' }
    }
    if ($status -ne 'OK') { $allOk = $false }

    $rows += [pscustomobject]@{
        File      = $clip.Name
        Bytes     = if (Test-Path -LiteralPath $path) { (Get-Item -LiteralPath $path).Length } else { 0 }
        Video     = if ($info) { "$($info.Codec)/$($info.PixFmt)" } else { '' }
        Primaries = if ($info) { $info.Primaries } else { '' }
        Transfer  = if ($info) { $info.Transfer } else { '' }
        Matrix    = if ($info) { $info.Matrix } else { '' }
        Range     = if ($info) { $info.Range } else { '' }
        SEI       = if ($info -and $info.Hdr10Sei) { 'yes' } elseif ($info) { 'no' } else { '' }
        Audio     = if ($info) { $info.Audio } else { '' }
        Status    = $status
        Detail    = $detail
    }
}

# ---------------------------------------------------------------------------
# Copies: a non-ASCII name and a path beyond MAX_PATH, both byte-identical to test_pq_noSEI.mp4.
# ---------------------------------------------------------------------------
$sourceClip = Join-Path $OutDir 'test_pq_noSEI.mp4'
$sourceOk = Test-Path -LiteralPath $sourceClip
$sourceHash = if ($sourceOk) { Get-Sha256Hex -Path $sourceClip } else { '' }

# Build a folder chain until the full file path is comfortably past 260 characters.
$segment = 'a_really_long_directory_name_segment_0123456789'
$deepDir = Join-Path $OutDir 'longpath'
while (($deepDir.Length + 1 + 'test_pq_noSEI_longpath.mp4'.Length) -le 270) { $deepDir = Join-Path $deepDir $segment }

$copies = @(
    @{ Label = 'test_ünïcode 日本.mp4';           Path = (Join-Path $OutDir 'test_ünïcode 日本.mp4') },
    @{ Label = "longpath (...)\test_pq_noSEI_longpath.mp4 [$($deepDir.Length + 27) chars]"; Path = (Join-Path $deepDir 'test_pq_noSEI_longpath.mp4') }
)

foreach ($copy in $copies) {
    $status = 'OK'
    $detail = ''
    $info = $null
    if (-not $sourceOk) {
        $status = 'FAIL'; $detail = 'source clip missing'
    } elseif (-not (Copy-ByteExact -Source $sourceClip -Destination $copy.Path)) {
        $status = 'FAIL'; $detail = 'copy failed'
    } else {
        # Byte identity first (authoritative), then ffprobe on the exotic path (proves tools can open it).
        $hash = Get-Sha256Hex -Path $copy.Path
        if ($hash -ne $sourceHash -or -not $hash) { $status = 'FAIL'; $detail = 'SHA-256 differs from source' }
        $info = Get-ClipInfo -Path $copy.Path
        $problems = @(Test-Expectation -Info $info -Expect $clips[0].Expect)
        if ($problems.Count -gt 0) {
            $status = 'FAIL'
            $detail = (@($detail, ($problems -join '; ')) | Where-Object { $_ }) -join '; '
        }
    }
    if ($status -ne 'OK') { $allOk = $false }

    $bytes = 0
    foreach ($candidate in @($copy.Path, ('\\?\' + $copy.Path))) {
        try { $bytes = (New-Object System.IO.FileInfo($candidate)).Length; break } catch { }
    }
    $rows += [pscustomobject]@{
        File      = $copy.Label
        Bytes     = $bytes
        Video     = if ($info -and $info.Ok) { "$($info.Codec)/$($info.PixFmt)" } else { '' }
        Primaries = if ($info -and $info.Ok) { $info.Primaries } else { '' }
        Transfer  = if ($info -and $info.Ok) { $info.Transfer } else { '' }
        Matrix    = if ($info -and $info.Ok) { $info.Matrix } else { '' }
        Range     = if ($info -and $info.Ok) { $info.Range } else { '' }
        SEI       = if ($info -and $info.Ok) { if ($info.Hdr10Sei) { 'yes' } else { 'no' } } else { '' }
        Audio     = if ($info -and $info.Ok) { $info.Audio } else { '' }
        Status    = $status
        Detail    = $detail
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ''
$rows | Format-Table -AutoSize -Wrap -Property File, Bytes, Video, Primaries, Transfer, Matrix, Range, SEI, Audio, Status, Detail | Out-String -Width 220 | Write-Host
Write-Host "Long path: $deepDir"

if ($allOk) {
    Write-Host 'All test clips generated and verified.'
    exit 0
}
Write-Host 'At least one clip FAILED (see Detail).'
exit 1
