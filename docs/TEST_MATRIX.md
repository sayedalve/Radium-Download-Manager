# Flux Download Manager -- Requirement-to-Test Verification Matrix

**Document Status:** Active Verification Matrix (`v0.3.0` — `62 / 62` C++20 Suites, `15 / 15` Extension Suites, `8 / 8` Smoke Suites, `7 / 7` Playwright Suites, and `5 / 5` Security Checks Executed & Passed on Windows 11 Host)  
**Last Reviewed:** 2026-10-20  
**Scope:** Maps every functional (`FR-001`–`FR-035`), non-functional (`NFR-001`–`NFR-016`), and bug-regression (`BUG-001`–`BUG-035`, `FDM-001`–`FDM-029`) test to its exact command, verification category, and outcome.  
**Related Documents:** [archive/maintenance-audits/RELEASE_READINESS_REPORT.md](archive/maintenance-audits/RELEASE_READINESS_REPORT.md), [archive/maintenance-audits/BUG_FIX_LOG.md](archive/maintenance-audits/BUG_FIX_LOG.md), [FEATURE_MATRIX.md](FEATURE_MATRIX.md), [TEST_STRATEGY.md](TEST_STRATEGY.md)

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
| **`[12/53]`** | `TestKeyboardNavigationAndShortcuts` (queue navigation and Settings shortcut reference) | `FR-011`, `NFR-011`, `BUG-001` | `Verified by Execution` | **PASSED** |
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

### HTTP concurrency instrumentation follow-up (2026-10-25)

`benchmarks/scripts/run_local_benchmark.py` prefers a build-tree executable, accepts `--exe` to pin the binary, and supports fixed response latency plus per-256 KiB pacing. The fixture records each request's start/end timestamps, requested and returned ranges, status, handler count, payload-handler count, body completion, and bytes offered to the socket. Server handler counts are separate from client HTTP request intervals and engine worker-thread lifecycle. Engine worker start/stop and scheduler idle-wait events are available; scheduler lock-wait and disk-write duration are not. Server socket-write bytes are an upper bound on payload offered by the fixture and do not prove peer receipt.

When the runner sets `RADIUM_HTTP_RANGE_TRACE=1`, the engine also writes per-request traces with requested and response ranges, response status, client body bytes delivered to the engine callback, successful file-write bytes and offsets, start/completion times, cancellation reason, and result. The runner derives unique committed bytes, duplicate received bytes from overlapping received intervals, request overlap, live client request peak, and phase timings. `client_bytes_received` is WinHTTP body data returned to the engine callback; it is distinct from fixture socket writes and configured/worker counts. The engine phase boundaries are: process launch to `ExecuteDownload`, download initialization through worker start, transfer through worker join and file flush, verification/commit, and process exit tail. The runner separately times its post-process SHA-256 check.

Verified by execution using `build_msvc/Release/radium.exe`; each completed output matched SHA-256:

| Fixture | Configured connections | Delay per 256 KiB | Peak server payload handlers | Payload GETs (completed / incomplete) | Server socket bytes written | Whole-command duration |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| 32 MiB | 8 | 0 ms | 3 | 5 / 3 of 8 | 35,782,656 | 0.523 s |
| 32 MiB | 4 | 5 ms | 6 | 3 / 3 of 6 | 34,603,008 | 0.652 s |
| 32 MiB | 8 | 5 ms | 6 | 5 / 3 of 8 | 34,930,688 | 0.781 s |
| 64 MiB | 8 | 0 ms | 3 | 3 / 4 of 7 | 69,484,544 | 0.450 s |

Every request in these traces received HTTP `206`; the incomplete responses ended with server-side connection reset/abort errors after the client stopped reading. The final file still passed SHA-256. The traces show that configured concurrency is not immediately reached on the fast loopback path and that dynamic segment splitting can truncate already-issued ranges, causing additional range GETs and some server-side socket writes beyond the final payload size. A 5 ms per-chunk delay produced more concurrent handlers, confirming the fixture can exercise concurrency above two; it is an artificial pacing condition, not an Internet RTT simulation. The earlier two-handler result cannot be reconstructed exactly because its raw per-request log was not saved. The current results are not an IDM comparison and do not establish real-world speed.

The 16 MiB, 8-connection, 5 ms/chunk, simulated-429-cap-2 run observed one HTTP `429`, one exact-range retry, 3 peak server payload handlers, and completed with matching SHA-256 (1.874 s whole-command time). The instrumentation recorded the retry separately from the three responses that ended when the client aborted a now-shortened range.

