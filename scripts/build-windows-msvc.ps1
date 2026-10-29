# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- Reproducible Windows 11 x64 Portable Build, PE32+ Verification & Test Pipeline
# Detects MSVC / Windows SDK / CMake, compiles flux.exe (WIN32 GUI subsystem),
# flux-nmh.exe (CUI subsystem), and flux_unit_tests.exe, validates PE32+ x64 headers,
# provisions stream workers, runs automated unit/extension/integration tests, and stages
# the portable application at dist/Radium.exe with the loadable browser extension in extension/.
# Note: Does NOT create an installer, setup executable, or MSI by default (Release Requirement 6).

param(
    [switch]$Clean = $false,
    [switch]$Incremental = $false,
    [switch]$SkipWorkerFetch = $false,
    [switch]$SkipTests = $false,
    [switch]$SkipInstaller = $true
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = Join-Path $RepoRoot "build\win-x64"
$ToolsWinDir = Join-Path $RepoRoot "tools\win-x64"
$DistDir = Join-Path $RepoRoot "dist"

if ($Clean) {
    Write-Host "`n==> [Clean] Resetting build directory for full clean build: $BuildDir..."
    if (Test-Path $BuildDir) {
        Remove-Item -Recurse -Force $BuildDir
    }
}

Write-Host "================================================================="
Write-Host "   Radium Download Manager v1.0.0 Windows 11 x64 Portable Pipeline "
Write-Host "================================================================="

# -----------------------------------------------------------------------------
# 1. Toolchain and Environment Detection
# -----------------------------------------------------------------------------
Write-Host "`n==> [1/7] Detecting Compiler, Windows SDK, CMake, and Build Tools..."

$CMakeCmd = $null
if (Get-Command "cmake.exe" -ErrorAction SilentlyContinue) {
    $CMakeCmd = (Get-Command "cmake.exe").Source
} else {
    $vsCMakeCandidates = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\CMake\bin\cmake.exe"
    )
    foreach ($cand in $vsCMakeCandidates) {
        if (Test-Path $cand) {
            $CMakeCmd = $cand
            $env:PATH = (Split-Path $cand) + ";" + $env:PATH
            break
        }
    }
}
if (-not $CMakeCmd) {
    throw "CMake not found! Install Visual Studio 2022/2026 C++ CMake tools or CMake 3.25+."
}
Write-Host "  [+] CMake: $CMakeCmd"

$ConfigureArgs = @("-S", $RepoRoot, "-B", $BuildDir, "-A", "x64", "-DCMAKE_BUILD_TYPE=Release")
if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    $helpText = & $CMakeCmd --help
    if (($helpText -join "`n") -match '\*\s+(Visual Studio \d+ \d+)') {
        $Generator = $Matches[1]
        $ConfigureArgs += @("-G", $Generator)
        Write-Host "  [+] Generator: $Generator (x64)"
    }
} else {
    Write-Host "  [+] Using existing CMakeCache.txt configuration"
}

# -----------------------------------------------------------------------------
# 2. Configure CMake Build
# -----------------------------------------------------------------------------
Write-Host "`n==> [2/7] Configuring CMake Release build in $BuildDir..."
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
& $CMakeCmd @ConfigureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed with exit code $LASTEXITCODE" }

# -----------------------------------------------------------------------------
# 3. Compile Windows x64 Targets
# -----------------------------------------------------------------------------
Write-Host "`n==> [3/7] Compiling Windows x64 targets (radium, radium-nmh, radium_unit_tests, N_m3u8DL-RE)..."
$BuildTargets = @("sqlite3", "radium", "radium-nmh", "N_m3u8DL-RE", "radium_unit_tests")
foreach ($bt in $BuildTargets) {
    & $CMakeCmd --build $BuildDir --config Release --target $bt
    if ($LASTEXITCODE -ne 0) { throw "CMake compilation of target $bt failed with exit code $LASTEXITCODE" }
}

$BuiltDir = $BuildDir
if (Test-Path (Join-Path $BuildDir "Release\radium.exe")) {
    $BuiltDir = Join-Path $BuildDir "Release"
}

