# Flux Download Manager -- Requirement-to-Test Verification Matrix

**Document Status:** Active Verification Matrix (`v0.3.0` — `62 / 62` C++20 Suites, `15 / 15` Extension Suites, `8 / 8` Smoke Suites, `7 / 7` Playwright Suites, and `5 / 5` Security Checks Executed & Passed on Windows 11 Host)  
**Last Reviewed:** 2026-10-20  
**Scope:** Maps every functional (`FR-001`–`FR-035`), non-functional (`NFR-001`–`NFR-016`), and bug-regression (`BUG-001`–`BUG-035`, `FDM-001`–`FDM-029`) test to its exact command, verification category, and outcome.  
**Related Documents:** [RELEASE_READINESS_REPORT.md](RELEASE_READINESS_REPORT.md), [BUG_FIX_LOG.md](BUG_FIX_LOG.md), [FEATURE_MATRIX.md](FEATURE_MATRIX.md), [TEST_STRATEGY.md](TEST_STRATEGY.md)

---

## 1. Automated Verification Summary (`Windows 11 x64 Host`)

### 1.1 C++20 Unit & Regression Test Suites (`tests/cpp/unit_tests.cpp` — `62 / 62` Suites: Verified by Execution)

| Suite # | Suite Function (`tests/cpp/unit_tests.cpp`) | Mapped Requirements & Bug IDs | Verification Category | Result |
| :--- | :--- | :--- | :--- | :--- |
| **`[1/53]`** | `TestPathSanitizerAndContentDisposition` | `FR-001`, `FR-026`, `NFR-006`, `NFR-012`, `SEC-PATH-01` | `Verified by Execution` | **PASSED** |
| **`[2/53]`** | `TestDynamicWorkerAndDiskSpaceGuard` | `FR-020`, `FR-026`, `BUG-005`, `BUG-009`, `BUG-022` | `Verified by Execution` | **PASSED** |
| **`[3/53]`** | `TestLoggerRedactionAndCrypto` | `FR-007`, `NFR-009`, `SEC-PRIV-01`, `UT-HASH-01` | `Verified by Execution` | **PASSED** |
| **`[4/53]`** | `TestBriskSegmentSchedulerWorkStealingAndRollback` | `FR-003`, `FR-004`, `UT-SEG-01`, `BUG-008` | `Verified by Execution` | **PASSED** |
| **`[5/53]`** | `TestSqliteStoreAndCrashRecovery` | `FR-005`, `NFR-005`, `NFR-009`, `IT-CRASH-01` | `Verified by Execution` | **PASSED** |
| **`[6/53]`** | `TestStreamMetaJsonAndDrmRejection` | `FR-019`, `NFR-015`, `SEC-DRM-01`, `THREAT-004`, `BUG-024` | `Verified by Execution` | **PASSED** |
| **`[7/53]`** | `TestControllerLifecycleAndDeletedTaskRace` | `FR-023`, `BUG-007`, `BUG-010`, `BUG-034` (Webpage Guard) | `Verified by Execution` | **PASSED** |
| **`[8/53]`** | `TestDesktopUiDesignSystemAndHighDpiLayout` | `FR-011`, `NFR-011`, `BUG-001` | `Verified by Execution` | **PASSED** |
| **`[9/53]`** | `TestNativeAnimationsAndTaskbarProgress` | `FR-011`, `NFR-011` | `Verified by Execution` | **PASSED** |
| **`[10/53]`** | `TestProfessionalSettingsCenterValidationAndPersistence` | `FR-011`, `FR-025` | `Verified by Execution` | **PASSED** |
| **`[11/53]`** | `TestDesktopUiInteractionsFilteringSortingAndRedaction` | `FR-011`, `FR-023`–`FR-025`, `NFR-009`, `BUG-001`, `BUG-010`, `BUG-011` | `Verified by Execution` | **PASSED** |
| **`[12/53]`** | `TestKeyboardNavigationAndShortcuts` | `FR-011`, `NFR-011`, `BUG-001` | `Verified by Execution` | **PASSED** |
| **`[13/53]`** | `TestDesktopUiHitTestingAndMouseInteractions` | `FR-011`, `BUG-018`, `BUG-026` | `Verified by Execution` | **PASSED** |
| **`[14/53]`** | `TestWorkerLifecycleAndIncompatibleWorkerDetection` | `FR-020`, `BUG-006`, `BUG-027` | `Verified by Execution` | **PASSED** |
| **`[15/53]`** | `TestFluxBrandingAndLegacyDataMigration` | `FR-028`, `BUG-030` | `Verified by Execution` | **PASSED** |
| **`[16/53]`** | `TestPartFileIsolationAndSafeCommit` | `FR-026`, `NFR-006` | `Verified by Execution` | **PASSED** |
| **`[17/53]`** | `TestResourceAwareResumeAndValidatorMismatch` | `FR-005`, `NFR-005` | `Verified by Execution` | **PASSED** |
| **`[18/53]`** | `TestSegmentSchedulerBoundarySplitRaceGuard` | `FR-003`, `FR-004` | `Verified by Execution` | **PASSED** |
| **`[19/53]`** | `TestIpcSecurityDaclAndPathTraversal` | `NFR-006`, `NFR-008` | `Verified by Execution` | **PASSED** |
| **`[20/53]`** | `TestGlobalRateLimiting` | `FR-024` | `Verified by Execution` | **PASSED** |
| **`[21/53]`** | `TestRelativeAndProtocolRelativeRedirectResolution` | `FR-001` | `Verified by Execution` | **PASSED** |
| **`[22/53]`** | `TestCompleteSegmentRangeMapValidation` | `FR-003` | `Verified by Execution` | **PASSED** |
| **`[23/53]`** | `TestPosixHttpChunkedDecoder` | `FR-001`, `FR-002` | `Verified by Execution` | **PASSED** |
| **`[24/53]`** | `TestUrlParsingAndSecurityChecks` | `FR-001`, `NFR-006` | `Verified by Execution` | **PASSED** |
| **`[25/53]`** | `TestIpcShutdownAndFirstPipeInstance` | `NFR-007`, `NFR-008` | `Verified by Execution` | **PASSED** |
| **`[26/53]`** | `TestConflictPolicies` | `FR-026` | `Verified by Execution` | **PASSED** |
| **`[27/53]`** | `TestSystemSuspendResumeAndPauseReason` | `FR-005`, `FR-023` | `Verified by Execution` | **PASSED** |
| **`[28/53]`** | `TestConcurrentAtomicCommitsAndSha256` | `FR-007`, `FR-026` | `Verified by Execution` | **PASSED** |
| **`[29/53]`** | `TestDnsRebindingAndDestinationSecurity` | `FR-001`, `NFR-007` | `Verified by Execution` | **PASSED** |
| **`[30/53]`** | `TestStreamContainerValidation` | `FR-020` | `Verified by Execution` | **PASSED** |
| **`[31/53]`** | `TestFtpClientParsingAndCommands` | `FR-029` (FTP / FTPS RFC 959/2428/3659/4217) | `Verified by Execution` | **PASSED** |
| **`[32/53]`** | `TestBencodeAndTorrentParser` | `FR-030` (BitTorrent Bencode & Magnet URI) | `Verified by Execution` | **PASSED** |
| **`[33/53]`** | `TestRtspMmsStreamDetection` | `FR-031` (RTSP, MMS & RTMP Legacy Streams) | `Verified by Execution` | **PASSED** |
| **`[34/53]`** | `TestSiteGrabberCrawlerAndSsrf` | `FR-032` (Site Grabber & Crawler SSRF Guard) | `Verified by Execution` | **PASSED** |
| **`[35/53]`** | `TestZipArchivePreviewAndSecurity` | `FR-033` (ZIP Previewer, Zip Slip & Zip Bomb Guard) | `Verified by Execution` | **PASSED** |
| **`[36/53]`** | `TestTaskImportExportAndMultiQueue` | `FR-034`, `FR-035` (Import/Export & Multi-Queue) | `Verified by Execution` | **PASSED** |
| **`[37/53]`** | `TestAntivirusScannerSafetyAndHandling` | `FR-034` (Antivirus Scanner Safety & Malicious Execution Block) | `Verified by Execution` | **PASSED** |
| **`[38/53]`** | `TestDualRateLimiterAndPerTaskSpeedLimit` | `FR-024` (Dual Token-Bucket Rate Limiter & Per-Task Speed Control) | `Verified by Execution` | **PASSED** |
| **`[39/53]`** | `TestDynamicCategorizationAndSiteRulesPriority` | `FR-010`, `FR-023` (Category Rules, Custom Extension Mappings & Priority Hierarchy) | `Verified by Execution` | **PASSED** |
| **`[40/53]`** | `TestAlternateUrlsAndMirrorValidation` | `FR-001`, `FR-004` (Mirror Failover & Content-Length Identity Validation) | `Verified by Execution` | **PASSED** |
| **`[41/53]`** | `TestFtpReliabilityEpsvAndResumeTruncation` | `FR-029` (RFC 2428 EPSV, PASV Fallback, NAT Bypass & REST 350 Truncation) | `Verified by Execution` | **PASSED** |
| **`[42/53]`** | `TestSchedulerStopTimesAndPostCompletionActions` | `FR-025` (Stop Times, Queue Scheduling Loop & Post-Completion Sleep/Shutdown) | `Verified by Execution` | **PASSED** |
| **`[43/53]`** | `TestPeerWireProtocolFramingAndCompactPeers` | `FR-030` (BEP 3 Peer Wire Framing & Compact Peer Unpacking) | `Verified by Execution` | **PASSED** |
| **`[44/53]`** | `TestMultiFileStorageMappingAndBoundarySplitting` | `FR-030` (Multi-File Storage Offset Mapping & File Boundary Slicing) | `Verified by Execution` | **PASSED** |
| **`[45/53]`** | `TestPieceManagerAndBlockScheduling` | `FR-030` (Piece Manager, Block Request Generation & SHA-1 Verification) | `Verified by Execution` | **PASSED** |
| **`[46/53]`** | `TestTrackerClientQueryEncodingAndResponseParsing` | `FR-030` (Tracker Query Binary URL Percent-Encoding & Response Parsing) | `Verified by Execution` | **PASSED** |
| **`[47/53]`** | `TestHttpRangeSatisfactionValidation` | `FR-001`, `FR-004` (HTTP 206 Range Satisfaction & Entity Integrity Validation) | `Verified by Execution` | **PASSED** |
| **`[48/53]`** | `TestDuplicateTaskDetectionThreeTiers` | `FR-010`, `FR-023` (3-Tier Duplicate Detection: Normalized URL, Name/Size, Strong ETag & Magnet BTIH) | `Verified by Execution` | **PASSED** |
| **`[49/53]`** | `TestExpiredLinkRefreshAndCookieReplacement` | `FR-006` (Expired Link 403/410 Detection, Presigned URL Refresh & Cookie Replacement) | `Verified by Execution` | **PASSED** |
| **`[50/53]`** | `TestSegmentSchedulerChunkNormalizationAndTruncationGuard` | `FR-003`, `FR-005` (Segment Restored Chunk Normalization & Truncation Guard) | `Verified by Execution` | **PASSED** |
| **`[51/53]`** | `TestUrlUserinfoExtractionAndBasicAuthGeneration` | `FR-001` (URL Userinfo Extraction & HTTP Basic Authorization Header Generation) | `Verified by Execution` | **PASSED** |
| **`[52/53]`** | `TestModernCoralThemeSettingsModalBoundsAndSafeDelete` | `FR-011`, `FR-026`, `NFR-011` (Coral Charcoal Theme `#FF4F57`, Modal Hit-Testing Bounds, Recycle Bin Safe Delete & Header Sorting Direction) | `Verified by Execution` | **PASSED** |
| **`[53/61]`** | `TestYouTubeSpeedParsingMidnightCrossingAndBitTorrentUpload` | `FR-019`, `FR-024`, `FR-030` (YouTube Regex Speed Extraction, Scheduled Midnight-Crossing Window & BT Unchoke/Block Upload Accounting) | `Verified by Execution` | **PASSED** |
| **`[54/61]`** | `TestFinalPhaseRemediationAndLiveMetrics` | `FR-011`, `FR-019`, `FR-026` (Toolbar Sort Stabilization, Recycle Bin vs Disk Delete, YouTube Googlevideo Routing) | `Verified by Execution` | **PASSED** |
| **`[55/61]`** | `TestYouTubeCanonicalRoutingAndSchedulerThroughput` | `FR-003`, `FR-019`, `FR-020` (SegmentScheduler Dynamic Split-Eligibility, YouTube Watch Canonicalization & VP9/AV1 1080p Format Verification) | `Verified by Execution` | **PASSED** |
| **`[56/61]`** | `TestSpeedFormattingMultiTrackProgressAndContainerVerification` | `FR-019`, `FR-020` (Multi-Track Progress Aggregation, Speed Formatting & Magic Bytes Container Check) | `Verified by Execution` | **PASSED** |
| **`[57/61]`** | `TestPimpbunnyStreamRoutingAndYouTubeNetscapeCookieFile` | `FR-019`, `FR-022` (Pimpbunny Stream URL Extraction & Netscape Cookie File Construction) | `Verified by Execution` | **PASSED** |
| **`[58/61]`** | `TestRemediationPhaseRegressionSuites` | `FR-001`–`FR-026` (Remediation Phase Regression Suites & Memory Integrity) | `Verified by Execution` | **PASSED** |
| **`[59/61]`** | `TestAuditBehaviorImprovements` | `FR-001`, `FR-004`, `FR-020` (Audit-derived Behavior Improvements: Range Satisfaction, Stream Part Files, Crash Recovery) | `Verified by Execution` | **PASSED** |
| **`[60/62]`** | `TestQualitySelectionSizeAndColumnDocking` | `FR-011`, `FR-019` (Video Quality Selection, Size Reporting & Table Column Right-Docking Anchors) | `Verified by Execution` | **PASSED** |
| **`[61/62]`** | `TestNewDownloadDialogSubmissionAndTaskCreation` | `FR-011`, `FR-019`, `FR-025` (New Download Dialog Submission, Quality Options Propagation & State Reset) | `Verified by Execution` | **PASSED** |
| **`[62/62]`** | `TestYouTubeQualitySelectionIntegrityAndVerification` | `FR-019`, `FR-020`, `FR-021` (YouTube 1080p Format Selection Integrity, ffprobe Media Verification, Progress Capping & Indeterminate Status) | `Verified by Execution` | **PASSED** |

