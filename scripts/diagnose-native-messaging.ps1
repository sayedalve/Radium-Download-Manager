# SPDX-License-Identifier: GPL-3.0-or-later
# OpenIDM / Flux - Read-Only Diagnostic Utility for Native Messaging and Browser Registration
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\scripts\diagnose-native-messaging.ps1
#
# Inspects Brave, Chrome, Edge registrations, verifies manifest integrity, checks binary existence,
# derives extension IDs (both RSA public key and unpacked path), and compares against allowed_origins.
#
# STRICTLY READ-ONLY: Never modifies registry, filesystem, or processes.

[CmdletBinding()]
param()

$ErrorActionPreference = "Continue"

Write-Host ""
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "  Radium Download Manager - Native Messaging Diagnostic Utility " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host ""

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir

$Findings = [System.Collections.Generic.List[PSCustomObject]]::new()

function Add-Finding {
    param(
        [string]$Category,
        [string]$Check,
        [string]$Status,
        [string]$Details
    )
    $color = switch ($Status) {
        "PASS" { "Green" }
        "FAIL" { "Red" }
        "WARN" { "Yellow" }
        default { "Gray" }
    }
    Write-Host ("[{0,-4}] {1,-32} : {2}" -f $Status, $Check, $Details) -ForegroundColor $color
    $Findings.Add([PSCustomObject]@{
        Category = $Category
        Check    = $Check
        Status   = $Status
        Details  = $Details
    })
}

# -----------------------------------------------------------------------------
# 1. Extension Manifest and ID Derivation
# -----------------------------------------------------------------------------
Write-Host "--- 1. Extension Manifest and ID Derivation ---" -ForegroundColor Yellow

$manifestPath = Join-Path $RepoRoot "extension\manifest.json"
$keyDerivedId = $null

if (Test-Path $manifestPath) {
    Add-Finding -Category "Extension" -Check "extension/manifest.json" -Status "PASS" -Details "File exists"
    try {
        $mJson = Get-Content $manifestPath -Raw | ConvertFrom-Json
        if ($mJson.key) {
            $keyBytes = [System.Convert]::FromBase64String($mJson.key)
            $sha256 = [System.Security.Cryptography.SHA256]::Create()
            $hash = $sha256.ComputeHash($keyBytes)
            $derivedId = ""
            for ($i = 0; $i -lt 16; $i++) {
                $b = $hash[$i]
                $derivedId += [char](97 + [int](($b -shr 4) -band 0x0F))
                $derivedId += [char](97 + [int]($b -band 0x0F))
            }
            $keyDerivedId = $derivedId
            Add-Finding -Category "Extension" -Check "RSA Public Key Derivation" -Status "PASS" -Details "ID = $keyDerivedId"
        } else {
            Add-Finding -Category "Extension" -Check "RSA Public Key Derivation" -Status "WARN" -Details "No 'key' field found in manifest.json"
        }
    } catch {
        Add-Finding -Category "Extension" -Check "manifest.json Parsing" -Status "FAIL" -Details "JSON parse error: $_"
    }
} else {
    Add-Finding -Category "Extension" -Check "extension/manifest.json" -Status "FAIL" -Details "File not found at $manifestPath"
}

# Derive Path-based Unpacked Extension ID (Chromium UTF-16LE SHA-256 of absolute folder path)
$extDir = Join-Path $RepoRoot "extension"
$pathDerivedId = $null
if (Test-Path $extDir) {
    try {
        $pathBytes = [System.Text.Encoding]::Unicode.GetBytes($extDir)
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        $hash = $sha256.ComputeHash($pathBytes)
        $pathHashId = ""
        for ($i = 0; $i -lt 16; $i++) {
            $b = $hash[$i]
            $pathHashId += [char](97 + [int](($b -shr 4) -band 0x0F))
            $pathHashId += [char](97 + [int]($b -band 0x0F))
        }
        $pathDerivedId = $pathHashId
        Add-Finding -Category "Extension" -Check "Unpacked Path ID Derivation" -Status "PASS" -Details "ID = $pathDerivedId (from $extDir)"
    } catch {
        Add-Finding -Category "Extension" -Check "Unpacked Path ID Derivation" -Status "WARN" -Details "Could not compute path ID: $_"
    }
}

