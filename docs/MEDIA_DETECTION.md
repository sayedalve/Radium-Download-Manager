# Browser Media Detection Specification

**Document Status:** Proposed Subsystem Specification  
**Last Reviewed:** 2026-10-16  
**Scope:** Multi-layered media discovery, manifest classification, header correlation, deduplication, confidence scoring, and MSE `blob:` URL handling in `extension/`.  
**Related Documents:** [BROWSER_EXTENSION.md](BROWSER_EXTENSION.md), [STREAM_DOWNLOAD_PIPELINE.md](STREAM_DOWNLOAD_PIPELINE.md), [UPSTREAM_PROJECT_ANALYSIS.md](UPSTREAM_PROJECT_ANALYSIS.md), [adr/0004-media-detection.md](adr/0004-media-detection.md)

---

## 1. Overview & Detection Philosophy

Competing with IDM requires detecting video and audio resources accurately across modern single-page applications (SPAs), embedded cross-origin player iframes, and adaptive streaming players (`hls.js`, `dash.js`, Shaka Player, Video.js) while avoiding the two classic failure modes of browser media sniffers:
1. **Segment Flooding (False Positives):** Listing hundreds of individual `.ts`, `.m4s`, or `.aac` segments or tiny ad/tracking pixels in the download menu.
2. **Broken `blob:` URLs:** Attempting to pass a browser-local `blob:https://example.com/uuid` MediaSource URL to an external downloader, which fails immediately because `blob:` URLs exist only inside the browser renderer process.

OpenIDM solves both problems via a **Three-Layer Detection Pipeline** in the browser extension that correlates network-level manifest/file requests with DOM-level `<video>` elements and forwards only validated, deduplicated `MediaCandidateDescriptor` payloads to `openidm.exe`.

---

## 2. Classification of Web Media Sources

| Source Type | Network Signature | DOM Signature | OpenIDM Detection & Download Route |
| :--- | :--- | :--- | :--- |
| **1. Progressive HTTP Media** | `Content-Type: video/mp4`, `video/webm`, `video/x-matroska`, `audio/mpeg`, `audio/mp4` with `Content-Length >= 512 KiB` (and not a byte-range fragment of a DASH/HLS stream). | `<video src="https://...">` or `<source src="https://...">` | **Ordinary File Engine (`WinHttpDownloadEngine`):** Downloaded directly via multi-connection HTTP byte ranges. |
| **2. HLS Adaptive Stream (`.m3u8`)** | URL path matching `\\.m3u8(\\?|$)` or `Content-Type` matching `application/vnd.apple.mpegurl`, `application/x-mpegurl`, `audio/mpegurl`. | `<video src="blob:https://...">` driven by `hls.js` or native `<video>` playback. | **Stream Pipeline (`N_m3u8DL-RE` + `FFmpeg`):** Probed for variants/audio/subtitles, downloaded concurrently, and muxed to `.mp4`/`.mkv`. |
| **3. MPEG-DASH Adaptive Stream (`.mpd`)** | URL path matching `\\.mpd(\\?|$)` or `Content-Type: application/dash+xml`. | `<video src="blob:https://...">` driven by `dash.js` or Shaka Player. | **Stream Pipeline (`N_m3u8DL-RE` + `FFmpeg`):** Probed and muxed via `N_m3u8DL-RE.exe`. |
| **4. Smooth Streaming (`.ism`)** | URL matching `\\.ism/Manifest` or `Content-Type: application/vnd.ms-sstr+xml`. | `<video src="blob:https://...">` | **Stream Pipeline (`N_m3u8DL-RE` + `FFmpeg`).** |
| **5. MSE `blob:` URLs** | No direct HTTP request for `blob:...`; underlying player fetches `.m3u8`/`.mpd` or raw `.m4s` ranges via `fetch()`/`XHR`. | `<video src="blob:https://example.com/9a8b...">` | **Never pass `blob:` URL to `openidm.exe`.** Map the `<video>` element's `tabId` + `frameId` to the `.m3u8` / `.mpd` manifest captured in Layer 1 or Layer 3. (Note: `blob:` URLs created from in-memory `File`/`Blob` constructors without an underlying stream cannot be reconstructed externally; see Section 6.) |
| **6. DRM-Protected Streams** | Manifest contains `#EXT-X-KEY:METHOD=SAMPLE-AES-CTR`, Widevine/PlayReady `cenc:pssh` boxes, or browser triggers `navigator.requestMediaKeySystemAccess`. | `<video>` has non-null `mediaKeys` (`HTMLMediaElement.mediaKeys !== null`). | **Detected as `DRM_PROTECTED`:** Overlay badge displays `"Protected Stream (DRM — Cannot Download)"` (`NFR-015`). |

---

## 3. Three-Layer Detection Pipeline

### 3.1 Layer 1: Passive `webRequest` Network Sniffer (Service Worker)
Implemented in `extension/src/background/MediaSniffer.ts`:
1. **`onBeforeSendHeaders` Listener (`['requestHeaders', 'extraHeaders']`):**
   - Stores a short-lived LRU map (`max 2,000 entries`, TTL `120 seconds`) keyed by `details.requestId` containing:
     - `url`, `tabId`, `frameId`, `initiator`
     - Extracted request headers: `Referer`, `Origin`, `User-Agent`, `Cookie`, `Authorization`