---

### 1.2 Browser Extension & Media Detection Test Suites (`node tests/test_extension_detection.js` — `15 / 15 PASSED (24 / 24 Edge Scenarios)`)

| Suite # | Suite Description (`tests/test_extension_detection.js`) | Mapped Requirements & Bug IDs | Verification Category | Result |
| :--- | :--- | :--- | :--- | :--- |
| **`[1/12]`** | Direct MP4/WebM & HLS/DASH Detection + Header Correlation | `FR-014` | `Verified by Execution` | **PASSED** |
| **`[2/12]`** | Master vs. Child HLS Deduplication & `.ts` Segment Suppression | `FR-014`, `FR-016` | `Verified by Execution` | **PASSED** |
| **`[3/12]`** | Per-Site Exclusion Setting & Popup Rendering | `FR-009`, `FR-014` | `Verified by Execution` | **PASSED** |
| **`[4/12]`** | Page-Context Deep Search Hook (Obfuscated Manifest Capture + Nonce) | `FR-013`, `FR-017`, `BUG-032` | `Verified by Execution` | **PASSED** |
| **`[5/12]`** | In-Page Closed Shadow DOM Video Overlay & Fullscreen Repositioning | `FR-015`, `BUG-013`, `BUG-032` | `Verified by Execution` | **PASSED** |
| **`[6/12]`** | Download Interception, Bypass Modifier & Safe Browser Fallback | `FR-008`, `FR-009`, `FR-012`, `BUG-031` | `Verified by Execution` | **PASSED** |
| **`[7/12]`** | Manifest V3 Service Worker Session State Persistence & Recovery | `FR-014`, `RISK-002`, `BUG-013` | `Verified by Execution` | **PASSED** |
| **`[8/12]`** | Popup Filter Tabs, Search, Disconnected States & Direct Media Fallback | `FR-008`, `FR-014`, `BUG-012`, `BUG-033` | `Verified by Execution` | **PASSED** |
| **`[9/12]`** | Link-Click Interception & Alt-Key Bypass Behavior | `FR-008`, `FR-009`, `BUG-031` | `Verified by Execution` | **PASSED** |
| **`[10/12]`** | Webpage Watch URL Guard & Background Runtime Message Routing | `FR-014`, `FR-021`, `BUG-033`, `BUG-034` | `Verified by Execution` | **PASSED** |
| **`[11/12]`** | Cosmic Dark Extension Styling & Offscreen Overlay Visibility | `FR-011`, `FR-015` | `Verified by Execution` | **PASSED** |
| **`[12/14]`** | Quality Size Estimation & Canonical YouTube Watch Routing | `FR-014`, `FR-019`, `FR-020` | `Verified by Execution` | **PASSED** |
| **`[13/14]`** | Chromium Download Interception & Pre-Download New Dialog Offer Protocol | `FR-008`, `FR-009`, `FR-012`, `BUG-031` | `Verified by Execution` | **PASSED** |
| **`[14/14]`** | Edge Scenarios (Duplicate Events, Navigation Bypass, Alt-Key, Frames, SW Restart & Concurrency) | `FR-008`, `FR-014`, `RISK-002` | `Verified by Execution` | **PASSED** |
| **`[15/15]`** | YouTube Cipher Quality Extraction with Sizes & Floating Overlay Attachment | `FR-014`, `FR-019`, `FR-020` | `Verified by Execution` | **PASSED** |

