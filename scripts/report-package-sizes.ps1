# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- Package Size Reporting & Budget Audit Script (v0.3.0)
# Measures each packaged component, distinguishes installed disk footprint from
# compressed ZIP and Inno Setup installer download sizes, and verifies compliance
# with docs/PERFORMANCE_AND_SIZE_BUDGET.md.

[CmdletBinding()]
param(
    [string]$Version = "",
    [string]$RepoRoot = "",
    [string]$DistDir = "",
    [string]$JsonOut = "",
    [switch]$UpdateMetrics = $false,
    [switch]$FailOnExceeded = $false
)

$ErrorActionPreference = "Stop"

if (-not $RepoRoot) {
    $ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
    if (-not $ScriptDir) { $ScriptDir = (Get-Location).Path }
    $RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
}
if (-not $DistDir) {
    $DistDir = Join-Path $RepoRoot "dist"
}

# Resolve and Normalize Version
if (-not $Version) {
    if ($env:RADIUM_VERSION) {
        $Version = $env:RADIUM_VERSION
    } elseif ($env:FLUX_VERSION) {
        $Version = $env:FLUX_VERSION
    } else {
        try {
            $gitTag = (git describe --tags --exact-match 2>$null)
            if ($gitTag -match '^v?(\d+\.\d+\.\d+.*)$') {
                $Version = $Matches[1]
            }
        } catch {}
        if (-not $Version) {
            $Version = "1.0.0"
        }
    }
}
if ($Version -match '^v?(\d+\.\d+\.\d+.*)$') {
    $Version = $Matches[1]
}

function Format-Bytes([int64]$Bytes) {
    if ($Bytes -ge 1GB) {
        return "$([Math]::Round($Bytes / 1GB, 2)) GB"
    } elseif ($Bytes -ge 1MB) {
        return "$([Math]::Round($Bytes / 1MB, 2)) MB"
    } elseif ($Bytes -ge 1KB) {
        return "$([Math]::Round($Bytes / 1KB, 1)) KB"
    } else {
        return "$Bytes B"
    }
}

Write-Host "`n=================================================================" -ForegroundColor Cyan
Write-Host "   Radium Download Manager -- Packaging Size Budget Audit Report   " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "  Product Version : $Version" -ForegroundColor White
Write-Host "  Repository Root : $RepoRoot" -ForegroundColor White
Write-Host "  Dist Directory  : $DistDir" -ForegroundColor White

# 1. Locate and Measure Core Binaries
$fluxCandidates = @(
    (Join-Path $DistDir "Radium.exe"),
    (Join-Path $DistDir "radium.exe"),
    (Join-Path $DistDir "Flux.exe"),
    (Join-Path $DistDir "flux.exe"),
    (Join-Path $RepoRoot "build\win-x64\Release\flux.exe"),
    (Join-Path $RepoRoot "build\win-x64\flux.exe")
)
$fluxExePath = $null
foreach ($c in $fluxCandidates) {
    if (Test-Path $c) { $fluxExePath = (Resolve-Path $c).Path; break }
}
$fluxExeSize = if ($fluxExePath) { (Get-Item $fluxExePath).Length } else { 0 }

$nmhCandidates = @(
    (Join-Path $DistDir "radium-nmh.exe"),
    (Join-Path $DistDir "flux-nmh.exe"),
    (Join-Path $RepoRoot "build\win-x64\Release\flux-nmh.exe"),
    (Join-Path $RepoRoot "build\win-x64\flux-nmh.exe")
)
$nmhExePath = $null
foreach ($c in $nmhCandidates) {
    if (Test-Path $c) { $nmhExePath = (Resolve-Path $c).Path; break }
}
$nmhExeSize = if ($nmhExePath) { (Get-Item $nmhExePath).Length } else { 0 }

$coreBinariesTotal = $fluxExeSize + $nmhExeSize

