# Windows Build and Packaging Specification

**Document Status:** Active Build & Packaging Specification (`v0.3.0`)  
**Last Reviewed:** 2026-10-18  
**Scope:** Compiler and Windows SDK requirements, CMake configuration, native C++20 stream worker compilation, dynamic worker discovery, Inno Setup installer, and clean-machine distribution validation.  
**Related Documents:** [RELEASE_AND_UPDATE_PROCESS.md](RELEASE_AND_UPDATE_PROCESS.md), [DEVELOPMENT_SETUP.md](DEVELOPMENT_SETUP.md), [FEATURE_MATRIX.md](FEATURE_MATRIX.md), [adr/0008-packaging-and-updates.md](adr/0008-packaging-and-updates.md)

---

## 1. Build Targets & Executables (`CMakeLists.txt`)

Radium Download Manager `v0.3.1` compiles native C++20 executables:

| Target Name | Source Entry Point | Windows 11 Output | Subsystem | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **`radium`** (aliases: `flux`, `openidm`) | `src/app/main.cpp` (`wWinMain` / `main`) | `build/win-x64/Release/radium.exe` | Windows GUI (`/SUBSYSTEM:WINDOWS`) | Main Desktop GUI, Unified Download Controller, Brisk HTTP Engine, Stream Supervisor, and IPC Server |
| **`radium-nmh`** (aliases: `flux-nmh`, `openidm-nmh`) | `src/nmh/main.cpp` | `build/win-x64/Release/radium-nmh.exe` | Windows Console (`/SUBSYSTEM:CONSOLE`) | Browser Native Messaging Host (`stdio` length-prefixed JSON-RPC 2.0 bridge over Named Pipe) |
| **`N_m3u8DL-RE`** | `src/worker_stream/nm3u8dl_re_worker.cpp` | `tools/win-x64/N_m3u8DL-RE.exe` | Windows Console | Standalone compiled C++20 HLS/DASH stream downloader, AES-128 decryptor, and FFmpeg muxer worker |
| **`radium_unit_tests`** | `tests/cpp/unit_tests.cpp` | `build/win-x64/Release/radium_unit_tests.exe` | Windows Console | Automated 61-suite C++20 unit and regression test binary |

---

## 2. Dynamic Worker & Directory Resolution

Radium dynamically resolves all operational directories and worker binaries at runtime via `PathSanitizer` (`src/common/path_sanitizer.cpp`):

1. **Executable Directory (`PathSanitizer::GetExecutableDir()`):**
   - Resolves the directory containing `radium.exe` (or `flux.exe`) via `GetModuleFileNameW(nullptr, ...)`.