---

### 1.3 End-to-End Integration, Stream Pipeline & Benchmark Suites (`python tests/run_all_integration_tests.py` — `8 / 8 PASSED`)

| Suite # | Test ID & Description (`tests/run_all_integration_tests.py`) | Mapped Requirements & Bug IDs | Verification Category | Result |
| :--- | :--- | :--- | :--- | :--- |
| **`[1/8]`** | `IT-BIN-01`: Verify Platform-Aware Compiled Executables (`flux.exe`, `flux-nmh.exe`, `N_m3u8DL-RE.exe`) | `FR-020`, `BUG-006`, `BUG-030` | `Verified by Execution` | **PASSED** |
| **`[2/8]`** | `IT-HTTP-01` & `IT-HTTP-03`: Segmented HTTP Range Download (8 Connections + 302 Redirect + RFC 5987) | `FR-001`, `FR-003`, `BUG-003` | `Verified by Execution` | **PASSED** |
| **`[3/8]`** | `IT-HTTP-02`: Non-Range Server Single-Stream Fallback | `FR-002`, `BUG-008` | `Verified by Execution` | **PASSED** |
| **`[4/8]`** | `IT-HTTP-04`: Dropped Connection Reset & Resume from Exact Byte Offset | `FR-004` | `Verified by Execution` | **PASSED** |
| **`[5/8]`** | `IT-STREAM-01` & `IT-STREAM-02`: HLS Master Manifest Probe & FFmpeg Container Muxing | `FR-019`, `FR-020`, `BUG-023`, `BUG-024` | `Verified by Execution` | **PASSED** |
| **`[6/8]`** | `IT-STREAM-03`: MPEG-DASH (`.mpd`) Manifest Download & FFmpeg Muxing | `FR-020`, `BUG-023` | `Verified by Execution` | **PASSED** |
| **`[7/8]`** | `IT-IPC-01`, `IT-STREAM-06` & `IT-UI-01`: Native Messaging Host, AES-128 Decryption, Log Redaction & Desktop UI Snapshot | `FR-021`, `FR-022`, `NFR-007`–`NFR-009`, `BUG-002`, `BUG-004`, `BUG-025` | `Verified by Execution` | **PASSED** |
| **`[8/8]`** | `PERF-BENCH-01`: Core Binaries (`2,047.0 KB` <= 3072 KB), Startup (`71.05 ms` <= 150 ms), Idle RSS (`10.34 MB` <= 25 MB), HTTP Peak RSS (`15.7 MB` <= 65 MB), Stream Peak RSS (`11.04 MB` <= 65 MB), UI Snapshot (`143.09 ms` <= 300 ms), Throughput (`223.0 Mbps` >= 50 Mbps) | `NFR-001`–`NFR-004`, `BUG-017`, `BUG-021` | `Verified by Execution` | **PASSED** |

