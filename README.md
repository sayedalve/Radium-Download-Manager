# Radium Download Manager

[![Latest release](https://img.shields.io/github/v/release/sayedalve/Radium-Download-Manager?display_name=tag)](https://github.com/sayedalve/Radium-Download-Manager/releases/latest)
[![Windows release build](https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml/badge.svg?branch=main)](https://github.com/sayedalve/Radium-Download-Manager/actions/workflows/windows-release.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](LICENSE)

Radium is a native Windows download manager built in C++20. It combines segmented HTTP/HTTPS downloads, a dedicated BitTorrent engine, stream downloading, and a Manifest V3 browser extension.

The latest published release is **v0.3.2** (October 21, 2026). The main branch contains newer changes and may differ from the published packages.

## Status

Radium is actively developed. The latest main commit was verified by the [Windows production build](https://github.com/sayedalve/Radium-Download-Manager/actions/runs/40196911810) and [Windows compatibility matrix](https://github.com/sayedalve/Radium-Download-Manager/actions/runs/40196911819) on October 25, 2026. Some desktop flows and live peer-to-peer behavior still need additional runtime verification; see [Testing](#testing-and-verification).

## Interface

The existing main-window preview is retained unchanged; it predates the current Radium branding. The dialog captures below show the current application running on Windows.

<p align="center">
  <img src="docs/images/readme/flux-main-window.png" alt="Existing main-window preview showing the download queue; legacy branding is visible" width="100%">
</p>

### New Download

Configure a direct download URL, filename, destination, format, and start or schedule choice.

![Radium New Download dialog with URL, filename, destination, format, and scheduling controls](docs/images/readme/radium-new-download-dialog.jpg)

### Settings

Review and configure application preferences in the native Settings window.

![Radium Settings window showing application preferences](docs/images/readme/radium-settings-dialog.jpg)

<!-- Planned capture: docs/images/readme/radium-add-torrent-dialog.png. Pending a suitable existing executable that shows the current selector layout. -->

## Features

- Segmented HTTP/HTTPS downloads with pause, resume, retry, queues, categories, and per-task controls.
- A separate BitTorrent v1 engine with magnet metadata retrieval, tracker/DHT discovery, selective multi-file storage, and SHA-1 piece checks.
- HLS and DASH stream downloading through managed worker processes; standard HLS AES-128 is supported.
- Browser-assisted download interception and media detection through Chromium and Firefox extensions.
- Native Windows interface, persistent task state, and settings.
- FTP support; FTPS is currently rejected because TLS negotiation is not implemented.

## HTTP and HTTPS downloads

The direct-download engine supports range requests and concurrent segments when the server permits them. Workers can reuse unclaimed ranges, and the scheduler reduces concurrency when a server pushes back. If range requests are unavailable, the engine falls back to a single connection where supported. Download state and destination handling are managed by the desktop application.

The published performance report includes a 10-trial live comparison against IDM using a rate-limited 100 MiB test file. In that test, IDM was slightly faster (76.64 s median versus Radium’s 79.44 s baseline), while Radium used less measured memory and CPU. A later single optimized Radium run completed in 64.72 s; it is not a like-for-like repeated comparison. These results describe that machine, route, server, and build, and are not a general speed guarantee. See [benchmark methodology and results](benchmarks/RESULTS.md), [benchmark files](benchmarks/), and the [performance report](docs/PERFORMANCE_OPTIMIZATION_REPORT.md).

## BitTorrent

Radium has a dedicated torrent engine. It accepts local .torrent files and magnet links, discovers peers through supported trackers and DHT, and transfers data using the BitTorrent v1 peer-wire protocol.

Implemented engine capabilities include:

- Magnet metadata exchange using BEP 9.
- HTTP and UDP tracker communication and bounded DHT peer discovery.
- Piece and block scheduling, with SHA-1 checks before pieces are retained as verified data.
- Multi-file storage mapping and selective file selection. Unselected files are excluded from the download layout and selected-byte progress.
- Pause, cancellation, task persistence, and verified-piece recovery paths in the controller and storage layers.
- Source implementation for incoming TCP handshakes and uploading verified blocks for seeding.

**Verification status:** Parser, peer-wire framing, tracker parsing, storage mapping, piece scheduling, and SHA-1 behavior have automated C++ coverage. Tracker discovery and magnet metadata exchange also have live test coverage. The feature matrix still classifies overall torrent behavior as partially verified. Deterministic incoming-peer block transfer and real seeding remain pending; the selector interaction and selective output path also need additional Windows UI verification. Treat the upload/seeding implementation as present in source, not as fully validated in a live swarm.

The Add Torrent dialog connects file selection, Select All/None, filtering, destination browsing, category, start choice, Add, Cancel, and Save Torrent actions. Several other visible fields are not connected to the task controller yet: management mode, tags, default-category persistence, alternate incomplete-file path, remember-path persistence, stop condition, queue placement, skip-hash, sequential/first-last priority, content layout, and Never show again. Their presence in the dialog does not mean those settings affect a torrent task.

**Not supported:** Peer Exchange (PEX/BEP 11), µTP, and BitTorrent v2 (BEP 52). Radium does not claim complete compatibility with all clients or extensions.

## HLS, DASH, and media streams

The stream pipeline delegates supported HLS and DASH manifests to out-of-process worker components and FFmpeg for muxing. Standard HLS AES-128 is supported. RTSP, MMS, and RTMP ingestion is separately listed in the feature matrix. Site-specific JavaScript challenge solving is outside the supported scope, and DRM-protected streams are rejected; Radium does not bypass DRM.

See [stream pipeline details](docs/STREAM_DOWNLOAD_PIPELINE.md) and [known limitations](docs/KNOWN_LIMITATIONS.md).

## Browser extension and download interception

The Manifest V3 extension targets Chromium browsers (including Chrome, Edge, and Brave) and Firefox. It detects direct download candidates and supported media manifests, and communicates with the desktop app through Native Messaging and a per-user Windows named pipe rather than an unauthenticated localhost HTTP server.

Load the appropriate manifest from the extension directory using the browser’s developer-mode extension page. Then register the native host from PowerShell after the desktop executable has been built or installed:

~~~powershell
.\scripts\register-native-messaging.ps1
~~~

For an unpacked extension, the registration script uses the repository’s configured extension origin. Review the prompts and script output, then restart the browser. See [browser extension setup](docs/BROWSER_EXTENSION.md) and [Native Messaging and IPC](docs/NATIVE_MESSAGING_AND_IPC.md).

## Download management and settings

The desktop application provides a persistent download queue, task filtering and sorting, categories, scheduling and speed controls, and settings for download behavior. Available actions depend on task state and source type. The torrent selector notes above call out fields that are currently presentation-only or awaiting verification.

## Security and privacy

- Native Messaging is bridged to a Windows named pipe with per-user access controls.
- Filenames and destinations are validated to prevent path traversal.
- Stream workers run out of process; sensitive request headers are not passed on command lines.
- Logs redact credentials and sensitive URL parameters.
- DRM circumvention is not supported.

See the [security threat model](docs/SECURITY_THREAT_MODEL.md) and [privacy and data handling](docs/PRIVACY_AND_DATA_HANDLING.md).

## Download and install

Download the current **v0.3.2** packages from [GitHub Releases](https://github.com/sayedalve/Radium-Download-Manager/releases/tag/v0.3.2). The Full packages include stream workers; Core packages are smaller and omit the bundled stream workers.

| Windows architecture | Installer | Portable Full | Portable Core |
|---|---|---|---|
| x64 | [Setup](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-Setup-x64-0.3.2.exe) | [ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-x64-Full.zip) | [ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-x64-Core.zip) |
| ARM64 | [Setup](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-Setup-ARM64-0.3.2.exe) | [ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-ARM64-Full.zip) | [ZIP](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/Radium-v0.3.2-Windows-ARM64-Core.zip) |

Verify downloaded packages with [SHA256SUMS.txt](https://github.com/sayedalve/Radium-Download-Manager/releases/download/v0.3.2/SHA256SUMS.txt). Release details and third-party notices are also attached to the [v0.3.2 release](https://github.com/sayedalve/Radium-Download-Manager/releases/tag/v0.3.2).

## Build from source

Use Windows 11, Visual Studio 2022 with the Desktop development with C++ workload, and CMake 3.25 or newer. From a PowerShell prompt:

~~~powershell
git clone https://github.com/sayedalve/Radium-Download-Manager.git
cd Radium-Download-Manager
.\scripts\build-windows-msvc.ps1
~~~

The build script compiles the native application, Native Messaging Host, worker, and C++ tests. See [development setup](docs/DEVELOPMENT_SETUP.md) and [Windows build and packaging](docs/WINDOWS_BUILD_AND_PACKAGING.md) for prerequisites and options.

## Architecture

- **Desktop application:** C++20, native Win32 UI, core task controller, SQLite persistence, and native download engines.
- **Direct downloads:** HTTP/HTTPS segmented engine and FTP engine.
- **Torrent downloads:** separate BitTorrent v1 engine with tracker/DHT discovery, peer-wire communication, piece management, and multi-file storage.
- **Stream downloads:** managed external worker processes and FFmpeg muxing.
- **Browser communication:** Manifest V3 extension → Native Messaging Host → user-restricted Windows named pipe → desktop application.

See [system architecture](docs/SYSTEM_ARCHITECTURE.md), [data model and storage](docs/DATA_MODEL_AND_STORAGE.md), and [architecture decisions](docs/adr/).

## Performance benchmarks and reproducibility

The IDM comparison above used five alternating trials per application, configured both products for eight connections, and downloaded the same 100 MiB URL sequentially to the same storage volume. The target server rate-limited concurrent connections, so each application used a different effective connection count. The published report includes the test environment, raw logs, checksums, and limitations.

To reproduce the project’s benchmark suite on a configured development machine, follow [benchmarks/README.md](benchmarks/README.md). Live results depend on network conditions, server policy, hardware, and software versions. The single optimized run is reported separately from the repeated baseline.

## Known limitations

- BitTorrent PEX, µTP, and BitTorrent v2 are unsupported.
- Torrent seeding upload and Add Torrent selector paths still need deterministic/live Windows verification.
- Several visible Add Torrent options are not currently applied to the controller; see the [BitTorrent section](#bittorrent).
- FTPS is unsupported; plain FTP is supported.
- DRM-protected streams and site-specific JavaScript challenge solving are unsupported.
- Browser restrictions on privileged pages and some embedded contexts apply.
- The current main branch has changes newer than the latest v0.3.2 release; do not assume those changes are included in the release packages.

See [known limitations](docs/KNOWN_LIMITATIONS.md) and the [feature matrix](docs/FEATURE_MATRIX.md).

## Testing and verification

**Verified by execution:** On October 25, 2026, GitHub Actions completed successfully for the current main commit: [Windows production build and verification](https://github.com/sayedalve/Radium-Download-Manager/actions/runs/40196911810) and [Windows compatibility matrix and testing lab](https://github.com/sayedalve/Radium-Download-Manager/actions/runs/40196911819). The C++ suite most recently run locally in this work history reported 71/71 passing; the full project matrix and live torrent UI behavior have broader pending checks.

**Verified by source/test-matrix inspection:** Torrent parser, peer framing, tracker handling, file mapping, block scheduling, selective storage, and integrity checks have dedicated test entries. Incoming-peer live transfer and the new selector UI route are still pending in the matrix.

**Unverified or partially verified:** Do not infer live swarm compatibility, every selector setting’s behavior, or current-branch functionality in the v0.3.2 packages from the successful CI runs alone. See [test matrix](docs/TEST_MATRIX.md) and [feature matrix](docs/FEATURE_MATRIX.md).

## Contributing and documentation

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md), follow [AGENTS.md](AGENTS.md), and use [docs/INDEX.md](docs/INDEX.md) to find the project’s design and implementation documents. Issues and pull requests are welcome on [GitHub](https://github.com/sayedalve/Radium-Download-Manager).

## License and acknowledgements

Radium is licensed under [GPL-3.0-or-later](LICENSE). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [LICENSE_REVIEW.md](LICENSE_REVIEW.md) for bundled component licenses and attributions. The project builds on upstream work including [Brisk](https://github.com/BrisklyDev/brisk), [Cat Catch](https://github.com/xifangczy/cat-catch), [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE), [FFmpeg](https://ffmpeg.org/), and SQLite.
