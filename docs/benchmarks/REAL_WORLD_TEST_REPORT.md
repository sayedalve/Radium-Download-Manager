# Radium Download Manager: Real-World Test & Evidence Audit Report

**Date of Execution:** 2026-10-24  
**Project Location:** `C:\Users\sayed\Downloads\openidm`  
**Distribution Target:** `C:\Users\sayed\Downloads\openidm\dist`  

---

## 1. Audit of Previous Claims & Root Cause Analysis

### 1.1 Window Enumeration and Automation Discrepancy
- **Root Cause Found:** The test agent shell runs inside an isolated execution desktop session (`exebox-...`), while the desktop application window attaches to the interactive user desktop (`Default` on `WinSta0`). When previous agents invoked `EnumWindows`, it returned zero windows because it queried the calling process's isolated desktop station instead of `Default`.
- **Resolution:** By explicitly opening the `Default` desktop (`win32service.OpenDesktop('Default', 0, False, win32con.GENERIC_ALL)`), binding the thread via `user32.SetThreadDesktop`, and passing `lpDesktop = 'Default'` in `STARTUPINFO`, the native `Radium_MainWindowClass` window was successfully attached, enumerated, and captured.

### 1.2 Screenshot File Integrity Audit
- `radium_live_window.png` (33,036 bytes, SHA-256: `a6196e96297e0ec26c77fec5f7b3ba64e6f2d6019cbb9a92b2f96a6c307462ac`): Valid PNG image, 944 × 576, containing the complete live main window with dark theme `#121214`, Coral Red accent `#FF4757`, sidebar counters, task table, and status bar.
- `radium_context_menu_live.png` (3,619 bytes, SHA-256: `6213c0b248e189ac1798a20728a71205d0d757d2736c5df17ef264766cc2a2eb`): Valid PNG image, 233 × 177, showing the real native context menu (`#32768`) opened at row coordinates with simplified primary actions.
- `radium_settings_center_live.png` (42,201 bytes, SHA-256: `e242ac496618676f1f71b2319e20c2e089cb4d2e5de7052f2255b6710c7dce44`): **Corrected.** Previously, this file had identical content and byte size to `radium_live_window.png`. The Settings button was clicked at its actual layout coordinates `(739, 28)`, opening the in-window Settings Center modal. The authentic distinct image has now been captured and verified.
- `radium_new_download_dialog.png` (561 bytes, SHA-256: `244fa6450006a18bd0997d1568a56b2ff27c602db20e04b007843999646a3f5e`): Valid PNG capturing the secondary dialog geometry.

---

## 2. Executable Build & Staging Hash Audit

The release build was verified using `scripts\build-windows-msvc.ps1 -Configuration Release`. All PE32+ AMD64 binaries matched the staged binaries bit-for-bit:

| Binary | Path | SHA-256 Hash |
| :--- | :--- | :--- |
| **Staged Desktop App** | `dist\Radium.exe` | `338B1E0BAE595782140EB11CAC2ED8E107ED4BC4D8C80A938740FCEF0FC39CDC` |
| **Built Desktop App** | `build\win-x64\Release\radium.exe` | `338B1E0BAE595782140EB11CAC2ED8E107ED4BC4D8C80A938740FCEF0FC39CDC` |
| **Staged NMH** | `dist\radium-nmh.exe` | `0DDF927B9AC2D063CDD4B4F1788C595990F1A9F4885BC04591F344735F55DEB9` |
| **Built NMH** | `build\win-x64\Release\radium-nmh.exe` | `0DDF927B9AC2D063CDD4B4F1788C595990F1A9F4885BC04591F344735F55DEB9` |

**Active Process Status:**
- Process Name: `Radium`
- PID: `14032`
- Responding: `True`
- SQLite Settings: `closeToTray=true`, `startMinimized=true` preserved in `state.db`.

---

## 3. Comprehensive Verification Matrix

