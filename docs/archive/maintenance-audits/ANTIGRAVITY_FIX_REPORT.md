# ANTIGRAVITY FIX REPORT: FLUX DOWNLOAD MANAGER REVERSE ENGINEERING & REMEDIATION

**Document Status:** Forensic Source Audit, Engineering Remediation, and Safe Verification Report  
**Date:** 2026-10-18  
**Scope:** Resolves core functional failures in browser download interception, global video pipeline routing, native confirmation workflow, Win32 table interaction, and stream worker provisioning.

---

## 1. Executive Summary of Audit Findings & Remediation

| Issue ID | Subsystem & Affected Files | Root Cause | Impact | Implemented Correction & Verification | Status |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **ISSUE-01** | **Global Video Stream Routing**<br>`src/core/download_controller.cpp`<br>`src/engine_stream/nm3u8dl_re_adapter.cpp`<br>`src/ipc/ipc_server.cpp` | Broad heuristic `cached_manifest.front() == '{' \|\| '['` treated any JSON manifest as YouTube. Direct progressive video URLs (`.mp4`, `.webm`) were fed into stream extraction workers rather than the fast segmented HTTP engine. | Generic video streaming sites (e.g. Pimpbunny) failed with misleading `ERR_YOUTUBE_EXTRACTION_FAILED` or `ERR_YOUTUBE_NO_PLAYABLE_STREAM`. | Restricted `is_yt` strictly to verified YouTube watch/short/embed endpoints. Direct progressive media files bypass stream worker and route directly to `TaskType::kHttpFile`. Worker failures on generic sites produce `ERR_STREAM_WORKER_FAILED` or `ERR_MANIFEST_PARSE_FAILED`. | **Remediated** |
| **ISSUE-02** | **Ghost Sidebar Category Switcher**<br>`src/ui/win32_main_window.cpp` | `ComputeLayoutGeometry` initialized dummy sidebar category rectangles at `filt_x ~ 180`, `y = 10 + 30*i` overlapping table column 1 in rows 0–5. `HitTestPoint` tested categories before rows. | Clicking in the upper rows of the download table unexpectedly switched sidebar category filters. | Dummy category rectangles for `i > 0` set to `{0, 0, 0, 0}`. Added `!r.rect.IsEmpty()` check in `HitTestPoint`. | **Remediated** |
| **ISSUE-03** | **Unified New Download Dialog**<br>`src/ui/win32_main_window.hpp`<br>`src/ui/win32_main_window.cpp`<br>`src/ipc/ipc_server.hpp`<br>`src/ipc/ipc_server.cpp` | `download.offer` and drag-and-drop lacked an asynchronous bridge to a native Win32 confirmation modal dialog. | Downloads began automatically without allowing the user to review pre-populated URLs, filenames, destination paths, quality options, or scheduled execution. | Added `OfferNotificationCallback` and `WM_OPENIDM_SHOW_NEW_DOWNLOAD` message. Built 600x510px native dark dialog with disk space query, format dropdown, mutual exclusivity between immediate start and schedule, coral red `#FF4F57` Download button, and task lifecycle binding. | **Remediated** |
| **ISSUE-04** | **Table Scrolling & Resizable Columns**<br>`src/ui/win32_main_window.hpp`<br>`src/ui/win32_main_window.cpp` | `DesktopView` lacked scroll offset state (`m_scroll_offset`) and column widths were hardcoded constants. | Users could not scroll through long download lists via mouse wheel or vertical scrollbar, and could not resize columns to read truncated filenames. | Implemented `ScrollBy`, `GetColumnWidth`, `SetColumnWidth`, `StartColumnResize`, `UpdateColumnResize`, `EndColumnResize`, `HitTestColumnDivider`, `WM_MOUSEWHEEL`, `WM_VSCROLL`, and `WM_SETCURSOR` with `IDC_SIZEWE`. | **Remediated** |
| **ISSUE-05** | **Visual Styling & Typography**<br>`src/ui/win32_main_window.cpp` | Settings toolbar button used a generic 3-slider glyph, file icons were uncolored, progress bars were thin (4px), and search edit had redundant right padding from removed Ctrl K chip. | Visual appearance diverged from 2026 Cosmic Dark design standards. | Tinted file icons in coral red (`#FF4F57`), added 6-tooth vector gear icon (`DrawGearGlyphGdi`), expanded search box with `-14` Segoe UI typography, and enlarged progress bar height to 8px with active percentage text. | **Remediated** |
| **ISSUE-06** | **Worker Distribution Directory Sync**<br>`scripts/build-windows-msvc.ps1` | `build-windows-msvc.ps1` staged `yt-dlp.exe` to both `dist/tools/` and `dist/tools/win-x64/`, but only staged `N_m3u8DL-RE.exe` and `ffmpeg.exe` to `dist/tools/win-x64/`. | Tools searching `dist/tools/` directly could fail to locate `N_m3u8DL-RE` or `ffmpeg`. | Synchronized all worker binaries across both `dist/tools/` and `dist/tools/win-x64/`. | **Remediated** |
| **ISSUE-07** | **Brave / Chromium Download Interception**<br>`extension/js/background.js`<br>`extension/js/content-script.js` | Calling `chrome.downloads.cancel()` before resolving `suggest()` in `onDeterminingFilename` throws a runtime exception in Chromium, causing Brave to fall back to its internal download manager. | Clicking download links in Brave resulted in competing or exclusive browser downloads. | Ensured `invokeSuggestIfNeeded()` executes before `chrome.downloads.cancel()`, tracked in-flight offers per download ID, and verified Alt-key bypass preservation. | **Remediated** |

