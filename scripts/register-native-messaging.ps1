# SPDX-License-Identifier: GPL-3.0-or-later
# Radium Download Manager -- Native Messaging Host Per-User Registration Script (v0.3.0)
# Configures HKCU registry keys for Chrome, Edge, Brave, and Firefox without requiring admin rights.
# Supports portable mode by pointing directly to the local radium-nmh.exe executable.

[CmdletBinding()]
param(
    [string]$ExtensionId = "",
    [string]$HostExePath = "",
    [string]$BuildDir = "",
    [switch]$Unregister = $false,
    [switch]$Uninstall = $false,
    [switch]$Clean = $false
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$doUnregister = $Unregister -or $Uninstall -or $Clean

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "   Radium Download Manager -- Native Messaging Host Setup (HKCU)   " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan

# 1. Handle Unregistration / Clean
if ($doUnregister) {
    Write-Host "`n==> Removing Native Messaging Host Registrations from HKCU..." -ForegroundColor Yellow
    $regPaths = @(
        "HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.flux.downloadmanager",
        "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\com.flux.downloadmanager",
        "HKCU:\Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\com.flux.downloadmanager",
        "HKCU:\Software\Mozilla\NativeMessagingHosts\com.flux.downloadmanager",
        "HKCU:\Software\Google\Chrome\NativeMessagingHosts\org.flux.native",
        "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\org.flux.native",
        "HKCU:\Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\org.flux.native",
        "HKCU:\Software\Mozilla\NativeMessagingHosts\org.flux.native",
        "HKCU:\Software\Google\Chrome\NativeMessagingHosts\org.openidm.native",
        "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\org.openidm.native",
        "HKCU:\Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\org.openidm.native",
        "HKCU:\Software\Mozilla\NativeMessagingHosts\org.openidm.native"
    )
    foreach ($rp in $regPaths) {
        if (Test-Path $rp) {
            Remove-Item -Path $rp -Recurse -Force
            Write-Host "  [-] Removed: $rp" -ForegroundColor Gray
        }
    }

    $manifestDir = Join-Path $env:LOCALAPPDATA "Radium Download Manager\manifests"
    if (Test-Path $manifestDir) {
        Remove-Item -Recurse -Force $manifestDir -ErrorAction SilentlyContinue
        Write-Host "  [-] Cleaned manifest cache: $manifestDir" -ForegroundColor Gray
    }

    Write-Host "`nNative Messaging Host unregistration completed successfully." -ForegroundColor Green
    exit 0
}

# 2. Locate radium-nmh.exe
if (-not $HostExePath) {
    $candidates = @()
    if ($BuildDir) {
        $resolvedBuildDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }
        $candidates += @(
            (Join-Path $resolvedBuildDir "radium-nmh.exe"),
            (Join-Path $resolvedBuildDir "flux-nmh.exe"),
            (Join-Path $resolvedBuildDir "openidm-nmh.exe")
        )
    }
    $candidates += @(
        # Prefer the staged executable: clean builds may remove build\win-x64.
        (Join-Path $RepoRoot "dist\radium-nmh.exe"),
        (Join-Path $RepoRoot "build\win-x64\Release\radium-nmh.exe"),
        (Join-Path $RepoRoot "build\Release\radium-nmh.exe"),
        (Join-Path $RepoRoot "radium-nmh.exe"),
        (Join-Path $RepoRoot "build\win-x64\Release\flux-nmh.exe"),
        (Join-Path $RepoRoot "build\Release\flux-nmh.exe"),
        (Join-Path $RepoRoot "build\win-x64\flux-nmh.exe"),
        (Join-Path $RepoRoot "build\flux-nmh.exe"),
        (Join-Path $RepoRoot "build\radium-nmh.exe"),
        (Join-Path $RepoRoot "dist\flux-nmh.exe"),
        (Join-Path $RepoRoot "flux-nmh.exe")
    )
    foreach ($cand in $candidates) {
        if (Test-Path $cand) {
            $HostExePath = (Resolve-Path $cand).Path
            break
        }
    }
}