# Create compatibility aliases from primary Radium binaries
Copy-Item -Force (Join-Path $BuiltDir "radium.exe") (Join-Path $BuiltDir "flux.exe")
Copy-Item -Force (Join-Path $BuiltDir "radium.exe") (Join-Path $BuiltDir "openidm.exe")
Copy-Item -Force (Join-Path $BuiltDir "radium-nmh.exe") (Join-Path $BuiltDir "flux-nmh.exe")
Copy-Item -Force (Join-Path $BuiltDir "radium-nmh.exe") (Join-Path $BuiltDir "openidm-nmh.exe")
Copy-Item -Force (Join-Path $BuiltDir "radium_unit_tests.exe") (Join-Path $BuiltDir "flux_unit_tests.exe")
Copy-Item -Force (Join-Path $BuiltDir "radium_unit_tests.exe") (Join-Path $BuiltDir "openidm_unit_tests.exe")

$Targets = @("radium.exe", "radium-nmh.exe", "radium_unit_tests.exe", "flux.exe", "flux-nmh.exe", "flux_unit_tests.exe", "N_m3u8DL-RE.exe")
foreach ($t in $Targets) {
    $srcPath = Join-Path $BuiltDir $t
    Copy-Item -Force $srcPath (Join-Path $BuildDir $t)
    Copy-Item -Force $srcPath (Join-Path $RepoRoot "build\$t")
}
Copy-Item -Force (Join-Path $BuiltDir "openidm.exe") (Join-Path $BuildDir "openidm.exe")
Copy-Item -Force (Join-Path $BuiltDir "openidm-nmh.exe") (Join-Path $BuildDir "openidm-nmh.exe")
Copy-Item -Force (Join-Path $BuiltDir "openidm_unit_tests.exe") (Join-Path $BuildDir "openidm_unit_tests.exe")

# -----------------------------------------------------------------------------
# 4. Verify PE32+ Headers and Subsystems
# -----------------------------------------------------------------------------
Write-Host "`n==> [4/7] Verifying PE32+ AMD64 signatures and subsystem types..."

