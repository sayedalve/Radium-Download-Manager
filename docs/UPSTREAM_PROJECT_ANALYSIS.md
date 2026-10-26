# Upstream Project Analysis

**Document Status:** Verified Upstream Research  
**Last Reviewed:** 2026-10-16  
**Scope:** Technical and architectural investigation of Brisk, Brisk Browser Extension, Cat Catch, and N_m3u8DL-RE.  
**Related Documents:** [LICENSE_REVIEW.md](LICENSE_REVIEW.md), [adr/0004-media-detection.md](adr/0004-media-detection.md), [adr/0005-download-engine-integration.md](adr/0005-download-engine-integration.md), [DOWNLOAD_ENGINE_DESIGN.md](DOWNLOAD_ENGINE_DESIGN.md), [MEDIA_DETECTION.md](MEDIA_DETECTION.md), [STREAM_DOWNLOAD_PIPELINE.md](STREAM_DOWNLOAD_PIPELINE.md)

---

## 1. Cross-Project Summary Matrix

| Attribute | A1. Brisk Desktop | A2. Brisk Browser Extension | B. Cat Catch (`猫抓`) | C. N_m3u8DL-RE |
| :--- | :--- | :--- | :--- | :--- |
| **Repository URL** | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) (plus [brisk_download_engine](https://github.com/BrisklyDev/brisk_download_engine) & [kasma](https://github.com/BrisklyDev/kasma)) | [BrisklyDev/brisk-browser-extension](https://github.com/BrisklyDev/brisk-browser-extension) | [xifangczy/cat-catch](https://github.com/xifangczy/cat-catch) & [cat-catch-book](https://github.com/xifangczy/cat-catch-book) | [nilaoda/N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) |
| **Inspected Version / Commit** | `main` branch (2026-10, ~1,543 stars, 61 forks) | Tag `v1.4.1`, Commit `4f60c7f` (59 commits) | Tags `2.7.1` (`b6cd7c5`) & `2.7.2`; `cat-catch-book` commit `71d2329` | Tag `v0.6.0-beta` (`0.6.0 20261012`), Commit `14b12f5` (612 commits, 8.9k stars) |
| **Verified License** | `GPL-3.0` (`brisk_download_engine` and `kasma` are `MIT`) | Unspecified in root listing (requires `package.json` check; treat as clean-room only) | `GPL-3.0` (v2.0+); `MIT` (v1.0.16 and earlier) | `MIT License` |
| **Primary Language & Stack** | Dart / Flutter SDK `v3.32.0`, `http`, `hive` | JavaScript (83.3%), CSS (11.9%), HTML (4.7%), Yarn | JavaScript (Manifest V3), `hls.min.js`, HTML/CSS | C# (99.3%) / .NET 8–9 NativeAOT, `System.CommandLine`, `Spectre.Console` |
| **Integration Strategy in OpenIDM** | **Algorithmic Adaptation Only:** Re-implement dynamic connection spawn, work-stealing byte-range reuse, and stall reset natively in C++20 (`WinHTTP` + IOCP). Do **not** embed Dart/Flutter. | **Concept Reference Only:** Implement download capture, text-selection batch link extraction, and iframe-aware video button cleanly in TypeScript + closed Shadow DOM. | **Adapt Detection Rules & Architecture:** Retain passive `webRequest` MIME/manifest sniffing, header correlation, and opt-in page hooks; replace in-browser downloading and `m3u8dl://` URL protocol with Native Messaging IPC. | **Out-of-Process Worker Integration:** Bundle unmodified `N_m3u8DL-RE.exe` (`MIT`), invoke via `CreateProcessW` inside a Windows Job Object using `--skip-download --write-meta-json` (probe) and `-M` (download/mux). |

---

## 2. Project A1: Brisk Download Manager (`BrisklyDev/brisk`)

### 2.1 Repository & Implementation Facts
- **Repositories Inspected:**
  - Desktop App: [https://github.com/BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) (`GPL-3.0`, built with Flutter SDK `v3.32.0` per [README.md](https://github.com/BrisklyDev/brisk/blob/main/README.md)).
  - Standalone Dart Engine: [https://github.com/BrisklyDev/brisk_download_engine](https://github.com/BrisklyDev/brisk_download_engine) (`MIT`).
  - Experimental Rust Engine: [https://github.com/BrisklyDev/kasma](https://github.com/BrisklyDev/kasma) (`MIT`, marked `[WIP] high-performance download engine & utility`).
- **Existing Verified Capabilities (from [README.md](https://github.com/BrisklyDev/brisk/blob/main/README.md) & [Releases](https://github.com/AminBhst/brisk/releases)):**
  1. **Dynamic Connection Spawn:** Downloads begin with a single HTTP connection. As transfer progresses and byte-range support is confirmed, additional connections are spawned dynamically so short downloads do not suffer TCP/TLS handshake overhead while large downloads scale up to peak concurrency.
  2. **Dynamic Connection Reuse (Work-Stealing Range Splitting):** When a connection finishes receiving its assigned byte range, it is immediately reassigned to assist another still-active connection by splitting its remaining byte range, keeping maximum connections active until the end of the file.
  3. **Automatic Connection Reset:** Hanging or stalled connections are detected and reset automatically without failing the entire task.
  4. **M3U8 Stream Capture & Soft-Subbing:** Captures M3U8 streams from `brisk-browser-extension`, prefetches/caches video stream metadata, and relies on an automatic FFmpeg installation (`Fixed automatic ffmpeg installation not working` noted in release history).
  5. **Download Queues, Scheduling, and Clipboard Hotkey:** Supports scheduled queues, multiple newline-separated URLs, and a hotkey to add URLs from the clipboard.
- **Dependencies & Windows Deployment Requirements:**
  - Requires the Flutter Windows runner (`flutter_windows.dll`, `icudtl.dat`, `app.so` AOT snapshot), `window_manager`, `hive` / `hive_generator` for local state, and `ffmpeg` for M3U8 merging.
  - Note in upstream `README.md`: Windows builds trigger SmartScreen warnings (*"On Windows, if you encountered a 'Windows Protected your PC' warning, click more info -> run anyway"*) due to lack of EV/OV Authenticode signing.
- **Architectural & Reuse Assessment (Answering Section 8 Questions):**
  - *Can Brisk's download engine be called independently from a native Win32 app?* **No.** `brisk_download_engine` is written in pure Dart using Dart's `http` package and Dart isolates. Calling it directly would require embedding the Dart VM runtime (~20+ MB) or running a separate Dart AOT executable. Meanwhile, `BrisklyDev/kasma` (Rust) is still marked `[WIP]` with only 13 stars and has not replaced the Dart engine in Brisk.
  - *Can its algorithmic ideas be adapted without copying its Dart implementation?* **Yes.** Dynamic connection spawning, work-stealing range splitting, and stall reset are clean state-machine algorithms over HTTP `Range: bytes=start-end` requests. We specify their exact C++20/WinHTTP implementation in [DOWNLOAD_ENGINE_DESIGN.md](DOWNLOAD_ENGINE_DESIGN.md).

---

## 3. Project A2: Brisk Browser Extension (`BrisklyDev/brisk-browser-extension`)

### 3.1 Repository & Implementation Facts
- **Repository:** [https://github.com/BrisklyDev/brisk-browser-extension](https://github.com/BrisklyDev/brisk-browser-extension)
- **Inspected Commit / Tag:** Commit `4f60c7f`, Tag `v1.4.1` (59 commits; source files under `app/`, built via `yarn install && yarn build {browser-name}` using `build-script.sh`).
- **Companion Repo:** `https://github.com/BrisklyDev/brisk-extension-dynamic-plugins` (`MIT`).
- **Verified Capabilities & Recent Fixes:**
  - Captures browser download requests and forwards them to Brisk (along with captured cookies added in recent releases).
  - Extracts all download links from a user-selected text region on a webpage and adds them in batch.
  - Captures M3U8 video streams and injects an in-page video download button (including into video stream iframes as of `v1.4.0`).
  - Recent commit history (`activity`) shows: `fix: send the fetch request for download video via background script` and `Fix Brisk port not...`.
- **Known Limitations & Security Risks:**
  1. **Localhost HTTP Port Communication:** The extension communicates with the Brisk desktop app by sending `fetch()` requests from the background script to a local TCP port (`localhost`). Any webpage or local process can probe or send cross-origin requests to a localhost HTTP listener unless protected by strict authentication tokens and Native Messaging.
  2. **Manual Installation Friction on Chromium:** As noted in `README.md`, Chrome, Edge, and Opera users must manually download a release zip and load it in developer mode (only Firefox is published on `addons.mozilla.org`).
  3. **Conflict with Other Extensions:** Upstream `README.md` explicitly warns: *"For the browser extension to work properly, disable other download manager extensions."*

---

## 4. Project B: Cat Catch (`xifangczy/cat-catch` & `xifangczy/cat-catch-book`)

### 4.1 Repository & Implementation Facts
- **Repositories Inspected:**
  - Extension Source: [https://github.com/xifangczy/cat-catch](https://github.com/xifangczy/cat-catch) (Tags `2.7.1` commit `b6cd7c5` and `2.7.2`, ~22.3k stars, 1.9k forks, `GPL-3.0`).
  - Documentation Archive: [https://github.com/xifangczy/cat-catch-book](https://github.com/xifangczy/cat-catch-book) (Commit `71d2329`, archived July 5, 2026) and active GitBook documentation ([https://o2bmm.gitbook.io/cat-catch](https://o2bmm.gitbook.io/cat-catch)).
- **Source Modules & Permissions (Verified from [Chrome-Stats](https://chrome-stats.com/d/iekecadicdlboncnocjmnkajchdafecb?hl=zh-CN) & Repository Tree):**
  - **Manifest V3 Permissions:** `tabs`, `webRequest`, `downloads`, `storage`, `webNavigation`, `alarms`, `declarativeNetRequest`, `scripting`, `sidePanel`, `contextMenus`, and host permissions `*://*/*`, `<all_urls>`.
  - **Core Scripts:** `manifest.json`, `js/background.js`, `js/content-script.js`, `js/init.js`, `js/function.js`, `catch-script/catch.js` (Deep Search / MSE buffer capture), `catch-script/recorder.js` (WebRTC / canvas / media element recorder), `lib/hls.min.js`, `m3u8.html`, `mpd.html`, `preview.html`, `popup.html`, `options.html`.
- **Verified Capabilities (from [GitBook Docs](https://o2bmm.gitbook.io/cat-catch/docs/settings)):**
  1. **Passive Network Sniffing (`webRequest`):** Filters network traffic by file extension, MIME type (supporting wildcards like `video/*`), relational size operators (`>= 200KB`, `< 1GB`, added in v2.6.8), and highest-priority URL regular expressions. Captures `Referer`, `Origin`, `Cookie`, and `User-Agent` request headers per `tabId`.
  2. **Active Page Instrumentation ("Deep Search" & "Cache Capture" in `catch-script/catch.js`):** Requires Chromium 102+ (`scripting` API). Intercepts page-context functions (`fetch`, `XMLHttpRequest`, `MediaSource`, `SourceBuffer.prototype.appendBuffer`, `btoa`/`atob`) to capture hidden `.m3u8`/`.mpd` URLs, cache one-time-read `.m3u8` file contents in memory, and collect candidate 16-byte AES keys (`addKey`).
  3. **Built-in M3U8 & MPD Parsers (`m3u8.html`, `mpd.html`):** Parses HLS playlists, supports custom AES-128 keys (Base64, Hex, or URL), supports `${range:1-10,3}` URL expansion macros, converts simple DASH MPD manifests into virtual M3U8 lists, and supports streaming-to-disk ("边下边存" on Chromium) or online WebAssembly `ffmpeg.wasm` muxing.
  4. **External Program & Webhook Integration:**
     - Supports custom URL protocols (`m3u8dl://` and `m3u8dlre:`) via [xifangczy/URLProtocol](https://github.com/xifangczy/URLProtocol) with template variables (`"${url}" --save-dir ... --save-name "${title}_${now}" ${referer|exists:'-H "Referer:*"'}`).
     - Supports a JSON webhook ("发送数据") posting `{"action": "catch" | "addKey", "data": {...}, "tabId": "..."}` to a configured endpoint.
- **Known Limitations & Why Cat Catch Cannot Simply Be Embedded:**
  1. **Browser Memory & Concurrency Limits:** As documented in [Cat Catch M3U8 Docs](https://o2bmm.gitbook.io/cat-catch/docs/m3u8parse), Chrome restricts extensions to **6 concurrent connections per host**, in-memory segment merging crashes tabs on large videos, `ffmpeg.wasm` is capped at **2 GB**, and Cat Catch's built-in DASH parser **does not support encrypted or live MPD streams** ([Dash MPD Docs](https://o2bmm.gitbook.io/cat-catch/docs/mpdparse)).
  2. **URL Protocol Command Injection Risk:** Launching `N_m3u8DL-RE.exe` via `m3u8dlre:"${url}" ... --save-name "${title}"` passes webpage-controlled strings (`${url}`, `${title}`, `${referer}`) directly into a Windows URL protocol command line, creating a classic argument-injection / command-injection vector if a malicious page crafts a `<title>` or URL containing quotes and flags.
  3. **Firefox vs. Chromium MV3 Divergence:** As documented in [Issue #662](https://github.com/xifangczy/cat-catch/issues/662) and [Install Docs](https://o2bmm.gitbook.io/cat-catch/docs/install), Firefox does not support Cat Catch's Chromium-specific `catch-script` injection or streaming file-saver ("边下边存"), while Chromium MV3 restricts `webRequest` header modification to `declarativeNetRequest`.
  4. **Store Takedown Pressure:** In v2.6.5 ([Appinn analysis](https://meta.appinn.net/t/topic/77635)), the author had to add a hardcoded domain blocklist (`douyin.com`) for browser store builds following legal notices, demonstrating why detection rules and site policies must be modular.

---

## 5. Project C: N_m3u8DL-RE (`nilaoda/N_m3u8DL-RE`)

### 5.1 Repository & Implementation Facts
- **Repository:** [https://github.com/nilaoda/N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE)
- **Inspected Commit / Tag:** Commit `14b12f5`, Version `0.6.0 20261012` / Tag `v0.6.0-beta` (`MIT License`, 99.3% C#, 0.7% PowerShell).
- **Verified CLI Interface & Capabilities (from [README.md](https://www.jsdelivr.com/package/gh/nilaoda/N_m3u8DL-RE) & [README.en.md](https://github.com/nilaoda/N_m3u8DL-RE/blob/main/README.en.md)):**
  - **Supported Stream Formats:** HLS (`.m3u8`, including multi-`#EXT-X-MAP` and discontinuity sections), MPEG-DASH (`.mpd`, including multi-Period VOD and dynamic live streams), Microsoft Smooth Streaming (`.ism`), and IPTV `.m3u` channel lists.
  - **Subcommands (`v0.6.0`):**
    - Default `<input> [options]` download workflow.
    - `concat` — Binary concatenation of segments without FFmpeg.
    - `merge` — FFmpeg segment merging from a directory.
    - `mux` — Standalone video/audio/subtitle muxing with automatic subtitle timeline adjustment (`--auto-subtitle-fix`).
    - `doctor [--json]` — Checks external tool versions (`ffmpeg`, `mkvmerge`, `mp4decrypt`) and environment readiness in machine-readable JSON.
  - **Key CLI Flags for OpenIDM Worker Integration:**
    - Path & Config Control: `--no-config` (ignores `%APPDATA%\N_m3u8DL-RE\config.conf` for deterministic execution), `@args.txt` response file support, `--tmp-dir <dir>`, `--save-dir <dir>`, `--save-name <name>`, `--save-pattern <pattern>`, `--write-meta-json` (default `True`), `--del-after-done` (default `True`).
    - Network & Concurrency: `--thread-count <N>` (default `10`), `-mt / --concurrent-download` (downloads video, audio, and subtitles concurrently), `--download-retry-count <N>` (default `7` in `20261012`), `--http-request-timeout <sec>` (default `100`), `-R / --max-speed <Mbps|Kbps>`, `-H / --header <header>`, `--cookies <Netscape file>`, `--use-system-proxy`, `--custom-proxy <URL>`, `--append-url-params`.
    - Track Selection & Filtering: `--auto-select`, `-sv / --select-video`, `-sa / --select-audio`, `-ss / --select-subtitle`, `-dv / -da / -ds` (supporting `id=`, `lang=`, `name=`, `codecs=`, `res=`, `frame=`, `channel=`, `range=`, `url=`, `period=`, `segsMin=`, `segsMax=`, `plistDurMin=`, `plistDurMax=`, `bwMin=`, `bwMax=`, `role=`, `for=best|worst|all`), `--vod-select-parts false` (disables interactive console prompt in non-interactive worker runs!), `--vod-list-parts`, `--vod-drop-parts`, `--ad-keyword <REGEX>`, `--custom-range <RANGE>`.
    - Merging & Muxing: `--ffmpeg-binary-path <PATH>`, `--ffmpeg-concat-mode <LOCAL_HTTP|PROTOCOL|DEMUXER>` (default `LOCAL_HTTP` binds a temporary `127.0.0.1` stream so FFmpeg can concatenate thousands of segments without hitting Windows command-line length or `fopen` handle limits!), `-M / --mux-after-done format=mp4|mkv|ts:muxer=ffmpeg|mkvmerge:bin_path=...:skip_sub=bool:keep=bool`, `--mux-import`.
    - Console Output & Logging: `--no-ansi-color` (strips ANSI escape codes), `--log-level <DEBUG|ERROR|INFO|OFF|WARN>`, `--log-file-path <PATH>`, `--no-log`, `--ui-language en-US`, `--disable-update-check`.
- **Known Limitations & Operational Caveats:**
  1. **Interactive Prompts Can Hang Background Workers:** If a VOD manifest has multiple discontinuity sections or Periods, `N_m3u8DL-RE` v0.6.0 may display an interactive `Spectre.Console` prompt (`Use Space to toggle groups and Enter to confirm`) unless `--vod-select-parts false` or `--auto-select` / explicit `-sv` filters are passed. **OpenIDM must always pass `--vod-select-parts false --disable-update-check --no-ansi-color --no-config --ui-language en-US`** when invoking `N_m3u8DL-RE.exe`.
  2. **Live Stream Termination Hang (e.g., Instagram Live DASH):** Community reports and upstream issues (#405) show that when certain dynamic DASH live streams end on the server, `N_m3u8DL-RE` may wait indefinitely unless `--live-idle-timeout <SEC>` is set or the supervisor sends Ctrl+C (`GenerateConsoleCtrlEvent`).

---

## 6. Feature Ownership & Deduplication Across Projects

| Capability | Brisk | Cat Catch | N_m3u8DL-RE | OpenIDM Architectural Assignment |
| :--- | :--- | :--- | :--- | :--- |
| **Desktop Task Queue, Tray UI & Scheduler** | Flutter GUI (High RAM) | None (Browser only) | None (CLI only) | **Independently implemented** in Native C++20 / Win32 (`openidm.exe`). |
| **Ordinary HTTP/HTTPS Segmented Downloading** | Dart `http` engine (dynamic spawn + reuse) | Browser native or 6-conn fetch | Not designed for single-file HTTP range downloads | **Adapted algorithmically** from Brisk into C++20 `WinHttpDownloadEngine`. |
| **Browser File Download Interception** | `brisk-browser-extension` | Not its primary focus | None | **Independently implemented** in `extension/` (`chrome.downloads` listener). |
| **Browser Media & Manifest Sniffing** | Basic `.m3u8` URL capture | Comprehensive (`webRequest` + DOM + `catch-script`) | None | **Adapted** from Cat Catch's multi-layered detection rules in `extension/`. |
| **In-Page Floating Video Button** | Basic iframe-compatible button | Only shows overlay during "Cache Capture" | None | **Independently implemented** in `extension/` using a closed Shadow DOM overlay. |
| **HLS / DASH / MSS Manifest Parsing & Segment Downloading** | Basic M3U8 only (no DASH/ISM) | In-browser JS (6-conn limit, no encrypted/live DASH) | Comprehensive, fast multi-threaded C# NativeAOT | **Delegated** exclusively to `N_m3u8DL-RE.exe` worker process. |
| **Audio/Video/Subtitle Container Muxing** | Invokes `ffmpeg` | In-browser `ffmpeg.wasm` (2 GB limit) | Invokes `ffmpeg` (`LOCAL_HTTP` concat mode + `-M`) | **Delegated** to `N_m3u8DL-RE.exe` + bundled native `ffmpeg.exe`. |
