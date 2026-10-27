; SPDX-License-Identifier: GPL-3.0-or-later
; Radium Download Manager -- Inno Setup 6 Windows 11 x64 Installer Script (v0.3.0)
; Installs radium.exe (WIN32 GUI), radium-nmh.exe (CUI Native Messaging Host),
; optional stream workers (N_m3u8DL-RE & FFmpeg), browser extension assets, and
; registers Native Messaging manifests for Chrome, Edge, Brave, and Firefox.
;
; Safety Invariants:
; - Lowest privilege execution by default (per-user {autopf} without UAC elevation required).
; - Clean upgrade preservation of SQLite WAL databases and task state in %LOCALAPPDATA%\Radium.
; - Clean uninstallation of application binaries and Radium HKCU registry keys, preserving
;   user download directories and application configuration databases.

#define MyAppName "Radium Download Manager"
#define MyAppShortName "Radium"
#ifndef MyAppVersion
  #define MyAppVersion "0.3.1"
#endif
#define MyAppPublisher "Radium Contributors"
#define MyAppURL "https://github.com/sayedalve/Radium-Download-Manager"
#define MyAppExeName "radium.exe"
#define MyAppNmhName "radium-nmh.exe"

; Resolve executable source directory dynamically across build configurations
#ifndef TargetArch
  #define TargetArch "x64"
#endif

#ifndef SourceDir
  #if TargetArch == "ARM64"
    #if FileExists("..\build\win-arm64\Release\radium.exe")
      #define SourceDir "..\build\win-arm64\Release"
    #elif FileExists("..\dist\arm64\radium.exe")
      #define SourceDir "..\dist\arm64"
    #elif FileExists("..\build\Release\radium.exe")
      #define SourceDir "..\build\Release"
    #elif FileExists("..\build\win-arm64\radium.exe")
      #define SourceDir "..\build\win-arm64"
    #else
      #define SourceDir "..\build\Release"
    #endif
  #else
    #if FileExists("..\build\win-x64\Release\radium.exe")
      #define SourceDir "..\build\win-x64\Release"
    #elif FileExists("..\dist\radium.exe")
      #define SourceDir "..\dist"
    #elif FileExists("..\dist\Radium.exe")
      #define SourceDir "..\dist"
    #elif FileExists("..\build\Release\radium.exe")
      #define SourceDir "..\build\Release"
    #elif FileExists("..\build\win-x64\radium.exe")
      #define SourceDir "..\build\win-x64"
    #elif FileExists("..\build\win-x64\Release\flux.exe")
      #define SourceDir "..\build\win-x64\Release"
    #elif FileExists("..\dist\Flux.exe")
      #define SourceDir "..\dist"
    #else
      #define SourceDir "..\build\win-x64\Release"
    #endif
  #endif
#endif

; Resolve N_m3u8DL-RE worker: prioritize staged worker from tools\win-x64 (or build output fallback)
#if FileExists("..\tools\win-x64\N_m3u8DL-RE.exe")
  #define Nm3u8dlSource "..\tools\win-x64\N_m3u8DL-RE.exe"
#elif FileExists(SourceDir + "\N_m3u8DL-RE.exe")
  #define Nm3u8dlSource SourceDir + "\N_m3u8DL-RE.exe"
#elif FileExists("..\build\win-x64\Release\N_m3u8DL-RE.exe")
  #define Nm3u8dlSource "..\build\win-x64\Release\N_m3u8DL-RE.exe"
#elif FileExists("..\build\Release\N_m3u8DL-RE.exe")
  #define Nm3u8dlSource "..\build\Release\N_m3u8DL-RE.exe"
#elif FileExists("..\dist\tools\win-x64\N_m3u8DL-RE.exe")
  #define Nm3u8dlSource "..\dist\tools\win-x64\N_m3u8DL-RE.exe"
#else
  #define Nm3u8dlSource "..\tools\win-x64\N_m3u8DL-RE.exe"
#endif

