# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- Windows x64 Release Packaging Script (v1.0.0)
# Assembles standalone portable zip packages (Full and Core editions),
# performs strict pre-flight binary validation, stages clean extension assets,
# writes PACKAGE_INFO.json metadata, and updates SHA256SUMS.txt.

[CmdletBinding()]
param(
    [string]$Version = "",
    [ValidateSet("Full", "Core", "Both")]
    [string]$PackageType = "Both",
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture = "x64",
    [string]$BuildDir = "",
    [string]$OutputDir = "",
    [switch]$SkipHash = $false,
    [switch]$AllowLargeWorkers = $false
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

# 1. Resolve and Validate Release Version
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
} else {
    throw "Invalid version format '$Version'. Expected semantic version like '1.0.0'."
}

# 2. Resolve Output Directory
if (-not $OutputDir) {
    $OutputDir = Join-Path $RepoRoot "dist"
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "   Radium Download Manager -- Windows $Architecture Release Packager" -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "  Version      : $Version" -ForegroundColor White
Write-Host "  Package Type : $PackageType" -ForegroundColor White
Write-Host "  Architecture : $Architecture" -ForegroundColor White
Write-Host "  Output Dir   : $OutputDir" -ForegroundColor White

# 2. Locate Core Executables
$FluxExe = $null
$FluxNmhExe = $null

$fluxCandidates = @()
if ($BuildDir) {
    $fluxCandidates += (Join-Path $BuildDir "radium.exe")
    $fluxCandidates += (Join-Path $BuildDir "Release\radium.exe")
    $fluxCandidates += (Join-Path $BuildDir "Radium.exe")
    $fluxCandidates += (Join-Path $BuildDir "flux.exe")
    $fluxCandidates += (Join-Path $BuildDir "Release\flux.exe")
    $fluxCandidates += (Join-Path $BuildDir "Flux.exe")
}
$fluxCandidates += @(
    (Join-Path $RepoRoot "build\win-x64\Release\radium.exe"),
    (Join-Path $RepoRoot "build\Release\radium.exe"),
    (Join-Path $RepoRoot "build\win-x64\radium.exe"),
    (Join-Path $RepoRoot "dist\Radium.exe"),
    (Join-Path $RepoRoot "dist\radium.exe"),
    (Join-Path $RepoRoot "radium.exe"),
    (Join-Path $RepoRoot "build\win-x64\Release\flux.exe"),
    (Join-Path $RepoRoot "build\Release\flux.exe"),
    (Join-Path $RepoRoot "build\win-x64\flux.exe"),
    (Join-Path $RepoRoot "dist\Flux.exe"),
    (Join-Path $RepoRoot "dist\flux.exe"),
    (Join-Path $RepoRoot "flux.exe")
)

foreach ($c in $fluxCandidates) {
    if (Test-Path $c) {
        $FluxExe = (Resolve-Path $c).Path
        break
    }
}

$nmhCandidates = @()
if ($BuildDir) {
    $nmhCandidates += (Join-Path $BuildDir "radium-nmh.exe")
    $nmhCandidates += (Join-Path $BuildDir "Release\radium-nmh.exe")
    $nmhCandidates += (Join-Path $BuildDir "flux-nmh.exe")
    $nmhCandidates += (Join-Path $BuildDir "Release\flux-nmh.exe")
}
$nmhCandidates += @(
    (Join-Path $RepoRoot "build\win-x64\Release\radium-nmh.exe"),
    (Join-Path $RepoRoot "build\Release\radium-nmh.exe"),
    (Join-Path $RepoRoot "build\win-x64\radium-nmh.exe"),
    (Join-Path $RepoRoot "dist\radium-nmh.exe"),
    (Join-Path $RepoRoot "radium-nmh.exe"),
    (Join-Path $RepoRoot "build\win-x64\Release\flux-nmh.exe"),
    (Join-Path $RepoRoot "build\Release\flux-nmh.exe"),
    (Join-Path $RepoRoot "build\win-x64\flux-nmh.exe"),
    (Join-Path $RepoRoot "dist\flux-nmh.exe"),
    (Join-Path $RepoRoot "flux-nmh.exe")
)

foreach ($c in $nmhCandidates) {
    if (Test-Path $c) {
        $FluxNmhExe = (Resolve-Path $c).Path
        break
    }
}

# Pre-flight binary architecture and existence validation
$expectedMachine = if ($Architecture -eq "ARM64") { 0xAA64 } else { 0x8664 }
$expectedArchName = if ($Architecture -eq "ARM64") { "ARM64 (0xAA64)" } else { "x64 AMD64 (0x8664)" }

function Test-BinaryArchitecture([string]$FilePath, [uint16]$TargetMachine) {
    if (-not (Test-Path $FilePath)) { return $false }
    try {
        $bytes = [System.IO.File]::ReadAllBytes($FilePath)
        if ($bytes.Length -lt 256 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $false }
        $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
        $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
        return ($machine -eq $TargetMachine)
    } catch {
        return $false
    }
}

$missing = @()
if (-not $FluxExe) { 
    $missing += "radium.exe / flux.exe (searched: $($fluxCandidates -join ', '))" 
} elseif (-not (Test-BinaryArchitecture $FluxExe $expectedMachine)) {
    throw "Packaging error: Binary $FluxExe does not match target architecture $expectedArchName"
}

if (-not $FluxNmhExe) { 
    $missing += "radium-nmh.exe / flux-nmh.exe (searched: $($nmhCandidates -join ', '))" 
} elseif (-not (Test-BinaryArchitecture $FluxNmhExe $expectedMachine)) {
    throw "Packaging error: Binary $FluxNmhExe does not match target architecture $expectedArchName"
}

if ($missing.Count -gt 0) {
    Write-Error "Pre-flight validation failed! The following required core binaries were not found:`n - $($missing -join "`n - ")`nPlease build Radium before running packaging:`n  cmake --build build --config Release`nor`n  .\scripts\build-windows-msvc.ps1"
    exit 1
}

Write-Host "[+] Found Desktop binary : $FluxExe" -ForegroundColor Green
Write-Host "[+] Found NMH binary     : $FluxNmhExe" -ForegroundColor Green

# 3. Locate Stream Workers (for Full package)
$Nm3u8dlExe = $null
$FfmpegExe = $null
$YtDlpExe = $null

# Check standardized tools\win-x64 directory first for uniform worker selection
$toolsWinDir = Join-Path $RepoRoot "tools\win-x64"
if (Test-Path $toolsWinDir) {
    $stagedNm = Join-Path $toolsWinDir "N_m3u8DL-RE.exe"
    if (Test-Path $stagedNm) { $Nm3u8dlExe = (Resolve-Path $stagedNm).Path }

    $stagedFf = Join-Path $toolsWinDir "ffmpeg.exe"
    if (Test-Path $stagedFf) { $FfmpegExe = (Resolve-Path $stagedFf).Path }

    $stagedYt = Join-Path $toolsWinDir "yt-dlp.exe"
    if (Test-Path $stagedYt) { $YtDlpExe = (Resolve-Path $stagedYt).Path }
}

# If N_m3u8DL-RE not in tools\win-x64, search build directories
if (-not $Nm3u8dlExe) {
    $nativeWorkerCandidates = @()
    if ($BuildDir) {
        $nativeWorkerCandidates += (Join-Path $BuildDir "N_m3u8DL-RE.exe")
        $nativeWorkerCandidates += (Join-Path $BuildDir "Release\N_m3u8DL-RE.exe")
    }
    $nativeWorkerCandidates += @(
        (Join-Path $RepoRoot "build\win-x64\Release\N_m3u8DL-RE.exe"),
        (Join-Path $RepoRoot "build\win-x64\N_m3u8DL-RE.exe"),
        (Join-Path $RepoRoot "build\Release\N_m3u8DL-RE.exe"),
        (Join-Path $RepoRoot "dist\tools\win-x64\N_m3u8DL-RE.exe")
    )
    foreach ($nw in $nativeWorkerCandidates) {
        if (Test-Path $nw) {
            $nwSize = (Get-Item $nw).Length
            if ($nwSize -lt 5MB) {
                $Nm3u8dlExe = (Resolve-Path $nw).Path
                break
            }
        }
    }
}

$toolsCandidates = @(
    (Join-Path $RepoRoot "tools"),
    (Join-Path $RepoRoot "dist\tools\win-x64"),
    (Join-Path $RepoRoot "dist\tools")
)

foreach ($tc in $toolsCandidates) {
    $nm = Join-Path $tc "N_m3u8DL-RE.exe"
    if ((-not $Nm3u8dlExe) -and (Test-Path $nm)) { $Nm3u8dlExe = (Resolve-Path $nm).Path }

    $ff = Join-Path $tc "ffmpeg.exe"
    if ((-not $FfmpegExe) -and (Test-Path $ff)) { $FfmpegExe = (Resolve-Path $ff).Path }

    $yt = Join-Path $tc "yt-dlp.exe"
    if ((-not $YtDlpExe) -and (Test-Path $yt)) { $YtDlpExe = (Resolve-Path $yt).Path }
    $ytMin = Join-Path $tc "yt-dlp_min.exe"
    if ((-not $YtDlpExe) -and (Test-Path $ytMin)) { $YtDlpExe = (Resolve-Path $ytMin).Path }
}

if ($PackageType -in @("Full", "Both")) {
    $missingWorkers = @()
    if (-not $Nm3u8dlExe) { $missingWorkers += "N_m3u8DL-RE.exe" }
    if (-not $FfmpegExe) { $missingWorkers += "ffmpeg.exe" }

    if ($missingWorkers.Count -gt 0) {
        Write-Warning "Full package requested, but the following stream workers are missing:`n - $($missingWorkers -join "`n - ")`nRun '.\scripts\fetch-workers.ps1' to provision them, or specify -PackageType Core."
        if ($PackageType -eq "Full") {
            Write-Error "Cannot assemble Full package without required stream workers."
            exit 1
        }
    } else {
        $nmSize = (Get-Item $Nm3u8dlExe).Length
        $nmType = if ($nmSize -lt 5MB) { "native C++ worker ($([Math]::Round($nmSize/1KB, 1)) KB)" } else { "upstream .NET worker ($([Math]::Round($nmSize/1MB, 2)) MB)" }
        $ffSize = (Get-Item $FfmpegExe).Length
        $ffType = if ($ffSize -lt 25MB) { "minimal LGPL build ($([Math]::Round($ffSize/1MB, 2)) MB)" } else { "upstream build ($([Math]::Round($ffSize/1MB, 2)) MB)" }

        if (-not $AllowLargeWorkers) {
            if ($ffSize -gt 25MB) {
                throw "Packaging halted: ffmpeg.exe ($FfmpegExe) size ($([Math]::Round($ffSize/1MB, 2)) MB) exceeds minimal budget (< 25 MB). Silent inclusion of bloated upstream FFmpeg is prohibited. Run scripts/fetch-workers.ps1, or pass -AllowLargeWorkers."
            }
            if ($nmSize -gt 5MB) {
                throw "Packaging halted: N_m3u8DL-RE.exe ($Nm3u8dlExe) size ($([Math]::Round($nmSize/1MB, 2)) MB) is not the native C++ worker (< 5 MB). Run scripts/fetch-workers.ps1 or build with CMake, or pass -AllowLargeWorkers."
            }
        }

        $nmHash = (Get-FileHash -Path $Nm3u8dlExe -Algorithm SHA256).Hash
        $ffHash = (Get-FileHash -Path $FfmpegExe -Algorithm SHA256).Hash
        Write-Host "[+] Packaged N_m3u8DL-RE : $Nm3u8dlExe ($nmType) [SHA256: $nmHash]" -ForegroundColor Green
        Write-Host "[+] Packaged FFmpeg     : $FfmpegExe ($ffType) [SHA256: $ffHash]" -ForegroundColor Green
        if ($YtDlpExe) {
            $ytSize = (Get-Item $YtDlpExe).Length
            $ytHash = (Get-FileHash -Path $YtDlpExe -Algorithm SHA256).Hash
            Write-Host "[+] Packaged yt-dlp     : $YtDlpExe ($([Math]::Round($ytSize/1MB, 2)) MB) [SHA256: $ytHash]" -ForegroundColor Green
        }
    }
}

# 4. Helper Function: Assemble and Zip a Distribution Package
function Build-PackageArchive([string]$Type, [string]$ZipFileName) {
    Write-Host "`n==> Assembling $Type package..." -ForegroundColor Cyan

    $stageDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Radium_pkg_" + $Type + "_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

    try {
        # Core binaries (Radium primary)
        Copy-Item -Path $FluxExe -Destination (Join-Path $stageDir "radium.exe") -Force
        Copy-Item -Path $FluxExe -Destination (Join-Path $stageDir "Radium.exe") -Force
        Copy-Item -Path $FluxNmhExe -Destination (Join-Path $stageDir "radium-nmh.exe") -Force

        # License and Documentation
        $docFiles = @("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md")
        foreach ($doc in $docFiles) {
            $src = Join-Path $RepoRoot $doc
            if (Test-Path $src) {
                Copy-Item -Path $src -Destination (Join-Path $stageDir $doc) -Force
            }
        }

        # Portable Registration Script
        $regScript = Join-Path $RepoRoot "scripts\register-native-messaging.ps1"
        if (Test-Path $regScript) {
            $scriptsDir = Join-Path $stageDir "scripts"
            New-Item -ItemType Directory -Force -Path $scriptsDir | Out-Null
            Copy-Item -Path $regScript -Destination (Join-Path $scriptsDir "register-native-messaging.ps1") -Force
        }

        # Manifests
        $manifestsSrc = Join-Path $RepoRoot "installer\manifests"
        if (Test-Path $manifestsSrc) {
            $manifestsDest = Join-Path $stageDir "manifests"
            Copy-Item -Path $manifestsSrc -Destination $manifestsDest -Recurse -Force
        }

        # Clean Browser Extension Staging
        $extSrc = Join-Path $RepoRoot "extension"
        if (Test-Path $extSrc) {
            $extDest = Join-Path $stageDir "extension"
            New-Item -ItemType Directory -Force -Path $extDest | Out-Null

            # Copy extension files strictly excluding dev/test/scratch files
            $excludeItems = @(".git", ".github", "node_modules", "package.json", "package-lock.json", "tsconfig.json", "tests", "scratch", "*.ts", "*.map", "*.log", "*.tmp", ".DS_Store", "Thumbs.db")
            Get-ChildItem -Path $extSrc -Recurse | Where-Object {
                $item = $_
                $relPath = $item.FullName.Substring($extSrc.Length)
                $skip = $false
                foreach ($ex in $excludeItems) {
                    if ($relPath -like "*\$ex*" -or $relPath -like "*$ex") {
                        $skip = $true
                        break
                    }
                }
                -not $skip
            } | ForEach-Object {
                $targetPath = Join-Path $extDest $_.FullName.Substring($extSrc.Length)
                if ($_.PSIsContainer) {
                    if (-not (Test-Path $targetPath)) { New-Item -ItemType Directory -Force -Path $targetPath | Out-Null }
                } else {
                    $parent = Split-Path -Parent $targetPath
                    if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
                    Copy-Item -Path $_.FullName -Destination $targetPath -Force
                }
            }
        }

        # Stream Workers Handling - single standard location tools\win-x64
        $toolsDest = Join-Path $stageDir "tools\win-x64"
        New-Item -ItemType Directory -Force -Path $toolsDest | Out-Null

        $components = @("radium.exe", "radium-nmh.exe", "extension", "manifests")

        if ($Type -eq "Full") {
            if ($Nm3u8dlExe -and (Test-Path $Nm3u8dlExe)) {
                Copy-Item -Path $Nm3u8dlExe -Destination (Join-Path $toolsDest "N_m3u8DL-RE.exe") -Force
                $components += "tools/win-x64/N_m3u8DL-RE.exe"
            }
            if ($FfmpegExe -and (Test-Path $FfmpegExe)) {
                Copy-Item -Path $FfmpegExe -Destination (Join-Path $toolsDest "ffmpeg.exe") -Force
                $components += "tools/win-x64/ffmpeg.exe"
            }
            if ($YtDlpExe -and (Test-Path $YtDlpExe)) {
                Copy-Item -Path $YtDlpExe -Destination (Join-Path $toolsDest "yt-dlp.exe") -Force
                $components += "tools/win-x64/yt-dlp.exe"
            }
        } else {
            # Core Package: Write helpful instructions for stream workers
            $coreReadme = @"
Radium Download Manager -- Core Edition
======================================
This lightweight package contains the core HTTP/HTTPS download engine, desktop UI,
and browser Native Messaging Host.

Stream Extraction Notice:
To enable HLS, DASH, and video stream downloading:
1. Place 'N_m3u8DL-RE.exe' and 'ffmpeg.exe' inside this 'tools\win-x64\' folder, OR
2. Ensure FFmpeg is accessible in your system PATH environment variable, OR
3. Download the full offline release (Radium-v$Version-Windows-x64-Full.zip).

Official Upstream Projects:
- N_m3u8DL-RE : https://github.com/nilaoda/N_m3u8DL-RE
- FFmpeg      : https://ffmpeg.org/
"@
            [System.IO.File]::WriteAllText((Join-Path $toolsDest "README.txt"), $coreReadme, [System.Text.Encoding]::UTF8)
            $components += "tools/win-x64/README.txt"
        }

        # Calculate uncompressed installed disk footprint before archiving
        $stagedFiles = Get-ChildItem -Path $stageDir -Recurse -File
        $uncompressedBytes = ($stagedFiles | Measure-Object -Property Length -Sum).Sum
        $uncompressedMb = [Math]::Round($uncompressedBytes / 1MB, 2)

        # Package Metadata: PACKAGE_INFO.json
        $gitCommit = "unknown"
        try {
            $gitCommit = (git rev-parse --short HEAD 2>$null)
            if (-not $gitCommit) { $gitCommit = "release" }
        } catch { $gitCommit = "release" }

        $pkgInfo = [ordered]@{
            name = "Radium Download Manager"
            version = $Version
            edition = $Type
            platform = if ($Architecture -eq "ARM64") { "Windows 11 ARM64" } else { "Windows 11 / Windows 10 x64" }
            architecture = $Architecture
            buildDate = [DateTime]::UtcNow.ToString("o")
            gitCommit = $gitCommit
            repository = "https://github.com/sayedalve/Radium-Download-Manager"
            license = "Apache-2.0"
            installedDiskFootprintBytes = $uncompressedBytes
            installedDiskFootprintMB = $uncompressedMb
            standardWorkerLocation = "tools/win-x64"
            components = $components
        }

        $pkgInfoJson = $pkgInfo | ConvertTo-Json -Depth 4
        [System.IO.File]::WriteAllText((Join-Path $stageDir "PACKAGE_INFO.json"), $pkgInfoJson, [System.Text.Encoding]::UTF8)

        # Create Zip Archive
        $destZip = Join-Path $OutputDir $ZipFileName
        if (Test-Path $destZip) { Remove-Item -Force $destZip }

        Write-Host "[*] Compressing $ZipFileName..." -ForegroundColor Gray
        Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $destZip -CompressionLevel Optimal

        $zipSize = (Get-Item $destZip).Length
        $sizeMb = [Math]::Round($zipSize / 1MB, 2)
        $compressionRatio = if ($uncompressedBytes -gt 0) { [Math]::Round((1 - ($zipSize / $uncompressedBytes)) * 100, 1) } else { 0 }

        Write-Host "[+] Created: $destZip" -ForegroundColor Green
        Write-Host "    - Installed Disk Footprint (Uncompressed) : $uncompressedMb MB ($uncompressedBytes bytes)" -ForegroundColor Cyan
        Write-Host "    - Portable ZIP Download Size (Compressed) : $sizeMb MB ($zipSize bytes)" -ForegroundColor Green
        Write-Host "    - Compression Space Savings               : $compressionRatio%" -ForegroundColor Gray

        return $destZip
    } finally {
        Remove-Item -Recurse -Force $stageDir -ErrorAction SilentlyContinue
    }
}

# 5. Execute Requested Packaging Runs
$createdZips = @()

$zipPrefix = "Radium-v$Version-Windows-$Architecture"
if ($PackageType -in @("Full", "Both") -and $Nm3u8dlExe -and $FfmpegExe) {
    $fullZip = Build-PackageArchive -Type "Full" -ZipFileName "$zipPrefix-Full.zip"
    $createdZips += $fullZip

    # Also stage legacy alias Radium-v$Version-Windows-<arch>.zip for backward compatibility
    $compatZip = Join-Path $OutputDir "$zipPrefix.zip"
    Copy-Item -Force $fullZip $compatZip
    $createdZips += $compatZip
}

if ($PackageType -in @("Core", "Both")) {
    $coreZip = Build-PackageArchive -Type "Core" -ZipFileName "$zipPrefix-Core.zip"
    $createdZips += $coreZip
}

# 6. Checksums and SHA256SUMS.txt Generation
if (-not $SkipHash -and $createdZips.Count -gt 0) {
    Write-Host "`n==> Generating SHA-256 Checksums..." -ForegroundColor Cyan
    $sumsPath = Join-Path $OutputDir "SHA256SUMS.txt"
    $existingLines = @()
    if (Test-Path $sumsPath) {
        $existingLines = Get-Content $sumsPath
    }

    $newEntries = @()
    $filesToHash = @()
    foreach ($z in ($createdZips | Select-Object -Unique)) {
        $filesToHash += $z
    }
    $setupCandidates = @(
        (Join-Path $OutputDir "Radium-Setup-$Architecture-$Version.exe"),
        (Join-Path $OutputDir "Radium-Setup-$Architecture-Core-$Version.exe"),
        (Join-Path $OutputDir "Radium-Setup-x64-$Version.exe"),
        (Join-Path $OutputDir "Radium-Setup-x64-Core-$Version.exe"),
        (Join-Path $OutputDir "Radium-Setup-ARM64-$Version.exe"),
        (Join-Path $OutputDir "Radium-Setup-ARM64-Core-$Version.exe")
    )
    foreach ($sc in $setupCandidates) {
        if (Test-Path $sc) {
            $filesToHash += (Resolve-Path $sc).Path
        }
    }

    foreach ($f in ($filesToHash | Select-Object -Unique)) {
        $h = Get-FileHash -Path $f -Algorithm SHA256
        $fileName = Split-Path -Leaf $f
        $entry = "$($h.Hash)  dist\$fileName"
        Write-Host "  $entry" -ForegroundColor White
        $newEntries += $entry
    }

    # Merge entries cleanly without duplicates
    $allLines = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $existingLines) {
        $isReplaced = $false
        foreach ($ne in $newEntries) {
            $namePart = ($ne -split "\s+")[1]
            if ($line -like "*$namePart*") {
                $isReplaced = $true
                break
            }
        }
        if (-not $isReplaced -and $line.Trim()) {
            $allLines.Add($line)
        }
    }
    foreach ($ne in $newEntries) {
        $allLines.Add($ne)
    }

    [System.IO.File]::WriteAllLines($sumsPath, $allLines, [System.Text.Encoding]::UTF8)
    Write-Host "[+] Updated: $sumsPath" -ForegroundColor Green
}

# 7. Comprehensive Packaging Size Audit Report
$reportScript = Join-Path $ScriptDir "report-package-sizes.ps1"
if (Test-Path $reportScript) {
    & $reportScript -Version $Version -RepoRoot $RepoRoot -DistDir $OutputDir
}

Write-Host "`n=================================================================" -ForegroundColor Cyan
Write-Host "   Release Packaging Completed Successfully!                     " -ForegroundColor Green
Write-Host "=================================================================" -ForegroundColor Cyan
