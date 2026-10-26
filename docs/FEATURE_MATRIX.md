# Radium Download Manager -- Requirement-to-Implementation Feature Matrix

**Document Status:** Verified Implementation Traceability Matrix (`v0.3.0` — Production Readiness Verified)  
**Last Reviewed:** 2026-10-20  
**Scope:** Maps every functional (`FR-001`–`FR-038`) and non-functional (`NFR-001`–`NFR-016`) requirement to its actual source module, upstream reference, verification classification, and test coverage.  
**Related Documents:** [PRODUCT_REQUIREMENTS.md](PRODUCT_REQUIREMENTS.md), [RELEASE_READINESS_REPORT.md](RELEASE_READINESS_REPORT.md), [TEST_MATRIX.md](TEST_MATRIX.md), [TEST_STRATEGY.md](TEST_STRATEGY.md), [BUG_FIX_LOG.md](BUG_FIX_LOG.md)

---

## 1. Verification Classification Definitions

- **`Verified by Execution`:** Compiled and verified in the automated test harness on Windows 11 host (`radium_unit_tests.exe`, `test_extension_detection.js`, `test_installer_smoke.py`, `test_extension_runtime.js`, `verify-release-security.ps1`, `run_all_integration_tests.py`, `run-benchmarks.py`).
- **`Verified by Static Inspection`:** Implemented in C++20/Win32 (`WinHTTP`, `CreateWindowExW`, `CreateJobObjectW`, Named Pipes, DPAPI) and verified for API/algorithmic correctness.
- **`Unverified (OSS Signing Key Limitation)`:** Authenticode code signing credentials in open-source builds without private signing keys (verified as clean non-elevated unsigned release).

---

## 2. Functional Requirements Matrix (`FR-001` to `FR-038`)