---

## 2. Phase-by-Phase Verification Log

### Phase 1: Forensic Source Audit
- **Branch**: `fix/audit-remediation`
- **Commit**: `f1e1706`
- **Submodule Revisions**:
  - `third_party/N_m3u8DL-RE`: `9d995eb5e88ff63886a7d7e2f112905a8bbbb4c5`
  - `third_party/brisk`: `517c669953f2e7f261aec45a96b0611d541313fe`
  - `third_party/cat-catch`: `4e1a1586f12c9376d8c5aa7334318ff4ee19316f`
- **Toolchain Discovery**:
  - Node.js: `v24.18.0`
  - Python: `3.12.10`
  - MSVC / VS 2026 / Visual Studio 18 Community CMake: `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`

### Phase 2: Browser Interception Source Logic
- Verified capturing link click listeners (`click` + `auxclick`) in `extension/js/content-script.js`.
- Verified `handleLinkClick` Alt-key bypass preservation (`bypassNextDownload`).
- Verified `onDownloadCreatedHandler` in `extension/js/background.js` coordinating `onCreated` pause, `onDeterminingFilename` `suggest()` resolution, and cancel-on-accept.
- Tested via isolated Node.js test suite (`tests/test_extension_detection.js`): 11 / 11 suites passed without network access or browser launch.

### Phase 3: Media Detection and Source Routing
- Verified `extension/catch-script/catch.js` MAIN world injection and deep search hooks (`fetch`, `XMLHttpRequest`, `URL.createObjectURL`).
- Verified typed candidate categorization: `DIRECT_PROGRESSIVE`, `HLS_MANIFEST`, `DASH_MANIFEST`, `YOUTUBE_EXTRACTION`, `UNSUPPORTED`.
- Verified in `src/ipc/ipc_server.cpp` that progressive video files (`.mp4`, `.webm`, `.mkv`, etc.) bypass stream adapters and route directly to `TaskType::kHttpFile`.
- Verified YouTube watch URLs are guarded and never treated as raw downloadable files.

### Phase 4: YouTube Extraction and Media Worker Integration
- Verified `FindWorkerBinary` in `src/common/path_sanitizer.cpp` resolves tools deterministically relative to the executable (`tools/win-x64/`, `tools/`, parent directories, and system PATH).
- Verified `scripts/fetch-workers.ps1` provisions pinned `yt-dlp.exe` with SHA-256 verification (`66674953fe251b89f4d08c5f0e35e0728679bd67ab3d7d05c0562af101dd3e7a`).
- Verified `src/engine_stream/nm3u8dl_re_adapter.cpp` returns `ERR_STREAM_WORKER_FAILED` or `ERR_MANIFEST_PARSE_FAILED` on generic stream worker failures instead of false `ERR_YOUTUBE_*` errors.

### Phase 5: Compilation, Safe Regression Checks, and Final Report
- **Toolchain & Build Execution**:
  - Compiler: MSVC `19.38+` via MSBuild `18.8.2+ce25c0108` (.NET Framework)
  - Windows SDK: `10.0.26100.0` targeting Windows `10.0.26200`
  - Build Command: `powershell -ExecutionPolicy Bypass -File scripts\build-windows-msvc.ps1 -SkipTests` (Exit Code: 0)