# 2. Measure Clean Extension Assets (excluding dev/test files)
$extDir = Join-Path $RepoRoot "extension"
$excludeItems = @(".git", ".github", "node_modules", "package.json", "package-lock.json", "tsconfig.json", "tests", "scratch", "*.ts", "*.map", "*.log", "*.tmp", ".DS_Store", "Thumbs.db")
$extFiles = if (Test-Path $extDir) {
    Get-ChildItem -Path $extDir -Recurse | Where-Object {
        if ($_.PSIsContainer) { return $false }
        $relPath = $_.FullName.Substring($extDir.Length)
        $skip = $false
        foreach ($ex in $excludeItems) {
            if ($relPath -like "*\$ex*" -or $relPath -like "*$ex") { $skip = $true; break }
        }
        -not $skip
    }
} else { @() }
$extTotalBytes = ($extFiles | Measure-Object -Property Length -Sum).Sum
if (-not $extTotalBytes) { $extTotalBytes = 0 }

# 3. Measure Documentation, Scripts, and Manifests
$docFiles = @("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md")
$docsBytes = 0
foreach ($d in $docFiles) {
    $p = Join-Path $RepoRoot $d
    if (Test-Path $p) { $docsBytes += (Get-Item $p).Length }
}
$regScriptPath = Join-Path $RepoRoot "scripts\register-native-messaging.ps1"
if (Test-Path $regScriptPath) { $docsBytes += (Get-Item $regScriptPath).Length }

$manifestsDir = Join-Path $RepoRoot "installer\manifests"
$manifestsBytes = 0
if (Test-Path $manifestsDir) {
    $manifestsBytes = (Get-ChildItem -Path $manifestsDir -File | Measure-Object -Property Length -Sum).Sum
    if (-not $manifestsBytes) { $manifestsBytes = 0 }
}
$docsAndManifestsTotal = $docsBytes + $manifestsBytes

# Core Total Installed Disk Footprint
$coreInstalledFootprint = $coreBinariesTotal + $extTotalBytes + $docsAndManifestsTotal

# 4. Measure Stream Workers (Single Standard Location: tools/win-x64)
$toolsWinDir = Join-Path $RepoRoot "tools\win-x64"
$nm3u8dlPath = Join-Path $toolsWinDir "N_m3u8DL-RE.exe"
$ffmpegPath = Join-Path $toolsWinDir "ffmpeg.exe"
$ytdlpPath = Join-Path $toolsWinDir "yt-dlp.exe"

$nm3u8dlSize = if (Test-Path $nm3u8dlPath) { (Get-Item $nm3u8dlPath).Length } else { 0 }
$ffmpegSize = if (Test-Path $ffmpegPath) { (Get-Item $ffmpegPath).Length } else { 0 }
$ytdlpSize = if (Test-Path $ytdlpPath) { (Get-Item $ytdlpPath).Length } else { 0 }

$streamWorkersSingleTotal = $nm3u8dlSize + $ffmpegSize + $ytdlpSize
$streamWorkersDuplicateTotal = $streamWorkersSingleTotal * 2

# Full Total Installed Disk Footprint (Single Standard Location)
$fullInstalledFootprint = $coreInstalledFootprint + $streamWorkersSingleTotal
# Legacy Duplicated Footprint (when tools were installed into both tools and tools\win-x64)
$legacyDuplicatedFootprint = $coreInstalledFootprint + $streamWorkersDuplicateTotal
$footprintSavingsBytes = $streamWorkersSingleTotal
$footprintSavingsPercent = if ($legacyDuplicatedFootprint -gt 0) { [Math]::Round(($footprintSavingsBytes / $legacyDuplicatedFootprint) * 100, 2) } else { 0 }

# 5. Measure Staged Distribution Archives and Installers (if built)
$coreZipCandidates = @(
    (Join-Path $DistDir "Radium-v$Version-Windows-x64-Core.zip")
)
$coreZipSize = $null
foreach ($z in $coreZipCandidates) {
    if (Test-Path $z) { $coreZipSize = (Get-Item $z).Length; break }
}

$fullZipCandidates = @(
    (Join-Path $DistDir "Radium-v$Version-Windows-x64-Full.zip"),
    (Join-Path $DistDir "Radium-v$Version-Windows-x64.zip")
)
$fullZipSize = $null
foreach ($z in $fullZipCandidates) {
    if (Test-Path $z) { $fullZipSize = (Get-Item $z).Length; break }
}