---

### 1.3b Core Operations, Concurrency & Resilience Suites (`python tests/test_core_operations.py` — `10 / 10 PASSED`)

| Test ID | Test Scenario (`tests/test_core_operations.py`) | Mapped Requirements & Invariants | Verification Category | Result |
| :--- | :--- | :--- | :--- | :--- |
| **`TC-CORE-01`** | Application Startup & CLI Version Check (`--version`) | `FR-028`, `NFR-002` | `Verified by Execution` | **PASSED** |
| **`TC-CORE-02`** | Core Multi-Connection Segmented Download & SHA-256 Check | `FR-001`, `FR-003`, `FR-007` | `Verified by Execution` | **PASSED** |
| **`TC-CORE-03`** | SQLite Schema, WAL Mode & Task Persistence | `FR-005`, `NFR-005` | `Verified by Execution` | **PASSED** |
| **`TC-CORE-04`** | AppSettings Configuration Persistence Across Invocations | `FR-024`, `FR-025` | `Verified by Execution` | **PASSED** |
| **`TC-CORE-05`** | Desktop UI Virtual View & Snapshot Rendering | `FR-011`, `NFR-004` | `Verified by Execution` | **PASSED** |
| **`TC-CORE-06`** | Concurrent Multi-Task Queue Execution | `FR-024`, `NFR-003` | `Verified by Execution` | **PASSED** |
| **`TC-CORE-07`** | Pause/Resume Byte Offset & Full Integrity Check | `FR-004`, `FR-005` | `Verified by Execution` | **PASSED** |
| **`TC-CORE-08`** | Non-Range Server Single Stream Fallback (`Accept-Ranges: none`) | `FR-002`, `BUG-008` | `Verified by Execution` | **PASSED** |
| **`TC-CORE-09`** | HTTP 429 Rate Limiting & Retry-After Recovery | `FR-003`, `FR-004`, `NFR-005` | `Verified by Execution` | **PASSED** |
| **`TC-CORE-10`** | Connection Reset Auto-Recovery & Resumption | `FR-004`, `NFR-005` | `Verified by Execution` | **PASSED** |