- **PE32+ Binary Verification**:
  - `build\win-x64\flux.exe` -> AMD64 PE32+ Subsystem=2 (IMAGE_SUBSYSTEM_WINDOWS_GUI), Size=2036.5 KB
  - `build\win-x64\flux-nmh.exe` -> AMD64 PE32+ Subsystem=3 (IMAGE_SUBSYSTEM_WINDOWS_CUI), Size=54.5 KB
  - `build\win-x64\flux_unit_tests.exe` -> AMD64 PE32+ Subsystem=3 (IMAGE_SUBSYSTEM_WINDOWS_CUI), Size=2161.5 KB
  - `build\win-x64\N_m3u8DL-RE.exe` -> AMD64 PE32+ Subsystem=3 (IMAGE_SUBSYSTEM_WINDOWS_CUI), Size=334 KB
- **Worker Binary Verification**:
  - `tools\win-x64\N_m3u8DL-RE.exe` -> AMD64 PE32+ Subsystem=3, Size=16794.5 KB
  - `tools\win-x64\ffmpeg.exe` -> AMD64 PE32+ Subsystem=3, Size=165000 KB
  - `tools\win-x64\yt-dlp.exe` -> AMD64 PE32+ Subsystem=3, Size=17422.3 KB
- **Staged Portable Distribution Artifacts (`dist/`)**:
  - `dist\Flux.exe`: SHA-256 `C8C128E21C0EC8F1DF3A6F5B6890EE0489FF88AD83539150CE76359D7565E84E`
  - `dist\flux-nmh.exe`: SHA-256 `52594B3F064FB943E2DA05CBCBAEE2A6A66A2D193D141657159E30FCC8C6E237`
  - `dist\tools\win-x64\N_m3u8DL-RE.exe`: SHA-256 `E5C93A8DE0C58C50922C17A8A3BABD7D2193FB43B1A7FACD0B18DE9274EF8061`
  - `dist\tools\win-x64\ffmpeg.exe`: SHA-256 `AA24A30E1194D4CE54999EFF72C58484B1A5AEE32DD9933A9242D15711DF1E48`
  - `dist\tools\win-x64\yt-dlp.exe`: SHA-256 `66674953FE251B89F4D08C5F0E35E0728679BD67AB3D7D05C0562AF101DD3E7A`
- **Safe Unit & Regression Test Results**:
  - **Browser Extension Unit Test Harness** (`node tests/test_extension_detection.js`):
    - **11 / 11 suites PASSED** (100% pass rate).
    - Verified URL parsing, candidate categorization, Alt bypass, and deduplication logic without launching browsers.
  - **Native C++20 Test Suite** (`build\win-x64\Release\flux_unit_tests.exe`):
    - **36 / 36 suites PASSED** (100% pass rate).
    - Verified PathSanitizer, DiskSpaceGuard, LoggerRedaction, CryptoUtils, BriskSegmentScheduler, SqliteStore, Nm3u8dlReAdapter, DownloadController, DesktopUiDesignSystem, CosmicStarfield, SettingsCenter, KeyboardNavigation, and HitTesting without network or stream downloads.

---

## 3. Truthful Reporting Standard & Verification Classification

In strict adherence to the **Truthful Reporting Standard** and the **Zero Live Network / Zero Transfer Rule** (Rules 11 & 12), the verification status is cataloged as follows:

| Verification Classification | Items Covered | Verification Details |
| :--- | :--- | :--- |
| **Verified by Execution** | - MSVC C++20 Release Build<br>- PE32+ Header & Architecture Check<br>- Node.js Isolated Extension Tests<br>- C++20 Automated Unit & Regression Tests (36 Suites) | Full local build passed with exit code 0. Both test runners (`node tests/test_extension_detection.js` and `flux_unit_tests.exe`) executed locally and produced 100% passing results with zero network dependency. |
| **Verified by Static Inspection** | - Windows Named Pipe Security (`\\.\pipe\OpenIdm_IPC_<SID>`)<br>- Native Messaging stdio framing<br>- MAIN-world injection security & nonce validation<br>- `onDeterminingFilename` `suggest()` before `cancel()` sequence<br>- Direct progressive video `.mp4` fast-path routing in `ipc_server.cpp` | Code paths, type safety, error codes, and thread-safety invariants inspected across modified files (`download_controller.cpp`, `nm3u8dl_re_adapter.cpp`, `ipc_server.cpp`, `win32_main_window.cpp`). |
| **Unverified (Requires Live Host / Prohibited by Rules 11 & 12)** | - Real video stream downloads from YouTube or external websites<br>- Live browser automation in Google Chrome or Brave<br>- End-to-end integration tests that transmit data over the internet | Not executed pursuant to user instructions explicitly forbidding live downloads, live website interactions, and browser automation. |

