<p align="center">
  <picture>
    <img alt="Radium Download Manager" src="https://img.shields.io/badge/RADIUM-DOWNLOAD%20MANAGER-e03131?style=for-the-badge&logo=windows11&logoColor=white" height="42">
  </picture>
</p>

<h1 align="center">Radium Download Manager</h1>

<p align="center">
  <b>Native Windows downloads for HTTP/HTTPS, BitTorrent, and media streams.</b><br>
  A native C++20 / Win32 download manager with segmented HTTP/HTTPS, BitTorrent v1, HLS/DASH stream downloading, and Manifest V3 browser integration.<br>
  The latest published release is <b>v0.3.2</b>; the main branch contains newer changes.
</p>

<p align="center">
  <img alt="Language: C++20" src="https://img.shields.io/badge/Language-ISO%20C%2B%2B20-00599C?style=flat-square&logo=c%2B%2B">
  <img alt="Platform: Windows 11 / Win32" src="https://img.shields.io/badge/Platform-Windows%2011%20%7C%20Win32-0078D4?style=flat-square&logo=windows11">
  <img alt="UI: Native Win32" src="https://img.shields.io/badge/UI-Native%20Win32-22c55e?style=flat-square">
  <img alt="Extension: Manifest V3" src="https://img.shields.io/badge/Extension-Manifest%20V3-eab308?style=flat-square&logo=googlechrome">
  <img alt="License: GPL-3.0-or-later" src="https://img.shields.io/badge/License-GPL--3.0--or--later-3a3a3a?style=flat-square">
  <a href="https://github.com/sayedalve/Radium-Download-Manager/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/sayedalve/Radium-Download-Manager?style=flat-square"></a>
  <a href="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml"><img alt="Windows release build" src="https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml/badge.svg?branch=main"></a>
</p>

<p align="center">
  <a href="#features"><b>Features</b></a> ·
  <a href="#everything-in-the-box"><b>Everything in the box</b></a> ·
  <a href="#high-performance-download-engine"><b>Download Engine</b></a> ·
  <a href="#bittorrent-downloads"><b>BitTorrent</b></a> ·
  <a href="#adaptive-stream-pipeline"><b>Stream Pipeline</b></a> ·
  <a href="#performance-benchmarks"><b>Benchmarks</b></a> ·
  <a href="#browser-integration"><b>Browser Capture</b></a> ·
  <a href="#built-for-automation"><b>Automation & CLI</b></a> ·
  <a href="#under-the-hood"><b>Architecture</b></a> ·
  <a href="#get-started"><b>Get Started</b></a> ·
  <a href="#downloads"><b>Downloads</b></a> ·
  <a href="#testing-and-verification"><b>Verification</b></a> ·
  <a href="#known-limitations"><b>Limitations</b></a> ·
  <a href="#contributing"><b>Contributing</b></a> ·
  <a href="#license-and-credits"><b>License & Credits</b></a>
</p>

<br>

<p align="center">
  <img src="docs/images/readme/flux-main-window.png" alt="Existing main-window screenshot retained as the application preview; legacy branding is visible" width="100%">
  <br>
  <sub>The Radium main application window: Deep Charcoal dark theme, live progress bars, speed and size indicators, and filter controls.<br>
  Existing repository capture retained unchanged; it predates the current Radium branding.</sub>
</p>

> [!NOTE]
> **Radium Download Manager is an open-source native Windows download manager.**
> Built with C++20 and Win32, Radium combines segmented HTTP/HTTPS downloads, a dedicated BitTorrent engine, browser-assisted download capture, and an out-of-process stream pipeline.

<br>

<table>
  <tr>
    <td width="25%" valign="top">
      <h3>⚡ Native and fast</h3>
      Native C++20 and Win32 desktop application. Startup and memory use are measured in project benchmarks; results depend on build and machine.
    </td>
    <td width="25%" valign="top">
      <h3>🔀 Segmented engine</h3>
      Dynamic connection scaling from 1 to 32 workers with work-stealing byte-range reuse and rollback when a server rejects a range. Parallel downloads depend on server support and network conditions.
    </td>
    <td width="25%" valign="top">
      <h3>🌐 Seamless browser capture</h3>
      A Manifest V3 browser extension for Chromium (Chrome, Brave, Edge) and Firefox. Captures files, video streams (HLS/DASH), and media playlists without unauthenticated local HTTP servers.
    </td>
    <td width="25%" valign="top">
      <h3>🔒 Private and secure</h3>
      Download tasks and settings are stored locally in SQLite. Browser communication uses Native Messaging and per-user ACL-restricted Windows Named Pipes.
    </td>
  </tr>
