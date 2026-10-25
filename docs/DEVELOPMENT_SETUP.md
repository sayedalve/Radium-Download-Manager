# Developer Environment Setup Guide

**Document Status:** Active Setup Guide (`v0.3.1`)  
**Last Reviewed:** 2026-10-20  
**Scope:** Preparing a clean Windows 11 `x64` workstation to build, debug, and test Radium Download Manager.  
**Related Documents:** [WINDOWS_BUILD_AND_PACKAGING.md](WINDOWS_BUILD_AND_PACKAGING.md), [TECHNOLOGY_STACK.md](TECHNOLOGY_STACK.md), [TEST_STRATEGY.md](TEST_STRATEGY.md), [FEATURE_MATRIX.md](FEATURE_MATRIX.md)

---

## 1. Required & Optional Tools on Windows 11 x64

### 1.1 Required Tools
1. **Git for Windows** (`2.43+`)
2. **Visual Studio 2022 Community / Professional / Enterprise** (`17.8+`) or **Visual Studio Build Tools 2022** with workload:
   - *Desktop development with C++*
   - *MSVC v143 - VS 2022 C++ x64/x86 build tools*
   - *Windows 11 SDK (10.0.22621.0 or 10.0.26100.0)*
   - *C++ CMake tools for Windows*
3. **Node.js** (`20 LTS+` or `22 LTS+`) & `npm` (for testing `extension/`).
4. **Python** (`3.11+` with `cryptography`) -- required for running the end-to-end integration and benchmark test harnesses.

### 1.2 Optional Tools
- **Inno Setup 6 (`6.3+`):** Required only when compiling the `.exe` installer (`installer/radium.iss` / `installer/flux.iss`).
- **Windows Sandbox:** Built-in Windows 11 Pro/Enterprise feature used for clean-machine installer and uninstaller testing.
- **Sysinternals Process Explorer:** Recommended for verifying Job Object process isolation, Named Pipe DACLs, and memory usage.

### 1.3 Quick Toolchain Installation via `winget` (PowerShell)
```powershell
winget install --id Git.Git -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install --id OpenJS.NodeJS.LTS -e
winget install --id Python.Python.3.12 -e
winget install --id JRSoftware.InnoSetup -e
```

---

## 2. Developer Build & Test Workflow

### 2.1 Provisioning Upstream Stream Workers (`N_m3u8DL-RE.exe` & `ffmpeg.exe`)
Run `scripts/fetch-workers.ps1` to download and cryptographically verify the pinned stream workers into `tools/win-x64/`:
```powershell
powershell -ExecutionPolicy Bypass -File scripts\fetch-workers.ps1
```

### 2.2 Building `radium.exe`, `radium-nmh.exe`, and C++ Unit Tests
From a **x64 Native Tools Command Prompt for VS 2022** (or PowerShell):

```powershell
# 1. Configure Release build with CMake
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release

# 2. Compile desktop app, native messaging host, and test binaries
cmake --build build --config Release --target flux flux-nmh flux_unit_tests

# 3. Run C++ unit and regression tests
.\build\Release\flux_unit_tests.exe
```

Alternatively, use the automated build pipeline script:
```powershell
.\scripts\build-windows-msvc.ps1
```

### 2.3 Browser Extension Setup & Debugging (`extension/`)
1. **Register the Native Messaging Host in `HKCU`:**
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\register-native-messaging.ps1
   ```
2. **Google Chrome / Brave / Microsoft Edge:**
   - Open `chrome://extensions`, `brave://extensions`, or `edge://extensions`.
   - Enable **Developer mode** in the top-right corner.
   - Click **Load unpacked** and select the repository's `extension/` directory.
   - Confirm the extension ID matches the authoritative key or discovered profile ID.
3. **Mozilla Firefox:**
   - Open `about:debugging#/runtime/this-firefox`.
   - Click **Load Temporary Add-on...** and select `extension/manifest.firefox.json` (or `extension/manifest.json`).

### 2.4 Packaging Portable Archives & Installer
```powershell
# Assemble portable zip packages (Full and Core) with SHA256SUMS.txt
powershell -ExecutionPolicy Bypass -File scripts\package-release-zip.ps1 -Version "0.3.0" -PackageType Both

# Compile Inno Setup installer
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\flux.iss
```

---

## 3. Troubleshooting Common Setup & Debugging Failures

| Symptom / Error | Root Cause | Resolution |
| :--- | :--- | :--- |
| **`Specified native messaging host not found`** in browser console | `HKCU\Software\...\NativeMessagingHosts\com.flux.downloadmanager` is not registered, or the path in the JSON manifest is invalid. | Run `.\scripts\register-native-messaging.ps1` and verify the JSON `"path"` points to a valid `flux-nmh.exe`. |
| **`Access to the specified native messaging host is forbidden`** | The unpacked extension's ID in `chrome://extensions` is not listed in `allowed_origins`. | Re-run `.\scripts\register-native-messaging.ps1` without arguments; it automatically derives and adds the unpacked path-based extension ID. |
| **`N_m3u8DL-RE.exe` exits immediately with access denied** | `N_m3u8DL-RE` defaults to creating a `Logs/` directory next to its binary in `%ProgramFiles%`. | Flux passes `--no-log` and executes workers with user-scoped response files in `%LOCALAPPDATA%\Flux\tmp\`. |
| **CMake cannot find compiler** | Running from a standard shell without MSVC environment variables. | Launch from the *x64 Native Tools Command Prompt for VS 2022* or invoke `vcvars64.bat`. |

Native messaging registration prefers the staged `dist\radium-nmh.exe` so cleaning the C++ build directory does not invalidate the browser bridge. Rebuilds stage extension assets in `dist\extension` and do not remove the source `extension\` directory.