function Verify-PeBinary([string]$FilePath, [int]$ExpectedSubsystem, [string]$SubsystemName) {
    if (-not (Test-Path $FilePath)) {
        throw "Verification failed: missing binary $FilePath"
    }
    $bytes = [System.IO.File]::ReadAllBytes($FilePath)
    if ($bytes.Length -lt 256 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Invalid MZ DOS signature in $FilePath"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    $peSig = [BitConverter]::ToUInt32($bytes, $peOffset)
    if ($peSig -ne 0x00004550) {
        throw "Invalid PE signature in $FilePath"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -ne 0x8664) {
        throw "Binary is not x64 AMD64: $FilePath"
    }
    $optOffset = $peOffset + 24
    $magic = [BitConverter]::ToUInt16($bytes, $optOffset)
    if ($magic -ne 0x20B) {
        throw "Binary is not PE32+: $FilePath"
    }
    $subsystem = [BitConverter]::ToUInt16($bytes, $optOffset + 68)
    if ($subsystem -ne $ExpectedSubsystem) {
        throw "Binary subsystem mismatch in $FilePath"
    }
    $sizeKb = [Math]::Round($bytes.Length / 1024.0, 1)
    Write-Host "  [+] Verified $FilePath -> AMD64 PE32+ Subsystem=$subsystem ($SubsystemName), Size=${sizeKb} KB"
}

Verify-PeBinary (Join-Path $BuildDir "radium.exe") 2 "IMAGE_SUBSYSTEM_WINDOWS_GUI"
Verify-PeBinary (Join-Path $BuildDir "radium-nmh.exe") 3 "IMAGE_SUBSYSTEM_WINDOWS_CUI"
Verify-PeBinary (Join-Path $BuildDir "radium_unit_tests.exe") 3 "IMAGE_SUBSYSTEM_WINDOWS_CUI"
Verify-PeBinary (Join-Path $BuildDir "N_m3u8DL-RE.exe") 3 "IMAGE_SUBSYSTEM_WINDOWS_CUI"

# -----------------------------------------------------------------------------
# 5. Provision and Verify Upstream Stream Workers
# -----------------------------------------------------------------------------
Write-Host "`n==> [5/7] Provisioning and verifying upstream stream workers..."
New-Item -ItemType Directory -Force -Path $ToolsWinDir | Out-Null
$NmExe = Join-Path $ToolsWinDir "N_m3u8DL-RE.exe"
$FfExe = Join-Path $ToolsWinDir "ffmpeg.exe"
$YtExe = Join-Path $ToolsWinDir "yt-dlp.exe"

if (-not $SkipWorkerFetch -and (-not (Test-Path $NmExe) -or -not (Test-Path $FfExe) -or -not (Test-Path $YtExe))) {
    & (Join-Path $PSScriptRoot "fetch-workers.ps1") -TargetDir $ToolsWinDir
}

if (-not (Test-Path $NmExe)) {
    throw "Upstream N_m3u8DL-RE.exe missing from $ToolsWinDir!"
}
if (-not (Test-Path $FfExe)) {
    throw "FFmpeg.exe missing from $ToolsWinDir!"
}
if (-not (Test-Path $YtExe)) {
    throw "yt-dlp.exe missing from $ToolsWinDir!"
}

Verify-PeBinary $NmExe 3 "IMAGE_SUBSYSTEM_WINDOWS_CUI"
Verify-PeBinary $FfExe 3 "IMAGE_SUBSYSTEM_WINDOWS_CUI"
Verify-PeBinary $YtExe 3 "IMAGE_SUBSYSTEM_WINDOWS_CUI"

# Stream workers are provisioned exclusively in tools\win-x64 (standard location)
# Note: Redundant copying to repo tools\ root is eliminated to prevent duplicate disk footprint.

# -----------------------------------------------------------------------------
# 6. Stage Portable Distribution at dist/Radium.exe (No Installer / No MSI)
# -----------------------------------------------------------------------------
Write-Host "`n==> [6/7] Staging Portable Distribution at dist/Radium.exe..."
Get-Process | Where-Object {
    try { $_.Path -and $_.Path.StartsWith($DistDir, [System.StringComparison]::OrdinalIgnoreCase) } catch { $false }
} | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 200

# Ensure single standard tools location tools\win-x64 in dist
$DistToolsWinDir = Join-Path $DistDir "tools\win-x64"
New-Item -ItemType Directory -Force -Path $DistToolsWinDir | Out-Null

# Clean up legacy duplicate files from older builds if present
$legacyDupFiles = @(
    (Join-Path $DistDir "tools\ffmpeg.exe"),
    (Join-Path $DistDir "tools\ffprobe.exe"),
    (Join-Path $DistDir "tools\N_m3u8DL-RE.exe"),
    (Join-Path $DistDir "tools\yt-dlp.exe"),
    (Join-Path $DistDir "N_m3u8DL-RE.exe")
)
foreach ($dup in $legacyDupFiles) {
    if (Test-Path $dup) {
        Remove-Item -Force $dup -ErrorAction SilentlyContinue
    }
}

Copy-Item -Force (Join-Path $BuildDir "radium.exe") (Join-Path $DistDir "Radium.exe")
Copy-Item -Force (Join-Path $BuildDir "radium.exe") (Join-Path $DistDir "radium.exe")
Copy-Item -Force (Join-Path $BuildDir "radium-nmh.exe") (Join-Path $DistDir "radium-nmh.exe")

# Ensure legacy Flux binaries and packages are removed from dist
$legacyDistFiles = @(
    (Join-Path $DistDir "Flux.exe"),
    (Join-Path $DistDir "flux.exe"),
    (Join-Path $DistDir "flux-nmh.exe"),
    (Join-Path $DistDir "openidm.exe"),
    (Join-Path $DistDir "openidm-nmh.exe"),
    (Join-Path $DistDir "Flux-Setup-x64-0.3.0.exe"),
    (Join-Path $DistDir "Flux-Setup-x64-Core-0.3.0.exe"),
    (Join-Path $DistDir "Flux-v0.3.0-win-x64-portable.zip"),
    (Join-Path $DistDir "Flux-v0.3.0-Windows-x64-Core.zip"),
    (Join-Path $DistDir "Flux-v0.3.0-Windows-x64-Full.zip"),
    (Join-Path $DistDir "Flux-v0.3.0-Windows-x64.zip")
)
foreach ($f in $legacyDistFiles) {
    if (Test-Path $f) { Remove-Item -Force $f -ErrorAction SilentlyContinue }
}
# Prioritize native C++ stream worker (< 5MB) from build output if available
$builtWorker = Join-Path $BuildDir "N_m3u8DL-RE.exe"
if (-not (Test-Path $builtWorker)) {
    $builtWorker = Join-Path $BuildDir "Release\N_m3u8DL-RE.exe"
}
if (Test-Path $builtWorker) {
    Write-Host "  Staged native C++ stream worker: $builtWorker" -ForegroundColor Green
    Copy-Item -Force $builtWorker (Join-Path $DistToolsWinDir "N_m3u8DL-RE.exe")
} elseif (Test-Path (Join-Path $ToolsWinDir "N_m3u8DL-RE.exe")) {
    Copy-Item -Force (Join-Path $ToolsWinDir "N_m3u8DL-RE.exe") (Join-Path $DistToolsWinDir "N_m3u8DL-RE.exe")
}
Copy-Item -Force (Join-Path $ToolsWinDir "ffmpeg.exe") (Join-Path $DistToolsWinDir "ffmpeg.exe")
Copy-Item -Force (Join-Path $ToolsWinDir "yt-dlp.exe") (Join-Path $DistToolsWinDir "yt-dlp.exe")

# Clean browser extension staging (strictly excludes development/test files)
$extSrc = Join-Path $RepoRoot "extension"
$extDest = Join-Path $DistDir "extension"
New-Item -ItemType Directory -Force -Path $extDest | Out-Null
$excludeItems = @(".git", ".github", "node_modules", "package.json", "package-lock.json", "tsconfig.json", "tests", "scratch", "*.ts", "*.map", "*.log")
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

$CheckFiles = @(
    (Join-Path $DistDir "Radium.exe"),
    (Join-Path $DistDir "radium-nmh.exe"),
    (Join-Path $DistDir "tools\win-x64\N_m3u8DL-RE.exe"),
    (Join-Path $DistDir "tools\win-x64\ffmpeg.exe"),
    (Join-Path $DistDir "tools\win-x64\yt-dlp.exe")
)

$ChecksumReport = Join-Path $DistDir "SHA256SUMS.txt"
$hashLines = @()
foreach ($f in $CheckFiles) {
    if (Test-Path $f) {
        $h = Get-FileHash -Algorithm SHA256 $f
        $rel = $f
        if ($f.StartsWith($RepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            $rel = $f.Substring($RepoRoot.Length).TrimStart('\', '/')
        }
        $line = "$($h.Hash)  $rel"
        $hashLines += $line
        Write-Host "  $line"
    }
}
$hashLines | Out-File -Encoding utf8 -FilePath $ChecksumReport

# -----------------------------------------------------------------------------
# 7. Test Suite Execution
# -----------------------------------------------------------------------------
if (-not $SkipTests) {
    Write-Host "`n==> [7/7] Executing Test Suites..."

    Write-Host "  --> [A] C++20 Unit and Regression Tests..."
    & (Join-Path $BuildDir "radium_unit_tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "C++20 unit test suite failed!" }

    if (Get-Command "node" -ErrorAction SilentlyContinue) {
        Write-Host "  --> [B] Browser Extension Media Detection Tests..."
        node (Join-Path $RepoRoot "tests\test_extension_detection.js")
        if ($LASTEXITCODE -ne 0) { throw "Browser extension test suite failed!" }
    } else {
        Write-Warning "node.exe not found in PATH -- skipped extension tests."
    }

    if (Get-Command "python" -ErrorAction SilentlyContinue) {
        Write-Host "  --> [C] Deterministic Pipeline & Integration Suite..."
        python (Join-Path $RepoRoot "tests\test_repair_pipeline_e2e.py")
        if ($LASTEXITCODE -ne 0) { throw "Deterministic repair pipeline test suite failed!" }
    } else {
        Write-Warning "python.exe not found in PATH -- skipped integration tests."
    }
} else {
    Write-Host "`n==> [7/7] Tests skipped as requested (-SkipTests)."
}

Write-Host "`n================================================================="
Write-Host "   Windows 11 x64 Portable Build & Verification Completed        "
Write-Host "   Portable App : $DistDir\Radium.exe                              "
Write-Host "   Native Host  : $DistDir\radium-nmh.exe                          "
Write-Host "   Extension Dir: $RepoRoot\extension                            "
Write-Host "   Register NMH : .\scripts\register-native-messaging.ps1        "
Write-Host "=================================================================`n"