| Req ID | Feature Summary | Responsible Module (`src/` or `extension/`) | Upstream Reference | Verification Status | Mapped Tests |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **FR-001** | HTTP/HTTPS Probe, WinHTTP Proxy & Mirror URL Failover | `src/brisk_engine/http_client.cpp`, `http_download_engine.cpp` | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) | `Verified by Static Inspection` (Phase 2 proxy/mirrors) | `UT-HTTP-01`, `IT-HTTP-01`, `TestAlternateUrlsAndMirrorValidation` |
| **FR-002** | Single-Stream Fallback for Non-Range Servers | `src/brisk_engine/http_download_engine.cpp` | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) | `Verified by Execution` | `IT-HTTP-02` |
| **FR-003** | Dynamic Connection Spawn, Adaptive Scale-Down & Work-Stealing Reuse | `src/brisk_engine/segment_scheduler.cpp`, `http_download_engine.cpp` | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) | `Verified by Execution` | `UT-SEG-01`, `IT-HTTP-03`, `TC-CORE-09` |
| **FR-004** | Stall Detection, Alternate URL Failover, Reset Recovery & Secondary Rollback | `src/brisk_engine/http_download_engine.cpp`, `src/core/download_controller.cpp` | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) | `Verified by Execution` | `IT-HTTP-04`, `UT-SEG-02`, `TC-CORE-10`, `TestAlternateUrlsAndMirrorValidation` |
| **FR-005** | Pause, Crash-Safe Resume & Validator Check | `src/core/download_controller.cpp`, `src/storage/sqlite_store.cpp` | Independent Design | `Partially Verified: unit regressions pass; live multi-engine GUI stress remains pending` | `TestPauseAllManualPauseScheduleAndRecovery`; HTTP/torrent/stream stress pending |
| **FR-006** | Expired URL Refresh ("Refresh Download Address") | `src/core/download_controller.cpp` (`RefreshTaskUrl`) | Independent Design | `Verified by Execution` | `UT-CTRL-01` |
| **FR-007** | Post-Download Checksum Verification (`SHA-256`) | `src/common/crypto_utils.cpp` | Independent Design | `Verified by Execution` | `UT-HASH-01` |
| **FR-008** | Automatic Browser Download & Link-Click Interception + Alt Bypass | `extension/js/content-script.js`, `extension/js/background.js`, `extension/js/function.js` | Independent State Machine Architecture | `Verified by Execution` (Non-invasive click correlation, Chromium `onDeterminingFilename` synchronization, and 22 edge scenarios) | `tests/test_extension_detection.js` (15/15 suites, 23/23 scenarios), `test_extension_runtime.js` |
| **FR-009** | Bypass Key & Per-Site Exclusion List | `extension/js/function.js`, `extension/js/background.js` | [xifangczy/cat-catch](https://github.com/xifangczy/cat-catch) | `Verified by Execution` | `Suite 3/10` |
| **FR-010** | Batch Link Capture (`download.batchAdd`) | `src/core/download_controller.cpp`, `src/ipc/ipc_server.cpp` | [BrisklyDev/brisk-browser-extension](https://github.com/BrisklyDev/brisk-browser-extension) | `Verified by Execution` | `IT-IPC-01` |
| **FR-011** | Desktop UI Modernization, Coral-Charcoal Identity (`#FF4757`), Modal Bounds & Header Sorting | `src/ui/design_system.hpp`, `src/ui/win32_main_window.cpp` | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) | `MSVC Release build verified; setup-dialog visual verification pending` | `Suites 8, 9, 10, 11/13`, `Suite 52`, `IT-UI-01` |
| **FR-012** | Conflict Prevention (`byExtensionId` & `blob:`/`data:` native delegation) | `extension/js/background.js`, `extension/catch-script/catch.js` | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) | `Verified by Execution` | `Suite 6/11` |
| **FR-013** | Nonce-Validated Page Hook Bridge & MAIN-world Injection | `extension/catch-script/catch.js`, `extension/js/content-script.js` | [xifangczy/cat-catch](https://github.com/xifangczy/cat-catch) | `Verified by Execution` | `Suite 4/11` (`BUG-032`) |
| **FR-014** | In-Page Playback Hook, Network Sniffing & MV3 Session Persistence | `extension/js/content-script.js`, `extension/js/background.js` | [xifangczy/cat-catch](https://github.com/xifangczy/cat-catch) | `Verified by Execution` | `Suites 1, 2, 7, 10/11` (`BUG-032`, `BUG-033`), `test_extension_runtime.js` |
| **FR-015** | Non-Intrusive Closed Shadow DOM Video Overlay + Fullscreen Sync | `extension/js/content-script.js` | [BrisklyDev/brisk-browser-extension](https://github.com/BrisklyDev/brisk-browser-extension) | `Verified by Execution` | `Suite 5/11`, `test_extension_runtime.js` |
| **FR-016** | Master vs. Media Manifest Deduplication & Grouping | `extension/js/function.js` (`addOrDeduplicateMediaCandidate`) | [xifangczy/cat-catch](https://github.com/xifangczy/cat-catch) | `Verified by Execution` | `Suite 2/11` |
| **FR-017** | Opt-In Page-Context ("Deep Search") Hook | `extension/catch-script/catch.js` | [xifangczy/cat-catch](https://github.com/xifangczy/cat-catch) | `Verified by Execution` | `Suite 4/11` |
| **FR-018** | Live Stream Discovery Metadata (`isLive`) | `src/worker_stream/nm3u8dl_re_worker.cpp`, `src/engine_stream/` | [nilaoda/N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) | `Verified by Execution` | `UT-STREAM-01` |
| **FR-019** | Out-of-Process Manifest Probing, Redirect Resolution, URL Expired, Multi-Track Progress & Speed Detection | `src/engine_stream/nm3u8dl_re_adapter.cpp`, `src/common/path_sanitizer.cpp` | [nilaoda/N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) | `Verified by Execution` | `Suite 6/13`, `Suite 7/13`, `Suite 53`, `Suite 56`, `IT-STREAM-01` (`BUG-034`) |
| **FR-020** | Compiled Native C++20 Stream Worker & FFmpeg Muxing | `src/worker_stream/nm3u8dl_re_worker.cpp`, `tools/win-x64/` | [nilaoda/N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) + `FFmpeg` | `Verified by Execution` | `IT-BIN-01`, `IT-STREAM-02`, `IT-STREAM-03` |
| **FR-021** | Safe `@job.rsp` Credential Passing & Job Object Termination | `src/engine_stream/nm3u8dl_re_adapter.cpp` | Independent Security Design | `Verified by Execution` | `SEC-PROC-01`, `IT-STREAM-06` |
| **FR-022** | Standard HLS AES-128 In-Process Decryption Support | `src/worker_stream/nm3u8dl_re_worker.cpp` (`DecryptAes128Cbc`) | [nilaoda/N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) | `Verified by Execution` | `IT-STREAM-06` |
| **FR-023** | Hierarchical Category Routing, Site Rules & Task Lifecycle | `src/core/download_controller.cpp`, `src/ui/win32_main_window.cpp` | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) | `Verified by Execution` | `UT-CTRL-01`, `UT-UI-02`, `TestDynamicCategorizationAndSiteRulesPriority` |
| **FR-024** | Queue Concurrency, Dual Token-Bucket Speed Limiter & Real Aggregate Metrics | `src/core/download_controller.cpp`, `src/brisk_engine/http_download_engine.cpp` | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) | `Verified by Execution` | `UT-UI-02`, `TestDualRateLimiterAndPerTaskSpeedLimit`, `Suite 53` |
| **FR-025** | Scheduled Start/Stop Windows, Auto-Retries & Post-Queue Actions | `src/core/download_controller.cpp`, `src/storage/sqlite_store.cpp` | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) | `Partially Verified: scheduling and persistence regressions execute; transient retry deadline/UI stress remains pending` | `TestSchedulerStopTimesAndPostCompletionActions`, `TestPauseAllManualPauseScheduleAndRecovery` |
| **FR-026** | Atomic File Replacement, Pre-Flight Space Check, Auto-Rename, Recycle Bin, Permanent Output Delete & Remove from List | `src/common/path_sanitizer.cpp`, `src/core/download_controller.cpp`, `src/ui/win32_main_window.cpp` | Independent Design | `Implementation updated; MSVC Release build verified; native runtime verification pending` | `UT-FS-01`, `UT-FS-02`, `Suite 52` |
| **FR-027** | Per-User Native Messaging HKCU Registration | `scripts/register-native-messaging.ps1` | Independent Design | `Verified by Execution` | HKCU Registry Keys Verified (`BUG-035`) |
| **FR-028** | Release Integrity Verification (`SHA256SUMS.txt`) | `dist/SHA256SUMS.txt`, `scripts/build-windows-msvc.ps1` | Independent Design | `Verified by Execution` | `IT-BIN-01`, `verify-release-security.ps1` |
| **FR-029** | RFC 959 / 2428 / 3659 FTP Engine (EPSV, PASV, NAT Bypass, REST 350 Truncation) | `src/engine_ftp/ftp_client.cpp` | Independent RFC Implementation | `Verified by Execution` | `TestFtpClientParsingAndCommands`, `TestFtpReliabilityEpsvAndResumeTruncation` |
| **FR-030** | BitTorrent BEP 3 Peer Wire Protocol, Selective Multi-File Storage, Magnet Metadata Selection, DHT Discovery, Incoming Verified-Block Upload, Unchoke Accounting & Live Received-Block Progress | `src/engine_torrent/torrent_engine.cpp`, `src/engine_torrent/peer_wire_protocol.hpp`, `src/engine_torrent/piece_manager.hpp`, `src/engine_torrent/multi_file_storage.hpp`, `src/engine_torrent/tracker_client.hpp`, `src/engine_torrent/dht_client.hpp`, `src/ui/win32_main_window.cpp` | BEP 3 / BEP 5 / BEP 9 Specification & aria2 / Motrix Architecture | `Partially Verified: peer handshakes and tracker discovery observed live; UI progress for unverified blocks requires Windows runtime verification` | `TestBencodeAndTorrentParser`, `TestPeerWireProtocolFramingAndCompactPeers`, `TestMultiFileStorageMappingAndBoundarySplitting`, `TestPieceManagerAndBlockScheduling`, `TestTrackerClientQueryEncodingAndResponseParsing`, `TestMagnetMetadataExchangeBEP9Live`, `Suite 53` |
| **FR-031** | RTSP, MMS & RTMP Legacy Live Streaming Ingestion | `src/engine_stream/rtsp_mms_adapter.cpp` | FFmpeg Integration Protocol | `Verified by Execution` | `TestRtspMmsStreamDetection` |
| **FR-032** | Recursive Breadth-First Site Grabber & Media Crawler (HTML/CSS & SSRF Guard) | `src/crawler/site_grabber.cpp` | Independent Architecture | `Verified by Execution` | `TestSiteGrabberCrawlerAndSsrf` |
| **FR-033** | Pre-Extraction ZIP Archive Previewer with Zip Slip & Bomb Guards | `src/archive/zip_preview.cpp` | PKWARE APPNOTE.TXT Specification | `Verified by Execution` | `TestZipArchivePreviewAndSecurity` |
| **FR-034** | Post-Download Windows Defender Antivirus Scanner & Sound Alert (with Threat Execution Block) | `src/security/antivirus_scanner.cpp`, `src/core/download_controller.cpp`, `src/ui/win32_main_window.cpp` | Windows Defender MpCmdRun Integration | `Verified by Execution` | `TestAntivirusScannerSafetyAndHandling` |
| **FR-035** | Multi-Format Task Import/Export (JSON/CSV) with Credential Scrubbing | `src/storage/import_export.cpp` | Independent Privacy Architecture | `Verified by Execution` | `TestTaskImportExportAndMultiQueue` |
| **FR-036** | Dual Portable Packaging (Full & Core) & Lowest-Privilege Installer (< 20 MB Full) | `scripts/package-release-zip.ps1`, `installer/flux.iss`, `scripts/build-minimal-ffmpeg.ps1` | Independent Release Architecture | `Verified by Execution` | `ISCC` compilation (19.28 MB Full, 2.89 MB Core), clean `/VERYSILENT` extraction test, `report-package-sizes.ps1` |
| **FR-037** | Automated GitHub Actions Competitor Benchmark Lab (Flux vs aria2, IDM, FDM) | `.github/workflows/benchmark-comparison.yml`, `scripts/run-competitor-benchmarks.py`, `scripts/benchmark_server.py` | Independent Benchmarking Architecture | `Verified by Execution` | Workflow automation, localhost RFC 7233 throttled test server, SHA-256 verification, `benchmarks/LATEST_COMPARISON.md` |
| **FR-038** | GitHub-Only Production Readiness & Clean Runner Smoke Testing | `.github/workflows/windows-release.yml`, `tests/test_installer_smoke.py`, `tests/browser/test_extension_runtime.js`, `scripts/verify-release-security.ps1` | Independent CI/CD Architecture | `Verified by Execution` | 4-job pipeline, clean runner install test, Playwright Chromium test, guarded release publishing |
| **FR-039** | YouTube Selected Quality Integrity, ffprobe Media Verification & Progress State Tracking | `src/engine_stream/nm3u8dl_re_adapter.cpp`, `src/core/download_controller.cpp`, `src/ui/win32_main_window.cpp` | Independent Quality & Verification Architecture | `Verified by Execution` | Strict resolution matching without 360p fallback, ffprobe media validation, multi-phase states (`kVerifying`, `kFinalizing`), 100% progress integrity guard (`TestYouTubeQualitySelectionIntegrityAndVerification`) |

---

## 3. Non-Functional Requirements Matrix (`NFR-001` to `NFR-016`)

| Req ID | Requirement Summary | Responsible Module | Verification Status | Measured / Verified Result |
| :--- | :--- | :--- | :--- | :--- |
| **NFR-001** | Core Binary Size (`<= 3.0 MB` uncompressed) | `CMakeLists.txt` | `Verified by Execution` | `2,047.0 KB` (`flux.exe` 2,040.3 KB + `flux-nmh.exe` 55.8 KB <= 3,072 KB budget) |
| **NFR-002** | Startup Latency (`< 150 ms` cold) | `src/app/main.cpp`, `src/common/path_sanitizer.cpp` | `Verified by Execution` | `71.05 ms` median (`flux --gui-snapshot`) |
| **NFR-003** | Idle Daemon & HTTP Memory Budget | `src/brisk_engine/http_download_engine.cpp` | `Verified by Execution` | Idle RSS: `10.34 MB` (<= 25 MB); Peak HTTP 8-Conn: `15.7 MB` (<= 65 MB); Peak Stream Worker: `11.04 MB` (<= 65 MB) |
| **NFR-004** | Multi-Connection Segmented Throughput | `src/brisk_engine/http_download_engine.cpp` | `Verified by controlled local execution; live-network saturation unverified` | Added an in-flight resplit guard after 3 baseline / 5 guarded identical 16 MiB loopback trials with artificial response delay: median transfer `770 ms -> 516 ms`, range requests `17 -> 6`, SHA-256 exact in every run. This synthetic result does not establish line-rate or IDM superiority. Additional trace details and limitations are in `TEST_MATRIX.md`. |
| **NFR-005** | Crash-Safe SQLite WAL/TRUNCATE State Persistence | `src/storage/sqlite_store.cpp` | `Verified by Execution` | 64 KiB safety rollback verified (`TestSqliteStoreAndCrashRecovery`) |
| **NFR-006** | Path & Filename Traversal Sanitization | `src/common/path_sanitizer.cpp` | `Verified by Execution` | Blocks `../`, NTFS ADS, DOS names, and `U+202E` BiDi (`TestPathSanitizerAndContentDisposition`) |
| **NFR-007** | Zero Unauthenticated Network Listeners | `src/ipc/ipc_server.cpp` | `Verified by Execution` | Uses Windows Named Pipe with `PIPE_REJECT_REMOTE_CLIENTS`; zero TCP listening ports |
| **NFR-008** | Named Pipe Per-User Access Control | `src/ipc/ipc_server.cpp`, `src/common/ipc_path.hpp` | `Verified by Execution` | Per-user SID ACL on `\\.\pipe\Flux_IPC_<UserSID>` |
| **NFR-009** | DPAPI / AES-256-GCM Secret Encryption & Redaction | `src/common/crypto_utils.cpp`, `src/common/logger.cpp`, `src/ui/win32_main_window.cpp` | `Verified by Execution` | Encrypted at rest in `state.db`, wiped on completion, `[REDACTED]` in logs and UI Details Panel |
| **NFR-010** | Zero Telemetry by Default | All modules | `Verified by Execution` | Zero background telemetry or analytics endpoints |
| **NFR-011** | Per-Monitor v2 High-DPI & Keyboard Accessibility | `src/ui/design_system.hpp`, `src/ui/win32_main_window.cpp` | `Verified by Execution` | Tested at `96`/`120`/`144`/`192` DPI (`UT-UI-01`, `UT-KEY-01`) |
| **NFR-012** | Full UTF-8 / UTF-16LE Unicode Path Handling | `src/common/path_sanitizer.cpp`, `src/brisk_engine/http_client.cpp` | `Verified by Execution` | Verified with CJK RFC 5987 `测试视频_1080p.mp4` (`UT-HTTP-01`) |
| **NFR-013** | Cross-Browser MV3 Build (Chrome, Edge, Brave, Firefox) | `extension/manifest.json`, `extension/manifest.firefox.json` | `Verified by Execution` | `15 / 15` suites, `23 / 23` edge scenarios passing (`tests/test_extension_detection.js`), Playwright Chromium runtime |
| **NFR-014** | Clean Engine & Adapter Boundaries | `src/brisk_engine/`, `src/engine_stream/`, `src/core/` | `Verified by Execution` | Verified across `56 / 56` C++ unit suites |
| **NFR-015** | Strict DRM Non-Circumvention (`ERR_STREAM_DRM_PROTECTED`) | `src/engine_stream/nm3u8dl_re_adapter.cpp`, `src/worker_stream/nm3u8dl_re_worker.cpp` | `Verified by Execution` | Widevine/CENC immediately rejected (`TestStreamMetaJsonAndDrmRejection`) |
| **NFR-016** | Cross-Architecture Clean Compilation Readiness | `CMakeLists.txt` | `Verified by Execution` | Compiles cleanly with MSVC `/W4` `/WX` |
| **NFR-017** | Opt-in HTTP runtime performance telemetry | `src/brisk_engine/http_download_engine.cpp`, `src/brisk_engine/segment_scheduler.cpp`, `benchmarks/scripts/run_local_benchmark.py`, `scripts/benchmark_server.py` | `Verified by Execution (local fixture; internet behavior unverified)` | Request/phase/worker, scheduler and segment-write lock, file-write, and wait telemetry reconciled with fixture logs and output SHA-256 across 24 repeated 128 MiB local trials at 4/8 connections; see `benchmarks/results/local_lock_disk_audit_20261026/report.md` |

### Follow-up verification (2026-10-23)

| Area | Implementation update | Verification status |
| :--- | :--- | :--- |
| Current-page media list | Per-tab document URL reconciliation and SPA/fragment navigation listeners clear stale candidates without affecting other tabs | Automated regression added; live Brave/site verification pending |
| Floating player control | Compact control positioned above the player and aligned with the player container | Geometry/style regression added; live embedded-player and scroll verification pending |

| Area | Implementation update | Verification status |
| :--- | :--- | :--- |
| Media size and filename | Direct progressive size lookup uses HEAD then a one-byte Range fallback; numeric filename suffixes receive a MIME-derived extension | Node regression passed; live authenticated media cases remain pending |
| New Download dialog | Label/input rows and lower controls have added vertical separation at the existing DPI scale | Source changed; rebuild/runtime verification pending |
| Row context menu | Global commands are only shown in the blank-window Application menu; settings gear and Settings Center remain available | Source changed; rebuild/runtime verification pending |

| New Download form spacing | Shared DPI-scaled field-row geometry preserves clearance between labels and controls | C++ layout regression added; rebuilt application screenshot pending |


| Task context menu | Only state-appropriate Start/Pause/Resume/Retry/Cancel actions appear; Stop Seeding is nested under More Actions for completed torrents with upload history | Rebuilt source; runtime menu screenshot verification pending |


| Brave installed persistence | Fresh profile inspection distinguishes stale Secure Preferences from an actual extension-list entry; stable LocalAppData source staged | Live UI shows extension absent. Load and restart verification pending required confirmation; historical removal cause undetermined. |

| Task removal persistence | Controller stops active work, cleans task-owned partial data, conditionally deletes completed output, then deletes the SQLite row before erasing the in-memory task; storage errors remain visible | Windows x64 Release regression run passed 71/71 C++ suites, including output preservation, successful/missing deletion, and read-only failure plus retry. |

| Torrent file entry point | Local torrent files and browser offers with a `.torrent` URL or suggested filename open the dedicated Add Torrent selector; remote metainfo retrieval is bounded to 16 MiB and runs off the UI thread; selection, category, destination, and start choice update the queued task | MSVC Release build and 71/71 existing C++ suites passed; the new remote browser-offer path and controlled selective transfer still need live UI verification. |


| Native compact table and New Download dialog | Saved widths shrink to preserve Name space; filename/progress drawing stays inside Name; compact single-row options reduce the dialog to 520×480 DPI-scaled px while retaining all controls | MSVC Release build and 71/71 C++ suites passed; staged Radium UI visually confirmed |

| Native task-row hover | Hover background is drawn at full strength and old/new row regions redraw synchronously on mouse movement | MSVC Release build and 71/71 C++ suites passed; updated executable staged in `dist/Radium.exe` |