</table>

<br>

## Features

The main-window image is the original repository preview and remains unchanged; the dialog screenshots are captures from the running Radium application.

<table>
  <tr>
    <td width="50%" valign="top">
      <img src="docs/images/readme/flux-main-window.png" alt="Existing main-window screenshot of the download queue; retained unchanged" width="100%">
      <br>
      <sub>Multi-column download queue with file-type vector icons, real-time progress bars, and speed statistics.</sub>
      <h3>Dynamic Segment Scheduling & Queue Control</h3>
      Every HTTP download starts with pre-flight header probing (`HEAD` / Range requests) and dynamically scales connections from 1 up to 32 parallel workers.
      <br><br>
      When faster connections finish their allocated byte ranges, the work-stealing scheduler automatically splits the largest remaining unfinished range from other active workers. If a remote server rejects sub-ranges, the engine rolls back cleanly without data loss.
      <br><br>
      <b>Capabilities:</b>
      <ul>
        <li>Dynamic connection spawning (`1 -> N`) and work-stealing range reuse</li>
        <li>Automatic stall detection with connection reset on silent timeouts</li>
        <li>Pre-flight disk space guard against destination drive exhaustion</li>
        <li>Safe disk allocation using sparse files and <code>SetFileValidData</code></li>
        <li>Graceful single-connection fallback for non-range servers</li>
      </ul>
    </td>
    <td width="50%" valign="top">
      <img src="docs/images/readme/radium-new-download-dialog.jpg" alt="Radium New Download prompt dialog with URL, filename, destination folder, and immediate or scheduled download options" width="100%">
      <br>
      <sub>Native Add / Intercept Download dialog with auto-detected filenames, save paths, stream quality, and scheduling.</sub>
      <h3>Smart Interception & Pre-Flight Validation</h3>
      Whether triggered from a browser link click, media detection banner, or clipboard URL detection, Radium presents an instant, responsive Win32 dialog before starting.
      <br><br>
      The dialog verifies available free disk space on the target volume, extracts suggested filenames from RFC 5987 / RFC 6266 headers, and offers quality stream presets for adaptive media.
      <br><br>
      <b>Capabilities:</b>
      <ul>
        <li>Instant clipboard detection and link-click browser interception</li>
        <li>Real-time drive free-space calculations and disk space warnings</li>
        <li>Adaptive stream quality selection (Best, Video Only, Audio Only)</li>
        <li>Immediate download execution or deferred scheduler timing</li>
        <li>Path sanitization stripping path traversal (<code>..</code>) and NTFS alternate data streams (<code>:</code>)</li>
      </ul>
    </td>
  </tr>
  <tr>
    <td colspan="2" valign="top">
      <img src="docs/images/readme/radium-settings-dialog.jpg" alt="Radium Settings window with application preferences" width="100%">
      <br>
      <sub>In-app Settings window showing application preferences.</sub>
      <h3>Deep Configuration & Native System Integration</h3>
      The Settings window groups application preferences in one native dialog. Settings are stored locally; available controls depend on feature and application version.
    </td>
  </tr>
</table>

<br>

## Everything in the box