2. **`onHeadersReceived` Listener (`['responseHeaders']`):**
   - Evaluates the response status (`200` or `206`), `Content-Type`, `Content-Length`, `Content-Range`, and URL path against the **Classification Rules**:
   - **Segment Suppression Filter (Anti-Flooding):**
     - Immediately ignore URLs matching `.ts`, `.m4s`, `.cmfv`, `.cmfa`, `.vtt` segment patterns or `206 Partial Content` requests whose `Content-Range` span is `< 4 MiB` when an HLS/DASH manifest has already been recorded for the same `(tabId, frameId)` within the last 5 minutes.
     - Immediately ignore responses with `Content-Length < 512 KiB` for `video/*` or `< 128 KiB` for `audio/*` (filters out UI sound effects and hover preview clips), while allowing all `.m3u8`/`.mpd` manifests regardless of size (since manifests are typically `1–50 KB`).

### 3.2 Layer 2: Isolated-World DOM & Iframe Observer (`extension/src/content/`)
Injected with `"all_frames": true`, `"run_at": "document_idle"`:
1. Observes `<video>` and `<audio>` elements via `MutationObserver` and document-level capturing listeners for `loadedmetadata`, `play`, and `playing`.
2. Checks `video.mediaKeys !== null` or `encrypted` event firing on the `<video>` element to immediately flag DRM-protected players before the user even clicks download!
3. Sends `REPORT_PLAYER_STATE` (`tabId`, `frameId`, `videoRect`, `currentSrc`, `isEncrypted`, `documentTitle`) to the Service Worker.
4. Renders the **Closed Shadow DOM Video Overlay Button** (`<openidm-overlay-host>`) anchored to the top-right corner of the `<video>` bounding box when hovered.

### 3.3 Layer 3: Opt-In Page-Context Instrumentation ("Deep Search" — `FR-017`)
Inspired by Cat Catch's `catch-script/catch.js` ([Cat Catch Deep Search Docs](https://o2bmm.gitbook.io/cat-catch/docs/popup-1)), but scoped strictly to manifest discovery and enabled per-site via the extension popup:
1. Executed in `world: "MAIN"` via `chrome.scripting.executeScript`.
2. Wraps `window.fetch` and `XMLHttpRequest.prototype.open/send`:
   - Clones responses whose text begins with `#EXTM3U` (HLS) or `<MPD` (DASH) even when the server returns `Content-Type: text/plain` or `application/octet-stream` on an obfuscated URL path.
   - Emits a structured `CustomEvent` containing `{ url, manifestType, manifestBodyText }` to the isolated content script, which forwards it to the Service Worker.
   - Caching `manifestBodyText` solves the **one-time-use manifest URL problem (`RISK-004`)**: `openidm.exe` can save `manifestBodyText` to disk and pass `--base-url <url>` to `N_m3u8DL-RE.exe` without re-requesting the expired manifest URL!

---

## 4. Deduplication, Master vs. Child Playlist Grouping, and Confidence Scoring (`FR-016`)

When an HLS player starts playback, the browser typically requests:
1. `https://cdn.example.com/videos/12345/master.m3u8?token=abc` (`t = 0 ms`)
2. `https://cdn.example.com/videos/12345/1080p/prog_index.m3u8?token=abc` (`t = 120 ms`)
3. `https://cdn.example.com/videos/12345/audio_en/prog_index.m3u8?token=abc` (`t = 140 ms`)

Without deduplication, the user sees three confusing `.m3u8` entries in the popup, two of which lack audio or video!  
`ManifestDeduplicator.ts` applies the following rules per `(tabId, frameId)`:

| Rule | Logic | Confidence Score Assigned |
| :--- | :--- | :--- |
| **Rule 1: Master Playlist Priority** | If Layer 3 cached body contains `#EXT-X-STREAM-INF`, or the URL filename matches `master`, `manifest`, `index`, `playlist` at a parent directory prefix of subsequent `.m3u8` requests within `2,000 ms`, classify as `HLS_MASTER` and group subsequent child `.m3u8` requests under the same parent directory as children of that master candidate. | **`95 / 100` (Primary Recommended)** |
| **Rule 2: DASH MPD Manifest** | Any `.mpd` or `application/dash+xml` response (since DASH always uses a single unified MPD for all AdaptationSets). | **`95 / 100` (Primary Recommended)** |
| **Rule 3: Standalone Media Playlist** | An `.m3u8` request with no parent master request detected in the same tab/frame. | **`80 / 100`** |
| **Rule 4: Full Progressive Video** | A `200 OK` (or `206` starting at `0` with total length `>= 5 MiB`) `video/mp4` or `video/webm` matching `<video>.currentSrc`. | **`90 / 100`** |
| **Rule 5: Uncorrelated Background Media** | A `video/*` request `512 KiB – 5 MiB` not linked to the active `<video>` element (likely a muted background loop or ad banner). | **`35 / 100` (Collapsed under "Other Media")** |