if (-not $HostExePath -or -not (Test-Path $HostExePath)) {
    Write-Error "radium-nmh.exe could not be found! Please build the solution first or specify -HostExePath.`nSearched candidates:`n - $($candidates -join "`n - ")"
    exit 1
}
Write-Host "[+] Found Native Host executable: $HostExePath" -ForegroundColor Green

# 3. Determine Allowed Origins
$authoritativeFluxId = "mdgmlcnpodoehidlplplfegngnjnilap"
$authoritativeOrigin = "chrome-extension://$authoritativeFluxId/"

# Automatically derive ID from extension/manifest.json if present
$manifestPath = Join-Path $RepoRoot "extension\manifest.json"
if (Test-Path $manifestPath) {
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
            if ($derivedId) {
                $authoritativeFluxId = $derivedId
                $authoritativeOrigin = "chrome-extension://$derivedId/"
                Write-Host "[+] Derived authoritative Extension ID from manifest key: $derivedId" -ForegroundColor Green
            }
        }
    } catch {
        Write-Warning "Could not parse manifest.json key; using default authoritative ID: $authoritativeFluxId"
    }
}

$allowedOrigins = [System.Collections.Generic.List[string]]::new()
$allowedOrigins.Add($authoritativeOrigin)

# Automatically derive unpacked folder path ID (Chromium UTF-16LE SHA-256 of absolute path)
$extDir = Join-Path $RepoRoot "extension"
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
        $pathOrigin = "chrome-extension://$pathHashId/"
        if (-not $allowedOrigins.Contains($pathOrigin)) {
            $allowedOrigins.Add($pathOrigin)
            Write-Host "[+] Authorized repository unpacked Extension ID: $pathHashId" -ForegroundColor Green
        }
    } catch {
        Write-Warning "Could not derive path-based unpacked extension ID: $_"
    }
}

# Automatically discover extension IDs installed in Brave, Chrome, and Edge profiles
$browserProfiles = @(
    "$env:LOCALAPPDATA\BraveSoftware\Brave-Browser\User Data\Default\Secure Preferences",
    "$env:LOCALAPPDATA\Google\Chrome\User Data\Default\Secure Preferences",
    "$env:LOCALAPPDATA\Microsoft\Edge\User Data\Default\Secure Preferences"
)
foreach ($bp in $browserProfiles) {
    if (Test-Path $bp) {
        try {
            $pref = Get-Content -Raw $bp | ConvertFrom-Json
            $extSettings = $pref.extensions.settings
            if ($extSettings) {
                $extSettings.PSObject.Properties | ForEach-Object {
                    $extObj = $_.Value
                    $ePath = $extObj.path
                    $eName = $extObj.manifest.name
                    if ($ePath -match "openidm|flux|radium" -or $eName -match "Flux|OpenIDM|Radium") {
                        $bOrigin = "chrome-extension://$($_.Name)/"
                        if (-not $allowedOrigins.Contains($bOrigin)) {
                            $allowedOrigins.Add($bOrigin)
                            Write-Host "[+] Discovered and authorized installed browser Extension ID: $($_.Name)" -ForegroundColor Green
                        }
                    }
                }
            }
        } catch {}
    }
}

if ($ExtensionId) {
    $cleanId = $ExtensionId.Trim().ToLower()
    if ($cleanId -notmatch '^[a-p]{32}$') {
        Write-Error "Invalid Extension ID format: '$ExtensionId'. Chromium extension IDs must be exactly 32 lowercase characters between 'a' and 'p'."
        exit 1
    }
    $customOrigin = "chrome-extension://$cleanId/"
    if (-not $allowedOrigins.Contains($customOrigin)) {
        $allowedOrigins.Add($customOrigin)
    }
    Write-Host "[+] Authorized custom Extension ID origin: $customOrigin" -ForegroundColor Green
} else {
    Write-Host "[+] Total authorized Chromium origins: $($allowedOrigins -join ', ')" -ForegroundColor Cyan
}

# 4. Create Manifest Directory in %LOCALAPPDATA%\Radium Download Manager\manifests
$manifestDir = Join-Path $env:LOCALAPPDATA "Radium Download Manager\manifests"
if (-not (Test-Path $manifestDir)) {
    New-Item -ItemType Directory -Force -Path $manifestDir | Out-Null
}

$chromiumManifestPath = Join-Path $manifestDir "com.flux.downloadmanager.chromium.json"
$firefoxManifestPath = Join-Path $manifestDir "com.flux.downloadmanager.firefox.json"

function Write-Utf8NoBom([string]$FilePath, [string]$Content) {
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($FilePath, $Content, $utf8NoBom)
}

# 5. Generate Chromium Manifest
$chromiumManifest = [ordered]@{
    name = "com.flux.downloadmanager"
    description = "Radium Download Manager Native Messaging Host"
    path = $HostExePath
    type = "stdio"
    allowed_origins = $allowedOrigins
}

$chromiumJson = $chromiumManifest | ConvertTo-Json -Depth 5
Write-Utf8NoBom -FilePath $chromiumManifestPath -Content $chromiumJson
Write-Host "[+] Wrote Chromium manifest (UTF-8 no BOM): $chromiumManifestPath" -ForegroundColor Green

# 5.1 Generate Backwards-Compatible Alias Manifests
$aliasManifests = @{}
foreach ($alias in @("org.flux.native", "org.openidm.native")) {
    $aliasPath = Join-Path $manifestDir "$alias.chromium.json"
    $aliasManifest = [ordered]@{
        name = $alias
        description = "Radium Download Manager Native Messaging Host ($alias)"
        path = $HostExePath
        type = "stdio"
        allowed_origins = $allowedOrigins
    }
    Write-Utf8NoBom -FilePath $aliasPath -Content ($aliasManifest | ConvertTo-Json -Depth 5)
    $aliasManifests[$alias] = $aliasPath
    Write-Host "[+] Wrote Alias manifest ($alias, UTF-8 no BOM): $aliasPath" -ForegroundColor Green
}

# 6. Generate Firefox Manifest
$firefoxManifest = [ordered]@{
    name = "com.flux.downloadmanager"
    description = "Radium Download Manager Native Messaging Host"
    path = $HostExePath
    type = "stdio"
    allowed_extensions = @(
        "radium-extension@radiumdownloadmanager.org",
        "flux-extension@fluxdownloadmanager.org",
        "flux@downloadmanager.org",
        "flux@openidm.org",
        "extension@openidm.org"
    )
}
$firefoxJson = $firefoxManifest | ConvertTo-Json -Depth 5
Write-Utf8NoBom -FilePath $firefoxManifestPath -Content $firefoxJson
Write-Host "[+] Wrote Firefox manifest (UTF-8 no BOM): $firefoxManifestPath" -ForegroundColor Green

# 7. Register HKCU Keys for Browsers
Write-Host "`n==> Registering Browser Native Messaging Keys in HKCU..." -ForegroundColor Cyan

$targets = @(
    @{ Name = "Google Chrome"; Key = "HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.flux.downloadmanager"; Manifest = $chromiumManifestPath },
    @{ Name = "Microsoft Edge"; Key = "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\com.flux.downloadmanager"; Manifest = $chromiumManifestPath },
    @{ Name = "Brave Browser"; Key = "HKCU:\Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\com.flux.downloadmanager"; Manifest = $chromiumManifestPath },
    @{ Name = "Mozilla Firefox"; Key = "HKCU:\Software\Mozilla\NativeMessagingHosts\com.flux.downloadmanager"; Manifest = $firefoxManifestPath }
)

foreach ($alias in @("org.flux.native", "org.openidm.native")) {
    $aManifest = $aliasManifests[$alias]
    $targets += @(
        @{ Name = "Google Chrome ($alias)"; Key = "HKCU:\Software\Google\Chrome\NativeMessagingHosts\$alias"; Manifest = $aManifest },
        @{ Name = "Microsoft Edge ($alias)"; Key = "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\$alias"; Manifest = $aManifest },
        @{ Name = "Brave Browser ($alias)"; Key = "HKCU:\Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\$alias"; Manifest = $aManifest }
    )
}

foreach ($t in $targets) {
    $parent = Split-Path -Parent $t.Key
    if (-not (Test-Path $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    New-Item -ItemType Directory -Force -Path $t.Key | Out-Null
    Set-ItemProperty -Path $t.Key -Name "(Default)" -Value $t.Manifest
    Write-Host "  [OK] $($t.Name) -> $($t.Key)" -ForegroundColor Green
}

Write-Host "`n=================================================================" -ForegroundColor Cyan
Write-Host "  SUCCESS: Native Messaging Host Registered Successfully!       " -ForegroundColor Green
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "Next Steps for Browser Integration (Brave, Chrome, Edge):" -ForegroundColor White
Write-Host "1. In Brave, Chrome, or Edge, navigate to: brave://extensions, chrome://extensions, or edge://extensions" -ForegroundColor Gray
Write-Host "2. Enable the 'Developer mode' toggle in the top-right corner." -ForegroundColor Gray
Write-Host "3. Click 'Load unpacked' and select the folder: $RepoRoot\extension" -ForegroundColor Gray
Write-Host "4. Notice the extension ID is now fixed and stable: $authoritativeFluxId" -ForegroundColor Gray
Write-Host "   (If you reload an existing unpacked extension, click the reload icon on the extension card)" -ForegroundColor Gray
Write-Host "5. Open Radium Download Manager (radium.exe) and verify that the extension popup displays 'Desktop Connected'." -ForegroundColor White