[Setup]
AppId={{4D9C5F31-6B2E-4D73-8B2E-1F3C3D4E5891}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
#if TargetArch == "ARM64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#ifdef CoreOnly
  #define BaseFilename "Radium-Setup-ARM64-Core-" + MyAppVersion
#else
  #define BaseFilename "Radium-Setup-ARM64-" + MyAppVersion
#endif
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#ifdef CoreOnly
  #define BaseFilename "Radium-Setup-x64-Core-" + MyAppVersion
#else
  #define BaseFilename "Radium-Setup-x64-" + MyAppVersion
#endif
#endif
OutputDir=..\dist
OutputBaseFilename={#BaseFilename}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
AppMutex=Local\Radium_SingleInstance,Local\Flux_SingleInstance,Local\OpenIDM_SingleInstance
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=..\src\app\radium.ico

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full Installation (Includes Stream Extraction Workers)"
Name: "compact"; Description: "Core Installation (HTTP/HTTPS Segment Engine Only)"
Name: "custom"; Description: "Custom Installation"; Flags: iscustom

[Components]
Name: "core"; Description: "Radium Core Application & Browser Native Messaging Host"; Types: full compact custom; Flags: fixed
Name: "stream_workers"; Description: "HLS / DASH Stream Extraction Workers (N_m3u8DL-RE & FFmpeg)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "Start Radium minimized in system tray on Windows startup"; GroupDescription: "Startup:"

[Files]
; Core application binaries
Source: "{#SourceDir}\{#MyAppExeName}"; DestDir: "{app}"; DestName: "{#MyAppExeName}"; Components: core; Flags: ignoreversion
Source: "{#SourceDir}\{#MyAppNmhName}"; DestDir: "{app}"; DestName: "{#MyAppNmhName}"; Components: core; Flags: ignoreversion

#ifndef CoreOnly
; Stream extraction workers (N_m3u8DL-RE, FFmpeg, and yt-dlp) - installed exclusively to tools\win-x64
Source: "{#Nm3u8dlSource}"; DestDir: "{app}\tools\win-x64"; DestName: "N_m3u8DL-RE.exe"; Components: stream_workers; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\tools\win-x64\ffmpeg.exe"; DestDir: "{app}\tools\win-x64"; Components: stream_workers; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\tools\win-x64\yt-dlp.exe"; DestDir: "{app}\tools\win-x64"; Components: stream_workers; Flags: ignoreversion skipifsourcedoesntexist
#endif

; Browser extension, manifests, and documentation (excludes development/test files)
Source: "..\extension\*"; DestDir: "{app}\extension"; Components: core; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "*.ts,*.map,*.log,.git*,.github*,node_modules*,package*.json,tsconfig*.json,tests*,scratch*"
Source: "..\README.md"; DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "..\scripts\register-native-messaging.ps1"; DestDir: "{app}\scripts"; Components: core; Flags: ignoreversion
Source: "manifests\com.radium.downloadmanager.chromium.json"; DestDir: "{app}\manifests"; Components: core; Flags: ignoreversion
Source: "manifests\com.radium.downloadmanager.firefox.json"; DestDir: "{app}\manifests"; Components: core; Flags: ignoreversion
Source: "manifests\com.flux.downloadmanager.chromium.json"; DestDir: "{app}\manifests"; Components: core; Flags: ignoreversion
Source: "manifests\com.flux.downloadmanager.firefox.json"; DestDir: "{app}\manifests"; Components: core; Flags: ignoreversion
Source: "manifests\org.openidm.native.chromium.json"; DestDir: "{app}\manifests"; Components: core; Flags: ignoreversion
Source: "manifests\org.openidm.native.firefox.json"; DestDir: "{app}\manifests"; Components: core; Flags: ignoreversion

[Dirs]
Name: "{app}\tools\win-x64"; Components: stream_workers; Flags: uninsalwaysuninstall
Name: "{app}\manifests"; Components: core; Flags: uninsalwaysuninstall
Name: "{app}\scripts"; Components: core; Flags: uninsalwaysuninstall
Name: "{app}\extension"; Components: core

[InstallDelete]
; Clean up legacy duplicate worker binaries from older installations that installed into {app}\tools
Type: files; Name: "{app}\tools\ffmpeg.exe"
Type: files; Name: "{app}\tools\ffprobe.exe"
Type: files; Name: "{app}\tools\N_m3u8DL-RE.exe"
Type: files; Name: "{app}\tools\yt-dlp.exe"
Type: dirifempty; Name: "{app}\tools"

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Primary Radium Native Messaging Host Registrations (HKCU per-user, non-admin)
Root: HKCU; Subkey: "Software\Google\Chrome\NativeMessagingHosts\com.radium.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\com.radium.downloadmanager.chromium.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Edge\NativeMessagingHosts\com.radium.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\com.radium.downloadmanager.chromium.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\com.radium.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\com.radium.downloadmanager.chromium.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Mozilla\NativeMessagingHosts\com.radium.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\com.radium.downloadmanager.firefox.json"; Flags: uninsdeletekey

; Compatibility Flux Native Messaging Host Registrations (HKCU per-user, non-admin)
Root: HKCU; Subkey: "Software\Google\Chrome\NativeMessagingHosts\com.flux.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\com.flux.downloadmanager.chromium.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Edge\NativeMessagingHosts\com.flux.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\com.flux.downloadmanager.chromium.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\com.flux.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\com.flux.downloadmanager.chromium.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Mozilla\NativeMessagingHosts\com.flux.downloadmanager"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\com.flux.downloadmanager.firefox.json"; Flags: uninsdeletekey

; Backwards-Compatibility Native Messaging Registrations (for existing OpenIDM extensions)
Root: HKCU; Subkey: "Software\Google\Chrome\NativeMessagingHosts\org.openidm.native"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\org.openidm.native.chromium.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Edge\NativeMessagingHosts\org.openidm.native"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\org.openidm.native.chromium.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\org.openidm.native"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\org.openidm.native.chromium.json"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Mozilla\NativeMessagingHosts\org.openidm.native"; ValueType: string; ValueName: ""; ValueData: "{app}\manifests\org.openidm.native.firefox.json"; Flags: uninsdeletekey

; Optional Startup Registration
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Radium"; ValueData: """{app}\{#MyAppExeName}"" --daemon"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
// Dynamically generate manifests with the exact installed path of radium-nmh.exe
procedure WriteNativeMessagingManifests();
var
  NmhPathEscaped: String;
  ManifestDir: String;
  RadiumChromiumJson: String;
  RadiumFirefoxJson: String;
  FluxChromiumJson: String;
  FluxFirefoxJson: String;
  LegacyChromiumJson: String;
  LegacyFirefoxJson: String;
begin
  NmhPathEscaped := ExpandConstant('{app}\{#MyAppNmhName}');
  StringChangeEx(NmhPathEscaped, '\', '\\', True);
  ManifestDir := ExpandConstant('{app}\manifests');
  ForceDirectories(ManifestDir);

  RadiumChromiumJson :=
    '{' + #13#10 +
    '  "name": "com.radium.downloadmanager",' + #13#10 +
    '  "description": "Radium Download Manager Native Messaging Host",' + #13#10 +
    '  "path": "' + NmhPathEscaped + '",' + #13#10 +
    '  "type": "stdio",' + #13#10 +
    '  "allowed_origins": [' + #13#10 +
    '    "chrome-extension://mdgmlcnpodoehidlplplfegngnjnilap/",' + #13#10 +
    '    "chrome-extension://chbojfoombcanhhpoclailollapkglma/"' + #13#10 +
    '  ]' + #13#10 +
    '}' + #13#10;
  SaveStringToFile(ManifestDir + '\com.radium.downloadmanager.chromium.json', RadiumChromiumJson, False);

  RadiumFirefoxJson :=
    '{' + #13#10 +
    '  "name": "com.radium.downloadmanager",' + #13#10 +
    '  "description": "Radium Download Manager Native Messaging Host",' + #13#10 +
    '  "path": "' + NmhPathEscaped + '",' + #13#10 +
    '  "type": "stdio",' + #13#10 +
    '  "allowed_extensions": [' + #13#10 +
    '    "radium-extension@radiumdownloadmanager.org",' + #13#10 +
    '    "flux-extension@fluxdownloadmanager.org",' + #13#10 +
    '    "flux@downloadmanager.org"' + #13#10 +
    '  ]' + #13#10 +
    '}' + #13#10;
  SaveStringToFile(ManifestDir + '\com.radium.downloadmanager.firefox.json', RadiumFirefoxJson, False);

  FluxChromiumJson :=
    '{' + #13#10 +
    '  "name": "com.flux.downloadmanager",' + #13#10 +
    '  "description": "Radium Download Manager Native Messaging Host (Compatibility)",' + #13#10 +
    '  "path": "' + NmhPathEscaped + '",' + #13#10 +
    '  "type": "stdio",' + #13#10 +
    '  "allowed_origins": [' + #13#10 +
    '    "chrome-extension://mdgmlcnpodoehidlplplfegngnjnilap/",' + #13#10 +
    '    "chrome-extension://chbojfoombcanhhpoclailollapkglma/"' + #13#10 +
    '  ]' + #13#10 +
    '}' + #13#10;
  SaveStringToFile(ManifestDir + '\com.flux.downloadmanager.chromium.json', FluxChromiumJson, False);

  FluxFirefoxJson :=
    '{' + #13#10 +
    '  "name": "com.flux.downloadmanager",' + #13#10 +
    '  "description": "Radium Download Manager Native Messaging Host (Compatibility)",' + #13#10 +
    '  "path": "' + NmhPathEscaped + '",' + #13#10 +
    '  "type": "stdio",' + #13#10 +
    '  "allowed_extensions": [' + #13#10 +
    '    "flux-extension@fluxdownloadmanager.org",' + #13#10 +
    '    "flux@downloadmanager.org"' + #13#10 +
    '  ]' + #13#10 +
    '}' + #13#10;
  SaveStringToFile(ManifestDir + '\com.flux.downloadmanager.firefox.json', FluxFirefoxJson, False);

  LegacyChromiumJson :=
    '{' + #13#10 +
    '  "name": "org.openidm.native",' + #13#10 +
    '  "description": "Radium Download Manager Legacy Bridge",' + #13#10 +
    '  "path": "' + NmhPathEscaped + '",' + #13#10 +
    '  "type": "stdio",' + #13#10 +
    '  "allowed_origins": [' + #13#10 +
    '    "chrome-extension://mdgmlcnpodoehidlplplfegngnjnilap/",' + #13#10 +
    '    "chrome-extension://chbojfoombcanhhpoclailollapkglma/"' + #13#10 +
    '  ]' + #13#10 +
    '}' + #13#10;
  SaveStringToFile(ManifestDir + '\org.openidm.native.chromium.json', LegacyChromiumJson, False);

  LegacyFirefoxJson :=
    '{' + #13#10 +
    '  "name": "org.openidm.native",' + #13#10 +
    '  "description": "Radium Download Manager Legacy Bridge",' + #13#10 +
    '  "path": "' + NmhPathEscaped + '",' + #13#10 +
    '  "type": "stdio",' + #13#10 +
    '  "allowed_extensions": [' + #13#10 +
    '    "extension@openidm.org"' + #13#10 +
    '  ]' + #13#10 +
    '}' + #13#10;
  SaveStringToFile(ManifestDir + '\org.openidm.native.firefox.json', LegacyFirefoxJson, False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    WriteNativeMessagingManifests();
  end;
end;

// Uninstaller safety: preserve user databases and downloaded files
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    // Note: Inno Setup uninstalls only files listed in [Files] or explicitly registered.
    // user task state in %LOCALAPPDATA%\Radium\state.db and %USERPROFILE%\Downloads\Radium
    // are strictly preserved by design so uninstalling does not cause data loss.
  end;
end;
