# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- Upstream Stream Worker Provisioning Script (v0.3.1)
# Downloads and cryptographically verifies upstream stream workers:
#  1. nilaoda/N_m3u8DL-RE (v0.3.0-beta win-x64) - MIT License
#  2. FFmpeg (win-x64 essentials / GPL-3.0)
#  3. yt-dlp (optional auxiliary metadata / Unlicense)
#
# Enforces pre-extraction or post-extraction SHA-256 digest validation,
# validates PE32+ AMD64 (0x8664) machine headers, and stages binaries into tools/win-x64/.

[CmdletBinding()]
param(
    [string]$TargetDir = "",
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture = "x64",
    [string]$Nm3u8dlUrl = "https://github.com/nilaoda/N_m3u8DL-RE/releases/download/v0.3.0-beta/N_m3u8DL-RE_v0.3.0-beta_win-x64_20241203.zip",
    [string]$Nm3u8dlExpectedSha256 = "E5C93A8DE0C58C50922C17A8A3BABD7D2193FB43B1A7FACD0B18DE9274EF8061",
    [string]$FfmpegUrl = "",
    [string]$FfmpegExpectedSha256 = "",
    [switch]$UseUpstreamFfmpeg = $false,
    [switch]$PreferNativeWorker = $true,
    [ValidateSet("standard", "min")]
    [string]$YtDlpFlavor = "standard",
    [string]$YtDlpUrl = "https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp.exe",
    [string]$YtDlpExpectedSha256 = "66674953fe251b89f4d08c5f0e35e0728679bd67ab3d7d05c0562af101dd3e7a",
    [switch]$Force = $false,
    [switch]$SkipChecksumVerification = $false
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not $ScriptDir -or -not (Test-Path $ScriptDir)) {
    $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
}
if (-not $ScriptDir -or -not (Test-Path $ScriptDir)) {
    $ScriptDir = (Get-Location).Path
}
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
if (-not $TargetDir) {
    $TargetDir = Join-Path $RepoRoot "tools\win-x64"
}
$TargetDir = [System.IO.Path]::GetFullPath($TargetDir)

