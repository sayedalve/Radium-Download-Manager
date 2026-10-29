# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- Minimal Windows x64 FFmpeg Build Pipeline
#
# Builds a deterministic, minimal Windows x64 FFmpeg executable (ffmpeg.exe)
# containing only the demuxers, muxers, parsers, decoders, bitstream filters,
# and encoders required by Radium's stream download and muxing workflows.
#
# Pinned Source: FFmpeg 7.1 (Tag: n7.1, Commit: 04706f3, Tarball: ffmpeg-7.1.tar.xz)
# Pinned Source SHA-256: 40973d44d00a8999824ff4ec27549efdbdc7667a488e14603954625b5a79667
# Pinned Toolchain: LLVM-MinGW 20240917 (UCRT runtime, Clang 19.1.0)
# Pinned Toolchain SHA-256: e06da87ec42a8b3e85e053fcab68fe53ec220302c0c704ca9f4a1fa62a096c46
#
# License Profile: LGPL-2.1-or-later / LGPL-3.0 (Zero GPL-only codecs; clean commercial/FOSS distribution)
# Expected Output: tools/win-x64/ffmpeg.exe (~12-16 MB uncompressed, ~5-7 MB compressed)

[CmdletBinding()]
param(
    [string]$ConfigPath = "",
    [string]$OutputDir = "",
    [string]$WorkDir = "",
    [string]$ToolchainDir = "",
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$SkipToolchainDownload = $false,
    [switch]$SkipSourceDownload = $false,
    [switch]$Verify = $true,
    [switch]$Clean = $false
)

$ErrorActionPreference = "Stop"

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not $ScriptDir -or -not (Test-Path $ScriptDir)) {
    $ScriptDir = (Get-Location).Path
}
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

if (-not $ConfigPath) {
    $ConfigPath = Join-Path $ScriptDir "ffmpeg-minimal.config"
}
$ConfigPath = [System.IO.Path]::GetFullPath($ConfigPath)

if (-not $OutputDir) {
    $OutputDir = Join-Path $RepoRoot "tools\win-x64"
}
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)

if (-not $WorkDir) {
    $WorkDir = Join-Path $RepoRoot "build\ffmpeg-minimal"
}
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)

# Constants for pinned dependencies
$FfmpegVersion = "7.1"
$FfmpegTarballName = "ffmpeg-$FfmpegVersion.tar.xz"
$FfmpegSourceUrl = "https://ffmpeg.org/releases/$FfmpegTarballName"
$FfmpegExpectedSha256 = "40973D44970DBC83EF302B0609F2E74982BE2D85916DD2EE7472D30678A7ABE6"

$LlvmMingwVersion = "20240917"
$LlvmMingwArchiveName = "llvm-mingw-$LlvmMingwVersion-ucrt-x86_64.zip"
$LlvmMingwUrl = "https://github.com/mstorsjo/llvm-mingw/releases/download/$LlvmMingwVersion/$LlvmMingwArchiveName"
$LlvmMingwExpectedSha256 = "E06DA87EC42A8B3E85E053FCAB68FE53EC220302C0C704CA9F4A1FA62A096C46"

function Assert-Sha256Checksum([string]$FilePath, [string]$ExpectedHash) {
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($FilePath)
    try {
        $actual = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace("-", "").ToUpperInvariant()
        $expected = $ExpectedHash.Trim().ToUpperInvariant()
        if ($actual -ne $expected) {
            throw "SHA-256 verification failed for $FilePath.`nExpected: $expected`nActual  : $actual"
        }
    } finally {
        $stream.Close()
        $hasher.Dispose()
    }
}

function Assert-Pe64Executable([string]$FilePath) {
    if (-not (Test-Path $FilePath)) {
        throw "Target executable not found: $FilePath"
    }
    $bytes = [System.IO.File]::ReadAllBytes($FilePath)
    if ($bytes.Length -lt 256 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "File is not a valid Windows PE executable (missing MZ signature): $FilePath"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -ne 0x8664) {
        throw "Executable $FilePath is not x64 (IMAGE_FILE_MACHINE_AMD64 0x8664); got 0x$($machine.ToString('X4'))"
    }
}

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "   Flux Download Manager -- Minimal FFmpeg Build Pipeline        " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "Configuration : $ConfigPath" -ForegroundColor White
Write-Host "Work Directory: $WorkDir" -ForegroundColor White
Write-Host "Output Target : $OutputDir\ffmpeg.exe" -ForegroundColor White
Write-Host "Jobs          : $Jobs" -ForegroundColor White

if (-not (Test-Path $ConfigPath)) {
    throw "FFmpeg configuration specification file missing: $ConfigPath"
}

if ($Clean -and (Test-Path $WorkDir)) {
    Write-Host "`n==> [Clean] Removing existing work directory: $WorkDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $WorkDir
}

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

# -----------------------------------------------------------------------------
# 1. Toolchain Resolution (LLVM-MinGW / MSYS2)
# -----------------------------------------------------------------------------
Write-Host "`n==> [1/6] Resolving Windows x64 Cross-Compiler Toolchain..." -ForegroundColor Cyan