---

### 1.4 Clean Installation Smoke & Recovery Suites (`python tests/test_installer_smoke.py` — `8 / 8 PASSED`)

| Suite # | Test Scenario (`tests/test_installer_smoke.py`) | Mapped Requirements & Invariants | Verification Category | Result |
| :--- | :--- | :--- | :--- | :--- |
| **`[1/8]`** | Silent Inno Setup Installation (`/VERYSILENT /TYPE=full`) | `FR-026`, `NFR-006`, `NFR-012` | `Verified by Execution` | **PASSED** |
| **`[2/8]`** | Installed Files, Worker Binaries & PE32+ Subsystem Audit | `FR-020`, `NFR-001`, `BUD-001`–`BUD-004` | `Verified by Execution` | **PASSED** |
| **`[3/8]`** | Native Messaging Manifest Path Interpolation & HKCU Registry | `FR-008`, `FR-021`, `NFR-007` | `Verified by Execution` | **PASSED** |
| **`[4/8]`** | Executable Version Launch & Response Contract (`--version`) | `FR-028`, `NFR-002` | `Verified by Execution` | **PASSED** |
| **`[5/8]`** | Real HTTP Download via Installed Application with SHA-256 Check | `FR-001`, `FR-003`, `FR-007` | `Verified by Execution` | **PASSED** |
| **`[6/8]`** | Clean Part-File Lifecycle, 404 Handling & Mid-Stream Reset Resume | `FR-004`, `FR-026`, `NFR-005` | `Verified by Execution` | **PASSED** |
| **`[7/8]`** | Database State Persistence Across Process Restart | `FR-005`, `NFR-005` | `Verified by Execution` | **PASSED** |
| **`[7b/8]`** | Positive Browser Native Messaging Integration (Chromium + Installed Extension) | `FR-008`, `FR-021`, `NFR-007` | `Verified by Execution` | **PASSED** |
| **`[8/8]`** | Safe Uninstallation with Strict User Data Preservation | `FR-026`, `NFR-006`, `SEC-PRIV-01` | `Verified by Execution` | **PASSED** |