# -----------------------------------------------------------------------------
# 2. Installed Browser Extension Detection (Brave, Chrome, Edge)
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "--- 2. Installed Extension in Browsers ---" -ForegroundColor Yellow

$browserProfiles = @(
    @{ Name = "Brave"; Path = "$env:LOCALAPPDATA\BraveSoftware\Brave-Browser\User Data\Default\Secure Preferences" },
    @{ Name = "Chrome"; Path = "$env:LOCALAPPDATA\Google\Chrome\User Data\Default\Secure Preferences" },
    @{ Name = "Edge"; Path = "$env:LOCALAPPDATA\Microsoft\Edge\User Data\Default\Secure Preferences" }
)

$detectedBrowserExtIds = [System.Collections.Generic.List[string]]::new()

foreach ($bp in $browserProfiles) {
    if (Test-Path $bp.Path) {
        try {
            $pref = Get-Content -Raw $bp.Path | ConvertFrom-Json
            $extSettings = $pref.extensions.settings
            $foundForBrowser = $false
            if ($extSettings) {
                $extSettings.PSObject.Properties | ForEach-Object {
                    $extObj = $_.Value
                    $extPath = $extObj.path
                    $extName = $extObj.manifest.name
                    if ($extPath -match "radium|openidm|flux" -or $extName -match "Radium|Flux|OpenIDM") {
                        $foundForBrowser = $true
                        $instId = $_.Name
                        if (-not $detectedBrowserExtIds.Contains($instId)) {
                            $detectedBrowserExtIds.Add($instId)
                        }
                        Add-Finding -Category "Browser ($($bp.Name))" -Check "Installed Extension Found" -Status "PASS" -Details "ID = $instId (Path: $extPath)"
                    }
                }
            }
            if (-not $foundForBrowser) {
                Add-Finding -Category "Browser ($($bp.Name))" -Check "Installed Extension" -Status "WARN" -Details "No Radium/Flux/OpenIDM extension found in $($bp.Name) profile"
            }
        } catch {
            Add-Finding -Category "Browser ($($bp.Name))" -Check "Secure Preferences" -Status "WARN" -Details "Could not parse Secure Preferences: $_"
        }
    } else {
        Add-Finding -Category "Browser ($($bp.Name))" -Check "Profile Preferences" -Status "UNKNOWN" -Details "Path not found: $($bp.Path)"
    }
}

# -----------------------------------------------------------------------------
# 3. Desktop Application and IPC Named Pipe Status
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "--- 3. Desktop App and Named Pipe IPC Status ---" -ForegroundColor Yellow

$userSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
Add-Finding -Category "IPC" -Check "Current User SID" -Status "PASS" -Details "SID = $userSid"

$expectedPipeName = "Flux_IPC_$userSid"
$pipePath = "\\.\pipe\$expectedPipeName"

$activePipes = [System.IO.Directory]::GetFiles("\\.\\pipe\\") | Where-Object { $_ -match "Radium|Flux_IPC|openidm" }
if ($activePipes) {
    $matchedPipe = $activePipes | Where-Object { $_ -match [regex]::Escape($expectedPipeName) }
    if ($matchedPipe) {
        Add-Finding -Category "IPC" -Check "Named Pipe Active" -Status "PASS" -Details "Found listening pipe: $matchedPipe"
    } else {
        Add-Finding -Category "IPC" -Check "Named Pipe Active" -Status "WARN" -Details "Found pipes ($($activePipes -join ', ')), but not user-specific $expectedPipeName"
    }
} else {
    Add-Finding -Category "IPC" -Check "Named Pipe Active" -Status "FAIL" -Details "No Radium/Flux named pipe is currently listening! Is Radium running?"
}

