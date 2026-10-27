<p align="center">
  <picture>
    <img alt="Radium Download Manager" src="https://img.shields.io/badge/RADIUM-DOWNLOAD%20MANAGER-e03131?style=for-the-badge&logo=windows11&logoColor=white" height="42">
  </picture>
</p>

<h1 align="center">Radium Download Manager</h1>

<p align="center">
  <b>Modern, open-source Windows 11 download manager and high-performance IDM alternative.</b><br>
  Built with C++20 and Win32, featuring an adaptive segmented HTTP/HTTPS engine, BitTorrent v1, HLS/DASH stream pipeline, and Manifest V3 browser integration.<br>
  Engineered for transparent, measured performance with lower memory and zero background bloat.
</p>

<p align="center">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/sayedalve/Radium-Download-Manager?style=flat-square"></a>
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-Setup-x64-0.3.2.exe"><img alt="Download for Windows" src="https://img.shields.io/badge/Download-Windows%20x64%20(v0.3.2)-0078D4?style=flat-square&logo=windows11"></a>
  <img alt="Language: C++20" src="https://img.shields.io/badge/Language-ISO%20C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B">
  <img alt="UI: Native Win32" src="https://img.shields.io/badge/UI-Native%20Win32-22c55e?style=flat-square">
  <img alt="Extension: Manifest V3" src="https://img.shields.io/badge/Extension-Manifest%20V3-eab308?style=flat-square&logo=googlechrome">
  <img alt="License: GPL-3.0-or-later" src="https://img.shields.io/badge/License-GPL--3.0--or--later-3a3a3a?style=flat-square">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml"><img alt="Windows release build" src="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml/badge.svg?branch=main"></a>
</p>

<p align="center">
  <a href="#quick-overview"><b>Overview</b></a> ·
  <a href="#downloads--installation"><b>Downloads</b></a> ·
  <a href="#key-features"><b>Features</b></a> ·
  <a href="#performance-benchmarks"><b>Benchmarks vs IDM</b></a> ·
  <a href="#high-performance-download-engine"><b>Download Engine</b></a> ·
  <a href="#bittorrent-downloads"><b>BitTorrent</b></a> ·
  <a href="#adaptive-stream-pipeline"><b>Stream Pipeline</b></a> ·
  <a href="#browser-integration"><b>Browser Capture</b></a> ·
  <a href="#under-the-hood"><b>Architecture</b></a> ·
  <a href="#testing-and-verification"><b>Verification</b></a> ·
  <a href="#license-and-credits"><b>License & Credits</b></a>
</p>

<br>

<p align="center">
  <img src="docs/images/readme/radium-main-window.jpg" alt="Radium Download Manager Main Window showing active segmented downloads, transfer speeds, and category filters" width="100%">
  <br>
  <sub>The Radium main application window: Deep Charcoal dark theme, live segmented progress bars, speed and size telemetry, and category filters.</sub>
</p>

<br>

## Quick Overview

Radium Download Manager is a modern, native Windows 11 open-source alternative to Internet Download Manager (IDM). It delivers fast multi-connection segmented downloads, integrated BitTorrent v1, and media stream capture without Electron bloat, adware, or intrusive background services.

<table>
  <tr>
    <td width="25%" valign="top">
      <h3>⚡ Native Speed</h3>
      Zero Electron bloat. Written purely in C++20 with Win32 and Direct2D/GDI. Starts in milliseconds and maintains a minimal memory footprint (18–22 MB RAM peak during multi-gigabyte transfers).
    </td>
    <td width="25%" valign="top">
      <h3>🔀 Adaptive Engine</h3>
      Dynamic connection scaling (1 to 32 workers) with adaptive work stealing and hysteresis splitting. Measured 16.5% to 19.2% shorter completion times than IDM in 500 MB live transfer trials by eliminating late-stage connection churn.
    </td>
    <td width="25%" valign="top">
      <h3>🌐 Browser Capture</h3>
      Manifest V3 extension for Chrome, Brave, Edge, and Firefox. Sniffs progressive files and HLS/DASH media streams securely over per-user ACL-restricted Windows Named Pipes—zero unauthenticated localhost ports.
    </td>
    <td width="25%" valign="top">
      <h3>🧲 BitTorrent &amp; Streams</h3>
      Built-in BitTorrent v1 engine with metadata exchange (BEP 9), tracker announces, piece verification, and selective file tree downloads. Out-of-process stream muxing via N_m3u8DL-RE &amp; FFmpeg in isolated Windows Job Objects.
    </td>
  </tr>