---

### 1.5 Automated Browser Extension Runtime Playwright Suites (`node tests/browser/test_extension_runtime.js` — `7 / 7 PASSED`)

| Suite # | Test Scenario (`tests/browser/test_extension_runtime.js`) | Mapped Requirements & Invariants | Verification Category | Result |
| :--- | :--- | :--- | :--- | :--- |
| **`[1/7]`** | Extension Startup & Manifest V3 Service Worker Spawning | `FR-014`, `RISK-002` | `Verified by Execution` | **PASSED** |
| **`[2/7]`** | Ordinary File Download Interception & Non-Blocking Fallback | `FR-008`, `FR-009`, `BUG-031` | `Verified by Execution` | **PASSED** |
| **`[3/7]`** | HTML5 Video Player Detection, Shadow DOM Pill Attachment & Media Candidate Verification | `FR-015`, `BUG-013`, `BUG-032` | `Verified by Execution` | **PASSED** |
| **`[4/7]`** | HLS & DASH Stream Sniffing, Exact Type Identification & Variant Deduplication | `FR-014`, `FR-016`, `FR-019` | `Verified by Execution` | **PASSED** |
| **`[5/7]`** | Popup Interface (`popup.html`) Strict Visibility & Essential Control Audit | `FR-008`, `FR-011`, `BUG-012` | `Verified by Execution` | **PASSED** |
| **`[6/7]`** | Real Service Worker Lifecycle Termination (CDP) & Storage Recovery | `FR-014`, `RISK-002` | `Verified by Execution` | **PASSED** |
| **`[7/7]`** | Native Messaging Host Offline Fallback Contract (Flux Offline) | `FR-021`, `NFR-007`, `NFR-008` | `Verified by Execution` | **PASSED** |

---