$fluxProcs = Get-Process -Name "*radium*", "*flux*", "*openidm*" -ErrorAction SilentlyContinue
if ($fluxProcs) {
    $procSummary = ($fluxProcs | ForEach-Object { "$($_.ProcessName) (PID $($_.Id), Path: $($_.Path))" }) -join "; "
    Add-Finding -Category "Desktop" -Check "Radium Process Running" -Status "PASS" -Details $procSummary
} else {
    Add-Finding -Category "Desktop" -Check "Radium Process Running" -Status "FAIL" -Details "No Radium desktop process running. Downloads cannot be accepted while Radium is closed."
}

# -----------------------------------------------------------------------------
# 4. Brave Native Messaging Host Registry and Manifest Diagnostics
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "--- 4. Brave Native Messaging Host Registration ---" -ForegroundColor Yellow

$targetHosts = @("com.radium.downloadmanager", "com.flux.downloadmanager", "org.flux.native", "org.openidm.native")
$braveRegBasePath = "HKCU:\Software\BraveSoftware\Brave-Browser\NativeMessagingHosts"

foreach ($hostName in $targetHosts) {
    $regKeyPath = "$braveRegBasePath\$hostName"
    if (Test-Path $regKeyPath) {
        $manifestFilePath = (Get-ItemProperty -Path $regKeyPath -ErrorAction SilentlyContinue)."(default)"
        Add-Finding -Category "Registry (Brave)" -Check "Key: $hostName" -Status "PASS" -Details "Points to: $manifestFilePath"

        if ($manifestFilePath -and (Test-Path $manifestFilePath)) {
            Add-Finding -Category "Manifest ($hostName)" -Check "File Existence" -Status "PASS" -Details "Manifest file exists"
            try {
                $manifestJson = Get-Content -Raw $manifestFilePath | ConvertFrom-Json
                Add-Finding -Category "Manifest ($hostName)" -Check "JSON Validity" -Status "PASS" -Details "Valid JSON syntax"

                # Check name
                if ($manifestJson.name -eq $hostName) {
                    Add-Finding -Category "Manifest ($hostName)" -Check "Manifest Name Match" -Status "PASS" -Details "Name = $($manifestJson.name)"
                } else {
                    Add-Finding -Category "Manifest ($hostName)" -Check "Manifest Name Match" -Status "FAIL" -Details "Expected '$hostName', found '$($manifestJson.name)'"
                }

                # Check binary path
                $exePath = $manifestJson.path
                if ($exePath -and (Test-Path $exePath)) {
                    Add-Finding -Category "Manifest ($hostName)" -Check "Binary Existence" -Status "PASS" -Details "NMH executable exists: $exePath"
                } else {
                    Add-Finding -Category "Manifest ($hostName)" -Check "Binary Existence" -Status "FAIL" -Details "NMH executable NOT found at: $exePath"
                }

                # Check allowed origins
                $origins = @($manifestJson.allowed_origins)
                if ($origins.Count -gt 0) {
                    Add-Finding -Category "Manifest ($hostName)" -Check "Allowed Origins Count" -Status "PASS" -Details "Origins: $($origins -join ', ')"

                    # Check key-derived ID
                    if ($keyDerivedId) {
                        $keyOrigin = "chrome-extension://$keyDerivedId/"
                        if ($origins -contains $keyOrigin) {
                            Add-Finding -Category "Manifest ($hostName)" -Check "Key ID Authorized" -Status "PASS" -Details "Contains $keyOrigin"
                        } else {
                            Add-Finding -Category "Manifest ($hostName)" -Check "Key ID Authorized" -Status "FAIL" -Details "Missing key-derived origin: $keyOrigin"
                        }
                    }

                    # Check path-derived ID
                    if ($pathDerivedId) {
                        $pathOrigin = "chrome-extension://$pathDerivedId/"
                        if ($origins -contains $pathOrigin) {
                            Add-Finding -Category "Manifest ($hostName)" -Check "Unpacked Path ID Authorized" -Status "PASS" -Details "Contains $pathOrigin"
                        } else {
                            Add-Finding -Category "Manifest ($hostName)" -Check "Unpacked Path ID Authorized" -Status "WARN" -Details "Missing path-derived origin: $pathOrigin (Unpacked extensions without key use this)"
                        }
                    }

                    # Check detected browser extension IDs
                    foreach ($dId in $detectedBrowserExtIds) {
                        $dOrigin = "chrome-extension://$dId/"
                        if ($origins -contains $dOrigin) {
                            Add-Finding -Category "Manifest ($hostName)" -Check "Installed Browser ID Authorized" -Status "PASS" -Details "Contains installed browser origin: $dOrigin"
                        } else {
                            Add-Finding -Category "Manifest ($hostName)" -Check "Installed Browser ID Authorized" -Status "FAIL" -Details "Installed extension in Brave has ID '$dId', but '$dOrigin' is NOT in allowed_origins! Brave will reject with 'Access to specified native messaging host is forbidden'."
                        }
                    }
                } else {
                    Add-Finding -Category "Manifest ($hostName)" -Check "Allowed Origins" -Status "FAIL" -Details "allowed_origins array is empty or missing!"
                }
            } catch {
                Add-Finding -Category "Manifest ($hostName)" -Check "JSON Parsing" -Status "FAIL" -Details "Could not parse JSON: $_"
            }
        } else {
            Add-Finding -Category "Manifest ($hostName)" -Check "File Existence" -Status "FAIL" -Details "Manifest file does not exist at: $manifestFilePath"
        }
    } else {
        Add-Finding -Category "Registry (Brave)" -Check "Key: $hostName" -Status "FAIL" -Details "Registry key not found at $regKeyPath"
    }
}

