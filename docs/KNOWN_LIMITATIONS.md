# Known Limitations and Constraints

**Document Status:** Active Engineering Limitations Log (`v0.2.0`)  
**Last Reviewed:** 2026-10-16  
**Scope:** Documented browser restrictions, unsupported DRM/streaming formats, upstream tool constraints, and host environment boundaries.  
**Related Documents:** [archive/maintenance-audits/PRODUCT_COMPLETION_AUDIT.md](archive/maintenance-audits/PRODUCT_COMPLETION_AUDIT.md), [archive/maintenance-audits/RELEASE_READINESS_REPORT.md](archive/maintenance-audits/RELEASE_READINESS_REPORT.md), [MEDIA_DETECTION.md](MEDIA_DETECTION.md), [STREAM_DOWNLOAD_PIPELINE.md](STREAM_DOWNLOAD_PIPELINE.md)

---

## 1. Known to Be Unsupported (By Design & Legal Policy)

1. **DRM-Protected Streams (Widevine, PlayReady, FairPlay, WisePlay, CENC `SAMPLE-AES-CTR`):**
   - **Limitation:** Streams encrypted with commercial DRM systems require hardware/browser Content Decryption Modules (CDMs) and license server key exchange.
   - **Behavior:** OpenIDM strictly refuses to bypass DRM (`NFR-015`). When a `<video>` element has `mediaKeys !== null` or `N_m3u8DL-RE`'s `meta.json` reports DRM encryption without an open standard key, OpenIDM rejects the task immediately with `ERR_STREAM_DRM_PROTECTED` (*"Stream uses unsupported DRM protection. OpenIDM does not circumvent DRM."*).
2. **Proprietary Non-Manifest MSE `blob:` Streams & WebRTC Video Calls:**
   - **Limitation:** Standard MSE players (`hls.js`, `dash.js`) fetch `.m3u8` or `.mpd` manifests over HTTP, which OpenIDM captures and downloads at full speed. However, if a website feeds raw video frames into a `MediaSource` `SourceBuffer` over a custom encrypted WebSocket or WebRTC `RTCPeerConnection` without any `.m3u8`/`.mpd` manifest, `N_m3u8DL-RE` cannot download the stream from a URL.
   - **Behavior:** `blob:` URLs themselves are never sent to `openidm` as downloadable file URLs because `blob:` handles exist only inside the browser renderer's memory.
3. **Sites Requiring Custom JavaScript Challenge Solving per Segment (e.g., YouTube Signature Cipher / PO Token):**
   - **Limitation:** OpenIDM is a general HTTP/HTTPS and standard HLS/DASH/MSS download manager, not a site-specific scraper engine like `yt-dlp`. Sites that dynamically sign or throttle every individual segment via proprietary JS VM challenges are not supported natively.
4. **BitTorrent Scope Boundaries (BEP 3, DHT, and incoming upload support):**
   - **Supported Implementation:** Flux implements core BEP 3 peer wire protocol framing, compact IPv4/IPv6 tracker communication, bounded DHT peer discovery, incoming TCP peer handshakes on port 6881, verified-piece bitfields, requested-block upload accounting, 16 KB piece/block scheduling with SHA-1 integrity verification, and multi-file disk layout mapping (`multi_file_storage.hpp`, `piece_manager.hpp`, `peer_wire_protocol.hpp`, `tracker_client.hpp`).
   - **Limitations:** Peer Exchange (PEX / BEP 11), micro Transport Protocol (uTP), and BitTorrent v2 (BEP 52 merkle trees) remain unsupported. Selective multi-file torrents advertise and upload only pieces whose complete ranges are selected; unselected ranges are never fabricated or served.
5. **Magnet multi-file selection:**
   - **Supported behavior:** After BEP 9 metadata exchange, Radium opens the same native file-selection dialog used for local `.torrent` files. The selected paths and selection mask are persisted with the task before storage allocation, so restart/recovery preserves the choice.
   - **Limitation:** The selector and incoming upload listener are Windows-native features; headless/controller clients without a metadata callback retain the safe default of selecting all files.