<table>
  <tr>
    <td width="33%" valign="top">
      <h4>🌐 Protocols &amp; Streams</h4>
      HTTP/1.1, HTTP/2, and HTTPS TLS 1.3 via native Windows <code>WinHTTP</code> with Direct, System, and Manual Proxy modes and Windows Integrated Auth (NTLM/Negotiate).<br><br>
      RFC 959 / 2428 / 3659 native WinSock2 FTP client with EPSV, PASV fallback, NAT private IP bypass, and REST 350 resume verification.<br><br>
      Dynamic mirror failover (alternate URLs) with byte-exact <code>Content-Length</code> validation to prevent partial file corruption.<br><br>
      HLS (<code>.m3u8</code>), MPEG-DASH (<code>.mpd</code>), Microsoft Smooth Streaming (MSS), and progressive containers (<code>.mp4</code>, <code>.webm</code>, <code>.mkv</code>, <code>.mov</code>, <code>.ts</code>) with pre-flight redirect resolution and 403 expired signed URL detection.
    </td>
    <td width="33%" valign="top">
      <h4>⚡ Download Engine</h4>
      Dynamic segment splitting inspired by Brisk: connection scaling (1 to 32), work-stealing range scheduler (<code>AcquireNextOrStealSegment</code>), and secondary rejection rollback (<code>RollbackUnstartedStolenSegment</code>).<br><br>
      Dual token-bucket rate limiting enforcing both per-task speed limits and global bandwidth caps without CPU busy-waiting.<br><br>
      Crash-resilient storage backed by SQLite 3 with WAL journaling. Atomic file commits via isolated <code>*.part</code> files prevent corrupt half-written downloads.<br><br>
      Automatic retry loop with exponential backoff for transient network interruptions.
    </td>
    <td width="33%" valign="top">
      <h4>🖥️ Windows 11 Native UX</h4>
      Native Win32 desktop application with a GDI-rendered interface. Deep Charcoal palette, lightweight native Windows 11 animations (smooth hover/press transitions, interpolated progress bars, dialog fades, action pulses), and live Windows Taskbar progress (<code>ITaskbarList3</code>).<br><br>
      Hierarchical category routing (Priority: User Explicit &gt; Domain Site Rules &gt; Custom Extension Mappings &gt; Configured Category Folders &gt; Subfolders &gt; Default Dir).<br><br>
      Task stop times, queue start/stop scheduling loop, and safe post-queue completion power actions (System Sleep / Shutdown via native Windows APIs).<br><br>
      Per-Monitor DPI v2 scaling, virtualized task table, middle-ellipsis filename truncation, and search-as-you-type.
    </td>
  </tr>
  <tr>
    <td width="33%" valign="top">
      <h4>🧩 Browser Integration</h4>
      Manifest V3 browser extension for Chromium (Google Chrome, Brave, Microsoft Edge) and Mozilla Firefox.<br><br>
      Stateless Native Messaging Host (<code>radium-nmh.exe</code>) bridges standard input/output to the desktop app over a per-user ACL-restricted Named Pipe (<code>\\.\pipe\Flux_IPC_&lt;SID&gt;</code>).<br><br>
      In-page "Download with Radium" video overlay button rendered inside a closed Shadow DOM (<code>mode: "closed"</code>) to prevent CSS leaks.
    </td>
    <td width="33%" valign="top">
      <h4>🤖 Automation &amp; Tools</h4>
      The desktop executable includes selected command-line modes, including daemon, queue listing, stream probing, and UI snapshots. The browser bridge uses JSON-RPC 2.0 over a local named pipe; it is not a general-purpose public API.
      Recursive Breadth-First Site Grabber with CSS <code>url()</code> &amp; HTML media attribute parsing, configurable destination folders, and SSRF private IP protection.<br><br>
      Batch task export and import via standard JSON and CSV files for easy queue backup and transfer.
    </td>
    <td width="33%" valign="top">
      <h4>🛡️ Security &amp; Privacy</h4>
      Strict path sanitization: strips path traversal (<code>..</code>), drive letters, control characters, and NTFS alternate streams (<code>:</code>).<br><br>
      Out-of-process stream extraction workers (<code>N_m3u8DL-RE</code>, <code>FFmpeg</code>) are isolated inside restricted Windows Job Objects with automatic child process cleanup.<br><br>
      Sensitive headers (<code>Cookie</code>, <code>Authorization</code>, tokens) and proxy/server credentials are encrypted at rest with Windows DPAPI and permanently redacted in logs.
    </td>
  </tr>
</table>

<br>

## High-Performance Download Engine