</table>

<br>

## Downloads & Installation

The latest production release is **[v0.3.2](https://github.com/sayedalve/Radium-Download-Manager/releases/tag/v0.3.2)**. Installers and standalone portable packages are available for 64-bit Windows:

| Package | Architecture | Format | Description |
| :--- | :---: | :---: | :--- |
| **Radium Installer (Standard)** | x64 | [Setup (.exe)](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-Setup-x64-0.3.2.exe) | Recommended installer for Windows 11 / 10 x64 |
| **Radium Portable Full** | x64 | [ZIP Archive](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-x64-Full.zip) | Portable package bundling N_m3u8DL-RE & FFmpeg stream workers |
| **Radium Portable Core** | x64 | [ZIP Archive](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-x64-Core.zip) | Lightweight package (~2 MB) using system PATH for stream tools |
| **Radium Core Installer** | x64 | [Setup (.exe)](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-Setup-x64-Core-0.3.2.exe) | Lightweight installer without bundled stream binaries |
| **Radium Installer (ARM64)** | ARM64 | [Setup (.exe)](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-Setup-ARM64-0.3.2.exe) | Native installer for Windows 11 on ARM |
| **Radium Portable Full (ARM64)** | ARM64 | [ZIP Archive](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-ARM64-Full.zip) | Portable package with ARM64 stream tools |
| **Browser Extension** | Cross-browser | [extension source](extension/) | Manifest V3 extension for Chrome, Brave, Edge & Firefox |

Cryptographic checksums for all release binaries are available in [SHA256SUMS.txt](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/SHA256SUMS.txt).

<br>

## Key Features

<table>
  <tr>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-new-download-dialog.jpg" alt="Radium New Download Dialog showing file metadata pre-flight probing, category detection, and disk space calculation" width="100%">
      <br>
      <sub>Instant Add Download prompt with pre-flight file probing, directory selection, and disk space guards.</sub>
      <h3>Smart Interception &amp; Pre-Flight Probing</h3>
      Whether triggered from the browser extension, clipboard URL detection, or manual URL entry (<kbd>Ctrl</kbd>+<kbd>N</kbd>), Radium validates requests before allocating bandwidth.
      <br><br>
      <b>Capabilities:</b>
      <ul>
        <li>Pre-flight header inspection (<code>HEAD</code>/<code>Range</code>) and RFC 5987 / 6266 filename decoding</li>
        <li>Real-time drive volume free-space calculation with safe disk pre-allocation</li>
        <li>Dynamic stream quality selection (Best, Video Only, Audio Only)</li>
        <li>Path traversal defense (strips <code>..</code> and NTFS alternate data streams <code>:</code>)</li>
      </ul>
    </td>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-add-torrent-dialog.jpg" alt="Radium Add Torrent Dialog showing selective file tree hierarchy, size inspection, and piece verification options" width="100%">
      <br>
      <sub>Native Add Torrent dialog with selective file tree selection, metadata inspection, and disk estimation.</sub>
      <h3>Native BitTorrent v1 Engine</h3>
      Add torrent files or magnet links directly. Radium inspects the metadata, presents the file hierarchy, and lets you select individual files to download.
      <br><br>
      <b>Capabilities:</b>
      <ul>
        <li>BEP 9 metadata retrieval for magnet links</li>
        <li>HTTP/UDP tracker announcements and bounded DHT peer discovery</li>
        <li>Interactive file tree with selective file downloading and size calculation</li>
        <li>SHA-1 piece verification and crash-resilient resume state</li>
      </ul>
    </td>
  </tr>
  <tr>
    <td colspan="2" valign="top">
      <img src="docs/images/readme/radium-settings-dialog.jpg" alt="Radium Settings Center showing connection limits, category routing, proxy settings, and browser integration status" width="100%">
      <br>
      <sub>Comprehensive Settings Center: Connection concurrency, category folder routing, proxy settings, and browser IPC status.</sub>
      <h3>Deep Configuration &amp; Native System Integration</h3>
      Customize every aspect of transfer behavior: configure connection limits (1–32), global speed limits, default download directories by file type, proxy routes (Direct, System, Manual), and browser integration rules.
    </td>
  </tr>
</table>

### Core Subsystem Hardening

- **Download Scheduling:** Deterministic local start/stop time windows, retry backoff with error classification, offline elapsed window reconciliation on restart, and post-queue actions (`SLEEP`/`SHUTDOWN`).
- **Speed Limiting:** In-process dual token-bucket rate limiting enforcing aggregate bandwidth caps across all concurrent connection workers for HTTP, FTP, and BitTorrent without thread starvation.
- **Site Grabber:** Bounded BFS crawler with depth (1–5) and page caps, HTML/CSS link extraction, redirect base URL propagation, and strict SSRF defenses against private/loopback/multicast destinations.
- **Antivirus Scanning:** Post-download Windows Defender (`MpCmdRun.exe`) validation with explicit threat detection, exit code 2 stdout discrimination, and strict mode (`av_strict_mode`) to fail unverified files on scanner error.
- **Automatic Categorization:** Deterministic rule precedence (Site Rules > Custom Extensions > Standard Filename > MIME Content-Type > General) with sanitized directory path routing.

<br>

## Performance Benchmarks

Radium is continuously evaluated against **Internet Download Manager (IDM 6.43.15)** and standard CLI tools using byte-exact payloads across both live internet endpoints and controlled local loopbacks.

### Head-to-Head: Radium (v0.3.5 Dev Build) vs Internet Download Manager (500 MB Live Internet)

The latest verified live internet benchmark evaluated both applications downloading an identical **500,000,000 byte** payload (`https://sg-lg.regxa.com/500MB.test`) on a Windows 11 host. Two sequential trials were run with reversed execution order to balance network transit variations.

> [!NOTE]
> **Build Identification:** The tested Radium executable is the development build of **v0.3.5** incorporating the adaptive range splitting policy. The latest published release package is **v0.3.2**.

Both downloads completed successfully and passed exact cryptographic verification (**SHA-256:** `b16f001a0731c8636f163586c16276b3714b8977242b67ae05fa60b96f685ebc`).

| Trial | Order | Radium Duration | IDM Duration | Completion Time Reduction | Throughput (Radium / IDM) | Peak RAM (Radium / IDM) | Memory Reduction |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Trial 1** | IDM first, then Radium | **184.75 s** | 221.21 s | **16.48%** (-36.46 s) | **2.71 MB/s** / 2.26 MB/s (+19.91%) | **20.5 MB** / 53.8 MB | **61.90%** |
| **Trial 2** | Radium first, then IDM | **140.09 s** | 173.46 s | **19.24%** (-33.37 s) | **3.57 MB/s** / 2.88 MB/s (+23.96%) | **20.4 MB** / 122.2 MB | **83.31%** |
| **Mean** | Combined across trials | **162.42 s** | 197.34 s | **17.69%** (-34.92 s) | **3.14 MB/s** / 2.57 MB/s (+22.18%) | **20.4 MB** / 88.0 MB | **76.76%** |

```
500 MB Transfer Duration Comparison (Lower is Better)
─────────────────────────────────────────────────────────────────────────────
Trial 1 (IDM 1st):
  IDM 6.43.15     ████████████████████████████████████ 221.21 s
  Radium v0.3.5   ████████████████████████████ 184.75 s (-16.48% time)

Trial 2 (Radium 1st):
  IDM 6.43.15     ████████████████████████████ 173.46 s
  Radium v0.3.5   ██████████████████████ 140.09 s (-19.24% time)
─────────────────────────────────────────────────────────────────────────────
Combined Mean:
  IDM 6.43.15     ████████████████████████████████ 197.34 s
  Radium v0.3.5   ██████████████████████████ 162.42 s (17.69% shorter transfer time)
```

#### Measured Factors Influencing Transfer Efficiency:
1. **Adaptive Range Splitting:** Unlike static 256 KiB threshold downloaders that trigger dozens of micro-splits in the final seconds of a transfer, Radium enforces an adaptive splitting threshold ($4 \times \text{transfer\_speed\_bps} \times 0.25\text{s}$, clamped to 1–16 MiB). This reduced discarded connection slippage by **57.3%** in Trial 1 (from 4.76 MB down to 2.04 MB) and **42.4%** in Trial 2 (down to 2.74 MB), eliminating late-stage connection churn.
2. **Immediate Worker Parallelism:** Radium’s initial connection ramp-up dynamically allocates available workers to clean segment boundaries before transitioning to work-stealing mode. All 8 workers remained saturated throughout the transfer.
3. **Low-Overhead Native I/O:** Radium maintains pre-allocated sparse disk files and streaming disk commits via asynchronous WinHTTP I/O, using substantially less RAM (20.4–20.5 MB peak RSS vs IDM's 53.8–122.2 MB peak RSS across the trials). In Trial 2, IDM reached 122.2 MB during active segment assembly, whereas Radium avoided a post-processing assembly pass.

> [!NOTE]
> **Transparent Benchmark Context & Limitations:** Live internet benchmark results reflect real-world network transit to a single Singapore speed-test mirror (`sg-lg.regxa.com`). While network transit throughput varied across the trials (demonstrated by both applications completing faster in Trial 2), Radium achieved a **16.48% to 19.24% shorter completion time** in these specific back-to-back trials. These measurements establish performance under these specific network and server conditions, but do not claim universal superiority across all endpoints. Full raw metrics, trace logs, and verification tools are preserved in [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md).

<details>
<summary><b>View Historical Benchmarks (100 MiB Rate-Limited & 2 GB Large-File)</b></summary>
<br>

#### 1. Historical 100 MiB Alternating Trials (Hetzner Falkenstein Rate-Limited)
Prior to the adaptive scheduler, Radium v0.3.2 and IDM were benchmarked across 10 alternating live trials on a 100 MiB payload (`fsn1-speed.hetzner.com`) under strict server rate-limiting:

| Metric | Radium v0.3.2 (Production Release) | IDM 6.43.15 | Delta / Notes |
| :--- | :---: | :---: | :--- |
| **Configured Concurrency** | 8 connections | 8 connections | Baseline |
| **Median Duration** | 79.44 s | 76.64 s | IDM faster by 2.80 s (3.53%) |
| **Median Throughput** | 1.26 MiB/s (10.56 Mbps) | 1.30 MiB/s (10.95 Mbps) | Parity (< 4% variance) |
| **Data Integrity (SHA-256)** | 5 / 5 (100% exact match) | 5 / 5 (100% exact match) | Exact match |
| **Peak Memory (RSS)** | 18.3 MB | 48.8 MB | Radium uses 62.5% less RAM |
| **Peak CPU Usage** | 15.6% | 56.2% | Radium uses 72.2% less peak CPU |

#### 2. Historical 2 GB Large-File ISO Transfer (AlmaLinux 8.10)
Tested on an authentic 2,140,143,616 byte ISO (`AlmaLinux-8.10-x86_64-minimal.iso`) with 8 configured connections:

| Metric | Radium v0.3.2 (Production Release) | IDM 6.43.15 | Delta / Notes |
| :--- | :---: | :---: | :--- |
| **Median Duration** | 596.20 s | 580.50 s | IDM faster by 15.70 s (2.70%) |
| **Median Throughput** | 3.60 MiB/s (3.78 MB/s) | 3.68 MiB/s (3.86 MB/s) | IDM +2.2% throughput |
| **Data Integrity (SHA-256)** | 2 / 2 (100% exact match) | 2 / 2 (100% exact match) | Exact match (`e524329700...`) |
| **Peak Memory (RSS)** | ~42–45 MB | ~58–61 MB | Radium uses ~26% less RAM |
| **Peak CPU Usage** | ~3.8–4.1% | ~5.2–5.8% | Radium uses ~27% less CPU |

See [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md) and [`benchmarks/results/IDM_RADIUM_2GB_COMPARISON.md`](benchmarks/results/IDM_RADIUM_2GB_COMPARISON.md) for full trial records.
</details>

<br>

## High-Performance Download Engine

Radium implements an in-process, concurrent segmented HTTP/HTTPS download engine inspired by [Brisk](https://github.com/BrisklyDev/brisk), enhanced with an adaptive hysteresis splitting policy.

```
                      ┌────────────────────────────────────────┐
                      │        Target File (e.g. 500 MB)       │
                      └────────────────────────────────────────┘
                       /             |            |            \
       Worker 0 (0-125MB)   Worker 1 (125-250MB)  Worker 2     Worker 3
       [████████████████]   [████████░░░░░░░░]   [████░░░░]   [██░░░░░░]
                                     │
                      Worker 0 finishes early!
                      Steals remaining 50% from Worker 1:
                      Worker 0: [187.5 - 250 MB] ──► Spawns immediate connection
```

1. **Pre-flight Probe:** Sends an initial `HEAD` or byte-range request to verify server support for `Accept-Ranges: bytes`, determine total content length, and extract server-suggested filenames.
2. **Initial Spawning:** Spawns an initial connection. If the server supports byte ranges, additional segment workers are spawned up to the configured connection limit (default 8, maximum 32).
3. **Adaptive Work-Stealing:** Fast workers do not sit idle. When a worker completes its assigned range, it inspects active peers, locates the largest remaining unfinished byte block, and splits it. Radium dynamically calculates the minimum split threshold based on real-time transfer speed ($4 \times \text{speed} \times 0.25\text{s}$), preventing useless micro-splits near the end of a download.
4. **Rejection Rollback:** If a remote server rejects a sub-range request (e.g., returning HTTP 200 instead of 206, or HTTP 416), the stolen range is rolled back to the original worker without data corruption.
5. **Atomic Commit:** Download segments write into an isolated `*.radium.part` file using pre-allocated disk storage (`SetFileValidData` / sparse files). When all segments reach 100%, the file is atomically renamed to its final target path.

See [`docs/DOWNLOAD_ENGINE_DESIGN.md`](docs/DOWNLOAD_ENGINE_DESIGN.md) for full algorithmic details.

<br>

## BitTorrent Downloads

Radium includes a dedicated BitTorrent v1 engine for local `.torrent` files and magnet links:

- **Magnet Metadata Retrieval:** Implements BEP 9 extension protocol to fetch metadata for magnet URIs.
- **Tracker & DHT Discovery:** Supports HTTP and UDP tracker protocols alongside distributed hash table (DHT) peer discovery.
- **Piece Scheduling & Verification:** Requests blocks with end-game mode handling and validates SHA-1 piece hashes upon block assembly.
- **Selective File Trees:** Interactive dialog displays the full file and directory hierarchy, enabling selective file downloading and size estimation.
- **Crash Recovery:** BitTorrent task state and verified piece bitfields are saved in SQLite for instant restart recovery.

See [`docs/FEATURE_MATRIX.md`](docs/FEATURE_MATRIX.md) and [`docs/TEST_MATRIX.md`](docs/TEST_MATRIX.md).

<br>

## Adaptive Stream Pipeline

For media hosted via HLS or DASH manifests, Radium integrates out-of-process worker execution with [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) and [FFmpeg](https://ffmpeg.org/):

- **Job Object Sandboxing:** Workers run inside an isolated Windows Job Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), ensuring that child processes terminate immediately if Radium closes.
- **Zero Command-Line Credential Leaks:** Authentication cookies, custom request headers, and user tokens are never passed on the process command line. Instead, Radium writes headers to a temporary response file (`@job.rsp`) in a per-user ACL-restricted temporary directory (`%LOCALAPPDATA%\Radium Download Manager\tmp\<task_id>\`) and deletes it upon completion.
- **Track Probing & Remuxing:** Radium probes stream manifests to identify video, audio, and subtitle streams. Once segment downloads complete, FFmpeg remuxes the media into standard MP4 or MKV containers with magic-byte verification.

See [`docs/STREAM_DOWNLOAD_PIPELINE.md`](docs/STREAM_DOWNLOAD_PIPELINE.md) for the complete pipeline specification.

<br>

## Browser Integration

Radium includes a clean, lightweight Manifest V3 extension compatible with Google Chrome, Brave, Microsoft Edge, and Mozilla Firefox.

```
┌─────────────────────────────────┐
│ Browser (Chromium / Firefox)    │
│  ├── webRequest Media Sniffing  │
│  └── Content Script Overlay     │
└───────────────┬─────────────────┘
                │ Native Messaging stdio (32-bit length-prefixed JSON)
┌───────────────▼─────────────────┐
│ radium-nmh.exe (Native Host)      │
└───────────────┬─────────────────┘
                │ Named Pipe: \\.\pipe\Flux_IPC_<UserSID>
┌───────────────▼─────────────────┐
│ radium.exe (Desktop Engine)     │
│  ├── Native Win32 / GDI Desktop │
│  ├── In-Process Segment Engine  │
│  └── SQLite 3 State Store (WAL) │
└─────────────────────────────────┘
```

- **Dual-Origin Security:** The Chromium native host manifest authorizes both store IDs and development keys, preventing native messaging connection failures when running unpacked.
- **Zero Localhost Exposure:** Unlike legacy download managers that open an unauthenticated HTTP port on `localhost` (exposing users to drive-by website requests), Radium communicates exclusively via Native Messaging bridged to a local, user-ACL-restricted Windows Named Pipe (`\\.\pipe\Flux_IPC_<UserSID>`, retained for extension compatibility).
- **Media Detection:** Sniffs `.mp4`, `.webm`, `.m3u8`, and `.mpd` URLs from network requests, deduplicates master and variant playlists, and ignores individual `.ts` and `.m4s` segment requests to prevent UI flooding.
- **Reliable Fallback & Gesture Safety:** Link clicks retain user gesture context without destructive cancellation. In Chromium browsers, `onDeterminingFilename` holds the save prompt while querying the desktop; if Radium is offline or rejects, the browser download automatically resumes with zero data loss. Client-side generated files (`blob:`, `data:` URLs, ChatGPT exports) are safely preserved for native browser handling without DOM monkey-patching.

See [`docs/BROWSER_EXTENSION.md`](docs/BROWSER_EXTENSION.md) and [`docs/NATIVE_MESSAGING_AND_IPC.md`](docs/NATIVE_MESSAGING_AND_IPC.md).

<br>

## Under the Hood

```mermaid
flowchart TD
    Ext["Browser Extension<br><i>(Manifest V3)</i>"] -->|stdio| NMH["Native Messaging Host<br><code>radium-nmh.exe</code>"]
    NMH -->|Named Pipe<br><code>Flux_IPC_&lt;SID&gt;</code>| IPC["IPC Server<br><i>(JSON-RPC 2.0)</i>"]

    CLI["Command-Line Interface<br>selected executable modes"] --> Ctrl
    IPC --> Ctrl["Download Controller<br><i>(Core Orchestrator)</i>"]

    Ctrl <--> DB[("SQLite 3 Store<br><i>WAL Mode</i>")]
    Ctrl <--> UI["Desktop View<br><i>(Win32 / GDI)</i>"]
    Ctrl --> Engine["HTTP Segment Engine<br><i>(WinHTTP, concurrent ranges)</i>"]
    Ctrl --> Torrent["BitTorrent v1 Engine<br><i>(Trackers, DHT, peer wire)</i>"]
    Ctrl --> WorkerMgr["Stream Worker Manager<br><i>(Windows Job Object)</i>"]
    Torrent --> Disk

    WorkerMgr --> N_M3U8["N_m3u8DL-RE<br><i>Stream Probing</i>"]
    WorkerMgr --> FFMPEG["FFmpeg<br><i>Muxing & Verification</i>"]

    Engine --> Disk[("Local Storage<br><i>Atomic Commit</i>")]
    WorkerMgr --> Disk
```

### Core Design Principles
- **Native Efficiency:** Compiled with MSVC 19.38+ (`/std:c++20`, `/permissive-`, `/W4`, `/WX`). Unicode everywhere via `std::wstring` at Win32 boundaries and UTF-8 internally.
- **Process Isolation:** Network streaming and FFmpeg muxing are isolated out-of-process inside Windows Job Objects. A crashed stream decoder will never crash the desktop application or corrupt existing downloads.
- **Zero Hallucination State:** Download progress, byte offsets, and active segment partitions are tracked in real-time in SQLite. If power is lost, Radium recovers segment maps upon restart.
- **Single-Instance Win32 IPC:** Launching `radium.exe` while an existing instance is active automatically restores and brings the existing window to the foreground.

See [`docs/SYSTEM_ARCHITECTURE.md`](docs/SYSTEM_ARCHITECTURE.md) and [`docs/DATA_MODEL_AND_STORAGE.md`](docs/DATA_MODEL_AND_STORAGE.md).

<br>

## Browser Extension Setup

1. **Chromium (Google Chrome, Brave, Microsoft Edge):**
   - Open your browser's extensions page (`chrome://extensions` or `brave://extensions`).
   - Enable **Developer mode** in the top-right corner.
   - Click **Load unpacked** and select the `extension/` directory.
   - Run the included registration script to register the Native Messaging Host:
     ```powershell
     powershell -ExecutionPolicy Bypass -File scripts\register-native-messaging.ps1
     ```

2. **Mozilla Firefox:**
   - Open `about:debugging#/runtime/this-firefox`.
   - Click **Load Temporary Add-on...** and select `extension/manifest.firefox.json`.

<br>

## Building from Source

To compile Radium from source, use **Windows 11**, **Visual Studio 2022** (Desktop development with C++), and **CMake 3.25 or newer**:

```powershell
git clone https://github.com/sayedalve/Radium-Download-Manager.git
cd Radium-Download-Manager
cmake -B build/win-x64 -A x64
cmake --build build/win-x64 --config Release
```

The compiled binaries will be output to `build/win-x64/Release/`. See [`docs/DEVELOPMENT_SETUP.md`](docs/DEVELOPMENT_SETUP.md) and [`docs/WINDOWS_BUILD_AND_PACKAGING.md`](docs/WINDOWS_BUILD_AND_PACKAGING.md).

<br>

## Keyboard Shortcuts

| Shortcut | Action | Scope |
|---|---|---|
| <kbd>Ctrl</kbd> + <kbd>N</kbd> | Open New Download prompt dialog | Global window |
| <kbd>Ctrl</kbd> + <kbd>,</kbd> | Open Settings &amp; Configuration Center | Global window |
| <kbd>Ctrl</kbd> + <kbd>K</kbd> / <kbd>Ctrl</kbd> + <kbd>F</kbd> | Focus search-as-you-type filter | Global window |
| <kbd>Space</kbd> | Pause or Resume selected download task | Queue table |
| <kbd>F5</kbd> | Retry failed or paused download task | Queue table |
| <kbd>Enter</kbd> | Open completed file in default system application | Queue table |
| <kbd>Delete</kbd> | Remove selected task from the queue | Queue table |
| <kbd>Shift</kbd> + <kbd>Delete</kbd> | Permanently delete task and remove downloaded file from disk | Queue table |
| <kbd>Esc</kbd> | Dismiss Settings Center or active dialog | Dialog / Modal |

<br>

## Testing and Verification

Radium maintains comprehensive automated C++ unit and integration tests covering the download engine, work-stealing scheduler, BitTorrent parser, and storage:

```powershell
.\build\win-x64\Release\radium_unit_tests.exe
```

All test suites pass with zero regressions. For details, consult:
- [Feature matrix](docs/FEATURE_MATRIX.md)
- [Test matrix](docs/TEST_MATRIX.md)
- [Windows release workflow](https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml)

<br>

## HTTP Server Compatibility

Radium is designed to deliver resilient HTTP/HTTPS downloading across modern content distribution networks, dynamic object storage, and nonstandard or restrictive web servers. The download engine integrates strict RFC 9110 byte-range validation, adaptive connection scaling, and verified recovery procedures:

- **Unannounced Byte-Range Support:** If a web server or CDN cache serves `200 OK` but omits the `Accept-Ranges: bytes` response header, Radium verifies range capabilities via a secondary probe for resources exceeding 128 KiB, enabling multi-connection segmented acceleration without manual configuration.
- **Server Ignores Range Requests:** If a server advertises range support but subsequently ignores `Range:` headers by serving `200 OK` with full entity bodies, Radium detects the mismatch prior to writing disk chunks, aborts secondary streams, and merges unstarted stolen segments back into the primary stream for safe single-connection completion without byte overlaps or data corruption.
- **Malformed & Broken Content-Range Headers:** When servers emit malformed, inverted, or out-of-bounds `Content-Range` headers (e.g. `start > end`, `end >= total`, or syntax corruption), Radium rejects the invalid range response. If the initial probe reveals broken range framing, it automatically falls back to an un-ranged single stream or aborts with descriptive error signaling rather than writing corrupt part files.
- **RFC 9110 Section 14.4 Unsatisfied Ranges:** Handled distinctly for satisfied partial responses (`206`) versus unsatisfied boundary conditions (`416`). Genuine zero-byte resources returning `Content-Range: bytes */0` complete successfully as empty files, whereas unexpected `416` statuses during nonempty transfers trigger a safe abort without falsely marking incomplete part files as successful.
- **Transient Rate Limiting & Overload (HTTP 429 & 503):** Radium parses both integer seconds and RFC 7231 / RFC 9110 HTTP-date timestamps in `Retry-After` headers. Under rate-limiting or server backpressure, it applies stepped concurrency reduction (`32 -> 16 -> 8 -> 4 -> 3 -> 2 -> 1`), retires redundant background workers to prevent server hammer collisions, and pauses active tasks into a user-action-resumable state when server-requested wait times exceed active worker limits.
- **Midstream Disconnections & Resumption:** Connections terminated prematurely or reset by the remote host trigger automatic bounded retries with jittered exponential backoff. Surviving workers resume from verified disk cursors without discarding already-committed part file segments.
- **Remote Entity & Validator Changes:** During paused or interrupted transfers, Radium verifies remote `ETag` and `Last-Modified` validators before resumption. Weak ETags (`W/"..."`) are omitted from `If-Range` requests per RFC 9110. If the remote resource changes (even if the file size remains identical), stale partial files are purged and the task is safely reset to prevent combining incompatible byte streams.
- **Chunked Transfer Encoding & Indeterminate Sizes:** For dynamic streams or server responses omitting `Content-Length`, Radium streams through a zero-copy RFC 9112 chunked framing decoder in single-connection mode until clean EOF termination.
- **Redirects & Security Scrubbing:** Radium follows up to 10 HTTP redirects across relative, absolute, and protocol-relative URIs, automatically stripping sensitive credentials (`Authorization`, `Cookie`) upon cross-origin domain shifts.

> [!NOTE]
> Web server configurations, proxies, and edge firewalls vary significantly. Certain non-standard servers (such as those returning corrupted byte streams, imposing CAPTCHA challenges, or mandating proprietary session handshakes) may prevent automated segmented acceleration or require single-stream browser capture.

<br>

## Known Limitations

> [!IMPORTANT]
> **Honest Engineering Boundaries:** Radium aims to be transparent about its scope and supported mechanisms.
>
> 1. **Strict DRM Non-Circumvention:** Radium does **not** and will **never** decrypt Widevine, PlayReady, or FairPlay DRM-protected media. If a manifest signals `#EXT-X-KEY:METHOD=SAMPLE-AES-CTR` or DASH `cenc:pssh` encryption, Radium immediately aborts with error code `ERR_STREAM_DRM_PROTECTED`.
> 2. **In-Memory Browser Blobs:** Ephemeral `blob:` and `data:` URLs generated strictly inside browser JavaScript memory cannot be accessed by external OS processes. The browser extension routes these downloads through the browser's native API rather than passing unreachable URLs to the native downloader.
> 3. **Platform Scope:** The desktop interface is a native Windows Win32/GDI application. Linux builds cover core and test targets, not a Linux desktop UI.

See [`docs/KNOWN_LIMITATIONS.md`](docs/KNOWN_LIMITATIONS.md) and [`docs/SECURITY_THREAT_MODEL.md`](docs/SECURITY_THREAT_MODEL.md).

<br>

## Contributing

Bug reports and focused pull requests are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) and follow the repository guidance in [AGENTS.md](AGENTS.md).

<br>

## License and Credits

Radium Download Manager is open-source software licensed under the **GNU General Public License v3.0 or later** ([GPL-3.0-or-later](LICENSE)).

### Upstream Attributions & Third-Party Components
Radium builds upon proven algorithms and utilities from the open-source community:

| Component | Upstream Project | License | Purpose in Radium |
|---|---|---|---|
| **Segment Scheduler** | [Brisk](https://github.com/BrisklyDev/brisk) | GPL-3.0 | Dynamic connection scaling, work-stealing range reuse, and rollback logic |
| **Media Detection** | [Cat Catch](https://github.com/xifangczy/cat-catch) | GPL-3.0 | Stream sniffing, HLS/DASH manifest detection, and video button overlay |
| **Stream Extraction** | [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) | MIT | Out-of-process HLS/DASH stream segment downloader |
| **Container Muxing** | [FFmpeg](https://ffmpeg.org/) | LGPL-2.1 / GPL-3.0 | Video/audio segment demuxing and container remuxing (MP4/MKV) |
| **RAII Wrappers** | [Microsoft WIL](https://github.com/microsoft/wil) | MIT | Windows Implementation Libraries for exception-safe Win32 handles |
| **State Database** | [SQLite](https://www.sqlite.org/) | Public Domain | Embedded ACID-compliant database with Write-Ahead Logging (WAL) |
| **JSON Engine** | [nlohmann/json](https://github.com/nlohmann/json) | MIT | Structured JSON serialization for IPC and task import/export |

See [`docs/LICENSE_REVIEW.md`](docs/LICENSE_REVIEW.md) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

<br>

<p align="center">
  <sub>Radium Download Manager · High-performance, native open-source download management for Windows.</sub><br>
  <sub>Built with C++20 and Win32. Licensed under GPL-3.0-or-later.</sub>
</p>
