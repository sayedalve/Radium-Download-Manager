# SPDX-License-Identifier: GPL-3.0-or-later
# Copy the Chromium extension to a per-user path that survives repository/build cleanup.
[CmdletBinding()]
param(
    [string]$SourcePath = "",
    [string]$DestinationPath = ""
)

if (-not $SourcePath) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    if (-not $scriptDir) { $scriptDir = $PSScriptRoot }
    if (-not $scriptDir) { $scriptDir = (Get-Location).Path }
    $SourcePath = Join-Path $scriptDir '..\extension'
}

if (-not $DestinationPath) {
    $DestinationPath = Join-Path $env:LOCALAPPDATA 'Radium Download Manager\extension'
}

$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $SourcePath).Path
$manifestPath = Join-Path $source 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Chromium manifest not found: $manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if (-not $manifest.key -or $manifest.name -ne 'Radium Download Manager') {
    throw 'Refusing to stage an extension without the expected pinned identity.'
}

$destination = [System.IO.Path]::GetFullPath($DestinationPath)
if (-not (Test-Path -LiteralPath $destination)) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
}

Get-ChildItem -LiteralPath $source -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($source.Length).TrimStart('\', '/')
    $target = Join-Path $destination $relative
    $targetParent = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $targetParent)) {
        New-Item -ItemType Directory -Path $targetParent -Force | Out-Null
    }
    Copy-Item -LiteralPath $_.FullName -Destination $target -Force
}

$stagedManifest = Get-Content -LiteralPath (Join-Path $destination 'manifest.json') -Raw | ConvertFrom-Json
if ($stagedManifest.key -ne $manifest.key -or $stagedManifest.version -ne $manifest.version) {
    throw 'Staged extension identity or version does not match the source.'
}
Write-Host "Staged $($manifest.name) v$($manifest.version) at $destination"
Write-Host 'Load this directory unpacked once in Brave; ordinary builds do not remove or replace it.'
