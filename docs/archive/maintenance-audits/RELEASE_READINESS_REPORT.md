# Flux Download Manager v0.3.0 Release Readiness Report

**Report Date:** 2026-10-16  
**Product Name:** Flux Download Manager (Short Name: Flux)  
**Status:** RELEASE READY (Verified on Windows 11 Host)  
**Verification Environment:** Windows 11 Pro 64-bit (`10.0.26200 AMD64`), Intel64 Family 6 Model 142  
**Toolchain:** MSVC 19.44 / Windows SDK 10.0.26100.0 / CMake 4.2 / Inno Setup 6  
**Runtimes:** Python 3.12, Node.js v20.18.0  
**Stream Workers:** Upstream `N_m3u8DL-RE.exe` v0.3.0-beta (`E5C93A8DE0C58C50922C17A8A3BABD7D2193FB43B1A7FACD0B18DE9274EF8061`), `ffmpeg.exe` (`AA24A30E1194D4CE54999EFF72C58484B1A5AEE32DD9933A9242D15711DF1E48`)

---

## 1. Executive Summary

Flux Download Manager `v0.3.0` has completed all brand transformations, core defect resolutions, browser extension media detection overhauls, streaming webpage protections, and portable packaging directly on Windows 11:

1. **Complete Rebranding to Flux Download Manager:**
   - Executables renamed to `flux.exe` and `flux-nmh.exe`.
   - Desktop UI, Win32 window title bar, About dialog, system tray icon, notification messages, file metadata, and RC resources fully rebranded to Flux Download Manager.
   - Zero legacy OpenIDM strings remaining in user-facing surfaces; automatic backward-compatible settings migration from `%LOCALAPPDATA%\OpenIDM` to `%LOCALAPPDATA%\Flux Download Manager`.

2. **Browser Media & Download Detection Overhaul:**
   - **Link-Click Download Interception (`BUG-031`):** Fixed missing link click listeners in `extension/js/content-script.js`. Captures downloads for binary file extensions (`.zip`, `.exe`, `.iso`, `.pdf`, `.tar.gz`, etc.) and `download` attributes, while honoring IDM-style `Alt` key bypass modifier (`e.altKey`).
   - **In-Page Media Playback Detection (`BUG-032`):** Hooked HTML5 `<video>` and `<audio>` runtime events (`play`, `playing`, `canplay`, `loadedmetadata`), and injected `catch-script/catch.js` directly into the webpage DOM (`MAIN` world) to intercept `window.fetch` and `XMLHttpRequest` stream manifests.
   - **Service Worker & Popup UI Lifecycle Bridge (`BUG-033`):** Connected `chrome.runtime.onMessage` in `background.js` to handle `getMediaList`, `downloadMedia`, `clearMedia`, and `setSiteCapture` RPC calls; wired active tab query and badge counts in `popup.js`.
   - **Streaming Webpage HTML Watch Page Guard (`BUG-034`):** Fixed YouTube watch page 429 errors by implementing `PathSanitizer::IsWebpageWatchUrl`, rejecting raw HTML watch pages (`youtube.com/watch?v=...`, `vimeo.com/...`) with `ERR_URL_IS_WEBPAGE` across desktop controller, IPC, and browser extension.
   - **Portable Native Messaging Registration (`BUG-035`):** Created `scripts/register-native-messaging.ps1` registering per-user HKCU keys for Chrome, Edge, Brave, and Firefox pointing directly to local `flux-nmh.exe` without admin privileges.

3. **100% Automated Test Suite Pass Rate on Windows 11 Host:**
   - **13 / 13** C++20 unit and regression test suites (`build\win-x64\Release\flux_unit_tests.exe`) -- **PASS**
   - **10 / 10** Node.js browser extension and media detection suites (`tests/test_extension_detection.js`) -- **PASS**
   - **8 / 8** Python end-to-end integration and benchmark suites (`tests/run_all_integration_tests.py` & `scripts/run-benchmarks.py`) -- **PASS**

---

## 2. Windows 11 Automated Verification Summary

| Test Step | Command Executed | Environment | Status | Verification Summary |
| :--- | :--- | :--- | :--- | :--- |
| **1. Toolchain & Build** | `powershell -File scripts/build-windows-msvc.ps1 -SkipWorkerFetch -SkipInstaller` | Windows 11 MSVC / CMake | **PASS** | Compiled `flux.exe`, `flux-nmh.exe`, `flux_unit_tests.exe` (AMD64 PE32+). |
| **2. C++20 Unit Tests** | `.\build\win-x64\Release\flux_unit_tests.exe` | Windows 11 x64 | **PASS** | **13 / 13 suites passed** (Brisk engine, SQLite storage, DPAPI encryption, path sanitization, UI hit-testing, worker lifecycle, Flux branding, webpage guard). |
| **3. Extension Detection** | `node tests/test_extension_detection.js` | Node.js v20.18.0 | **PASS** | **10 / 10 suites passed** (Media sniff, Master/Child HLS deduplication, site exclusion, deep search hooks, Shadow DOM overlay, download bypass, MV3 persistence, popup tabs, link-click capture, webpage watch guard). |
| **4. E2E Integration Suite**| `python tests/run_all_integration_tests.py` | Python 3.12 | **PASS** | **8 / 8 suites passed** (8-conn HTTP range, 302 redirect, RFC 5987 filename, connection reset resume, HLS probe/mux, DASH mux, NMH IPC + AES-128, UI snapshot). |
| **5. Performance Benchmark**| `python scripts/run-benchmarks.py` | Python 3.12 | **PASS** | All 8 performance budgets verified and saved to `benchmarks/latest_metrics.json`. |
| **6. Native Host Registration**| `powershell -File scripts/register-native-messaging.ps1` | Windows 11 PowerShell | **PASS** | HKCU registry keys created for Chrome, Edge, Brave, and Firefox pointing to `flux-nmh.exe`. |

