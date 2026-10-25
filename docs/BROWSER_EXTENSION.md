# Browser Extension Architecture Specification

**Document Status:** Proposed Subsystem Specification  
**Last Reviewed:** 2026-10-16  
**Scope:** Manifest V3 architecture, permissions minimization, background Service Worker, isolated content scripts, closed Shadow DOM video overlay, popup UI, and cross-browser packaging (`extension/`).  
**Related Documents:** [MEDIA_DETECTION.md](MEDIA_DETECTION.md), [NATIVE_MESSAGING_AND_IPC.md](NATIVE_MESSAGING_AND_IPC.md), [PRIVACY_AND_DATA_HANDLING.md](PRIVACY_AND_DATA_HANDLING.md), [adr/0003-browser-communication.md](adr/0003-browser-communication.md), [adr/0004-media-detection.md](adr/0004-media-detection.md)

---

## 1. Architectural Overview

The OpenIDM Browser Extension (`extension/`) is a lightweight Manifest V3 companion built from a single strict TypeScript codebase that compiles to two browser targets:
- **Chromium MV3 Target:** Google Chrome (114+), Microsoft Edge (114+), and Brave (114+).
- **Firefox MV3 Target:** Mozilla Firefox (115+ ESR / 128+).

Unlike [Cat Catch](https://github.com/xifangczy/cat-catch) (which bundles a 7.8 MB extension containing `hls.min.js`, preview players, and in-browser downloaders), the OpenIDM extension is strictly a **discovery and capture sensor** (`< 200 KB` total unpacked size, `< 15 KB` content script) that forwards downloads and media descriptors to `openidm.exe` over Native Messaging.

---

## 2. Internal Component Architecture

```mermaid
flowchart LR
    subgraph WebPage["Host Webpage (Main Frame & Iframes)"]
        DOM["<video> / <a> Elements"]
        Hook["Opt-In MAIN World Hook<br/>(page_hook/DeepSearchHook.ts)"]
        Overlay["ISOLATED Content Script<br/>(content/VideoOverlay.ts +<br/>Closed Shadow DOM)"]
        Hook -->|CustomEvent (Manifest Text)| Overlay
        DOM <--> Overlay
    end

    subgraph Background["Extension Background (MV3 Service Worker / Event Page)"]
        Sniffer["MediaSniffer.ts<br/>(webRequest Listener)"]
        Dedup["ManifestDeduplicator.ts"]
        DLInt["DownloadInterceptor.ts<br/>(chrome.downloads Listener)"]
        State["SessionStateStore.ts<br/>(chrome.storage.session)"]
        NMClient["NativeHostClient.ts<br/>(runtime.connectNative)"]

        Sniffer --> Dedup
        Dedup --> State
        DLInt --> NMClient
        State <--> NMClient
    end

    subgraph PopupUI["Extension Action Popup (popup/)"]
        Popup["Popup View<br/>(Detected Streams, Site Exclusion Toggle,<br/>Desktop Connection Status)"]
    end

    Overlay <-->|runtime.sendMessage| State
    Popup <-->|runtime.sendMessage| State
    NMClient <-->|stdio JSON-RPC| NMH["openidm-nmh.exe"]
```

---

## 3. Narrow Permission Model & Justification (`Section 8 Requirement`)

To minimize security risk and pass Chrome Web Store / Firefox AMO review without friction, every permission in `manifest.json` is strictly justified:

| Permission | Scope | Exact Engineering Justification | Why It Cannot Be Removed |
| :--- | :--- | :--- | :--- |
| **`nativeMessaging`** | Required | Allows `runtime.connectNative("com.flux.downloadmanager")` to spawn `flux-nmh.exe`. | Primary secure IPC mechanism (`ADR-0003`); eliminates localhost HTTP servers. |
| **`downloads`** | Required | Allows listening to `downloads.onCreated` / `onDeterminingFilename` (`FR-008`) and canceling browser downloads once `flux.exe` accepts the task. | Required to intercept browser-initiated downloads automatically. |
| **`webRequest`** | Required | Observational read-only access to `onBeforeSendHeaders` and `onHeadersReceived` to detect `.m3u8`/`.mpd` manifests and capture transient `Referer`/`Cookie` headers (`FR-014`). | Without `webRequest`, MSE-fetched manifests (`hls.js`/`dash.js`) are invisible to isolated content scripts. |
| **`storage`** | Required | Stores user exclusion lists (`chrome.storage.sync`/`local`) and survives MV3 Service Worker suspension via `chrome.storage.session` (`RISK-002`). | Required for MV3 state persistence. |
| **`contextMenus`** | Required | Adds `"Download with Flux"` and `"Download selected links with Flux"` (`FR-010`). | Required for right-click user workflows. |
| **`scripting`** | Required | Dynamically executes or injects the Layer 3 `MAIN`-world hook (`catch.js`) into webpage DOM (`FR-017`, `BUG-032`). | Allows deep search for obfuscated HLS/DASH streams in page JavaScript context. |
| **`tabs`** | Required | Queries active tab info and URLs for media classification, watch URL guard, and badge counts. | Required for popup tab binding and per-site capture toggle. |
| **`host_permissions`: `["*://*/*"]`** | Required | Needed by `webRequest` to observe media CDN requests (which are almost always hosted on third-party CDN domains different from the tab's top-level origin, e.g., `*.akamaized.net`, `*.cloudfront.net`). | Cross-origin CDN requests cannot be observed with `activeTab` alone. |


**Permissions Explicitly Excluded (Compared to Cat Catch):**
- **No `declarativeNetRequest`:** OpenIDM performs all downloading in `openidm.exe`, never inside the browser, so the extension never needs to modify browser request headers.
- **No `tabs` permission (where avoidable) / No browsing history access:** Tab URLs and titles are read only for tabs that actively trigger media detection or context-menu actions.
- **No `tabCapture` / `desktopCapture`:** Screen recording is out of scope.

---

## 4. In-Page Video Overlay Design (`content/VideoOverlay.ts`)

1. **Closed Shadow DOM Isolation (`FR-015`):**
   - When a qualifying `<video>` element (`width >= 240 && height >= 135 && duration > 3.0`) is observed, the content script creates a lightweight host element `<div data-openidm-overlay>` and attaches a shadow root with `attachShadow({ mode: "closed" })`.
   - All CSS styles are self-contained inside the closed shadow root (`all: initial; position: fixed; z-index: 2147483647; pointer-events: auto;`), preventing host page CSS from distorting the button and preventing page scripts from probing the extension DOM.
2. **Non-Intrusive Visibility Rules:**
   - The overlay button remains hidden (`opacity: 0; pointer-events: none`) until the user moves the pointer within the `<video>` bounding rect (tracked via passive `mousemove` / `mouseenter` listeners) and auto-hides after `2,500 ms` of cursor inactivity or when the user clicks the `[x]` dismiss button for that player.
3. **Iframe Coordination:**
   - Many video players run inside cross-origin `<iframe>` embeds. Because `content_scripts` runs with `"all_frames": true`, the overlay attaches directly inside the iframe housing the `<video>` element, while querying the background Service Worker for all media candidates matched to `(tabId, frameId)` or parent `tabId`.

---

## 5. Cross-Browser Differences: Chromium vs. Firefox

| Aspect | Chromium (Chrome, Edge, Brave) | Mozilla Firefox | OpenIDM Handling Strategy |
| :--- | :--- | :--- | :--- |
| **Manifest Background Key** | `"background": { "service_worker": "background.js", "type": "module" }` | `"background": { "scripts": ["background.js"], "type": "module" }` (Event Page) | Generated from `manifest.base.json` during `npm run build:chromium` vs `npm run build:firefox`. |
| **Extension ID & Native Messaging Origin** | Uses 32-char `chrome-extension://<id>/` derived from manifest `"key"` in `allowed_origins`. | Uses `"browser_specific_settings": { "gecko": { "id": "extension@openidm.org" } }` in `allowed_extensions`. | Installer writes both `org.openidm.native.chromium.json` and `org.openidm.native.firefox.json` ([NATIVE_MESSAGING_AND_IPC.md](NATIVE_MESSAGING_AND_IPC.md)). |
| **Download Interception Timing** | Supports `chrome.downloads.onDeterminingFilename` (fires before target file is created on disk). | Uses `browser.downloads.onCreated` + `webRequest.onHeadersReceived` (`Content-Disposition: attachment`). | `DownloadInterceptor.ts` binds `onDeterminingFilename` when defined (Chromium) and falls back to `onCreated` + header inspection on Firefox. |
| **`webRequest` Extra Headers** | Requires `'extraHeaders'` in `OnBeforeSendHeadersOptions` to read `Cookie` and `Referer`. | Does not support/require `'extraHeaders'` string in options array (throws if passed on older Gecko). | Feature-detect `chrome.webRequest.OnBeforeSendHeadersOptions.EXTRA_HEADERS` before appending `'extraHeaders'`. |

---

## 6. Avoiding Conflicts with Duplicate Extensions or Handlers (`FR-012`)

1. When intercepting a download in `DownloadInterceptor.ts`, the extension checks `downloadItem.byExtensionId`: if the download was initiated by another browser extension (such as a PDF generator or Cat Catch), OpenIDM ignores it so it does not break in-browser blob saves.
2. Before calling `chrome.downloads.cancel(downloadItem.id)`, the extension awaits a synchronous RPC acknowledgement (`download.offer -> { accepted: true, taskId: "..." }`, timeout `800 ms`) from `openidm.exe`. If `openidm.exe` is not installed, disabled, or times out, the browser download proceeds uninterrupted.

---

## 7. Client-Side Files and Hardened Download Interception Architecture

1. **Native Handling of Client-Side `blob:` and `data:` URLs:**
   - Client-side in-memory files (`blob:`, `data:` URLs, ChatGPT code/artifact exports, Canvas exports) reside exclusively in browser renderer memory and cannot be reached via external WinHTTP sockets.
   - The extension strictly preserves the browser's native download pipeline for `blob:` and `data:` URLs without suppressing user gestures or monkey-patching `HTMLAnchorElement.prototype.click` / `EventTarget.prototype.dispatchEvent`.
   - `catch.js` remains a passive media sniffer (XHR/fetch inspection, MediaSource, manifest parsing, YouTube player metadata) and never suppresses anchor clicks.

2. **Gesture-Preserving Click Intent Correlation:**
   - When a user clicks a downloadable link, `content-script.js` records the click metadata (URL, suggested filename, `download` attribute, referrer, and unique `correlationId`) and sends `{ action: "linkClicked" }` to the service worker.
   - The content script does **not** cancel the click (`preventDefault()` / `stopPropagation()`). The browser naturally begins the download request, preserving all browser cookies, session state, and redirected destinations.

3. **Chrome `onDeterminingFilename` Coordination:**
   - In Chromium browsers (Chrome, Edge, Brave), `chrome.downloads.onDeterminingFilename` receives the download with full HTTP response headers (`Content-Disposition`, MIME type, redirected final URL).
   - The event natively holds the browser file save prompt while `background.js` checks eligibility and queries `flux.exe` via `download.offer` over Native Messaging.
   - **Acceptance:** If Flux accepts, `suggest()` is called and `chrome.downloads.cancel()` cleanly cancels the browser download while Flux handles the segmented download.
   - **Offline / Declined / Timeout:** If Flux is offline, times out (5000 ms), or declines, `suggest()` is resolved and `chrome.downloads.resume()` is called, allowing the browser to download the file without user disruption.

4. **Manifest V3 Service Worker Lifecycle Persistence:**
   - Service worker state is persisted via `chrome.storage.session` (with automatic fallback to `chrome.storage.local`).
   - Persisted state includes active tab media candidates, tab URL mappings (up to 50 tabs), `recentDownloadResponses` (up to 500 items), `redirectChainMap`, `reverseRedirectMap`, and a ring buffer of recent link click correlations (up to 500 items), ensuring click correlation succeeds even if the service worker suspended between link click and download creation.
   - Tab closure (`tabs.onRemoved`) automatically commits storage pruning.

5. **Named Pipe Concurrency and Native Messaging Reliability:**
   - In `src/nmh/main.cpp`, client connection attempts handle `ERROR_PIPE_BUSY` by waiting on `WaitNamedPipeW` (3000 ms), eliminating spurious daemon launch attempts under concurrent tab activity.
   - Stdout frame serialization enforces a strict 1 MiB message limit, returning a standard JSON-RPC 2.0 error frame matching the request's exact `id` if exceeded.
   - Stdio and Named Pipe I/O enforce `ReadExact`, `WritePipeExact`, and `ReadPipeExact` to guarantee complete framing across chunked OS buffers.

6. **Authoritative Per-Download State Machine (`DownloadStateMachine`):**
   - Downloads are tracked by a strongly typed `DownloadCorrelationRecord` keyed strictly on Chromium `downloadId` (avoiding naive URL-keyed suppression that drops repeated downloads).
   - Lifecycle Stages:
     - `STAGE_INITIALIZATION` (`INIT`): Created upon first receipt of `onCreated` or `onDeterminingFilename`.
     - `STAGE_ELIGIBILITY`: Validated via unified `isDownloadEligible()` against expanded extension/MIME matrices, `Content-Disposition: attachment`, size thresholds, and gesture intent.
     - `STAGE_NATIVE_RPC` (`OFFERING`): Single shared `offerPromise` locked in `inFlightOffers` map, eliminating duplicate RPCs when both `onCreated` and `onDeterminingFilename` fire in parallel.
     - `STAGE_DESKTOP_ACCEPTANCE` (`ACCEPTED` / `HANDED_OFF`): Desktop accepts (`download.offer -> { accepted: true, taskId: "..." }`).
     - `STAGE_FAILURE_RECOVERY` (`FALLBACK` / `DECLINED`): Desktop offline, times out (5000 ms), returns malformed JSON, or user declines. `suggestCallback()` is resolved immediately and `chrome.downloads.resume()` called so native browser download continues without data loss.

7. **Brave and Chromium `onDeterminingFilename` Contract:**
   - Calling `chrome.downloads.cancel()` before resolving `suggest()` during filename determination causes Chromium and Brave to throw: `"Cannot cancel a download while determining filename"`, hanging the browser downloader until timeout and falling back to native download.
   - Flux resolves `suggest({ filename })` **before** executing `chrome.downloads.cancel()`, ensuring atomic, error-free handoff.
   - In-page click listeners in `content-script.js` never execute `preventDefault()` or `stopPropagation()`, preserving the browser's native cookies, session state, and HTTP redirect hops.

8. **22-Scenario Comprehensive Verification Suite (`tests/test_extension_detection.js`):**
   All 22 explicit edge scenarios required for production readiness pass in automated regression testing:
   1. Direct `.zip` or `.exe` links.
   2. URLs without file extension returning `Content-Disposition: attachment`.
   3. Multi-hop HTTP redirects to signed download URLs (e.g. S3 / CloudFront).
   4. Final filenames revealed only by response headers (`RFC 5987 / 6266`).
   5. Programmatic/direct downloads without preceding click records.
   6. Click records arriving before browser download event.
   7. Browser download events arriving before click records.
   8. Simultaneous downloads from different tabs without state collisions.
   9. Repeated downloads of identical URLs without false duplicate suppression.
   10. Desktop acceptance response handling (`accepted: true`).
   11. Desktop rejection response handling (`accepted: false`).
   12. Native messaging host missing error recovery.
   13. Native messaging timeout (5000 ms) graceful fallback.
   14. Malformed JSON-RPC responses fallback.
   15. Duplicate browser events for identical download ID deduplication via shared promise.
   16. Normal HTML page navigation guard (zero spurious capture).
   17. `blob:` and `data:` in-memory downloads preservation.
   18. User bypass modifier (Alt-key) and per-site exclusion handling.
   19. Cross-origin iframe frame correlation (`FRAME_ORIGINATED`).
   20. Failed browser cancellation attempt recovery (handoff preserved).
   21. Service worker suspension and wake-up session state restore.
   22. Concurrent media sniffer detection and file download handoff.

## Current-page media and compact video control update

Chromium uses `webNavigation.onHistoryStateUpdated` and `onReferenceFragmentUpdated` to clear media candidates when the main-frame document identity changes through SPA or fragment navigation. Full navigations and popup requests use the same per-tab identity check. Unrelated tabs retain their own candidate lists. The player control is aligned to the video container and positioned above its top edge with compact styling. Automated coverage exercises per-tab clearing and toolbar geometry; live Brave behavior still requires browser verification.

### Bounded direct-media size lookup

When a direct progressive media response has no trustworthy size, the service worker performs one per-URL lookup: an authenticated-cookie `HEAD` request followed, only when needed, by a `Range: bytes=0-0` request. The range body is canceled immediately; an unknown result is shown as “Checking size…” or “Size unknown”. The probe has a five-minute per-candidate retry guard and only updates a candidate still present in that tab's current list. The deterministic test covers HEAD failure, Content-Range total extraction, and body cancellation. Browser-specific authorization and server behavior remain environment-dependent.

### Stable installation diagnostics

Chromium extension identity is pinned by the public key in `manifest.chromium.json` / `manifest.json`. The Windows build cleans the C++ build directory and copies extension assets into `dist/extension`; it does not delete the source extension directory. Native Messaging registration is tied to the installed application directory. An unpacked extension under an application directory can stop loading if that directory is removed or moved, and Chromium policy can disable/remove extensions independently. This repository does not silently force-install an unpacked extension.

### Native host path stability

The registration script prefers the staged `dist/radium-nmh.exe` with build-directory fallbacks, so a normal clean build does not invalidate the registered host. An unpacked extension can stop loading if its own path is removed or moved, and Chromium policy can disable/remove extensions independently. The audited Brave profile currently registers the unpacked source `extension/` directory with no disable reason; persistence after a browser restart or policy-driven removal was not runtime-tested.