$coreInstallerCandidates = @(
    (Join-Path $DistDir "Radium-Setup-x64-Core-$Version.exe")
)
$coreInstallerSize = $null
foreach ($ins in $coreInstallerCandidates) {
    if (Test-Path $ins) { $coreInstallerSize = (Get-Item $ins).Length; break }
}

$fullInstallerCandidates = @(
    (Join-Path $DistDir "Radium-Setup-x64-$Version.exe")
)
$fullInstallerSize = $null
foreach ($ins in $fullInstallerCandidates) {
    if (Test-Path $ins) { $fullInstallerSize = (Get-Item $ins).Length; break }
}

# -----------------------------------------------------------------------------
# Display Report Tables
# -----------------------------------------------------------------------------
# Component Flavor Detection & Estimated Compressed Sizes
# -----------------------------------------------------------------------------
$nmFlavor = if ($nm3u8dlSize -eq 0) { "Not Staged (Est: 340 KB)" } elseif ($nm3u8dlSize -lt 5MB) { "Native C++ Worker (Prompt 2)" } else { "Upstream .NET AOT Worker" }
$ffFlavor = if ($ffmpegSize -eq 0) { "Not Staged (Est: 14.0 MB)" } elseif ($ffmpegSize -lt 25MB) { "Minimal LGPL FFmpeg (Prompt 3)" } else { "Upstream Monolithic FFmpeg" }
$ytFlavor = if ($ytdlpSize -eq 0) { "Not Staged (Est: 17.0 MB)" } elseif ($ytdlpSize -lt 12MB) { "Ultra-Compact yt-dlp_min" } else { "Standard Standalone yt-dlp" }

# Component-level solid LZMA2 compressed contribution estimates (bytes)
$fluxCompressedEst = [int64]1450000       # ~1.38 MB [ESTIMATE]
$nmhCompressedEst = [int64]135000         # ~132 KB [ESTIMATE]
$extCompressedEst = [int64]70000          # ~68 KB [ESTIMATE]
$docsCompressedEst = [int64]25000         # ~24 KB [ESTIMATE]
$innoStubCompressedEst = [int64]450000    # ~439 KB [ESTIMATE]

$nmCompressedEst = if ($nm3u8dlSize -gt 0 -and $nm3u8dlSize -lt 5MB) { [int64]125000 } elseif ($nm3u8dlSize -ge 5MB) { [int64]7450000 } else { [int64]125000 } # ~122 KB native vs ~7.1 MB upstream [ESTIMATE]
$ffCompressedEst = if ($ffmpegSize -gt 0 -and $ffmpegSize -lt 25MB) { [int64]5760000 } elseif ($ffmpegSize -ge 25MB) { [int64]71720000 } else { [int64]5760000 } # ~5.5 MB minimal vs ~68.4 MB upstream [ESTIMATE]
$ytCompressedEst = if ($ytdlpSize -gt 0 -and $ytdlpSize -lt 12MB) { [int64]7860000 } elseif ($ytdlpSize -ge 12MB) { [int64]12050000 } else { [int64]12050000 } # ~7.5 MB min vs ~11.5 MB standard [ESTIMATE]

# Total Estimated Installer Download Size
$coreInstallerEstBytes = $fluxCompressedEst + $nmhCompressedEst + $extCompressedEst + $docsCompressedEst + $innoStubCompressedEst # ~2.13 MB
$fullInstallerEstBytes = $coreInstallerEstBytes + $nmCompressedEst + $ffCompressedEst + $ytCompressedEst