2. **Application Data Directory (`PathSanitizer::GetDefaultAppDataDir()`):**
   - Resolves `%LOCALAPPDATA%\Radium Download Manager` on Windows 11 (`state.db`, `state.db-wal`, `radium.log`, `tmp\`). Automatically migrates legacy state from `%LOCALAPPDATA%\Flux` or `%LOCALAPPDATA%\OpenIDM` if legacy state exists.
3. **Default Downloads Directory (`PathSanitizer::GetDefaultDownloadsDir()`):**
   - Resolves `%USERPROFILE%\Downloads\Radium` on Windows 11 (customizable in Settings and persisted in `state.db`). Preserves existing folders if present.
4. **Stream Worker Binary Discovery (`PathSanitizer::FindWorkerBinary()`):**
   - Searches `<exe_dir>\tools\win-x64\<name>.exe`, `<exe_dir>\tools\<name>.exe`, `<exe_dir>\<name>.exe`, and the system `PATH` environment variable.
   - Allows users to either use the bundled `tools\win-x64\N_m3u8DL-RE.exe` and `ffmpeg.exe` or provide existing system installations.

---

## 3. Reproducible Build Commands

### 3.1 Windows 11 `x64` Native Build Pipeline
From a PowerShell terminal with Visual Studio 2022 (MSVC v143) and CMake 3.25+:

```powershell
# Compiles radium.exe, radium-nmh.exe, radium_unit_tests.exe, and N_m3u8DL-RE.exe:
.\scripts\build-windows-msvc.ps1
```

### 3.2 Compiling Minimal Static FFmpeg
From a PowerShell terminal with MSYS2 MinGW-w64 toolchain:

```powershell
# Compiles minimal static FFmpeg 7.1 (~5.2 MB) and runs capability auditor:
.\scripts\build-minimal-ffmpeg.ps1
```

### 3.3 Assembling Standalone Portable Release Packages
```powershell
# Assembles both Full and Core portable zip archives with SHA256SUMS.txt:
powershell -ExecutionPolicy Bypass -File scripts\package-release-zip.ps1 -Version "0.3.1" -PackageType Both
```

### 3.4 Compiling Inno Setup 6 Installers
```powershell
# Full Offline Installer (< 20 MB; ~19.28 MB actual):
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\radium.iss

# Core-Only Standalone Installer (~2.89 MB actual):
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" /DCoreOnly installer\radium.iss
```

---

## 4. Inno Setup Installer (`installer/radium.iss`)

- **Per-User Non-Elevated Execution:** Uses `PrivilegesRequired=lowest` with `PrivilegesRequiredOverridesAllowed=dialog`. Installs cleanly into `{autopf}\Flux Download Manager` without prompting for UAC elevation on standard accounts.
- **Component Selection:**
  - `core` (Fixed): Desktop application, Native Messaging Host, extension, and documentation.
  - `stream_workers` (Optional): Pre-bundles `N_m3u8DL-RE.exe` and `ffmpeg.exe` for offline stream downloads.
- **Native Messaging Registry Keys (`HKCU`):**
  - `Software\Google\Chrome\NativeMessagingHosts\com.flux.downloadmanager`
  - `Software\Microsoft\Edge\NativeMessagingHosts\com.flux.downloadmanager`
  - `Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\com.flux.downloadmanager`
  - `Software\Mozilla\NativeMessagingHosts\com.flux.downloadmanager`
  - Plus legacy aliases (`org.openidm.native`) for backward compatibility with existing installations.
- **Upgrade & Uninstall Safety:**
  - Uses `AppMutex=Local\Flux_SingleInstance,Local\OpenIDM_SingleInstance` to ensure clean shutdown before binary replacement.
  - Cleanly preserves `%LOCALAPPDATA%\Flux\state.db` and user download directories during uninstall and upgrades.


---

## 5. GitHub-Only Automated Production Verification & Release Pipeline

All routine production builds, full regression verification, clean-machine installation tests, browser extension runtime tests, and release publications are strictly automated via GitHub Actions on hosted `windows-2022` runners:

1. **Deterministic Build & Regression (`build-and-package`):** Builds MSVC C++20 executables, executes `flux_unit_tests.exe`, `test_extension_detection.js`, `run_all_integration_tests.py`, and `run-benchmarks.py`. Compiles both Full and Core Inno Setup installers, packages portable ZIPs, and enforces package size budgets (`report-package-sizes.ps1 -FailOnExceeded`) and security checksums (`verify-release-security.ps1`).
2. **Clean Installation Smoke Test (`clean-install-smoke-test`):** On an isolated fresh runner, downloads the generated installer and executes `tests/test_installer_smoke.py`: runs silent install, verifies all worker binaries and manifests, confirms HKCU registry entries, performs a real loopback HTTP download with exact SHA256 match, tests mid-stream reset auto-resume, verifies zero leftover `.part` files, tests database state persistence across restart, and validates safe uninstallation preserving `%LOCALAPPDATA%\Flux`.
3. **Browser Extension Runtime Test (`browser-extension-runtime-test`):** On an isolated fresh runner, launches Chromium via Playwright with the real unpacked extension loaded (`tests/browser/test_extension_runtime.js`): validates download interception, closed Shadow DOM video overlay attachment, dynamic HLS/DASH manifest detection, popup UI filtering, and service worker persistence.
4. **Guarded Release Publishing (`publish-release`):** Strictly blocked until jobs 1, 2, and 3 have all passed; executes only on semantic version tags (`v*`), ensuring no unverified or broken build can ever be published.
