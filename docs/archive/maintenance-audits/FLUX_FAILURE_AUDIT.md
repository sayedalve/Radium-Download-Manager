# Flux Download Manager: Browser Download & Media Detection Failure Diagnostic Audit

**Document Status:** Complete Forensic Diagnostic Audit  
**Date:** 2026-10-16  
**Scope:** In-depth forensic diagnosis of why the previous release of Flux Download Manager failed to intercept ordinary file downloads, failed to detect HTML5 video playback, produced an empty popup ("No media detected on this tab yet"), and failed with HTTP 429 Too Many Requests when downloading a YouTube watch page.

---

## 1. Executive Summary of Confirmed Failures

During live browser testing with Google Chrome, Microsoft Edge, and Mozilla Firefox:
1. **Ordinary Download Link Clicks Were Ignored**: Clicking links to `.zip`, `.pdf`, `.exe`, or files with `download` attributes resulted in default browser downloading. Flux desktop was never notified, and no download dialog appeared.
2. **Webpage Video Playback Was Undetected**: Playing HTML5 video streams (both direct `.mp4` and adaptive HLS/DASH) on popular video websites resulted in zero detections. The extension icon badge displayed no number, and the in-page "Download with Flux" button was never rendered.
3. **Popup Remained Permanently Empty**: Clicking the extension icon opened `popup.html`, which showed "Desktop Connected" and "Capture on site" enabled, but the media list remained static: `"No media detected on this tab yet. Play a video or start a download to capture streams."`
4. **YouTube Watch Page Produced HTTP 429 Too Many Requests**: Submitting a YouTube watch URL (`https://www.youtube.com/watch?v=...`) to Flux desktop resulted in `HttpDownloadEngine` attempting to download the HTML document via multi-connection HTTP range requests, triggering YouTube's rate-limiting / anti-scraping defense (HTTP 429 Too Many Requests).

---

## 2. Root Cause Analysis by Component

### 2.1 Failure Cause 1: Content Script Execution Inertia & Missing Initialization (`extension/js/content-script.js`)
- **Inspection Finding**: `extension/js/content-script.js` defined three top-level functions: `positionOverlayHost`, `scanAndAttachVideoOverlays`, and `observeDynamicVideosAndFullscreen`.
- **Breakdown Point**: At the bottom of `content-script.js`, the script contained only:
  ```javascript
  if (typeof module !== "undefined" && module.exports) {
    module.exports = { positionOverlayHost, scanAndAttachVideoOverlays, observeDynamicVideosAndFullscreen };
  }
  ```
- **Consequence**: When loaded into a real browser page via Chrome's content script engine, `observeDynamicVideosAndFullscreen` was **never invoked**! The DOM was never scanned, no observers were registered, no event listeners were attached to `<video>` elements, and no video overlay was ever mounted.
- **Unit Test Mirage**: In `tests/test_extension_detection.js`, the test explicitly called `observeDynamicVideosAndFullscreen(fakeDoc, fakeWin, ...)` as an exported module function. The unit tests passed completely while the actual browser script was totally dormant.

### 2.2 Failure Cause 2: Zero Link-Click Interception in Content Script
- **Inspection Finding**: `content-script.js` had zero event listeners for `click` or `auxclick`.
- **Breakdown Point**: When a user clicked an `<a href="...">` download link:
  1. The content script never intercepted the click event.
  2. The target URL, suggested filename, HTTP referrer, and `e.altKey` modifier were never captured.
  3. No notification was sent to the background service worker.

### 2.3 Failure Cause 3: Background Worker Listener Vacuum (`extension/js/background.js`)
- **Inspection Finding**: In `extension/js/background.js`:
  1. `chrome.runtime.onMessage.addListener` was **completely absent**. There was no handler to receive messages from `content-script.js` or `popup.js`!
  2. `chrome.downloads.onCreated.addListener` and `chrome.downloads.onDeterminingFilename.addListener` were **never registered**. Although `async function onDownloadCreatedHandler` was defined in code, it was never passed to `chrome.downloads` APIs!
  3. `chrome.action.setBadgeText` was never called; the extension toolbar icon badge never displayed the count of detected media.
  4. Tab navigation listeners (`chrome.tabs.onUpdated`, `chrome.tabs.onRemoved`, `chrome.tabs.onActivated`) were missing, meaning per-tab media cache in `G.cacheData` was never synchronized or cleaned up on page reloads.

### 2.4 Failure Cause 4: Popup UI Lifecycle Disconnection (`extension/js/popup.js`)
- **Inspection Finding**: `extension/js/popup.js` defined `formatMediaSize`, `filterPopupCandidates`, and `renderDetectedMediaList`, exported them to `module.exports`, and stopped.
- **Breakdown Point**:
  1. There was no `document.addEventListener("DOMContentLoaded", ...)` initialization block.
  2. The popup never called `chrome.tabs.query` to get the current tab ID.
  3. The popup never called `chrome.runtime.sendMessage({ action: "getMediaList" })` to fetch detected candidates from `G.cacheData`.
  4. The search input, filter category buttons, "Clear List", and download buttons had no event listeners attached.
  5. The popup simply displayed the default hardcoded HTML markup in `popup.html`.