Additional one-trial size/concurrency matrix, all with 5 ms delay after each 256 KiB response chunk; every output completed and matched SHA-256:

| Payload | Configured conns | Peak fixture handlers | Data GETs (complete / incomplete) | Request window | Whole-command time | Server socket bytes written |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 MiB | 1 | 1 | 1 (1 / 0) | 0.093 s | 0.458 s | 4,194,304 |
| 4 MiB | 4 | 2 | 2 (1 / 1) | 0.052 s | 0.389 s | 4,456,448 |
| 4 MiB | 8 | 2 | 2 (1 / 1) | 0.052 s | 0.460 s | 4,456,448 |
| 32 MiB | 1 | 1 | 1 (1 / 0) | 0.740 s | 1.127 s | 33,554,432 |
| 32 MiB | 4 | 6 | 6 (3 / 3) | 0.242 s | 0.597 s | 34,734,080 |
| 32 MiB | 8 | 6 | 8 (5 / 3) | 0.257 s | 0.774 s | 34,865,152 |
| 64 MiB | 1 | 1 | 1 (1 / 0) | 1.493 s | 1.924 s | 67,108,864 |
| 64 MiB | 4 | 6 | 6 (3 / 3) | 0.418 s | 0.824 s | 68,157,440 |
| 64 MiB | 8 | 9 | 12 (6 / 6) | 0.357 s | 0.865 s | 69,206,016 |

Each table row is a single run, not a median. The 64 MiB configured-eight fixture-handler peak of 9 does not mean the engine exceeded its configured worker limit: server handlers can remain alive briefly after the client cancels a range. This confirms why the report names the metric as handler concurrency and keeps engine worker count unavailable.

Two 64 MiB, 8-connection, no-delay runs completed in 0.450 s and 0.604 s, with server request windows of 0.089 s and 0.125 s. In the latter run, 0.205 s elapsed before the first payload GET and 0.275 s after the last server handler completed. Thus the whole-command rate for small/medium loopback fixtures is dominated by process/controller setup and post-transfer completion as well as network transfer; this CLI measurement is not a pure engine throughput metric. The sample-to-sample variation also means these single trials must not be generalized.

The historical 2 GiB comparison script was also audited without rerunning it. Its Radium `effective_connections` field was inferred as `configured_connections` unless a 429 log entry existed; the IDM value was a one-second sample of temporary segment files. Neither is an observed simultaneous HTTP request count. The script also times Radium from SQLite task-created/task-completed timestamps but IDM from launcher invocation until a one-second file-size poll. Therefore the historical 2.7% median IDM advantage is an integrity-verified end-to-end result, but the connection-count comparison and sub-3% timing difference are not precise enough to isolate engine performance. The two trials varied by about 57% for Radium and 53% for IDM between runs, consistent with substantial network/environment variation. Future benchmark-script output now marks effective connection counts unavailable unless actually instrumented.

### Active range resplit guard (2026-10-25)

`SegmentScheduler` now marks an active segment `split_pending` after assigning a suffix. The scheduler will not repeatedly shrink that same segment while its in-flight request still has the older end bound. It clears the mark after a request using the current bound reaches validated headers, or after rollback merges an unstarted stolen range. The regression `TestSegmentSchedulerDoesNotResplitInFlightRange` verifies the original range is not shrunk twice and the resulting byte partition remains contiguous and complete; the existing work-stealing rollback test also verifies the guard clears on rollback.

Three baseline runs and five guarded runs used the same 16 MiB payload, 8 configured connections, 200 ms artificial delay after response headers before the first body chunk, and 5 ms after each 256 KiB chunk. All eight outputs matched the authoritative SHA-256. Each row is the median of its runs:

| Scheduler | Runs | Engine transfer | Useful throughput | Whole command | Range requests | Split cancellations | Client received / unique committed | Duplicate received bytes | Fixture socket writes / reset errors | Peak live client requests |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Existing behavior | 3 | 770 ms | 20.8 MiB/s | 1.147 s | 17 | 11 | 16 / 16 MiB | 0 | 19,660,800 / 11 | 8 |
| In-flight resplit guard | 5 | 516 ms | 31.0 MiB/s | 0.898 s | 6 | 5 | 16 / 16 MiB | 0 | 18,087,936 / 5 | 6 |

