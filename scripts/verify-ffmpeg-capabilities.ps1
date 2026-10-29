# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- FFmpeg Capability and Requirement Auditor
#
# Inspects an ffmpeg.exe binary and verifies that all demuxers, muxers, parsers,
# decoders, encoders, bitstream filters (BSFs), and protocols required by Radium's
# stream download and muxing workflows are supported.
#
# Workflows Audited:
#   1. HLS TS segment muxing into MP4 (requires: demuxer mpegts, bsf aac_adtstoasc, muxer mp4)
#   2. DASH fMP4/CMAF audio/video track merging (requires: demuxer mov, muxer mp4, muxer matroska)
#   3. WebVTT / SRT subtitle embedding into MP4 (requires: demuxer webvtt/srt, encoder mov_text, muxer mp4)
#   4. WebVTT subtitle embedding into MKV (requires: demuxer webvtt, encoder subrip, muxer matroska)
#   5. Standalone audio remuxing (requires: demuxers aac/mp3/opus/flac, muxers ipod/mp3/opus/flac)
#   6. Segment concatenation via concat protocol and demuxer
#   7. Fast duration/stream probing (requires: decoders h264/hevc/av1/vp9/aac/mp3/opus, null muxer)

[CmdletBinding()]
param(
    [string]$FfmpegPath = "",
    [switch]$RunSmokeTests = $false,
    [bool]$FailOnMissing = $true
)

$ErrorActionPreference = "Stop"

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not $ScriptDir -or -not (Test-Path $ScriptDir)) {
    $ScriptDir = (Get-Location).Path
}
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

if (-not $FfmpegPath) {
    # Check default standard tools location
    $DefaultExe = Join-Path $RepoRoot "tools\win-x64\ffmpeg.exe"
    if (Test-Path $DefaultExe) {
        $FfmpegPath = $DefaultExe
    } elseif (Get-Command "ffmpeg.exe" -ErrorAction SilentlyContinue) {
        $FfmpegPath = (Get-Command "ffmpeg.exe").Source
    } else {
        $FfmpegPath = $DefaultExe
    }
}
$FfmpegPath = [System.IO.Path]::GetFullPath($FfmpegPath)

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "   Radium Download Manager -- FFmpeg Capability Auditor          " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "Target Executable: $FfmpegPath" -ForegroundColor White

if (-not (Test-Path $FfmpegPath)) {
    Write-Host "[ERROR] FFmpeg executable not found at: $FfmpegPath" -ForegroundColor Red
    if ($FailOnMissing) { exit 1 } else { return }
}

# -----------------------------------------------------------------------------
# 1. PE32+ Binary Header Verification
# -----------------------------------------------------------------------------
Write-Host "`n==> [1/4] Inspecting PE32+ Header and File Size..." -ForegroundColor Cyan

$fileInfo = Get-Item $FfmpegPath
$fileSize = $fileInfo.Length
$fileSizeMiB = [Math]::Round($fileSize / 1MB, 2)
Write-Host "  File Size: $fileSize bytes ($fileSizeMiB MiB)" -ForegroundColor White

$bytes = [System.IO.File]::ReadAllBytes($FfmpegPath)
if ($bytes.Length -lt 256 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
    throw "Target is not a valid Windows PE executable (missing MZ signature)."
}
$peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
$machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
if ($machine -ne 0x8664) {
    throw "Target is not x64 (IMAGE_FILE_MACHINE_AMD64 0x8664); got 0x$($machine.ToString('X4'))"
}
Write-Host "  [OK] Valid Windows PE32+ x64 (AMD64) machine header verified." -ForegroundColor Green

# -----------------------------------------------------------------------------
# 2. Query Version and Build Configuration
# -----------------------------------------------------------------------------
Write-Host "`n==> [2/4] Querying FFmpeg Version and Configuration..." -ForegroundColor Cyan

$prevEap = $ErrorActionPreference
$ErrorActionPreference = "SilentlyContinue"
$versionOutput = & $FfmpegPath -version 2>&1
$ErrorActionPreference = $prevEap

$versionLine = ($versionOutput | Select-Object -First 1)
$configLine = ($versionOutput | Where-Object { $_ -match "configuration:" } | Select-Object -First 1)

Write-Host "  $versionLine" -ForegroundColor Green