### 2.5 Failure Cause 5: In-Page Deep Search Execution World Isolation (`catch-script/catch.js`)
- **Inspection Finding**: In Chrome Manifest V3, content scripts execute in an `ISOLATED` world by default.
- **Breakdown Point**:
  1. Monkey-patching `window.fetch` or `window.XMLHttpRequest` inside an isolated world script only affects calls made by the extension, not the webpage's own JavaScript application (e.g. YouTube's player, video.js, Hls.js, Dash.js).
  2. `catch.js` was neither injected as a `<script>` tag into the webpage DOM nor executed via `chrome.scripting.executeScript({ world: "MAIN" })`.
  3. `catch.js` only hooked `fetch`; it did not hook `XMLHttpRequest` or `URL.createObjectURL`, missing media players that use XHR or Blob URLs.

### 2.6 Failure Cause 6: URL Classification Deficiency & YouTube Watch Page 429
- **Inspection Finding**: In both `extension/js/function.js` and desktop `src/ipc/ipc_server.cpp`:
  1. URLs like `https://www.youtube.com/watch?v=...` or `https://vimeo.com/...` are HTML web documents, not direct media streams or manifest playlists (`.m3u8`, `.mpd`).
  2. When the user attempted to download a YouTube watch page, Flux desktop created a `TaskType::kHttpFile` task.
  3. `HttpDownloadEngine` sent multi-threaded HTTP range requests to `youtube.com/watch?v=...`. YouTube's edge CDN detected segmented scraping on an HTML page and immediately blocked the client with HTTP 429 Too Many Requests.
  4. Neither the extension nor the desktop core had a URL classifier to distinguish between HTML watch pages, direct media binaries, and adaptive stream manifests.

---

## 3. Comparison with IDM Audit & Cat Catch Upstreams

| Feature / Mechanism | IDM Verified Behavior (`idm audit/`) | Cat Catch Behavior (`third_party/cat-catch/`) | Flux Prior Defect | Flux Overhaul Resolution |
| :--- | :--- | :--- | :--- | :--- |
| **Link Click Capture** | `content.js` capture event on `<a>`, tests `hasAttribute("download")`, URL extension, Alt bypass. | Browser `downloads.onCreated` / `onDeterminingFilename` coordination. | Content script had 0 click listeners. | Capture phase `click` + `auxclick` on `<a>`, Alt key bypass, plus `chrome.downloads` fallback. |
| **Download Interception** | `browser.downloads.pause`, sends offer, `cancel` on accept, `resume` on decline. | Extension monitors downloads API and prompts. | `chrome.downloads` listeners were never registered. | Registered `chrome.downloads.onCreated` and `onDeterminingFilename`, query desktop via RPC, cancel only on accept. |
| **Video Observation** | Observers for `<video>`, `<audio>`, `embed`, `object` + `getBoundingClientRect`. | `content-script.js` checks `currentSrc`, listens to `playing`, `play`, `canplay`. | Content script defined functions but never executed them. | Auto-executing `initContentScript()`, DOM observers, media event hooks (`playing`, `canplay`). |
| **Deep Search** | Native packet inspection + webRequest header inspection. | Injects `search.js` into page MAIN world to hook `fetch`, `XHR`, `createObjectURL`. | Script was in isolated world and only hooked `fetch`. | Inject `catch.js` into page DOM (MAIN world) hooking `fetch`, `XHR`, and `createObjectURL`. |
| **Popup Media Sync** | Native Win32 floating window over video viewport. | Popup queries background worker via `chrome.runtime.sendMessage` on open. | Popup JS had no DOM ready or message passing logic. | Full DOM initialization in `popup.js`, RPC query `getMediaList` to background, live filtering. |
| **Webpage Watch Guard** | Parses manifests via `IDMVMPrs.dll`, generates synthetic `hlstoidm://`, never raw HTML. | Detects embedded media URLs; does not treat watch page as raw media file. | Desktop treated YouTube watch URL as `kHttpFile`, triggering 429. | Central `classifyUrlIntent`: guard against watch pages, guide user to play video in browser. |

---

## 4. Corrective Architecture Blueprint

1. **`extension/js/content-script.js`**:
   - Immediate self-initialization on script load (`initContentScript()`).
   - Capturing `click`/`auxclick` listener for link interception and Alt-key bypass.
   - Dynamic media observers + media playback listeners (`play`, `playing`, `canplay`).
   - Closed Shadow DOM floating download button with reliable positioning.
   - Injection of `catch-script/catch.js` into DOM.
2. **`extension/catch-script/catch.js`**:
   - Runs in webpage MAIN world. Hooks `fetch`, `XMLHttpRequest`, and `URL.createObjectURL`.
   - Relays HLS `#EXTM3U`, DASH `<MPD`, and AES keys via `window.postMessage`.
3. **`extension/js/background.js`**:
   - Registers `chrome.runtime.onMessage` (`getMediaList`, `addMedia`, `downloadMedia`, `clearMedia`, `setSiteCapture`, `linkClicked`, `pingDesktop`).
   - Registers `chrome.downloads.onCreated` and `onDeterminingFilename` to intercept eligible files and offer to desktop.
   - Synchronizes badge text (`chrome.action.setBadgeText`).
4. **`extension/js/popup.js`**:
   - Complete interactive UI controller querying active tab and live background cache.
5. **`extension/js/function.js` & `src/ipc/ipc_server.cpp`**:
   - URL classification guard: reject HTML watch page URLs with `ERR_URL_IS_WEBPAGE`.
6. **Portable Native Messaging Registration**:
   - Provide `scripts/register-native-messaging.ps1` to configure HKCU keys for Chrome, Edge, Brave, and Firefox pointing to portable `flux-nmh.exe`.