Radium implements an in-process, concurrent segmented HTTP/HTTPS download engine inspired by [Brisk](https://github.com/BrisklyDev/brisk).

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
3. **Range Work-Stealing:** Fast workers do not sit idle. When a worker completes its assigned range, it inspects active peers, locates the largest remaining unread byte block, splits it in half, and claims the upper half.
4. **Rejection Rollback:** If a remote server rejects a sub-range request (e.g., returning HTTP 200 instead of 206, or HTTP 416), the stolen range is rolled back to the original worker without data corruption.
5. **Atomic Commit:** Download segments write into an isolated `*.radium.part` file using pre-allocated disk storage (`SetFileValidData` / sparse files). When all segments reach 100%, the file is atomically renamed to its final target path.

See [`docs/DOWNLOAD_ENGINE_DESIGN.md`](docs/DOWNLOAD_ENGINE_DESIGN.md) for full algorithmic details.

<br>

## BitTorrent Downloads

Radium includes a dedicated BitTorrent v1 engine for local .torrent files and magnet links. The current implementation includes:

- Magnet metadata retrieval through BEP 9.
- HTTP and UDP tracker communication, plus bounded DHT peer discovery.
- Peer-wire handling, piece and block scheduling, and SHA-1 piece integrity checks.
- Multi-file storage mapping and selective file downloads through the native Add Torrent dialog.
- Persisted torrent task state and recovery of verified piece data.
- Source implementation for incoming TCP handshakes and uploading verified pieces for seeding.

**Verification status:** Parser, peer framing, tracker parsing, multi-file mapping, and piece scheduling have automated C++ coverage. Magnet metadata and tracker/DHT test paths are present. The feature matrix marks torrent behavior as partially verified; deterministic incoming-peer block transfer, live seeding, the selector UI route, and selective output still need runtime verification.

**Not supported:** Peer Exchange (PEX/BEP 11), µTP, and BitTorrent v2 (BEP 52). Several visible Add Torrent settings are not connected to the task controller, including tags, alternate incomplete-file path, stop condition, queue placement, skip-hash, sequential/first-last priority, and content layout. See [known limitations](docs/KNOWN_LIMITATIONS.md) and [test matrix](docs/TEST_MATRIX.md).

<!-- Planned capture: docs/images/readme/radium-add-torrent-dialog.png. Pending a suitable existing executable with the current selector layout. -->

<br>

## Adaptive Stream Pipeline

For media hosted via HLS or DASH manifests, Radium integrates out-of-process worker execution with [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) and [FFmpeg](https://ffmpeg.org/):

- **Job Object Sandboxing:** Workers run inside an isolated Windows Job Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), ensuring that child processes terminate immediately if Radium closes.
- **Zero Command-Line Credential Leaks:** Authentication cookies, custom request headers, and user tokens are never passed on the process command line. Instead, Radium writes headers to a temporary response file (`@job.rsp`) in a per-user ACL-restricted temporary directory (`%LOCALAPPDATA%\Radium\tmp\<task_id>\`) and deletes it upon completion.
- **Track Probing & Remuxing:** Radium probes stream manifests to identify video, audio, and subtitle streams. Once segment downloads complete, FFmpeg remuxes the media into standard MP4 or MKV containers with magic-byte verification.

See [`docs/STREAM_DOWNLOAD_PIPELINE.md`](docs/STREAM_DOWNLOAD_PIPELINE.md) for the complete pipeline specification.

<br>

## Performance Benchmarks

Radium is continuously benchmarked against industry standards including **Internet Download Manager (IDM)** and **aria2** across both live internet endpoints and controlled RFC 7233 local loopbacks.

### Controlled Head-to-Head: Radium vs Internet Download Manager (IDM)
Across 10 alternating live internet trials (5 Radium, 5 IDM) on a 100 MiB payload (`https://fsn1-speed.hetzner.com/100MB.bin`) under Falkenstein server rate-limiting:

| Metric | Radium v0.3.2 | Internet Download Manager (IDM 6.43.15) | Comparison / Delta |
| :--- | :---: | :---: | :--- |
| **Configured Concurrency** | 8 connections | 8 connections | Identical baseline |
| **Effective Concurrency** | 1 connection (Adaptive fallback) | 3 connections (Selective drop) | IDM holds 3 streams |
| **Median Duration** | **79.44 s** | **76.64 s** | IDM faster by 2.80 s (3.5%) |
| **Median Throughput** | **1.26 MiB/s** (10.56 Mbps) | **1.30 MiB/s** (10.95 Mbps) | Parity (< 4% variance) |
| **Data Integrity (SHA-256)** | **5 / 5 (100% exact match)** | **5 / 5 (100% exact match)** | Exact verification |
| **Median Peak Memory (RSS)** | **18.30 MB** | **48.80 MB** | **Radium uses 62.5% less RAM** |
| **Median Peak CPU Usage** | **15.6%** | **56.2%** | **Radium uses 72.2% less CPU** |

A later optimized Radium run completed the same payload in 64.72 seconds. That was a single run, so it is not a repeated, like-for-like comparison with IDM. See [benchmark results](benchmarks/RESULTS.md) for the test context.

### Adaptive 429 Rate-Limit Recovery (32 Connections)
In the documented rate-limited test, Radium reduced concurrency in response to HTTP 429 replies and completed the file with a matching SHA-256 digest. Results depend on server policy; see the benchmark report and raw trial data for the test setup.

Detailed datasets, machine-readable JSON logs, and instructions to reproduce locally or on live connections are available in [`benchmarks/`](benchmarks/) and [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md).

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
│ Radium.exe (Desktop Engine)       │
│  ├── Native Win32 / GDI Desktop UI  │
│  ├── In-Process Segment Engine  │
│  └── SQLite 3 State Store (WAL) │
└─────────────────────────────────┘
```

- **Dual-Origin Security:** The Chromium native host manifest authorizes both store IDs and development keys, preventing native messaging connection failures when running unpacked.
- **Zero Localhost Exposure:** Unlike legacy download managers that open an unauthenticated HTTP port on `localhost` (exposing users to drive-by website requests), Radium communicates exclusively via Native Messaging bridged to a local, user-ACL-restricted Windows Named Pipe.
- **Media Detection:** Sniffs `.mp4`, `.webm`, `.m3u8`, and `.mpd` URLs from network requests, deduplicates master and variant playlists, and ignores individual `.ts` and `.m4s` segment requests to prevent UI flooding.
- **Reliable Fallback & Gesture Safety:** Link clicks retain user gesture context without destructive cancellation. In Chromium browsers, `onDeterminingFilename` holds the save prompt while querying the desktop; if Radium is offline or rejects, the browser download automatically resumes with zero data loss. Client-side generated files (`blob:`, `data:` URLs, ChatGPT exports) are safely preserved for native browser handling without DOM monkey-patching.

See [`docs/BROWSER_EXTENSION.md`](docs/BROWSER_EXTENSION.md) and [`docs/NATIVE_MESSAGING_AND_IPC.md`](docs/NATIVE_MESSAGING_AND_IPC.md).

<br>

## Built for Automation

Radium is designed for both interactive desktop use and scriptable automation. Every major capability is accessible from the command line and over the JSON-RPC named-pipe channel.

```sh
# Start Radium in the background as a headless daemon
Radium.exe --daemon

# Probe an adaptive media stream and output JSON stream metadata
Radium.exe --probe-stream "https://example.com/live/playlist.m3u8"

# Print the current download queue to stdout
Radium.exe --list

# Export current desktop UI state and layout geometry as text
Radium.exe --gui-snapshot

# Override default database or worker paths
Radium.exe --db "%APPDATA%\Radium\custom_state.db" --worker-binary "tools\N_m3u8DL-RE.exe"
```

Over the local IPC pipe (`\\.\pipe\Flux_IPC_<SID>`), scripts and agents can send standard JSON-RPC 2.0 messages:

```json
// Example: Add a new download task via JSON-RPC
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "download.offer",
  "params": {
    "url": "https://releases.ubuntu.com/24.04.1/ubuntu-24.04.1-desktop-amd64.iso",
    "filename": "ubuntu-24.04.1-desktop-amd64.iso",
    "saveDir": "%USERPROFILE%\\Downloads"
  }
}
```

<br>

## Under the Hood

Radium is architected as an engine-first application with clean layer boundaries:

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
- **Single-Instance Win32 IPC:** Launching `Radium.exe` while an existing instance is active automatically restores and brings the existing window to the foreground.

See [`docs/SYSTEM_ARCHITECTURE.md`](docs/SYSTEM_ARCHITECTURE.md) and [`docs/DATA_MODEL_AND_STORAGE.md`](docs/DATA_MODEL_AND_STORAGE.md).

<br>

## Get Started

### System Requirements
- **Operating System:** Windows 11 (the published Windows release pipeline targets Windows 11).
- **Architecture:** x64 or ARM64; both release architectures are published.
- **Runtime components:** Core downloads use native Windows APIs; Full packages include bundled stream workers.

### Downloads

The latest published release is [v0.3.2](https://github.com/sayedalve/Radium-Download-Manager/releases/tag/v0.3.2), published October 21, 2026. The main branch contains newer changes that are not included in that release. x64 and ARM64 installers and portable Full/Core packages are available:

| Package | Format | Description |
|---|---|---|
| **Radium Installer (x64)** | [Setup](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-Setup-x64-0.3.2.exe) | Per-user Windows installer |
| **Radium Portable Full (x64)** | [Full ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-x64-Full.zip) | Portable package with bundled stream workers |
| **Radium Portable Core (x64)** | [Core ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-x64-Core.zip) | Smaller package without bundled stream workers |
| **Radium Core Installer (x64)** | [Core Setup](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-Setup-x64-Core-0.3.2.exe) | Smaller installer without bundled stream workers |
| **Radium Installer (ARM64)** | [Setup](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-Setup-ARM64-0.3.2.exe) | Windows ARM64 installer |
| **Radium Core Installer (ARM64)** | [Core Setup](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-Setup-ARM64-Core-0.3.2.exe) | Smaller Windows ARM64 installer |
| **Radium Portable Full (ARM64)** | [Full ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-ARM64-Full.zip) | Portable package with bundled stream workers |
| **Radium Portable Core (ARM64)** | [Core ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-ARM64-Core.zip) | Smaller package without bundled stream workers |
| **Browser Extension** | [extension source](extension/) | Load the unpacked extension from the repository; no separate extension ZIP is attached to the release. |

Verify downloaded assets with [SHA256SUMS.txt](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/SHA256SUMS.txt).

### Browser Extension Setup

1. **Chromium (Google Chrome, Brave, Microsoft Edge):**
   - Open your browser's extensions page (`chrome://extensions` or `brave://extensions`).
   - Enable **Developer mode** in the top-right corner.
   - Click **Load unpacked** and select the <code>extension/</code> directory.
   - Run the included PowerShell script to register the Native Messaging Host in the Windows Registry:
     ```powershell
     powershell -ExecutionPolicy Bypass -File scripts\register-native-messaging.ps1
     ```

2. **Mozilla Firefox:**
   - Open `about:debugging#/runtime/this-firefox`.
   - Click **Load Temporary Add-on...** and select `extension/manifest.firefox.json` (or `extension/manifest.json`).

### Building from Source

To compile Radium from source, use **Windows 11**, **Visual Studio 2022** (Desktop development with C++), and **CMake 3.25 or newer**. The repository build script builds the native targets, worker, and tests, and stages the portable application under <code>dist/</code>:

```powershell
git clone https://github.com/sayedalve/Radium-Download-Manager.git
cd Radium-Download-Manager
.\scripts\build-windows-msvc.ps1
```
The build script stages the portable application and Native Messaging Host under dist. See [`docs/DEVELOPMENT_SETUP.md`](docs/DEVELOPMENT_SETUP.md) and [`docs/WINDOWS_BUILD_AND_PACKAGING.md`](docs/WINDOWS_BUILD_AND_PACKAGING.md).

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

The source state at main commit 5af0b06 passed the [Windows production build and verification](https://github.com/sayedalve/Radium-Download-Manager/actions/runs/40198169282) and [Windows compatibility matrix](https://github.com/sayedalve/Radium-Download-Manager/actions/runs/40198169275) on October 25, 2026. This README update does not change source code. The feature and test matrices identify behaviors that still need live Windows verification, including torrent UI interaction and incoming-peer seeding.

- [Feature matrix](docs/FEATURE_MATRIX.md)
- [Test matrix](docs/TEST_MATRIX.md)
- [Windows release workflow](https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml)

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

See [`LICENSE_REVIEW.md`](LICENSE_REVIEW.md) and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

<br>

<p align="center">
  <sub>Radium Download Manager · High-performance, native open-source download management for Windows.</sub><br>
  <sub>Built with C++20 and Win32. Licensed under GPL-3.0-or-later.</sub>
</p>