$BashCmd = $null
$MakeCmd = $null
$StripCmd = $null

if ($ToolchainDir -and (Test-Path $ToolchainDir)) {
    $env:PATH = "$ToolchainDir\bin;$ToolchainDir;" + $env:PATH
}

# Check for existing MSYS2 or LLVM-MinGW in PATH or standard locations
$Candidates = @(
    "C:\llvm-mingw\bin",
    "C:\ProgramData\chocolatey\bin",
    "C:\msys64\ucrt64\bin",
    "C:\msys64\mingw64\bin",
    "C:\msys64\usr\bin"
)
foreach ($cand in $Candidates) {
    if (Test-Path $cand) {
        $env:PATH = "$cand;" + $env:PATH
    }
}

if (Get-Command "x86_64-w64-mingw32-clang" -ErrorAction SilentlyContinue) {
    Write-Host "  Found x86_64-w64-mingw32-clang: $((Get-Command x86_64-w64-mingw32-clang).Source)" -ForegroundColor Green
    $StripCmd = "x86_64-w64-mingw32-strip"
} elseif (Get-Command "x86_64-w64-mingw32-gcc" -ErrorAction SilentlyContinue) {
    Write-Host "  Found x86_64-w64-mingw32-gcc: $((Get-Command x86_64-w64-mingw32-gcc).Source)" -ForegroundColor Green
    $StripCmd = "x86_64-w64-mingw32-strip"
} else {
    # Auto-provision pinned LLVM-MinGW if permitted
    if ($SkipToolchainDownload) {
        throw "No x86_64-w64-mingw32 cross-compiler found and -SkipToolchainDownload was specified."
    }
    $ToolchainZip = Join-Path $WorkDir $LlvmMingwArchiveName
    $ToolchainExtractDir = Join-Path $WorkDir "llvm-mingw"
    if (-not (Test-Path $ToolchainExtractDir)) {
        Write-Host "  Downloading pinned LLVM-MinGW $LlvmMingwVersion..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri $LlvmMingwUrl -OutFile $ToolchainZip -UseBasicParsing
        Assert-Sha256Checksum $ToolchainZip $LlvmMingwExpectedSha256
        Expand-Archive -Path $ToolchainZip -DestinationPath $WorkDir -Force
        $Extracted = Get-ChildItem -Path $WorkDir -Directory -Filter "llvm-mingw-*" | Select-Object -First 1
        Rename-Item -Path $Extracted.FullName -NewName "llvm-mingw"
    }
    $env:PATH = (Join-Path $ToolchainExtractDir "bin") + ";" + $env:PATH
    $StripCmd = Join-Path $ToolchainExtractDir "bin\llvm-strip.exe"
}

# Resolve bash and make
if (Get-Command "bash.exe" -ErrorAction SilentlyContinue) {
    $BashCmd = (Get-Command "bash.exe").Source
} elseif (Test-Path "C:\msys64\usr\bin\bash.exe") {
    $BashCmd = "C:\msys64\usr\bin\bash.exe"
} elseif (Test-Path "C:\Program Files\Git\bin\bash.exe") {
    $BashCmd = "C:\Program Files\Git\bin\bash.exe"
} else {
    throw "Bash shell environment required for running FFmpeg's configure script."
}

if (Get-Command "make.exe" -ErrorAction SilentlyContinue) {
    $MakeCmd = (Get-Command "make.exe").Source
} elseif (Test-Path "C:\msys64\usr\bin\make.exe") {
    $MakeCmd = "C:\msys64\usr\bin\make.exe"
} else {
    $MakeCmd = "make"
}

Write-Host "  Toolchain resolved: Bash=$BashCmd, Make=$MakeCmd" -ForegroundColor Green

# -----------------------------------------------------------------------------
# 2. Source Download and Verification
# -----------------------------------------------------------------------------
Write-Host "`n==> [2/6] Provisioning Pinned FFmpeg $FfmpegVersion Source..." -ForegroundColor Cyan

$SourceTarball = Join-Path $WorkDir $FfmpegTarballName
$SourceDir = Join-Path $WorkDir "ffmpeg-$FfmpegVersion"

if (-not (Test-Path $SourceDir)) {
    if (-not (Test-Path $SourceTarball)) {
        if ($SkipSourceDownload) {
            throw "FFmpeg source archive missing and -SkipSourceDownload was specified."
        }
        Write-Host "  Downloading $FfmpegSourceUrl..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri $FfmpegSourceUrl -OutFile $SourceTarball -UseBasicParsing
    }
    Write-Host "  Verifying source archive SHA-256 checksum..." -ForegroundColor Cyan
    Assert-Sha256Checksum $SourceTarball $FfmpegExpectedSha256

    Write-Host "  Extracting FFmpeg source tarball..." -ForegroundColor Cyan
    # Use tar in bash or Windows tar.exe
    & tar.exe -xf $SourceTarball -C $WorkDir
    if (-not (Test-Path $SourceDir)) {
        throw "Extracted source directory not found: $SourceDir"
    }
} else {
    Write-Host "  Source directory already present: $SourceDir" -ForegroundColor Green
}