# -----------------------------------------------------------------------------
# 5. Diagnostic Summary and Actionable Recommendations
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "                       DIAGNOSTIC SUMMARY                        " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan

$failCount = ($Findings | Where-Object { $_.Status -eq "FAIL" }).Count
$warnCount = ($Findings | Where-Object { $_.Status -eq "WARN" }).Count
$passCount = ($Findings | Where-Object { $_.Status -eq "PASS" }).Count

$summaryColor = if ($failCount -gt 0) { "Red" } elseif ($warnCount -gt 0) { "Yellow" } else { "Green" }
Write-Host "Checks Passed: $passCount | Warnings: $warnCount | Failures: $failCount" -ForegroundColor $summaryColor
Write-Host ""

if ($failCount -gt 0) {
    Write-Host "CRITICAL DEFECTS IDENTIFIED:" -ForegroundColor Red
    $Findings | Where-Object { $_.Status -eq "FAIL" } | ForEach-Object {
        Write-Host "  - [$($_.Category)] $($_.Check): $($_.Details)" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "Recommended Fix:" -ForegroundColor Yellow
    Write-Host "  Run: powershell -ExecutionPolicy Bypass -File .\scripts\register-native-messaging.ps1" -ForegroundColor White
} else {
    Write-Host "All static native messaging configuration checks passed!" -ForegroundColor Green
}

Write-Host ""
Write-Host "Manual Browser Verification Steps:" -ForegroundColor Cyan
Write-Host "1. In Brave, navigate to: brave://extensions" -ForegroundColor Gray
Write-Host "2. Enable Developer mode (top-right)." -ForegroundColor Gray
Write-Host "3. Find 'Flux Download Manager'. Note the exact Extension ID displayed." -ForegroundColor Gray
Write-Host "4. Click 'service worker' under 'Inspect views' on the Flux extension card." -ForegroundColor Gray
Write-Host "5. In the DevTools Console that opens, observe live logs when clicking a download link." -ForegroundColor Gray
Write-Host "   - Look for: [Flux Trace] NATIVE_RPC_RESULT or NATIVE_HOST_ATTEMPT_FAILED" -ForegroundColor Gray
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host ""