---

## 5. Handling Temporary URLs, Redirects, and Multiple Simultaneous Videos

1. **Redirect Chains:** If `https://site.example/play?id=99` redirects (`302`) to `https://edge-cdn.example/v/99/master.m3u8?exp=1760000000`, `MediaSniffer` records the **final resolved URL** (`edge-cdn.example`) along with the **headers actually sent to the final hop** (`onBeforeSendHeaders` for the redirected request).
2. **URL Expiry Warnings:** If the URL query string contains common expiration parameters (`exp=`, `expires=`, `hdnea=`, `X-Amz-Expires=`), the detector parses the UNIX timestamp and displays a TTL indicator in the popup and forwards `expires_at_unix` to `openidm.exe`.
3. **Multiple Simultaneous Videos on One Page (e.g., Social Feeds):**
   - Each detected `<video>` element in Layer 2 is correlated with network candidates by matching:
     1. Exact `frameId` (for embedded player iframes),
     2. Direct `video.currentSrc` match (for progressive MP4s), or
     3. Most recent manifest requested within `[-3000 ms, +1500 ms]` of that specific `<video>` element firing `loadstart` / `play`.
   - Clicking the floating overlay button on a specific `<video>` surfaces its correlated candidate at the top of the dropdown while keeping a `"Show all detected media on this tab (N)"` footer option.

---

## 6. Explicit Limitations on MSE and Client-Side `blob:` URLs (`KNOWN_LIMITATIONS.md`)

OpenIDM / Flux documentation and UI never promise that every `blob:` URL can be reconstructed into an external downloadable file:
- Standard `blob:` URLs created via `URL.createObjectURL(mediaSource)` are downloaded by capturing the **network manifest (`.m3u8`/`.mpd`)** that feeds the `MediaSource`.
- If a site uses a custom proprietary binary WebSocket protocol or custom WebAssembly cipher to feed raw decrypted frames into `SourceBuffer.appendBuffer` without any HTTP manifest, `N_m3u8DL-RE` cannot download it. Supporting raw MSE buffer recording (`SourceBuffer` tap) is deferred as an experimental post-MVP option (`OPEN_QUESTIONS.md` `OQ-004`).
- **Client-Side Document/Data `blob:` URLs:** In-memory client-side generated files (`blob:` or `data:` URLs from web apps, CSV/Canvas exports, and AI chat artifacts like ChatGPT exports) reside strictly in the browser renderer memory. Flux does not monkey-patch DOM click events or attempt to stream these in-memory blobs over Native Messaging, but preserves the browser's native download pipeline without disruption.

---

## 7. Webpage Watch URL Guard & Intent Classification (`BUG-034`)

Websites like YouTube (`youtube.com/watch?v=...`, `youtube.com/shorts/...`), Vimeo (`vimeo.com/...`), and Twitch (`twitch.tv/...`) present HTML watch pages rather than direct media files or static manifests.

1. **The Watch Page Failure Mode:**
   - If a user passes an HTML watch page URL directly to an ordinary segmented HTTP engine, the engine issues concurrent byte-range requests (`Range: bytes=0-1048575`) against an HTML web page. Streaming platforms detect this anomalous non-browser traffic and immediately return **HTTP 429 Too Many Requests** or generic HTML blocks.
2. **Intent Classification & Rejection Guard:**
   - `extension/js/function.js` (`isWebpageWatchUrl`) and `src/common/path_sanitizer.cpp` (`PathSanitizer::IsWebpageWatchUrl`) intercept watch URLs before network requests are dispatched.
   - The desktop controller and IPC layer reject watch URLs with error code `ERR_URL_IS_WEBPAGE`, informing the user that the URL is a streaming webpage and directing them to open the page in their browser and play the video so Flux's extension can capture the stream manifests.
3. **Capture via In-Page Playback Hooks:**
   - When the user plays the video in the browser, `catch-script/catch.js` (hooking `window.fetch` and `XMLHttpRequest` in the `MAIN` world) and the background network monitor capture the actual stream manifests (`.m3u8`, `.mpd`) and progressive segments, routing them cleanly through `N_m3u8DL-RE` or the segmented engine.


### Page-scoped candidate lifecycle

Media candidates are scoped to the active document URL within each tab. Main-frame full navigation, History API navigation, fragment navigation, and popup requests reconcile the current document identity and discard stale results for that tab only. Results from other tabs remain isolated, and duplicate URLs continue to use the existing candidate deduplication rules. SPA history and per-tab scoping have deterministic extension tests; actual multi-page site behavior requires live browser verification.

### Media size and filename resolution

For progressive media, the detector uses response `Content-Length` or the total from `Content-Range` when available. If neither supplies a size, the extension performs a bounded HEAD then one-byte-range metadata lookup, cancels the range body, and shows an explicit checking/unknown state until a reliable total is found. MIME-derived media extensions are used when the URL suffix is numeric or otherwise not a recognized media extension; numeric server filenames receive that resolved extension during handoff. Existing nonnumeric suffixes are retained.