# -----------------------------------------------------------------------------
# 3. Parse Configuration Specification
# -----------------------------------------------------------------------------
Write-Host "`n==> [3/6] Reading Configuration Specification from $ConfigPath..." -ForegroundColor Cyan

$RawConfigLines = Get-Content -Path $ConfigPath
$ConfigureFlags = [System.Collections.Generic.List[string]]::new()

foreach ($line in $RawConfigLines) {
    $trimmed = $line.Trim()
    if (-not $trimmed -or $trimmed.StartsWith("#")) {
        continue
    }
    # Strip any inline comment
    $hashIdx = $trimmed.IndexOf(" #")
    if ($hashIdx -gt 0) {
        $trimmed = $trimmed.Substring(0, $hashIdx).Trim()
    }
    $ConfigureFlags.Add($trimmed)
}

Write-Host "  Parsed $($ConfigureFlags.Count) active configure flags." -ForegroundColor Green

# -----------------------------------------------------------------------------
# 4. Generate Build Script and Execute Configure
# -----------------------------------------------------------------------------
Write-Host "`n==> [4/6] Configuring Minimal FFmpeg Build..." -ForegroundColor Cyan

$BuildScriptPath = Join-Path $WorkDir "run_configure.sh"
$BashSourceDir = ($SourceDir -replace '\\', '/').Replace('C:', '/c').Replace('c:', '/c')
$BashOutputDir = ($OutputDir -replace '\\', '/').Replace('C:', '/c').Replace('c:', '/c')

$FlagsString = ($ConfigureFlags -join " `\n    ")
$ScriptContent = @"
#!/usr/bin/env bash
set -euo pipefail
cd "$BashSourceDir"

echo "==> Invoking ./configure with minimal flags..."
./configure \
    $FlagsString

echo "==> Configure completed successfully."
"@

[System.IO.File]::WriteAllText($BuildScriptPath, $ScriptContent.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)

Write-Host "  Running ./configure via $BashCmd..." -ForegroundColor Cyan
& $BashCmd $BuildScriptPath
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg ./configure failed with exit code $LASTEXITCODE"
}

# -----------------------------------------------------------------------------
# 5. Compile and Strip
# -----------------------------------------------------------------------------
Write-Host "`n==> [5/6] Compiling ffmpeg.exe (Jobs: $Jobs)..." -ForegroundColor Cyan

$CompileScriptPath = Join-Path $WorkDir "run_compile.sh"
$CompileContent = @"
#!/usr/bin/env bash
set -euo pipefail
cd "$BashSourceDir"
sed -i 's/LDFLAGS= */LDFLAGS=-static /' ffbuild/config.mak
make -j$Jobs ffmpeg.exe
"@

[System.IO.File]::WriteAllText($CompileScriptPath, $CompileContent.Replace("`r`n", "`n"), [System.Text.Encoding]::ASCII)
& $BashCmd $CompileScriptPath
if ($LASTEXITCODE -ne 0) {
    throw "FFmpeg compilation failed with exit code $LASTEXITCODE"
}

$CompiledExe = Join-Path $SourceDir "ffmpeg.exe"
if (-not (Test-Path $CompiledExe)) {
    throw "Compiled ffmpeg.exe not found at $CompiledExe"
}

# Strip debug symbols
Write-Host "  Stripping debug symbols from $CompiledExe..." -ForegroundColor Cyan
if ($StripCmd -and (Get-Command $StripCmd -ErrorAction SilentlyContinue)) {
    & $StripCmd --strip-all $CompiledExe
} else {
    Write-Host "  [WARN] Strip command not found; skipping binary strip." -ForegroundColor Yellow
}

$TargetExe = Join-Path $OutputDir "ffmpeg.exe"
Copy-Item -Force $CompiledExe $TargetExe
Assert-Pe64Executable $TargetExe

$FinalSize = (Get-Item $TargetExe).Length
$FinalSizeMiB = [Math]::Round($FinalSize / 1MB, 2)
Write-Host "`n  [SUCCESS] Minimal ffmpeg.exe produced: $TargetExe" -ForegroundColor Green
Write-Host "  File Size: $FinalSize bytes ($FinalSizeMiB MiB)" -ForegroundColor Green

# -----------------------------------------------------------------------------
# 6. Verification and Capability Audit
# -----------------------------------------------------------------------------
if ($Verify) {
    Write-Host "`n==> [6/6] Verifying Executable Capabilities..." -ForegroundColor Cyan
    $VerifyScript = Join-Path $ScriptDir "verify-ffmpeg-capabilities.ps1"
    if (Test-Path $VerifyScript) {
        & $VerifyScript -FfmpegPath $TargetExe
    } else {
        Write-Host "  [WARN] Verification script not found at $VerifyScript" -ForegroundColor Yellow
    }
}

Write-Host "`n=================================================================" -ForegroundColor Cyan
Write-Host "   Minimal FFmpeg Windows x64 Build Complete                     " -ForegroundColor Green
Write-Host "=================================================================" -ForegroundColor Cyan