On this delayed-response fixture the guarded median transfer was 33% shorter, with 65% fewer requests. The server fixture accepted 1,572,864 fewer socket-write bytes across the whole payload and recorded 6 fewer connection-reset handler failures, but these socket writes are not a packet capture and do not prove how many bytes crossed the network. This indicates request setup/ramp churn was costly in this test, even though the baseline did not duplicate body bytes in this particular case. The fixture is synthetic and this does not prove improvement on live servers. In a separate guarded 32 MiB / 4-connection / 5 ms-per-chunk run, the client received 33,882,112 bytes while 33,554,432 unique bytes were committed (327,680 repeated bytes across received ranges); SHA-256 still matched. Thus boundary-crossing reads can waste real client-received bytes, but the amount depends on response timing.

Additional guarded checks completed with exact hashes: a 4 MiB / 8-connection fast local transfer finished with only 2 live client requests (showing the 50 ms ramp does not always reach configured concurrency); the 32 MiB / 4-connection paced case reached 4 live requests; and a 16 MiB simulated-429-cap-2 run received one HTTP 429 then completed. The full freshly rebuilt Release C++ suite passed 72/72, including range partition, rollback, resume, adaptive 429, and pause/recovery coverage. No live external comparison was performed.

| Native compact table and New Download dialog | Compact Name column reserves 260 DPI-scaled px after shrinking oversized saved columns; progress bounds stay within Name; compact dialog action row fits within 440 DPI-scaled client pixels across supported DPI values | MSVC Release build; 71/71 C++ suites passed; staged compact dialog visually confirmed |

| Task-row hover responsiveness | Moving between rows changes hover state and synchronously redraws the old/new row regions | `TestDesktopUiHitTestingAndMouseInteractions` included in 71/71 passing suites; updated executable staged |

| Local HTTP runtime telemetry | Event starts/ends, request ranges/status/bytes, actual active request peak, worker starts/stops, split/cancel/retry events, measured waits, lifecycle phases, and consistency with fixture logs and output integrity | `tests/test_local_benchmark_telemetry.py`; local loopback fixture only |

### Controlled local telemetry audit (2026-10-26)

The 500 MB live artifact retained only scalar aggregate counters and no request-event array or original runner log, so its missing request traces cannot be recovered. The local runner now saves per-run client runtime events, phase events, fixture request records, reconciliation checks, and output hashes as standalone JSON. No HTTP engine or scheduler behavior was changed for this audit.

Executed 3 repetitions of each 16 MiB scenario/configuration using `build_msvc/Release/radium.exe`: fast fixture (`0 ms` response latency, `0 ms` chunk pacing) and paced fixture (`25 ms` response latency, `5 ms` after each 256 KiB chunk), at 4 and 8 configured connections. The 12 16 MiB runs validated the test path; because those transfers ended too quickly to exercise 8-way concurrency, a second three-repeat matrix used 128 MiB files. The 128 MiB results are the diagnostic comparison below. All 12 128 MiB runs completed, all 12 final hashes matched, and all client/server request/range/status/byte consistency checks passed.

| Fixture | Configured connections | Transfer time (median; range), ms | Init / finalization (median), ms | Useful throughput (median), MiB/s | Client active-request peak | Server payload-handler peak | All server handlers peak | Worker threads started | Worker-slot occupancy | Requests incl. probe | Splits / canceled requests | Discarded client bytes | Scheduler idle wait (aggregate), ms |
| :--- | ---: | :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :--- | ---: | ---: |
| Fast | 4 | 965.6 (918.1–989.4) | 43.1 / 5.3 | 132.6 | 4 | 4 | 4 | 4 | 45.8% | 5 (5–8) | 3 / 3 | 196,608 | 522.7 |
| Fast | 8 | 838.4 (330.6–984.0) | 42.6 / 3.9 | 152.7 | 6 (4–6) | 6 (4–6) | 6 | 6 (4–6) | 26.5% | 9 (8–10) | 7 / 7 | 634,880 | 697.9 |
| Latency paced | 4 | 1,828.0 (1,650.4–2,052.3) | 66.7 / 4.0 | 70.0 | 4 | 4 | 4 (4–5) | 4 | 59.1% | 5 (5–6) | 3 / 3 | 196,608 | 774.7 |
| Latency paced | 8 | 1,987.0 (1,984.7–2,021.5) | 65.7 / 4.7 | 64.4 | 7 | 6 (6–7) | 8 (8–9) | 7 | 29.7% | 9 (9–10) | 7 / 7 | 684,032 (643,072–1,069,056) | 1,463.4 |