$isLgpl = $false
$isGpl = $false
if ($configLine) {
    if ($configLine -match "--enable-gpl") {
        $isGpl = $true
        Write-Host "  License Profile: GPL (monolithic/general-purpose build)" -ForegroundColor Yellow
    } else {
        $isLgpl = $true
        Write-Host "  License Profile: LGPL v2.1+ / v3.0 (minimal compliant build)" -ForegroundColor Green
    }
}

# -----------------------------------------------------------------------------
# 3. Audit Required Capabilities
# -----------------------------------------------------------------------------
Write-Host "`n==> [3/4] Auditing Required Subsystems, Formats, and Codecs..." -ForegroundColor Cyan

$MissingItems = [System.Collections.Generic.List[string]]::new()

function Test-CapabilityList([string]$Category, [string[]]$RequiredItems, [string]$CommandArg, [string]$PrefixPattern = "") {
    Write-Host "`n  [$Category]" -ForegroundColor Cyan
    $pEap = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    $output = & $FfmpegPath -hide_banner $CommandArg 2>&1
    $ErrorActionPreference = $pEap
    $outputStr = ($output -join "`n")

    foreach ($item in $RequiredItems) {
        $pattern = if ($PrefixPattern) { "$PrefixPattern\b$([regex]::Escape($item))\b" } else { "\b$([regex]::Escape($item))\b" }
        if ($outputStr -match $pattern) {
            Write-Host "    [PASS] $item" -ForegroundColor Green
        } else {
            Write-Host "    [FAIL] MISSING: $item" -ForegroundColor Red
            $script:MissingItems.Add("${Category}: $item")
        }
    }
}

# 3.1 Demuxers
$RequiredDemuxers = @(
    "mpegts",
    "mov",
    "matroska",
    "aac",
    "mp3",
    "flac",
    "ogg",
    "wav",
    "webvtt",
    "srt",
    "concat"
)
Test-CapabilityList "Demuxers" $RequiredDemuxers "-demuxers" "D[E\s]\s+"

# 3.2 Muxers
$RequiredMuxers = @(
    "mp4",
    "matroska",
    "webm",
    "ipod",
    "mp3",
    "mpegts",
    "ogg",
    "opus",
    "flac",
    "wav",
    "srt",
    "webvtt",
    "null"
)
Test-CapabilityList "Muxers" $RequiredMuxers "-muxers" "[D\s]E\s+"

# 3.3 Parsers
$RequiredParsers = @(
    "h264",
    "hevc",
    "av1",
    "vp9",
    "vp8",
    "aac",
    "ac3",
    "mpegaudio",
    "opus",
    "vorbis",
    "flac"
)

Write-Host "`n  [Parsers]" -ForegroundColor Cyan
$pEap = $ErrorActionPreference
$ErrorActionPreference = "SilentlyContinue"
$buildConfOutput = & $FfmpegPath -hide_banner -buildconf 2>&1
$ErrorActionPreference = $pEap
$buildConfStr = ($buildConfOutput -join "`n")
$isDisableAll = ($buildConfStr -match "--disable-all")

foreach ($parser in $RequiredParsers) {
    if (-not $isDisableAll -or ($buildConfStr -match "--enable-parser=$parser") -or ($buildConfStr -match "--enable-parsers")) {
        Write-Host "    [PASS] $parser" -ForegroundColor Green
    } else {
        Write-Host "    [FAIL] MISSING: $parser" -ForegroundColor Red
        $MissingItems.Add("Parsers: $parser")
    }
}

# 3.4 Decoders (Inspection / probing)
$RequiredDecoders = @(
    "h264",
    "hevc",
    "av1",
    "vp9",
    "vp8",
    "aac",
    "ac3",
    "mp3",
    "opus",
    "vorbis",
    "flac",
    "mov_text",
    "subrip",
    "webvtt"
)
Test-CapabilityList "Decoders" $RequiredDecoders "-decoders" "V\.\.\.\.\.|A\.\.\.\.\.|S\.\.\.\.\."

# 3.5 Encoders (Subtitles & stream copy companions)
$RequiredEncoders = @(
    "mov_text",
    "subrip",
    "webvtt",
    "aac",
    "pcm_s16le"
)
Test-CapabilityList "Encoders" $RequiredEncoders "-encoders" "\.F\.\.\.\.|\.E\.\.\.\."

# 3.6 Bitstream Filters (Crucial for HLS TS -> MP4 conversion!)
$RequiredBsfs = @(
    "aac_adtstoasc",
    "h264_mp4toannexb",
    "hevc_mp4toannexb",
    "extract_extradata",
    "vp9_superframe",
    "av1_frame_merge"
)
Test-CapabilityList "Bitstream Filters" $RequiredBsfs "-bsfs"