6. **FTPS (FTP over TLS/SSL) Disabled Pending Verified TLS Implementation:**
   - **Limitation:** Plain FTP (RFC 959, 2428, 3659) with EPSV/PASV modes and REST resume is supported. FTPS (RFC 4217) requires explicit TLS negotiation across both control and dynamic data channels with strict certificate chain validation, which is not yet implemented.
   - **Behavior:** `ftps://` URLs are rejected immediately with `ERR_FTPS_UNSUPPORTED` (*"FTPS (FTP over TLS/SSL) is not supported. Plaintext transmission over ftps:// is prohibited."*) before any socket connection is attempted, preventing plaintext credential or data leakage over unencrypted channels.

---

## 2. Browser & Extension Platform Restrictions

1. **Browser Internal & Store Pages:**
   - Chrome, Edge, Brave, and Firefox block all extension content scripts and network hooks on privileged internal pages (`chrome://`, `edge://`, `about:`, `view-source:`) and official extension store domains (`chromewebstore.google.com`, `microsoftedge.microsoft.com`, `addons.mozilla.org`).
2. **Cross-Origin Sandboxed Iframes Without `allow-scripts`:**
   - While OpenIDM's passive `webRequest` sniffer (`Layer 1`) still detects media and manifests inside all iframes and surfaces them in the extension toolbar popup, the in-page floating Shadow DOM button (`Layer 2`) cannot render inside an `<iframe>` if the host page sets a restrictive `sandbox` attribute that blocks script execution.
3. **Pause/Resume on Non-Range HTTP Servers (`FR-002`):**
   - When an HTTP server omits `Accept-Ranges: bytes` or responds `200 OK` to a `Range` request, pausing the download cannot preserve progress; resuming restarts cleanly from byte `0` after clearing stale segment state.
4. **Client-Side Generated Files (`blob:`, `data:` URLs, ChatGPT Exports):**
   - **Supported Capture Path:** In-memory files generated by client-side web application scripts (such as ChatGPT artifact exports, Canvas snapshots, and local CSV generators) cannot be fetched directly by external processes over HTTP. Flux captures these files using a dedicated chunked transfer protocol over Native Messaging (`download.startChunkedTransfer`, `download.transferChunk`, `download.completeChunkedTransfer`). Chunks are streamed in 512 KiB Base64 payloads into a staged directory, and atomically committed to the final destination via `DownloadController::IngestCompletedTask` with automatic antivirus scanning.
   - **Fallback Behavior:** If native transfer is unavailable or explicitly aborted by the user, the download cleanly delegates back to the browser's native download pipeline without corrupting files or hanging the browser session.

---

## 3. Build Environment vs. Windows 11 Host Verification Boundary

1. **Linux Container Build vs. Windows 11 Native `.exe` Compilation:**
   - The automated build and verification container runs Debian 12 Linux `x86_64` (`g++ 12.2.0`, `node 18.20.4`, `python3 3.11.2`, `ffmpeg 5.1.9`) without a Windows cross-compiler (`mingw-w64` or `MSVC`).
   - All Win32 API implementations (`WinHTTP`, `CreateFileW` + `OVERLAPPED`, `CreateJobObjectW` + `CreateProcessW`, Named Pipes, DPAPI/BCrypt, and the Win32 `RegisterClassExW`/`CreateWindowExW` GDI double-buffered GUI in `src/ui/win32_main_window.cpp`) are implemented in `#ifdef _WIN32` blocks and verified by source inspection, while the shared C++20 core, UI view-model, native `N_m3u8DL-RE` worker, and extension are verified by execution (`26 / 26` test suites passing).
   - Producing signed PE32+ `.exe` binaries (`openidm.exe`, `openidm-nmh.exe`, `N_m3u8DL-RE.exe`) and compiling the Inno Setup installer (`OpenIDM-Setup-x64-0.2.0.exe`) requires running `.\scripts\build-windows-msvc.ps1` on a Windows 11 `x64` workstation with Visual Studio 2022 and Inno Setup 6 (`ISCC.exe`).

### Brave unpacked extension install state (2026-10-23)

A fresh Brave launch showed no Radium extension card. The active Default profile's `Secure Preferences` retains an unpacked-path record, but `Preferences` has no matching extension entry. This points to stale profile metadata rather than proof of an active installation; the historical removal trigger is still undetermined. A stable LocalAppData extension copy is staged, but loading it into Brave and restart persistence checks await the required action-time confirmation.

