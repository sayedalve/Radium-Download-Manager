# Engineering Roadmap & Implementation Milestones

**Document Status:** Proposed Execution Roadmap  
**Last Reviewed:** 2026-10-16  
**Scope:** Phased implementation plan from greenfield repository (`M0`) to production release (`M5`), including goals, dependencies, exit criteria, risks, and required verification tests for each milestone.  
**Related Documents:** [PROJECT_BRIEF.md](PROJECT_BRIEF.md), [PRODUCT_REQUIREMENTS.md](PRODUCT_REQUIREMENTS.md), [FEATURE_MATRIX.md](FEATURE_MATRIX.md), [TEST_MATRIX.md](TEST_MATRIX.md)

---

## 1. Milestone Overview Diagram

```mermaid
gantt
    title OpenIDM Implementation Roadmap (Milestones M0 - M5)
    dateFormat  YYYY-MM-DD
    section Foundation
    M0: Architecture & Engineering Docs (Completed) :done, m0, 2026-10-16, 1d
    M1: Core Win32 App, SQLite Store & HTTP Engine  :active, m1, after m0, 21d
    section Browser & Streams
    M2: Native Messaging Host & Browser Extension   :m2, after m1, 14d
    M3: Stream Pipeline (N_m3u8DL-RE + FFmpeg Mux)  :m3, after m2, 14d
    section Polish & Release
    M4: Queues, Firefox Port, Opt-in Hook & Installer:m4, after m3, 14d
    M5: Signed Updater, Checksums, Live & ARM64     :m5, after m4, 14d
```

---

## 2. Detailed Milestone Specifications

### Milestone 0 (`M0`): Engineering Documentation & Architecture Baseline *(Completed 2026-10-16)*
- **Goals:** Inspect workspace and upstream projects (`Brisk`, `Cat Catch`, `N_m3u8DL-RE`); establish C++20/Win32 architecture, ADRs (`0001`–`0008`), security threat model, and complete Markdown documentation suite.
- **Exit Criteria:** All 35+ required Markdown files created and validated via [DOCUMENTATION_AUDIT.md](archive/maintenance-audits/DOCUMENTATION_AUDIT.md).

---

### Milestone 1 (`M1` — Recommended First Implementation Milestone): Core Win32 Shell, SQLite Persistence & Ordinary HTTP Segmented Engine
- **Goals:** Build a rock-solid standalone Windows 11 download manager (`openidm.exe`) capable of multi-connection HTTP/HTTPS segmented downloading, pause/resume, crash recovery, and basic queue management before adding browser or stream complexity.
- **Requirements Covered:** `FR-001`, `FR-002`, `FR-003`, `FR-004`, `FR-005`, `FR-011`, `FR-023`, `FR-024`, `FR-026`, `NFR-001`–`NFR-006`, `NFR-009`–`NFR-011`, `NFR-014`.
- **Deliverables:**
  1. Initialize Git repository, `CMakeLists.txt`, `vcpkg.json` (`sqlite3`, `nlohmann-json`, `wil`, `gtest`), and `src/app/openidm.manifest` (`PerMonitorV2`, Common Controls v6).
  2. Implement `src/common/` (`PathSanitizer`, `DpapiProtector`, redacted `Logger`, `FileCommitter` atomic rename + `Zone.Identifier`).
  3. Implement `src/storage/SqliteStore` (`state.db` in `WAL` mode, 1 Hz batched segment checkpointing, crash recovery reconciliation).
  4. Implement `src/engine_http/WinHttpDownloadEngine` and `SegmentScheduler` (Brisk-style dynamic connection spawn, work-stealing byte-range split, stall detection/reset, and non-range single-stream fallback).
  5. Implement `src/core/DownloadController`, `QueueScheduler`, and token-bucket `BandwidthLimiter`.
  6. Implement `src/ui/` (`MainWindow` with Virtual ListView `LVS_OWNERDATA`, Category Sidebar, `AddDownloadDialog`, `TaskProgressDialog` with segment progress bar, System Tray icon, and `Ctrl+Alt+N` clipboard hotkey).
- **Dependencies:** MSVC 2022 + Windows 11 SDK + vcpkg.
- **Risks Addressed:** `RISK-006` (misbehaving range servers), `RISK-009` (Win32 DPI scaling), `THREAT-003` (path traversal).
- **Required Exit Tests:** `UT-HTTP-01`, `UT-SEG-01`, `UT-CORE-01..02`, `UT-FS-01..02`, `SEC-PATH-01`, `SEC-PRIV-01`, `IT-HTTP-01..05`, `IT-HTTP-07`, `IT-CRASH-01`, `PERF-SIZE-01`, `PERF-START-01`, `PERF-MEM-01`, `PERF-NET-01`.

---