---

## 3. Enforced Performance Metrics (`benchmarks/latest_metrics.json`)

| Metric ID | Description | Enforced Limit | Windows 11 Measured | Status |
| :--- | :--- | :--- | :--- | :--- |
| **PERF-01** | Combined Core Binaries (`flux.exe` + `flux-nmh.exe`) | $\le 3,072\text{ KB}$ | **2,745.5 KB** (`1,664.5` + `1,081.0` KB) | **PASS** |
| **PERF-02** | Cold Startup Latency (`flux --list` on clean DB) | $\le 150.0\text{ ms}$ | **46.81 ms** (median) | **PASS** |
| **PERF-03** | Idle Daemon Working Set RSS (`flux --daemon`) | $\le 25.0\text{ MB}$ | **9.32 MB** | **PASS** |
| **PERF-04** | Peak HTTP 8-Connection RSS (16 MiB parallel download) | $\le 65.0\text{ MB}$ | **13.60 MB** | **PASS** |
| **PERF-05** | Peak Stream Worker RSS (`flux --probe-stream` upstream N_m3u8DL-RE) | $\le 65.0\text{ MB}$ | **10.54 MB** | **PASS** |
| **PERF-06** | Net Stream Overhead Above FFmpeg Baseline (`flux --stream`) | $\le 40.0\text{ MB}$ | **0.00 MB** (Total: `10.63 MB`, FFmpeg: `15.06 MB`) | **PASS** |
| **PERF-07** | Large Task List Responsiveness (1,000-task GUI snapshot) | $\le 300.0\text{ ms}$ | **163.97 ms** | **PASS** |
| **PERF-08** | Loopback Segmented HTTP Throughput (8 connections) | $\ge 50.0\text{ Mbps}$ | **386.9 Mbps** | **PASS** |
| **PERF-09** | Residual Temporary Files (`*.part`) After Completion | **0 files** | **0 files** | **PASS** |

---

## 4. Release Artifacts & Cryptographic Checksums

The portable release files are staged in `dist/`:

| Artifact | File Size | SHA-256 Checksum | Description |
| :--- | :--- | :--- | :--- |
| `dist/Flux.exe` | 1,704,448 B | `B0DAC6F2FBE11BA46E8ADA7277D9B5D0DA1DFA603E9F18F0AABC4DB295BC1355` | Main Win32 GUI desktop application (portable PE32+ GUI) |
| `dist/flux-nmh.exe` | 1,106,944 B | `363714E0C0562666E7BF975AD8A56CA2C05161AABA3D5A48BDFA5CE1EE08943A` | Browser Native Messaging Host (PE32+ CUI) |
| `dist/tools/win-x64/N_m3u8DL-RE.exe` | 17,197,568 B | `E5C93A8DE0C58C50922C17A8A3BABD7D2193FB43B1A7FACD0B18DE9274EF8061` | Pinned upstream N_m3u8DL-RE stream engine (MIT) |
| `dist/tools/win-x64/ffmpeg.exe` | 168,960,000 B | `AA24A30E1194D4CE54999EFF72C58484B1A5AEE32DD9933A9242D15711DF1E48` | Pinned upstream FFmpeg muxing engine (GPL-3.0) |
| `extension/` | Unpacked | N/A | Clean Manifest V3 extension ready for Chrome Developer Mode |

---

## 5. Verification Classification Summary

### 5.1 Verified by Execution on Windows 11 Host
- Full MSVC x64 compilation and linking with embedded SQLite.
- All 13 C++ unit test suites (`flux_unit_tests.exe`).
- All 10 browser extension detection test suites (`test_extension_detection.js`).
- All 8 end-to-end integration and benchmark suites (`run_all_integration_tests.py` & `run-benchmarks.py`).
- Native Messaging Named Pipe IPC communication with `_O_BINARY` stdio frames.
- AES-128 HLS stream decryption with Cookie header authentication.
- MPEG-DASH manifest downloading and FFmpeg MP4 container muxing.
- Per-user HKCU Native Messaging registration (`scripts/register-native-messaging.ps1`).

### 5.2 Unverified (Requires Manual Browser Step)
- Loading unpacked `extension/` in `chrome://extensions` (Developer Mode) and browsing external live media websites.

### 5.3 Known Unsupported Scenarios
- Encrypted streams protected by commercial DRM (`Widevine`, `PlayReady`, `FairPlay`, or CENC without keys) -- rejected with error code `ERR_STREAM_DRM_PROTECTED`.
- Webpage HTML watch pages (`youtube.com/watch?v=...`) submitted directly as raw binary download tasks -- rejected with `ERR_URL_IS_WEBPAGE`.