For the paced 128 MiB fixture, the 8-connection median was about 8.7% slower than the 4-connection median despite reaching 7 live client requests and 8–9 simultaneous server handlers (including handlers awaiting response headers). It also issued more ranges and discarded more callback bytes (0.51% of payload versus 0.15% at 4 connections). Worker-slot occupancy is calculated from actual worker start/stop lifetimes and is not CPU utilization. The excess bytes alone are too small to explain the timing difference. The fast 8-connection runs were highly variable, so their median is not stable evidence of a gain. These local fixture results do not establish behavior on public internet routes. No optimization is recommended from this sample: scheduler-lock wait and disk-write duration are still unmeasured, and the fixture's pacing is synthetic.

Raw traces and summary: `benchmarks/results/local_telemetry_audit_20261026/` (16 MiB validation) and `benchmarks/results/local_telemetry_audit_20261026_128m/` (12-run diagnostic matrix). Each matrix directory contains one JSON file per run plus `summary.json`.

### Lock and disk timing follow-up (2026-10-26)

Added runtime measurements for scheduler lock wait/hold, segment-write lock wait/hold, per-write duration, worker queue wait, coordinator polling, and server response-header/body phases. The local fixture separately controls fixed response latency and per-chunk body pacing. No scheduler decisions or download behavior changed.

Ran three repetitions for each of four cases (`fast`, 25 ms `latency_only`, 5 ms per-256-KiB `pacing_only`, and combined `latency_paced`) at 4 and 8 configured connections with identical 128 MiB payload bytes. All 24 runs completed, client/server telemetry reconciled, and output SHA-256 matched.

| Fixture | Connections | Transfer median (range), ms | Scheduler lock wait/hold median, ms | Segment-write lock wait/hold median, ms | File-write total median, ms | Worker occupancy median | Fixture header/body median, ms |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Fast | 4 | 777 (562–956) | 1.9 / 11.8 | 5.7 / 257.8 | 252.4 | 75.4% | 0.6 / 285.4 |
| Fast | 8 | 879 (603–1,168) | 25.8 / 105.5 | 62.8 / 504.6 | 477.3 | 43.6% | 0.6 / 194.2 |
| Latency only | 4 | 1,072 (890–1,077) | 46.7 / 121.2 | 104.2 / 534.7 | 519.9 | 52.3% | 26.0 / 15.9 |
| Latency only | 8 | 1,119 (1,114–1,176) | 144.2 / 238.3 | 174.1 / 714.0 | 672.9 | 37.6% | 26.1 / 81.5 |
| Pacing only | 4 | 1,414 (1,066–1,509) | 41.6 / 109.7 | 99.7 / 668.3 | 661.3 | 66.3% | 0.6 / 803.1 |
| Pacing only | 8 | 1,387 (921–2,267) | 41.3 / 151.3 | 131.3 / 788.0 | 769.9 | 53.2% | 0.6 / 219.7 |
| Latency paced | 4 | 1,352 (1,025–1,876) | 41.0 / 104.2 | 97.3 / 502.4 | 491.3 | 71.5% | 26.0 / 439.0 |
| Latency paced | 8 | 1,192 (934–1,393) | 69.8 / 155.8 | 116.2 / 540.3 | 521.4 | 60.8% | 26.1 / 182.7 |

Lock/write totals are accumulated across concurrent threads and can overlap in wall time; segment-write lock hold includes callback work. The request non-callback time is only an approximation and includes WinHTTP setup as well as response/network waiting. The latency-only case measured about 26 ms to response headers, while pacing-only showed sub-millisecond header delay and longer body-handler time, confirming the fixture distinguishes those effects. Scheduler-lock waits rise notably at 8 connections in fast and latency-only runs, making contention a plausible contributor, but the transfer medians also vary by scenario and overlap. Runtime tracing adds logging/timing overhead, so these transfer timings are diagnostic rather than production-performance measurements. These measurements do not establish lock contention, disk writing, worker scheduling, or network waiting as the single general bottleneck. No optimization is recommended from this local sample.

The 24 per-run JSON traces and `summary.json` are in `benchmarks/results/local_lock_disk_audit_20261026/`; `report.md` contains full interpretation and verification details. Incremental MSVC Release build succeeded, the C++ suite passed 72/72, and the standalone telemetry fixture test passed. The active Python environment has no `pytest` module, so that test was invoked directly.