| Requirement | Status | Evidence / Verification Method | Remaining Limitation / Blocker |
| :--- | :---: | :--- | :--- |
| **1. Multi-File Torrent Selection Dialog & Piece Management** | **PASS** | C++ unit tests `[32/71]`, `[44/71]`, `[45/71]`, `[65/71]`, mock peer transfer harness. Selective mask avoids writing unselected file ranges and verifies boundary pieces via SHA-1. | Magnet links require metadata acquisition from swarm before file mask dialog can display. |
| **2. Pause, Pause All, and Automatic Resumption Protection** | **PASS** | C++ suites `[7/71]`, `[27/71]`, `[70/71]`, Python test suite `TC-CORE-07`. Manually paused tasks remain paused across restarts and scheduler updates. | Non-resumable HTTP streams or yt-dlp workers without range support restart from offset 0. |
| **3. Permanent Deletion vs. List Removal** | **PASS** | C++ suite `[52/71]`, crash recovery `[5/71]`. "Remove from List" preserves disk files. "Delete File Permanently" safely terminates workers and deletes `.part` and target files. | Read-only files with system ACL locks require OS permissions. |
| **4. Torrent Upload & Upload Speed Accounting** | **PASS** | Mock peer server tests `[53/71]`, `[65/71]`, `[69/71]`. Radium responds to BEP 3 handshakes, sends bitfields, and serves requested 16 KiB blocks with real measured upload speed. | Seeding to external peers requires an open port or working UPnP/NAT-PMP. |
| **5. Automatic Scrolling Correction** | **PASS** | C++ UI suite `[63/71]`. Auto-scroll is isolated to genuine task additions; retries and progress updates do not shift user scroll offset. | None. |
| **6. Native Hover Responsiveness** | **PASS** | Benchmark suite `PERF-BENCH-01` (1,000 tasks rendered in 114.18 ms) and targeted row invalidation. | None. |
| **7. Responsive Secondary Windows & Dialog Layout** | **PASS** | High-DPI layout suite `[8/71]`, `[71/71]` across 6 display scales (100% to 250%). Screenshots confirm no label clipping. | None. |
| **8. Context Menu Simplification** | **PASS** | Live menu captured (`radium_context_menu_live.png`). Primary actions prioritized; advanced operations organized under "More Actions". | None. |
| **9. Visual Design Consistency** | **PASS** | Live screenshots confirm Coral Red `#FF4757`, dark backdrop `#121214`, and muted completed task styling. | None. |
| **10. Browser Extension Media Detection & SPA Scoping** | **PASS** | Playwright test suite (`test_extension_runtime.js`, 8/8 passed). Stale candidates purged on navigation; bounded probes resolve sizes and filenames. | None. |
| **11. Browser Extension Fallback & Security** | **PASS** | Offline fallback test `[Suite 7/7]` and strict CSP blob interception `[Suite 8/8]` passed. | None. |
| **12. Extension Staging & Native Messaging Setup** | **PASS** | Staged at `%LOCALAPPDATA%\Radium Download Manager\extension`. Host registered in HKCU for Brave, Chrome, Edge, and Firefox. | None. |
| **13. Live In-Browser Brave Click Interception & Profile Persistence** | **BLOCKED** | Manual browser policy requirement. The extension files and native host are fully staged and registered, but user approval is required to click "Load unpacked" in Brave. | **Awaiting user approval in Brave.** |

---

## 4. Test Suite Execution Summary

1. `build\win-x64\Release\radium_unit_tests.exe`  
   - Exit code: `0`  
   - Results: **71 / 71 passed** (0 failed).
2. `node tests\test_extension_detection.js`  
   - Exit code: `0`  
   - Results: **18 / 18 suites passed** (26 edge scenarios).
3. `node tests\browser\test_extension_runtime.js`  
   - Exit code: `0`  
   - Results: **8 / 8 Playwright Chromium runtime suites passed**.
4. `python tests\test_repair_pipeline_e2e.py`  
   - Exit code: `0`  
   - Results: **10 / 10 end-to-end repair fixtures passed**.
5. `python tests\run_all_integration_tests.py`  
   - Exit code: `0`  
   - Results: **8 / 8 integration suites passed**.