### Milestone 2 (`M2`): Native Messaging IPC Bridge & Chromium Browser Extension
- **Goals:** Connect Chrome, Edge, and Brave to `openidm.exe` via `openidm-nmh.exe` and the Named Pipe IPC server; enable automatic ordinary file interception and passive media/manifest discovery with the in-page Shadow DOM video button.
- **Requirements Covered:** `FR-006`, `FR-008`, `FR-009`, `FR-010`, `FR-014`, `FR-015`, `FR-016`, `NFR-007`, `NFR-008`, `NFR-013` (Chromium).
- **Deliverables:**
  1. Implement `src/ipc/NamedPipeServer` (`\\.\pipe\OpenIDM_IPC_<UserSID>` with per-user DACL and `FILE_FLAG_FIRST_PIPE_INSTANCE`).
  2. Implement `src/nmh/main.cpp` (`openidm-nmh.exe` stateless `stdio <-> Named Pipe` proxy with auto-wake of `openidm.exe --background`).
  3. Implement `extension/` in TypeScript: `DownloadInterceptor.ts`, `MediaSniffer.ts` (`webRequest`), `ManifestDeduplicator.ts`, `SessionStateStore.ts` (`chrome.storage.session`), `VideoOverlay.ts` (closed Shadow DOM button), and `Popup` UI.
  4. Implement "Refresh Download Address" (`FR-006`) workflow from browser to paused task.
- **Dependencies:** Completion of `M1`; Node.js 20+ toolchain.
- **Risks Addressed:** `RISK-001` (IPC security), `RISK-002` (MV3 Service Worker suspension), `THREAT-001`, `THREAT-006`.
- **Required Exit Tests:** `UT-EXT-01..03`, `SEC-NET-01`, `SEC-IPC-01`, `E2E-EXT-01..03`, `E2E-MEDIA-01..02`, `E2E-MEDIA-04`, `IT-HTTP-06`.

---

### Milestone 3 (`M3`): Adaptive Stream Download Pipeline (`N_m3u8DL-RE` + `FFmpeg`)
- **Goals:** Enable full HLS (`.m3u8`), DASH (`.mpd`), and MSS (`.ism`) manifest probing, Win32 Track Selection UI, concurrent segment downloading, standard AES-128 decryption, and container muxing to `.mp4` / `.mkv`.
- **Requirements Covered:** `FR-019`, `FR-020`, `FR-021`, `FR-022`, `NFR-015`.
- **Deliverables:**
  1. Create `scripts/fetch-workers.ps1` to provision SHA-256-verified `N_m3u8DL-RE.exe` (`v0.6.0-beta`) and `ffmpeg.exe`.
  2. Implement `src/engine_stream/Nm3u8dlReAdapter` and `MetaJsonParser` (ACL-restricted `@job.rsp` generation, Windows Job Object lifecycle, DRM rejection `ERR_STREAM_DRM_PROTECTED`, stdout progress parser).
  3. Implement `src/ui/TrackSelectorDialog` (native Win32 dialog displaying video resolutions/codecs, audio tracks, and subtitle tracks).
- **Dependencies:** Completion of `M2`; pinned `N_m3u8DL-RE.exe` and `ffmpeg.exe` binaries.
- **Risks Addressed:** `RISK-004`, `RISK-005` (`--vod-select-parts false`), `THREAT-002`, `THREAT-004`.
- **Required Exit Tests:** `UT-STREAM-01`, `IT-STREAM-01..04`, `IT-STREAM-06`, `SEC-PROC-01`, `SEC-MANIFEST-01`, `SEC-DRM-01`.

---

### Milestone 4 (`M4`): Scheduled Queues, Firefox Port, Opt-In Deep Search & Inno Setup Installer
- **Goals:** Complete `Should` requirements, Firefox MV3 extension packaging, opt-in Layer 3 page-context hook, and the dual-mode Inno Setup installer (`Full` and `Core` variants).
- **Requirements Covered:** `FR-007`, `FR-012`, `FR-017`, `FR-025`, `FR-027`, `NFR-012`, `NFR-013` (Firefox).
- **Required Exit Tests:** `UT-HASH-01`, `UT-CORE-03`, `E2E-EXT-04..05`, `E2E-MEDIA-03`, `SYS-INST-01..02`, `SYS-UI-01`.

---

### Milestone 5 (`M5`): Signed Auto-Updater, Live Recording & Windows ARM64 Build
- **Goals:** Implement Ed25519 + Authenticode update verification (`FR-028`), live stream recording controls (`FR-018`), UserScript message bridge (`FR-013`), and native `ARM64` compilation (`NFR-016`).
- **Required Exit Tests:** `SEC-UPD-01`, `SEC-EXT-01`, `IT-STREAM-05`, `SYS-BUILD-02`.