### 1.6 Release Security & Packaging Integrity Checks (`scripts/verify-release-security.ps1` — `5 / 5 PASSED`)

### 1.7 Torrent selection and pause responsiveness

| Scenario | Requirement | Verification category | Result |
| :--- | :--- | :--- | :--- |
| Multi-file torrent selection excludes unselected files, schedules only overlapping pieces, and reports selected-byte progress | Torrent file selection / selective storage | `Verified by Execution` (`TestPieceManagerAndBlockScheduling`) | **PASSED** |
| Pause All, schedule edits, scheduler ticks, restart recovery, and explicit Resume All preserve user pause intent | Pause lifecycle / scheduler precedence | `Verified by Execution` (`TestPauseAllManualPauseScheduleAndRecovery`) | **REGRESSION TEST ADDED; active multi-engine GUI stress pending** |
| Incoming BitTorrent handshake, verified-piece advertisement, requested-block response, and successful-upload byte accounting | BitTorrent seeding/upload | `Verified by Static Inspection; deterministic incoming-peer exchange pending` | **IMPLEMENTED IN SOURCE; BLOCK TRANSFER AND LIVE NETWORK VERIFICATION PENDING** |
| Magnet BEP 9 metadata selection is marshalled to the Win32 UI thread and selected paths/mask are persisted before allocation | Magnet selective download | `Verified by Source Inspection; UI interaction pending` | **GUI DIALOG AND SELECTIVE OUTPUT VERIFICATION PENDING** |

| Check # | Security Verification Scope (`scripts/verify-release-security.ps1`) | Threat Model & ADR Rule | Verification Category | Result |
| :--- | :--- | :--- | :--- | :--- |
| **`[1/5]`** | Authoritative SHA-256 Checksum Verification (`dist/SHA256SUMS.txt`) | `SEC-INTEG-01`, `ADR-0008` | `Verified by Execution` | **PASSED** |
| **`[2/5]`** | Stream Worker Size Ceilings & PE32+ AMD64 Architecture Audit | `BUD-001`, `BUD-002`, `SEC-01` | `Verified by Execution` | **PASSED** |
| **`[3/5]`** | Authenticode Digital Signatures Audit (Valid or Transparent Unsigned) | `SEC-AUTH-01`, `AGENTS.md #7` | `Verified by Execution` | **PASSED** |
| **`[4/5]`** | Credential, Token & Cookie Plaintext Redaction Invariant Audit | `NFR-009`, `SEC-PRIV-01`, `THREAT-002` | `Verified by Execution` | **PASSED** |
| **`[5/5]`** | Non-Elevated (`PrivilegesRequired=lowest`) & Path Traversal Guards | `NFR-006`, `SEC-PATH-01`, `THREAT-001` | `Verified by Execution` | **PASSED** |

---

## 2. Release & Host Deployment Status

| Verification Item | Target | Verification Procedure | Status |
| :--- | :--- | :--- | :--- |
| **Native PE32+ Compilation** | Windows 11 `x64` | Built with MSVC 19.44 / CMake 4.2; verified AMD64 subsystem on `flux.exe` and `flux-nmh.exe` | **Verified by Execution** |
| **Per-User Native Messaging** | HKCU Registry | Executed `.\scripts\register-native-messaging.ps1`; verified registry keys for Chrome, Edge, Brave, and Firefox pointing to `flux-nmh.exe` | **Verified by Execution** |
| **Portable Distribution Directory** | `dist/` | Verified `dist/Flux.exe`, `dist/flux.exe`, `dist/flux-nmh.exe`, and `dist/tools/win-x64/` (`ffmpeg.exe`, `N_m3u8DL-RE.exe`) | **Verified by Execution** |
| **Clean Machine Installation** | Fresh Windows Host | Executed `python tests/test_installer_smoke.py`; verified silent install, real download with SHA256 match, and safe uninstall | **Verified by Execution** |
| **Live Browser Extension Runtime** | Chromium (Playwright) | Executed `node tests/browser/test_extension_runtime.js`; verified download capture, video overlay pill, stream sniffing, and popup UI | **Verified by Execution** |
| **Authenticode Code Signing** | Windows PE Binaries | Verified via `Get-AuthenticodeSignature`. In OSS community CI without private secrets, reported cleanly as Unsigned (non-elevated per-user execution) | **Verified by Execution (Unsigned OSS Baseline)** |

## Extension navigation and player control regression coverage

| Test | Coverage | Status |
| :--- | :--- | :--- |
| `test_extension_detection.js` suite 17 | Same-tab SPA page identity clears prior results; unrelated tab results remain; same-document refresh preserves current results; floating control geometry stays above and aligned to the player | Automated test added; live Brave multi-page validation pending |