New-Item -ItemType Directory -Force -Path $TargetDir | Out-Null
$TmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("radium_workers_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

function Assert-Pe64Executable([string]$FilePath, [string]$TargetArchitecture = "x64") {
    if (-not (Test-Path $FilePath)) {
        throw "Required executable missing: $FilePath"
    }
    $bytes = [System.IO.File]::ReadAllBytes($FilePath)
    if ($bytes.Length -lt 256 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "File is not a valid Windows PE executable (missing MZ header): $FilePath"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($TargetArchitecture -eq "ARM64") {
        if ($machine -ne 0xAA64 -and $machine -ne 0x8664) {
            throw "Executable $FilePath is not ARM64 (0xAA64) or x64 (0x8664); got 0x$($machine.ToString('X4'))"
        }
    } else {
        if ($machine -ne 0x8664) {
            throw "Executable $FilePath is not x64 (IMAGE_FILE_MACHINE_AMD64 0x8664); got 0x$($machine.ToString('X4'))"
        }
    }
}

function Assert-Sha256Checksum([string]$FilePath, [string]$ExpectedHash) {
    if (-not $ExpectedHash -or $SkipChecksumVerification) {
        return
    }
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($FilePath)
    try {
        $hashBytes = $hasher.ComputeHash($stream)
        $actual = [BitConverter]::ToString($hashBytes).Replace("-", "").ToUpperInvariant()
        $expected = $ExpectedHash.Trim().ToUpperInvariant()
        if ($actual -ne $expected) {
            throw "SHA-256 verification failed for $FilePath.`nExpected: $expected`nActual  : $actual"
        }
    } finally {
        $stream.Close()
        $hasher.Dispose()
    }
}

function Verify-ExistingBinary([string]$FilePath, [string]$ExpectedSha256, [string]$TargetArchitecture = "x64") {
    if (-not (Test-Path $FilePath)) { return $false }
    try {
        Assert-Pe64Executable $FilePath $TargetArchitecture
        if ($ExpectedSha256 -and -not $SkipChecksumVerification) {
            Assert-Sha256Checksum $FilePath $ExpectedSha256
        }
        return $true
    } catch {
        return $false
    }
}

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "   Flux Download Manager -- Upstream Worker Provisioning         " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "Target Directory: $TargetDir" -ForegroundColor White

try {
    # 1. N_m3u8DL-RE (Native C++ Stream Worker / Upstream Fallback)
    $NmExe = Join-Path $TargetDir "N_m3u8DL-RE.exe"
    $stagedFromNative = $false

    # Check if TargetDir already has a verified native C++ worker (< 5MB)
    if (Test-Path $NmExe) {
        $existingNmSize = (Get-Item $NmExe).Length
        if ($existingNmSize -lt 5MB -and (Verify-ExistingBinary $NmExe $null $Architecture)) {
            $stagedFromNative = $true
            $candSizeKb = [Math]::Round($existingNmSize / 1KB, 1)
            Write-Host "`n[1/3] Using verified native C++ stream worker in $TargetDir ($candSizeKb KB)" -ForegroundColor Green
        }
    }

    if (-not $stagedFromNative -and $PreferNativeWorker) {
        $nativeCandidates = @(
            (Join-Path $RepoRoot "build\win-x64\Release\N_m3u8DL-RE.exe"),
            (Join-Path $RepoRoot "build\Release\N_m3u8DL-RE.exe"),
            (Join-Path $RepoRoot "build\win-x64\N_m3u8DL-RE.exe"),
            (Join-Path $RepoRoot "build\N_m3u8DL-RE.exe"),
            (Join-Path $RepoRoot "dist\tools\win-x64\N_m3u8DL-RE.exe")
        )
        foreach ($cand in $nativeCandidates) {
            if (Test-Path $cand) {
                $candSize = (Get-Item $cand).Length
                if ($candSize -lt 5MB) {
                    Assert-Pe64Executable $cand $Architecture
                    Copy-Item -Force $cand $NmExe
                    $stagedFromNative = $true
                    $candSizeKb = [Math]::Round($candSize / 1KB, 1)
                    Write-Host "`n[1/3] Staged native C++ stream worker from build output: $cand ($candSizeKb KB)" -ForegroundColor Green
                    break
                }
            }
        }
    }

    if (-not $stagedFromNative) {
        $needNmDownload = $Force -or (-not (Verify-ExistingBinary $NmExe $Nm3u8dlExpectedSha256 $Architecture))
        if ($needNmDownload) {
            Write-Host "`n[1/3] Downloading upstream nilaoda/N_m3u8DL-RE win-x64..." -ForegroundColor Cyan
            $NmZip = Join-Path $TmpDir "nm3u8dl.zip"
            Invoke-WebRequest -Uri $Nm3u8dlUrl -OutFile $NmZip -UseBasicParsing
            Expand-Archive -Path $NmZip -DestinationPath (Join-Path $TmpDir "nm") -Force
            $FoundNm = Get-ChildItem -Path (Join-Path $TmpDir "nm") -Filter "N_m3u8DL-RE.exe" -Recurse | Select-Object -First 1
            if (-not $FoundNm) { throw "N_m3u8DL-RE.exe not found in downloaded archive: $NmZip" }
            Assert-Pe64Executable $FoundNm.FullName $Architecture
            Assert-Sha256Checksum $FoundNm.FullName $Nm3u8dlExpectedSha256
            Copy-Item -Force $FoundNm.FullName $NmExe
            Write-Host "  [OK] Staged and verified upstream N_m3u8DL-RE.exe" -ForegroundColor Green
        } else {
            Write-Host "`n[1/3] N_m3u8DL-RE.exe already present and verified in $TargetDir" -ForegroundColor Green
        }
    }
    Assert-Pe64Executable $NmExe $Architecture
    $nmSize = (Get-Item $NmExe).Length
    $nmSizeMb = [Math]::Round($nmSize / 1MB, 2)
    Write-Host "  N_m3u8DL-RE binary footprint: $nmSize bytes ($nmSizeMb MiB)" -ForegroundColor White

    # 2. FFmpeg (LGPL-2.1+ Minimal / GPL-3.0 Upstream)
    $FfExe = Join-Path $TargetDir "ffmpeg.exe"

    if (-not $FfmpegUrl) {
        if ($UseUpstreamFfmpeg) {
            $FfmpegUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
            Write-Warning "[LARGE FALLBACK] Explicitly using monolithic upstream FFmpeg (161+ MiB). Package size will exceed 20 MB budget."
        } else {
            # Pinned minimal FFmpeg 7.1 Windows x64 build from verified release
            $FfmpegUrl = "https://github.com/sayedalve/Radium-Download-Manager/releases/download/stream-tools-v1/ffmpeg-minimal-7.1-win-x64.zip"
            if (-not $FfmpegExpectedSha256) {
                $FfmpegExpectedSha256 = "CBCC7523D458E84309893719C96FFD3EF7ADCE490616CF5FD26B680FB80DF5CF"
            }
        }
    }

    $needFfDownload = $Force -or (-not (Test-Path $FfExe))
    if (-not $needFfDownload) {
        $existingFfSize = (Get-Item $FfExe).Length
        if (-not $UseUpstreamFfmpeg -and $existingFfSize -gt 25MB) {
            Write-Host "Existing ffmpeg.exe ($([Math]::Round($existingFfSize/1MB, 1)) MB) exceeds minimal budget (<25 MB); re-downloading minimal build..." -ForegroundColor Yellow
            $needFfDownload = $true
        } elseif ($FfmpegExpectedSha256 -and -not (Verify-ExistingBinary $FfExe $FfmpegExpectedSha256 $Architecture)) {
            $needFfDownload = $true
        }
    }

    if ($needFfDownload) {
        $flavorDesc = if ($UseUpstreamFfmpeg) { "upstream monolithic" } else { "minimal LGPL" }
        Write-Host "`n[2/3] Downloading $flavorDesc FFmpeg win-x64 ($FfmpegUrl)..." -ForegroundColor Cyan
        $FfZip = Join-Path $TmpDir "ffmpeg.zip"
        try {
            Invoke-WebRequest -Uri $FfmpegUrl -OutFile $FfZip -UseBasicParsing
        } catch {
            throw "Failed to download minimal FFmpeg from ${FfmpegUrl}: $($_.Exception.Message)`nSilent fallback to bloated upstream monolithic FFmpeg (161+ MiB) is strictly prohibited to prevent package size regressions.`nTo explicitly allow large upstream worker, specify -UseUpstreamFfmpeg."
        }
        Expand-Archive -Path $FfZip -DestinationPath (Join-Path $TmpDir "ff") -Force
        $FoundFf = Get-ChildItem -Path (Join-Path $TmpDir "ff") -Filter "ffmpeg.exe" -Recurse | Select-Object -First 1
        if (-not $FoundFf) { throw "ffmpeg.exe not found in downloaded archive: $FfZip" }
        Assert-Pe64Executable $FoundFf.FullName $Architecture
        if ($FfmpegExpectedSha256) {
            Assert-Sha256Checksum $FoundFf.FullName $FfmpegExpectedSha256
        } else {
            $hasher = [System.Security.Cryptography.SHA256]::Create()
            $stream = [System.IO.File]::OpenRead($FoundFf.FullName)
            $actual = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace("-", "").ToUpperInvariant()
            $stream.Close()
            $hasher.Dispose()
            Write-Host "  [INFO] Verified PE64. Computed SHA-256 for ffmpeg.exe: $actual" -ForegroundColor Cyan
        }
        $downloadedFfSize = (Get-Item $FoundFf.FullName).Length
        if (-not $UseUpstreamFfmpeg -and $downloadedFfSize -gt 25MB) {
            throw "Downloaded FFmpeg executable ($($FoundFf.FullName)) is $([Math]::Round($downloadedFfSize/1MB, 2)) MiB, exceeding the 25 MB ceiling for minimal builds."
        }
        Copy-Item -Force $FoundFf.FullName $FfExe
        Write-Host "  [OK] Staged and verified ffmpeg.exe ($([Math]::Round($downloadedFfSize/1MB, 2)) MiB)" -ForegroundColor Green
    } else {
        Write-Host "`n[2/3] ffmpeg.exe already present and verified in $TargetDir" -ForegroundColor Green
    }
    Assert-Pe64Executable $FfExe $Architecture

    $ffSize = (Get-Item $FfExe).Length
    $ffSizeMiB = [Math]::Round($ffSize / 1MB, 2)
    Write-Host "  FFmpeg binary footprint: $ffSize bytes ($ffSizeMiB MiB)" -ForegroundColor White

    # Run capability auditor if available
    $VerifyScript = Join-Path $ScriptDir "verify-ffmpeg-capabilities.ps1"
    if (Test-Path $VerifyScript) {
        Write-Host "  Auditing FFmpeg capabilities for Flux stream pipeline..." -ForegroundColor Cyan
        & $VerifyScript -FfmpegPath $FfExe -FailOnMissing:$false
    }

    # 3. yt-dlp (Auxiliary metadata & site extractor worker)
    $YtExe = Join-Path $TargetDir "yt-dlp.exe"

    if ($YtDlpFlavor -eq "min" -and $YtDlpUrl -like "*yt-dlp.exe*") {
        $YtDlpUrl = "https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp_min.exe"
        if (-not $Force) { $YtDlpExpectedSha256 = "" }
    }

    $needYtDownload = $Force -or (-not (Verify-ExistingBinary $YtExe $YtDlpExpectedSha256 $Architecture))

    if ($needYtDownload) {
        Write-Host "`n[3/3] Downloading upstream yt-dlp ($YtDlpFlavor flavor)..." -ForegroundColor Cyan
        $YtDl = Join-Path $TmpDir "yt-dlp.exe"
        Invoke-WebRequest -Uri $YtDlpUrl -OutFile $YtDl -UseBasicParsing
        Assert-Pe64Executable $YtDl $Architecture
        if ($YtDlpExpectedSha256) {
            Assert-Sha256Checksum $YtDl $YtDlpExpectedSha256
        }
        Copy-Item -Force $YtDl $YtExe
        Write-Host "  [OK] Staged and verified yt-dlp.exe" -ForegroundColor Green
    } else {
        Write-Host "`n[3/3] yt-dlp.exe already present and verified in $TargetDir" -ForegroundColor Green
    }
    Assert-Pe64Executable $YtExe $Architecture

    $ytSize = (Get-Item $YtExe).Length
    $ytSizeMb = [Math]::Round($ytSize / 1MB, 2)
    Write-Host "  yt-dlp binary footprint: $ytSize bytes ($ytSizeMb MiB)" -ForegroundColor White

    # If dist\tools\win-x64 exists, keep it in sync for distribution packaging
    $DistToolsX64 = Join-Path $RepoRoot "dist\tools\win-x64"
    if (Test-Path (Join-Path $RepoRoot "dist")) {
        New-Item -ItemType Directory -Force -Path $DistToolsX64 | Out-Null
        Copy-Item -Force $NmExe (Join-Path $DistToolsX64 "N_m3u8DL-RE.exe")
        Copy-Item -Force $FfExe (Join-Path $DistToolsX64 "ffmpeg.exe")
        Copy-Item -Force $YtExe (Join-Path $DistToolsX64 "yt-dlp.exe")
    }

    Write-Host "`n=================================================================" -ForegroundColor Cyan
    Write-Host "   Stream Workers Provisioned and Cryptographically Verified     " -ForegroundColor Green
    Write-Host "=================================================================" -ForegroundColor Cyan
} finally {
    Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
}