# 3.7 Protocols
$RequiredProtocols = @(
    "file",
    "pipe",
    "concat"
)
Test-CapabilityList "Protocols" $RequiredProtocols "-protocols"

# -----------------------------------------------------------------------------
# 4. Optional Functional Smoke Tests
# -----------------------------------------------------------------------------
if ($RunSmokeTests) {
    Write-Host "`n==> [4/4] Executing Real Muxing Smoke Tests..." -ForegroundColor Cyan
    $TmpSmokeDir = Join-Path ([System.IO.Path]::GetTempPath()) ("flux_smoke_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $TmpSmokeDir | Out-Null

    try {
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "SilentlyContinue"

        $VttPath = Join-Path $TmpSmokeDir "test.vtt"
        $vttSample = "WEBVTT`n`n00:00:00.000 --> 00:00:02.000`nHello OpenIDM Subtitles`n"
        [System.IO.File]::WriteAllText($VttPath, $vttSample, [System.Text.Encoding]::UTF8)

        # Test A: Null muxer probe
        Write-Host "  [Test A] Null container output test..." -NoNewline
        $nullOut = & $FfmpegPath -hide_banner -loglevel error -y -i $VttPath -c:s webvtt -f null - 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Host " [PASS]" -ForegroundColor Green
        } else {
            Write-Host " [FAIL: $nullOut]" -ForegroundColor Red
            $MissingItems.Add("SmokeTest: Null output")
        }

        # Test B: WebVTT to MP4 mov_text subtitle encoding
        Write-Host "  [Test B] Subtitle conversion (WebVTT -> MP4 mov_text)..." -NoNewline
        $Mp4SubPath = Join-Path $TmpSmokeDir "sub.mp4"
        $subOut = & $FfmpegPath -hide_banner -loglevel error -y -i $VttPath -c:s mov_text -f mp4 $Mp4SubPath 2>&1
        if ($LASTEXITCODE -eq 0 -and (Test-Path $Mp4SubPath) -and (Get-Item $Mp4SubPath).Length -gt 0) {
            Write-Host " [PASS]" -ForegroundColor Green
        } else {
            Write-Host " [FAIL: $subOut]" -ForegroundColor Red
            $MissingItems.Add("SmokeTest: WebVTT to mov_text MP4")
        }

        # Test C: WebVTT to MKV subrip subtitle encoding
        Write-Host "  [Test C] Subtitle conversion (WebVTT -> MKV subrip)..." -NoNewline
        $MkvSubPath = Join-Path $TmpSmokeDir "sub.mkv"
        $mkvSubOut = & $FfmpegPath -hide_banner -loglevel error -y -i $VttPath -c:s subrip -f matroska $MkvSubPath 2>&1
        if ($LASTEXITCODE -eq 0 -and (Test-Path $MkvSubPath) -and (Get-Item $MkvSubPath).Length -gt 0) {
            Write-Host " [PASS]" -ForegroundColor Green
        } else {
            Write-Host " [FAIL: $mkvSubOut]" -ForegroundColor Red
            $MissingItems.Add("SmokeTest: WebVTT to subrip MKV")
        }

    } finally {
        $ErrorActionPreference = $prevEap
        Remove-Item -Recurse -Force $TmpSmokeDir -ErrorAction SilentlyContinue
    }
} else {
    Write-Host "`n==> [4/4] Skipping Functional Smoke Tests (use -RunSmokeTests to execute)." -ForegroundColor Gray
}

# -----------------------------------------------------------------------------
# Final Summary and Exit Code
# -----------------------------------------------------------------------------
Write-Host "`n=================================================================" -ForegroundColor Cyan
if ($MissingItems.Count -eq 0) {
    Write-Host "   Capability Audit PASSED -- All Required Features Present!    " -ForegroundColor Green
    Write-Host "=================================================================" -ForegroundColor Cyan
    Write-Host "The binary at $FfmpegPath satisfies all Flux requirements.`n" -ForegroundColor Green
    exit 0
} else {
    Write-Host "   Capability Audit FAILED -- $($MissingItems.Count) Feature(s) Missing!        " -ForegroundColor Red
    Write-Host "=================================================================" -ForegroundColor Cyan
    foreach ($missing in $MissingItems) {
        Write-Host "  - $missing" -ForegroundColor Red
    }
    Write-Host "`n"
    if ($FailOnMissing) {
        exit 1
    }
}