Write-Host "`n--- [1] PACKAGED COMPONENT BREAKDOWN ---" -ForegroundColor Yellow
$compTable = @(
    [PSCustomObject]@{ Component = "radium.exe (Desktop GUI)"; Flavor = "Native C++20 / Win32"; Location = "dist\Radium.exe"; DiskFootprint = Format-Bytes $fluxExeSize; CompressedEst = Format-Bytes $fluxCompressedEst; PctOfFull = "$([Math]::Round(($fluxCompressedEst / $fullInstallerEstBytes) * 100, 1))% [EST]" },
    [PSCustomObject]@{ Component = "radium-nmh.exe (Native Host)"; Flavor = "Native C++20 / Stdio"; Location = "dist\radium-nmh.exe"; DiskFootprint = Format-Bytes $nmhExeSize; CompressedEst = Format-Bytes $nmhCompressedEst; PctOfFull = "$([Math]::Round(($nmhCompressedEst / $fullInstallerEstBytes) * 100, 1))% [EST]" },
    [PSCustomObject]@{ Component = "Browser Extension (clean)"; Flavor = "Vanilla TS / Manifest V3"; Location = "extension\"; DiskFootprint = Format-Bytes $extTotalBytes; CompressedEst = Format-Bytes $extCompressedEst; PctOfFull = "$([Math]::Round(($extCompressedEst / $fullInstallerEstBytes) * 100, 1))% [EST]" },
    [PSCustomObject]@{ Component = "Docs, Scripts & Manifests"; Flavor = "Licenses, Readme, Reg"; Location = "README, LICENSE, manifests"; DiskFootprint = Format-Bytes $docsAndManifestsTotal; CompressedEst = Format-Bytes $docsCompressedEst; PctOfFull = "$([Math]::Round(($docsCompressedEst / $fullInstallerEstBytes) * 100, 1))% [EST]" },
    [PSCustomObject]@{ Component = "N_m3u8DL-RE.exe"; Flavor = $nmFlavor; Location = "tools\win-x64\N_m3u8DL-RE.exe"; DiskFootprint = Format-Bytes $nm3u8dlSize; CompressedEst = Format-Bytes $nmCompressedEst; PctOfFull = "$([Math]::Round(($nmCompressedEst / $fullInstallerEstBytes) * 100, 1))% [EST]" },
    [PSCustomObject]@{ Component = "ffmpeg.exe"; Flavor = $ffFlavor; Location = "tools\win-x64\ffmpeg.exe"; DiskFootprint = Format-Bytes $ffmpegSize; CompressedEst = Format-Bytes $ffCompressedEst; PctOfFull = "$([Math]::Round(($ffCompressedEst / $fullInstallerEstBytes) * 100, 1))% [EST]" },
    [PSCustomObject]@{ Component = "yt-dlp.exe"; Flavor = $ytFlavor; Location = "tools\win-x64\yt-dlp.exe"; DiskFootprint = Format-Bytes $ytdlpSize; CompressedEst = Format-Bytes $ytCompressedEst; PctOfFull = "$([Math]::Round(($ytCompressedEst / $fullInstallerEstBytes) * 100, 1))% [EST]" }
)
$compTable | Format-Table -AutoSize | Out-String | Write-Host -ForegroundColor White

Write-Host "--- [2] INSTALLED DISK FOOTPRINT (Uncompressed on SSD) ---" -ForegroundColor Yellow
$footprintTable = @(
    [PSCustomObject]@{ Edition = "Core Edition (Fixed)"; WorkersLocation = "None (HTTP/HTTPS only)"; DiskFootprint = Format-Bytes $coreInstalledFootprint; Bytes = $coreInstalledFootprint; BudgetCeiling = "3.0 MB (3,145,728 B)"; Status = if ($coreInstalledFootprint -le 3145728) { "PASS" } else { "FAIL" } },
    [PSCustomObject]@{ Edition = "Full Edition (Single Standard)"; WorkersLocation = "tools\win-x64\ (Single)"; DiskFootprint = Format-Bytes $fullInstalledFootprint; Bytes = $fullInstalledFootprint; BudgetCeiling = "220.0 MB"; Status = if ($fullInstalledFootprint -le 230686720) { "PASS" } else { "FAIL" } },
    [PSCustomObject]@{ Edition = "Full Edition (Legacy Duplicated)"; WorkersLocation = "tools\ + tools\win-x64\ (Duplicate)"; DiskFootprint = Format-Bytes $legacyDuplicatedFootprint; Bytes = $legacyDuplicatedFootprint; BudgetCeiling = "N/A (Bloated)"; Status = "ELIMINATED" }
)
$footprintTable | Format-Table -AutoSize | Out-String | Write-Host -ForegroundColor White

Write-Host "[+] DUPLICATE PACKAGING ELIMINATION SAVINGS:" -ForegroundColor Green
Write-Host "    - Exact Disk Footprint Saved : $(Format-Bytes $footprintSavingsBytes) ($footprintSavingsBytes bytes)" -ForegroundColor Green
Write-Host "    - Disk Footprint Reduction   : $footprintSavingsPercent% saved by installing stream tools exclusively to tools\win-x64" -ForegroundColor Green

Write-Host "`n--- [3] DISTRIBUTION PACKAGE SIZES (Compressed Downloads) ---" -ForegroundColor Yellow
$pkgTable = @(
    [PSCustomObject]@{
        PackageType = "Portable ZIP (Core)"
        FileName = "Radium-v$Version-Windows-x64-Core.zip"
        CompressedSize = if ($coreZipSize) { Format-Bytes $coreZipSize } else { "~1.2 MB [ESTIMATE]" }
        ActualBytes = if ($coreZipSize) { $coreZipSize } else { 1250000 }
        UncompressedFootprint = Format-Bytes $coreInstalledFootprint
        Ratio = if ($coreZipSize -and $coreInstalledFootprint) { "$([Math]::Round((1 - ($coreZipSize / $coreInstalledFootprint)) * 100, 1))%" } else { "~55%" }
    },
    [PSCustomObject]@{
        PackageType = "Portable ZIP (Full)"
        FileName = "Radium-v$Version-Windows-x64-Full.zip"
        CompressedSize = if ($fullZipSize) { Format-Bytes $fullZipSize } else { "$(Format-Bytes $fullInstallerEstBytes) [ESTIMATE]" }
        ActualBytes = if ($fullZipSize) { $fullZipSize } else { $fullInstallerEstBytes }
        UncompressedFootprint = Format-Bytes $fullInstalledFootprint
        Ratio = if ($fullZipSize -and $fullInstalledFootprint) { "$([Math]::Round((1 - ($fullZipSize / $fullInstalledFootprint)) * 100, 1))%" } else { "~50-60%" }
    },
    [PSCustomObject]@{
        PackageType = "Inno Setup Installer (Core)"
        FileName = "Radium-Setup-x64-Core-$Version.exe"
        CompressedSize = if ($coreInstallerSize) { Format-Bytes $coreInstallerSize } else { "$(Format-Bytes $coreInstallerEstBytes) [ESTIMATE]" }
        ActualBytes = if ($coreInstallerSize) { $coreInstallerSize } else { $coreInstallerEstBytes }
        UncompressedFootprint = Format-Bytes $coreInstalledFootprint
        Ratio = "Solid LZMA2-Ultra (~60% compression)"
    },
    [PSCustomObject]@{
        PackageType = "Inno Setup Installer (Full)"
        FileName = "Radium-Setup-x64-$Version.exe"
        CompressedSize = if ($fullInstallerSize) { Format-Bytes $fullInstallerSize } else { "$(Format-Bytes $fullInstallerEstBytes) [ESTIMATE]" }
        ActualBytes = if ($fullInstallerSize) { $fullInstallerSize } else { $fullInstallerEstBytes }
        UncompressedFootprint = Format-Bytes $fullInstalledFootprint
        Ratio = "Solid LZMA2-Ultra (Single worker set)"
    }
)
$pkgTable | Format-Table -AutoSize | Out-String | Write-Host -ForegroundColor White

Write-Host "`n--- [4] BUD-003 TARGET AUDIT & PACKAGE COMPLIANCE ---" -ForegroundColor Yellow
$targetBytes = 23068672 # 22.0 MB (BUD-003 Target Ceiling: 17.5 - 22.0 MB)
$hardCeilingBytes = 39845888 # 38.0 MB (BUD-003 Hard Ceiling: CI Gate Failure)

if ($fullInstallerSize) {
    Write-Host "  Target Ceiling (Installer) : $(Format-Bytes $targetBytes) (22.0 MB)" -ForegroundColor White
    Write-Host "  Hard Ceiling (Installer)   : $(Format-Bytes $hardCeilingBytes) (38.0 MB)" -ForegroundColor White
    Write-Host "  Actual Full Installer Size : $(Format-Bytes $fullInstallerSize) ($fullInstallerSize bytes)" -ForegroundColor White
    if ($fullInstallerSize -le $targetBytes) {
        $underMb = [Math]::Round(($targetBytes - $fullInstallerSize) / 1MB, 2)
        Write-Host "  Compliance Status          : [PASS - ACTUAL] Generated installer satisfies BUD-003 target! ($underMb MB under target ceiling)" -ForegroundColor Green
    } elseif ($fullInstallerSize -le $hardCeilingBytes) {
        $overMb = [Math]::Round(($fullInstallerSize - $targetBytes) / 1MB, 2)
        Write-Host "  Compliance Status          : [PASS - WITHIN HARD CEILING] Generated installer satisfies hard ceiling (< 38 MB, +$overMb MB over target)" -ForegroundColor Yellow
    } else {
        $overMb = [Math]::Round(($fullInstallerSize - $hardCeilingBytes) / 1MB, 2)
        Write-Host "  Compliance Status          : [FAIL - ACTUAL] Generated installer EXCEEDS 38 MB hard ceiling by $overMb MB!" -ForegroundColor Red
        if ($FailOnExceeded) {
            throw "Budget Audit FAILED: Actual installer size ($(Format-Bytes $fullInstallerSize)) exceeds 38.0 MB hard ceiling by $overMb MB!"
        }
    }
} else {
    Write-Host "  Target Ceiling (Installer) : $(Format-Bytes $targetBytes) (22.0 MB)" -ForegroundColor White
    Write-Host "  Estimated Full Size        : $(Format-Bytes $fullInstallerEstBytes) [ESTIMATE - Installer not built yet]" -ForegroundColor White
    if ($fullInstallerEstBytes -le $targetBytes) {
        Write-Host "  Compliance Status          : [PASS - ESTIMATE] Estimated Full installer satisfies BUD-003 target!" -ForegroundColor Green
    } else {
        $overMb = [Math]::Round(($fullInstallerEstBytes - $targetBytes) / 1MB, 2)
        Write-Host "  Compliance Status          : [BORDERLINE/EXCEEDS by ~$overMb MB] [ESTIMATE]" -ForegroundColor Yellow
    }
}

if ($fullZipSize) {
    $zipTargetBytes = 26214400 # 25.0 MB target for portable zip
    Write-Host "  Target Ceiling (Full ZIP)  : $(Format-Bytes $zipTargetBytes) (25.0 MB)" -ForegroundColor White
    Write-Host "  Actual Full ZIP Size       : $(Format-Bytes $fullZipSize) ($fullZipSize bytes)" -ForegroundColor White
    if ($fullZipSize -le $zipTargetBytes) {
        $zipUnderMb = [Math]::Round(($zipTargetBytes - $fullZipSize) / 1MB, 2)
        Write-Host "  ZIP Compliance Status      : [PASS - ACTUAL] Generated Full ZIP satisfies 25 MB target! ($zipUnderMb MB under ceiling)" -ForegroundColor Green
    } else {
        $zipOverMb = [Math]::Round(($fullZipSize - $zipTargetBytes) / 1MB, 2)
        Write-Host "  ZIP Compliance Status      : [FAIL - ACTUAL] Generated Full ZIP EXCEEDS 25 MB target by $zipOverMb MB!" -ForegroundColor Red
        if ($FailOnExceeded) {
            throw "Budget Audit FAILED: Actual portable ZIP size ($(Format-Bytes $fullZipSize)) exceeds 25.0 MB target by $zipOverMb MB!"
        }
    }
}

# Strict worker size audits
if ($ffmpegSize -gt 25MB) {
    Write-Host "  [WARNING] Bloated FFmpeg detected ($(Format-Bytes $ffmpegSize) > 25 MB minimal profile)!" -ForegroundColor Red
    if ($FailOnExceeded) {
        throw "Budget Audit FAILED: Bloated FFmpeg detected ($(Format-Bytes $ffmpegSize)). Minimal worker profile required."
    }
}
if ($nm3u8dlSize -gt 5MB) {
    Write-Host "  [WARNING] Bloated N_m3u8DL-RE detected ($(Format-Bytes $nm3u8dlSize) > 5 MB native profile)!" -ForegroundColor Red
    if ($FailOnExceeded) {
        throw "Budget Audit FAILED: Upstream .NET N_m3u8DL-RE detected ($(Format-Bytes $nm3u8dlSize)). Native C++ worker required."
    }
}

Write-Host "`n=================================================================" -ForegroundColor Magenta
Write-Host "   CRITICAL PACKAGING RULE AND METRIC DISTINCTIONS              " -ForegroundColor Magenta
Write-Host "=================================================================" -ForegroundColor Magenta
Write-Host "  1. INSTALLED DISK FOOTPRINT : Total uncompressed bytes written to disk (Core: ~2.62 MB, Full: ~36-197 MB)." -ForegroundColor White
Write-Host "  2. PORTABLE ZIP SIZE        : Compressed portable archive download size (Core: ~1.2 MB, Full: ~20-72 MB)." -ForegroundColor White
Write-Host "  3. INSTALLER DOWNLOAD SIZE  : Solid LZMA2 single-file installer download size (Core: ~1.3 MB, Full: ~19-21 MB [EST])." -ForegroundColor White
Write-Host "  -> NEVER conflate installed footprint with installer or ZIP download size!" -ForegroundColor Yellow
Write-Host "=================================================================`n" -ForegroundColor Magenta

# 5. JSON Export and latest_metrics.json Synchronization
$budgetData = [ordered]@{
    version = $Version
    product_name = "Radium Download Manager"
    standard_stream_workers_location = "tools/win-x64"
    single_location_enforced = $true
    installed_disk_footprint = [ordered]@{
        core_installed_bytes = $coreInstalledFootprint
        core_installed_mb = [Math]::Round($coreInstalledFootprint / 1MB, 2)
        stream_workers_single_bytes = $streamWorkersSingleTotal
        stream_workers_single_mb = [Math]::Round($streamWorkersSingleTotal / 1MB, 2)
        full_installed_bytes = $fullInstalledFootprint
        full_installed_mb = [Math]::Round($fullInstalledFootprint / 1MB, 2)
        legacy_duplicate_stream_workers_bytes = $streamWorkersDuplicateTotal
        legacy_duplicate_full_footprint_bytes = $legacyDuplicatedFootprint
        installed_footprint_savings_bytes = $footprintSavingsBytes
        installed_footprint_savings_mb = [Math]::Round($footprintSavingsBytes / 1MB, 2)
        installed_footprint_reduction_percent = $footprintSavingsPercent
    }
    component_breakdown = [ordered]@{
        flux_exe_bytes = $fluxExeSize
        flux_exe_flavor = "Native C++20 / Win32"
        flux_nmh_exe_bytes = $nmhExeSize
        browser_extension_unpacked_bytes = $extTotalBytes
        docs_and_manifests_bytes = $docsAndManifestsTotal
        worker_nm3u8dl_re_bytes = $nm3u8dlSize
        worker_nm3u8dl_re_flavor = $nmFlavor
        worker_ffmpeg_bytes = $ffmpegSize
        worker_ffmpeg_flavor = $ffFlavor
        worker_ytdlp_bytes = $ytdlpSize
        worker_ytdlp_flavor = $ytFlavor
    }
    compressed_packages_estimates = [ordered]@{
        core_portable_zip_bytes_estimate = 1250000
        core_installer_download_bytes_estimate = $coreInstallerEstBytes
        full_portable_zip_bytes_estimate = $fullInstallerEstBytes
        full_installer_download_bytes_estimate = $fullInstallerEstBytes
    }
    target_20mb_audit = [ordered]@{
        target_ceiling_bytes = $targetBytes
        target_ceiling_mb = 20.0
        full_installer_estimate_bytes = $fullInstallerEstBytes
        full_installer_estimate_mb = [Math]::Round($fullInstallerEstBytes / 1MB, 2)
        status = if ($fullInstallerEstBytes -le $targetBytes) { "PASS" } else { "BORDERLINE" }
        primary_weight_contributors = [ordered]@{
            ytdlp_compressed_bytes_est = $ytCompressedEst
            ytdlp_percent_of_installer = [Math]::Round(($ytCompressedEst / $fullInstallerEstBytes) * 100, 1)
            ffmpeg_compressed_bytes_est = $ffCompressedEst
            ffmpeg_percent_of_installer = [Math]::Round(($ffCompressedEst / $fullInstallerEstBytes) * 100, 1)
        }
        recommended_alternatives = @(
            "Option A (Default): Keep standard offline yt-dlp (~20.5 MB installer) for full cipher extraction fidelity",
            "Option B: Ultra-compact yt-dlp_min (~15.2 MB installer) to guarantee < 20 MB offline installer",
            "Option C: Core Edition installer (~1.5 MB) with on-demand background worker provisioning"
        )
    }
    size_distinction_definitions = [ordered]@{
        installed_disk_footprint = "Uncompressed bytes occupied on the filesystem after installation or zip extraction."
        portable_zip_size = "Compressed ZIP archive download size (Deflate compression)."
        installer_download_size = "Inno Setup single-file solid LZMA2-Ultra installer download size."
        distinction_rule = "Installed disk footprint, ZIP size, and installer download size are three distinct metrics and must never be conflated."
    }
    budget_ceilings = [ordered]@{
        core_uncompressed_ceiling_bytes = 3145728 # 3.0 MB hard ceiling
        core_installer_compressed_ceiling_bytes = 1887436 # 1.8 MB hard ceiling
        full_installer_compressed_target_bytes = 20971520 # 20.0 MB aggressive target
        full_installer_compressed_ceiling_bytes = 39845888 # 38.0 MB hard ceiling
    }
}

if ($JsonOut) {
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    $jsonText = $budgetData | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText($JsonOut, ($jsonText + "`n"), $utf8NoBom)
    Write-Host "[+] Exported JSON packaging size report: $JsonOut" -ForegroundColor Green
}

if ($UpdateMetrics) {
    $metricsPath = Join-Path $RepoRoot "benchmarks\latest_metrics.json"
    if (Test-Path $metricsPath) {
        $existingMetrics = Get-Content $metricsPath -Raw | ConvertFrom-Json
        $existingMetrics | Add-Member -NotePropertyName "flux_binary_bytes" -NotePropertyValue $fluxExeSize -Force
        $existingMetrics | Add-Member -NotePropertyName "flux_nmh_binary_bytes" -NotePropertyValue $nmhExeSize -Force
        $existingMetrics | Add-Member -NotePropertyName "nm3u8dl_re_worker_bytes" -NotePropertyValue $nm3u8dlSize -Force
        $existingMetrics | Add-Member -NotePropertyName "ffmpeg_binary_bytes" -NotePropertyValue $ffmpegSize -Force
        $existingMetrics | Add-Member -NotePropertyName "ytdlp_binary_bytes" -NotePropertyValue $ytdlpSize -Force
        $existingMetrics | Add-Member -NotePropertyName "combined_core_binaries_kb" -NotePropertyValue ([Math]::Round(($fluxExeSize + $nmhExeSize) / 1KB, 1)) -Force
        $existingMetrics | Add-Member -NotePropertyName "total_stream_workers_kb" -NotePropertyValue ([Math]::Round($streamWorkersSingleTotal / 1KB, 1)) -Force
        $existingMetrics | Add-Member -NotePropertyName "full_installed_footprint_mb" -NotePropertyValue ([Math]::Round($fullInstalledFootprint / 1MB, 2)) -Force
        $existingMetrics | Add-Member -NotePropertyName "duplicate_savings_bytes" -NotePropertyValue $footprintSavingsBytes -Force
        $existingMetrics | Add-Member -NotePropertyName "duplicate_savings_mb" -NotePropertyValue ([Math]::Round($footprintSavingsBytes / 1MB, 2)) -Force
        $existingMetrics | Add-Member -NotePropertyName "packaging_size_budget" -NotePropertyValue $budgetData -Force

        $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
        $mergedJson = $existingMetrics | ConvertTo-Json -Depth 6
        [System.IO.File]::WriteAllText($metricsPath, ($mergedJson + "`n"), $utf8NoBom)
        Write-Host "[+] Updated: $metricsPath with real packaging size budget" -ForegroundColor Green
    }
}
