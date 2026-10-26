# Flux Download Manager — Current State Audit

**Document Status:** Complete Phase 1 Audit  
**Date:** 2026-10-16  
**Scope:** Evaluates the current repository state, identifies defects and rebranding gaps, audits the local upstream repositories (`third_party/`), and defines the transition architecture for **Flux Download Manager** (short name: **Flux**).

---

## 1. Executive Summary

The repository was previously developed as "OpenIDM" (`v0.2.0`). While foundational C++20 engine components, SQLite persistence, and test suites were operational, the product exhibited several critical product completion, architectural, and branding deficiencies:

1. **Branding Inconsistencies**: All desktop window titles, dialogs, tray tooltips, process names, extension popups, manifests, and installer scripts bore the legacy "OpenIDM" name.
2. **Upstream Source Integration Traceability**: Upstream code from **Brisk**, **Cat Catch**, and **N_m3u8DL-RE** was present conceptually, but the repositories had not been cloned into a dedicated local `third_party/` directory for direct inspection, differential analysis, and verifiable component mapping.
3. **Desktop Interface Polish**: The Win32 UI used basic GDI primitives and a generic palette rather than a cohesive modern Windows 11 design system with polished typography, restrained blue accents, responsive layout geometry, and comprehensive dialogs.
4. **Data Directory & Migration**: Application data resided under `%LOCALAPPDATA%\OpenIDM` without any migration path for user settings, task databases, or active downloads when upgrading to Flux.
5. **Worker Management**: The project contained both a lightweight C++ stub worker (`tools/N_m3u8DL-RE`) and the official upstream release binary (`tools/win-x64/N_m3u8DL-RE.exe` v0.3.0), requiring unified resolution and runtime discovery.

This audit establishes the baseline for executing all 10 phases to deliver **Flux Download Manager**.

---

## 2. Upstream Repositories Audit (`third_party/`)

The three upstream repositories have been verified locally in the `third_party/` directory:

| Repository | Path | Version / Latest Commit | Key Source Modules to Leverage |
| :--- | :--- | :--- | :--- |
| **Brisk** | `third_party/brisk` | `517c669` (Sep 18, 2026) | `brisk_download_engine/lib/src/download_engine/`: `download_segment_tree.dart`, `http_download_engine.dart`, `http_download_connection.dart` (segmented range scheduling, work stealing, connection redistribution). |
| **Cat Catch** | `third_party/cat-catch` | `4e1a158` (Oct 15, 2026) | `js/background.js`, `js/content-script.js`, `js/function.js`, `catch-script/catch.js`, `popup.html`, `js/popup.js` (media MIME classification, HLS/DASH manifest detection, master/variant playlist grouping, page hook deep search). |
| **N_m3u8DL-RE** | `third_party/N_m3u8DL-RE` | `9d995eb` (Oct 12, 2026) | `src/N_m3u8DL-RE/` (CLI argument parsing, response file `@job.rsp` processing, HLS/DASH/MSS parsing, segment downloading, FFmpeg muxing). |

### 2.1 Binaries in `tools/win-x64/`
- `tools/win-x64/N_m3u8DL-RE.exe`: **Verified by Execution** (`0.3.0+ea9de55eacbd5e9dabf8dd92f5991ab1a37d80f2`, AMD64 PE32+).
- `tools/win-x64/ffmpeg.exe`: **Verified by Execution** (`N-127564-g3c92f003c7-20261015`, AMD64 PE32+ with libx264, libx265, libmp3lame, libopus, and Schannel).

---

## 3. Codebase Inventory and Defect Audit

### 3.1 Build System & Targets
- **Target Names**: Previously produced `openidm.exe`, `openidm-nmh.exe`, and `openidm_unit_tests.exe`.
  - **Required Change**: Produce `flux.exe` (GUI subsystem 2), `flux-nmh.exe` (CUI subsystem 3), and `flux_unit_tests.exe`.
- **Compiler Support**: Visual Studio 2026 (`C:\Program Files\Microsoft Visual Studio\18\Community`) and Windows SDK `10.0.26100.0` verified on the host machine.
- **CMakeLists.txt**: Needs renaming of project, targets, test names, and include paths.

### 3.2 Desktop Interface (`src/ui/`)
- Window class was `OpenIDM_MainWindowClass`, window title `OpenIDM`.
- Tray icon tip was `OpenIDM Download Manager`.
- Dialog titles and text referenced OpenIDM.
- Theme accent was `#3B82F6` with default dark background `#18181B`. Needs refinement into Flux's signature design palette (Flux Deep Navy `#0F172A` / `#1E293B` and Electric Blue accent `#2563EB`).
- About dialog must display **Flux Download Manager**, product version, license (`GPL-3.0-or-later`), and clear attribution to Brisk, Cat Catch, and N_m3u8DL-RE.

### 3.3 Storage and Path Handling (`src/common/`, `src/storage/`)
- Default data dir was `%LOCALAPPDATA%\OpenIDM`.
  - **Required Change**: Default data directory must be `%LOCALAPPDATA%\Flux Download Manager`.
  - **Migration Invariant**: On startup, if `%LOCALAPPDATA%\Flux Download Manager` does not exist or lacks `state.db`, check `%LOCALAPPDATA%\OpenIDM`. If found, safely migrate or copy `state.db`, settings, and log files without data loss or corruption.
- Default downloads directory: `%USERPROFILE%\Downloads\Flux`.

### 3.4 IPC & Native Messaging (`src/ipc/`, `src/nmh/`)
- Named Pipe: Was `\\.\pipe\OpenIDM_IPC_Default`.
  - **Required Change**: Primary pipe becomes `\\.\pipe\Flux_IPC_Default`. For seamless backward compatibility, the server also listens on or accepts connections from legacy clients, and `flux-nmh.exe` attempts `Flux_IPC_Default` first.
- Host Manifests: Previously `org.openidm.native.chromium.json`.
  - **Required Change**: Register `com.flux.downloadmanager` and `org.flux.native` native messaging manifests.

### 3.5 Browser Extension (`extension/`)
- `manifest.json`, `manifest.chromium.json`, `manifest.firefox.json` referenced OpenIDM.
- `popup.html` and `popup.js` displayed "OpenIDM Capture".
- Content script overlay button text was "Download with OpenIDM".
  - **Required Change**: Rebrand to "Download with Flux", with Flux styling, logo, and clean responsive overlay geometry.

---

## 4. Phase-by-Phase Execution Roadmap

1. **Phase 1**: Audit codebase, map IDM findings, map upstream source, establish branding document.
2. **Phase 2**: Rebrand build targets (`flux.exe`, `flux-nmh.exe`), implement data migration, create Windows `.rc` resources.
3. **Phase 3**: Polish desktop UI (Flux design tokens, high-DPI scaling, dialogs, sidebar filtering, task table).
4. **Phase 4**: Rebrand and enhance browser extension (Cat Catch detection, MV3 session storage, status indicators).
5. **Phase 5**: Build IDM-style video overlay button (in-page Closed Shadow DOM, dynamic video tracking, fullscreen sync).
6. **Phase 6**: Native messaging host & named pipe IPC handoff with automatic daemon launch.
7. **Phase 7**: Full integration of Brisk HTTP engine and N_m3u8DL-RE stream pipeline.
8. **Phase 8**: Application reliability fixes, error codes, bounds checks, thread safety.
9. **Phase 9**: Windows 11 build verification, PE header validation, test suites execution, performance benchmarks.
10. **Phase 10**: Installer (`installer/flux.iss`), rebranding completeness audit (`docs/FLUX_REBRANDING_AUDIT.md`), and final delivery report.