| Test | Coverage | Status |
| :--- | :--- | :--- |
| Extension suite 18 | Per-tab SPA scoping, unrelated-tab isolation, compact player toolbar geometry, range size probe/cancel, numeric media filename suffix | Passed in Node; Playwright Chromium runtime suite passed. Live Brave site tests pending. |
| Native New Download dialog | Field label/input vertical separation at scaled DPI, action buttons remain within the fixed dialog client area | MSVC build and live screenshot verification pending |

| Native New Download layout regression | Label/input and inter-row clearance at 96, 120, 144, 192, 288, and 384 DPI | Added C++ unit suite; rebuild and live visual verification pending |

| Native setup dialog visual consistency | New Download, Add Torrent, and Remove Download share the charcoal/coral palette; torrent save, tags, and filter fields use modern focus borders; selected torrent entries use the warm selected surface and coral indicator | MSVC Release build passed; live visual verification pending |


| Task context menu state filtering | State-appropriate actions avoid disabled Start/Pause/Resume/Retry entries on completed rows | C++ build and unit suite passed; refreshed native menu screenshot pending |


| Brave extension restart audit | Fresh Brave launch, `brave://extensions` search, Default profile path, Preferences/Secure Preferences, extension cache and manifest | Radium is absent from the live extensions list; Secure Preferences alone retains a stale path entry while Preferences has no entry. Stable folder is staged; Load unpacked and restart verification requires action-time install confirmation. |

### Task removal follow-up (2026-10-24)

`TestModernCoralThemeSettingsModalBoundsAndSafeDelete` covers partial task cleanup, remove-from-list preserving an output, explicit permanent deletion, idempotent deletion when the output is already missing, persisted task removal after reopening storage, and (on Windows) a read-only output failure that must preserve both the file and task record before a successful retry. Rerun is required after these latest additions.

Local `.torrent` open/drag and remote browser offers with a `.torrent` URL or suggested filename route to the dedicated Add Torrent selection dialog. Remote metainfo retrieval uses the offer headers, a 16 MiB body limit, and asynchronous UI dispatch; cancel removes the placeholder task and Add updates it with selected paths, category, destination, and start choice. Existing C++ regressions passed 71/71, but do not directly exercise this new UI route; live browser-offer, dialog, and controlled selective multi-file transfer checks remain pending.

Task removal actions are distinct: the Delete key opens a compact chooser with Remove from List, Permanently Delete, and Cancel; context-menu and toolbar Remove from List remain direct list-removal actions. Delete File / Shift+Delete opens the same chooser. Permanent torrent deletion uses selected metadata-relative file paths and retains the task record if path validation or file removal fails. Native UI interaction remains unverified.

### Dialog function audit (2026-10-25)

The Add Torrent and Remove Download dialogs were statically audited and the Release build plus 71 C++ regression suites passed. Automated coverage exercises torrent parsing/transfer and controller-level removal/deletion safety, but does not click every dialog control or verify rendered spacing in a live window. Add Torrent file selection, Select All/None, filter, folder browse, category, start choice, Add, Cancel, and Save .torrent are connected in the Win32 dialog. Mode, tags, default-category persistence, alternate incomplete-file path, remember-path persistence, stop condition, queue placement, skip-hash, sequential/first-last priorities, content layout, and Never show again are currently not applied to the task/controller. Treat these controls as unsupported until implementation and dedicated UI tests are added. Remove Download buttons dispatch to the existing permanent-delete, list-removal, and cancel actions; controller safety paths are covered by `TestModernCoralThemeSettingsModalBoundsAndSafeDelete`. Live native dialog interaction and screenshot verification remain pending.

Live BitTorrent progress now reports received blocks while their pieces are still being assembled; only verified-piece bytes are retained as resumable progress. The received-block UI progress change has not yet been covered by an automated regression or verified in the running Windows UI.

| Native compact table and New Download dialog | Compact Name column reserves 260 DPI-scaled px after shrinking oversized saved columns; progress bounds stay within Name; compact dialog action row fits within 440 DPI-scaled client pixels across supported DPI values | MSVC Release build; 71/71 C++ suites passed; staged compact dialog visually confirmed |

| Task-row hover responsiveness | Moving between rows changes hover state and synchronously redraws the old/new row regions | `TestDesktopUiHitTestingAndMouseInteractions` included in 71/71 passing suites; updated executable staged |
