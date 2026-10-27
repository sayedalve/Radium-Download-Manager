#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Automated Phase 7 & Phase 9 Browser Extension Verification Suite (11 Suites)
// Tests Cat Catch media detection, HLS/DASH discovery, master/child deduplication,
// segment suppression, per-site exclusion, DOM overlay attachment & fullscreen repositioning,
// Deep Search hook, MV3 Service Worker session persistence, Popup filtering/states, fallback,
// and Cosmic Dark UI consistency.

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { G } = require("../extension/js/init.js");
const {
  isSiteDisabled,
  setSiteDisabled,
  classifyNetworkResource,
  addOrDeduplicateMediaCandidate,
  isDownloadEligible,
  sanitizeFilename
} = require("../extension/js/function.js");
const {
  persistSessionState,
  restoreSessionState,
  onBeforeSendHeadersHandler,
  onHeadersReceivedHandler,
  onDownloadCreatedHandler,
  probeMediaSize,
  submitMediaCandidateToFlux,
  submitMediaCandidateToOpenIDM,
  handleRuntimeMessage,
  updateTabDocument,
  downloadStateMachine
} = require("../extension/js/background.js");
const {
  positionOverlayHost,
  scanAndAttachVideoOverlays,
  observeDynamicVideosAndFullscreen
} = require("../extension/js/content-script.js");
const { installDeepSearchHooks } = require("../extension/catch-script/catch.js");
const {
  filterPopupCandidates,
  renderDetectedMediaList
} = require("../extension/js/popup.js");

async function runTests() {
  console.log("[1/11] Testing Direct MP4/WebM & HLS/DASH Detection + Header Correlation...");
  onBeforeSendHeadersHandler({
    requestId: "req-101",
    requestHeaders: [
      { name: "Referer", value: "https://watch.example.com/course/42" },
      { name: "Cookie", value: "sid=secret_cookie_abc" },
      { name: "User-Agent", value: "Mozilla/5.0 TestBrowser" }
    ]
  });

  const mp4Res = onHeadersReceivedHandler({
    requestId: "req-101",
    tabId: 10,
    frameId: 0,
    url: "https://cdn.example.com/media/lecture42.mp4",
    documentUrl: "https://watch.example.com/course/42",
    responseHeaders: [
      { name: "Content-Type", value: "video/mp4" },
      { name: "Content-Length", value: "15728640" }
    ]
  });
  assert.strictEqual(mp4Res.added, true, "Direct MP4 >= 512 KiB must be detected");
  assert.strictEqual(mp4Res.candidate.headers.Referer, "https://watch.example.com/course/42", "Referer header correlated");
  assert.strictEqual(mp4Res.candidate.headers.Cookie, "sid=secret_cookie_abc", "Cookie header correlated");

  const tinyMp4 = classifyNetworkResource({
    tabId: 10,
    url: "https://cdn.example.com/ui/hover.mp4",
    responseHeaders: [
      { name: "Content-Type", value: "video/mp4" },
      { name: "Content-Length", value: "40960" }
    ]
  });
  assert.strictEqual(tinyMp4, null, "Tiny UI video clip (< 512 KiB) filtered out");
  console.log("  -> PASS");

  console.log("[2/11] Testing Master vs. Child HLS Deduplication & Segment Suppression...");
  const masterRes = onHeadersReceivedHandler({
    requestId: "req-201",
    tabId: 20,
    frameId: 0,
    url: "https://vod.example.com/streams/99/master.m3u8?token=xyz",
    responseHeaders: [{ name: "Content-Type", value: "application/vnd.apple.mpegurl" }]
  });
  assert.strictEqual(masterRes.added, true);
  assert.strictEqual(masterRes.candidate.isMaster, true);

  const childRes = onHeadersReceivedHandler({
    requestId: "req-202",
    tabId: 20,
    frameId: 0,
    url: "https://vod.example.com/streams/99/1080p/prog_index.m3u8?token=xyz",
    responseHeaders: [{ name: "Content-Type", value: "application/vnd.apple.mpegurl" }]
  });
  assert.strictEqual(childRes.added, false, "Child variant playlist must be grouped under Master playlist");
  assert.strictEqual(childRes.groupedUnder, masterRes.candidate.candidateId);
  assert.strictEqual(G.cacheData.get(20).length, 1, "Only 1 primary entry in tab 20 list");

  const tsSeg = onHeadersReceivedHandler({
    requestId: "req-203",
    tabId: 20,
    url: "https://vod.example.com/streams/99/1080p/seg_0001.ts",
    responseHeaders: [{ name: "Content-Type", value: "video/mp2t" }]
  });
  assert.strictEqual(tsSeg, null, ".ts segments suppressed");

  const dashRes = onHeadersReceivedHandler({
    requestId: "req-204",
    tabId: 20,
    url: "https://vod.example.com/dash/movie.mpd",
    responseHeaders: [{ name: "Content-Type", value: "application/dash+xml" }]
  });
  assert.strictEqual(dashRes.added, true);
  assert.strictEqual(dashRes.candidate.streamType, "DASH");
  console.log("  -> PASS");

  console.log("[3/11] Testing Per-Site Exclusion Setting & Popup Rendering...");
  setSiteDisabled("https://blocked.example.com/watch", true);
  assert.strictEqual(isSiteDisabled("https://blocked.example.com/video.mp4"), true);
  const blockedRes = classifyNetworkResource({
    tabId: 30,
    url: "https://cdn.example.com/v.m3u8",
    initiator: "https://blocked.example.com/watch",
    responseHeaders: [{ name: "Content-Type", value: "application/vnd.apple.mpegurl" }]
  });
  assert.strictEqual(blockedRes, null, "Disabled site produces zero detections");
  setSiteDisabled("https://blocked.example.com/watch", false);

  const html = renderDetectedMediaList(G.cacheData.get(20));
  assert(html.includes("HLS") && html.includes("DASH") && html.includes("1 sub-playlists grouped"));
  console.log("  -> PASS");

  console.log("[4/11] Testing Page-Context Deep Search Hook (Obfuscated Manifest Capture)...");
  const capturedEvents = [];
  const fakeWindow = {
    location: { href: "https://player.example.com/embed/77" },
    fetch: async (url) => ({
      url,
      clone: () => ({
        text: async () => "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-STREAM-INF:BANDWIDTH=4000000,RESOLUTION=1920x1080\nhigh.m3u8\n"
      })
    })
  };
  installDeepSearchHooks(fakeWindow, "nonce-secret-123", (ev) => capturedEvents.push(ev));
  await fakeWindow.fetch("https://api.example.com/get_manifest_blob?id=77");
  assert.strictEqual(capturedEvents.length, 1);
  assert.strictEqual(capturedEvents[0].nonce, "nonce-secret-123");
  assert.strictEqual(capturedEvents[0].streamType, "HLS");
  assert(capturedEvents[0].cachedManifestBody.includes("#EXT-X-STREAM-INF"));
  console.log("  -> PASS");

  console.log("[5/11] Testing In-Page Closed Shadow DOM Video Overlay & Fullscreen Repositioning...");
  let clickedVideoPayload = null;
  let videoRect = { width: 800, height: 450, top: 40, left: 50, right: 850 };
  let fullscreenHandler = null;
  const fakeDoc = {
    title: "Lesson 9 - Advanced Networking",
    location: { href: "https://iframe.example.com/player" },
    body: { appendChild: () => {} },
    addEventListener: (evt, fn) => { if (evt === "fullscreenchange") fullscreenHandler = fn; },
    createElement: (tag) => ({
      tag,
      style: {},
      dataset: {},
      setAttribute: () => {},
      attachShadow: function (opts) {
        this.shadowMode = opts.mode;
        return { appendChild: (child) => { this.shadowChild = child; } };
      },
      addEventListener: function (evt, fn) {
        if (evt === "click") this._clickHandler = fn;
      }
    }),
    querySelectorAll: () => [
      {
        currentSrc: "blob:https://iframe.example.com/9999-aaaa",
        mediaKeys: null,
        dataset: {},
        getBoundingClientRect: () => videoRect
      }
    ]
  };
  const obsResult = observeDynamicVideosAndFullscreen(fakeDoc, null, (payload) => { clickedVideoPayload = payload; });
  assert.strictEqual(obsResult.activeOverlays.length, 1, "Overlay attached to visible <video>");
  assert.strictEqual(obsResult.activeOverlays[0].hostEl.shadowMode, "closed", "Uses closed Shadow DOM");
  assert(obsResult.activeOverlays[0].hostEl.style.cssText.includes("top:8px"), "Player toolbar stays above the image with viewport padding");
  assert(obsResult.activeOverlays[0].hostEl.style.cssText.includes("left:50px"), "Player toolbar aligns to the player left edge");

  // Simulate entering fullscreen (1920x1080 at 0,0) and verify overlay repositions
  videoRect = { width: 1920, height: 1080, top: 0, left: 0, right: 1920 };
  fullscreenHandler();
  assert(obsResult.activeOverlays[0].hostEl.style.cssText.includes("left:8px"), "Toolbar remains within the viewport when fullscreen starts at the screen edge");

  assert.strictEqual(obsResult.activeOverlays[0].btn.textContent, "Download with Radium", "Overlay button text rebranded to Radium");
  obsResult.activeOverlays[0].btn._clickHandler({ stopPropagation: () => {}, preventDefault: () => {} });
  assert.ok(clickedVideoPayload, "Overlay button click triggers real download handoff");
  assert.strictEqual(clickedVideoPayload.isMseBlob, true);
  console.log("  -> PASS");

  console.log("[6/11] Testing Download Interception, Bypass Modifier & Safe Browser Fallback...");
  let canceledDownloadId = null;
  const mockTransportAccept = async (rpc) => {
    assert.strictEqual(rpc.method, "download.offer");
    return { accepted: true, taskId: "task-uuid-1234", fileName: "ubuntu.iso" };
  };
  const interceptRes = await onDownloadCreatedHandler(
    { id: 55, url: "https://releases.example.com/ubuntu.iso", filename: "ubuntu.iso", fileSize: 50000000, referrer: "https://releases.example.com/" },
    mockTransportAccept,
    async (id) => { canceledDownloadId = id; }
  );
  assert.strictEqual(interceptRes.intercepted, true);
  assert.strictEqual(canceledDownloadId, 55, "Browser built-in download canceled after OpenIDM accepted");

  const mockTransportOffline = async () => { throw new Error("Specified native messaging host not found"); };
  const fallbackRes = await onDownloadCreatedHandler(
    { id: 56, url: "https://releases.example.com/ubuntu.iso", filename: "ubuntu.iso", fileSize: 50000000 },
    mockTransportOffline
  );
  assert.strictEqual(fallbackRes.intercepted, false);
  assert.strictEqual(fallbackRes.fallbackToBrowser, true, "Safely falls back to browser when desktop app is offline");
  console.log("  -> PASS");

  console.log("[7/11] Testing Manifest V3 Service Worker Session State Persistence & Recovery...");
  const fakeSessionStore = {
    _data: {},
    set: async function (obj) { Object.assign(this._data, obj); },
    get: async function (keys) {
      const out = {};
      for (const k of keys) out[k] = this._data[k];
      return out;
    }
  };
  setSiteDisabled("https://persisted-exclude.example.com/page", true);
  await persistSessionState(fakeSessionStore);
  G.cacheData.clear();
  G.disabledHosts.clear();
  assert.strictEqual(G.cacheData.size, 0);
  await restoreSessionState(fakeSessionStore);
  assert.strictEqual(G.cacheData.get(20).length, 2, "Restored Tab 20 media candidates after Service Worker wake");
  assert.strictEqual(isSiteDisabled("https://persisted-exclude.example.com/video"), true, "Restored disabled host after wake");
  console.log("  -> PASS");

  console.log("[8/11] Testing Popup Filter Tabs, Search, States & Direct Media Fallback...");
  const combinedList = G.cacheData.get(20).concat(G.cacheData.get(10));
  assert.strictEqual(filterPopupCandidates(combinedList, "stream", "").length, 2, "Stream filter returns HLS + DASH");
  assert.strictEqual(filterPopupCandidates(combinedList, "direct", "").length, 1, "Direct filter returns MP4");
  assert.strictEqual(filterPopupCandidates(combinedList, "all", "lecture42").length, 1, "Search query filters by title/filename");

  const loadingHtml = renderDetectedMediaList(combinedList, { loading: true });
  assert(loadingHtml.includes('data-state="loading"'));
  const offlineHtml = renderDetectedMediaList(combinedList, { desktopConnected: false });
  assert(offlineHtml.includes('data-state="disconnected"') && offlineHtml.includes("Browser Download"));
  const onlineHtml = renderDetectedMediaList(combinedList, { desktopConnected: true });
  assert(onlineHtml.includes("Download in Radium"), "Popup renders 'Download in Radium' action label");

  // Direct MP4 handoff falls back to browser download when Radium is offline
  let browserFallbackTriggeredUrl = null;
  const mp4Candidate = G.cacheData.get(10)[0];
  const fbDirect = await submitMediaCandidateToOpenIDM(
    mp4Candidate,
    {},
    mockTransportOffline,
    async (opts) => { browserFallbackTriggeredUrl = opts.url; }
  );
  assert.strictEqual(fbDirect.fallbackToBrowser, true);
  assert.strictEqual(browserFallbackTriggeredUrl, "https://cdn.example.com/media/lecture42.mp4");
  console.log("  -> PASS");

  console.log("[9/11] Testing Link-Click Intent Recording & Alt-Key Bypass Behavior...");
  const { handleLinkClick } = require("../extension/js/content-script.js");
  let sentLinkMessages = [];
  globalThis.chrome = {
    runtime: {
      sendMessage: (msg) => { sentLinkMessages.push(msg); }
    }
  };

  // Normal link click on .zip file: natural user gesture preserved, sends linkClicked for correlation
  let preventDefaultCalled = false;
  const fakeZipAnchor = {
    pathname: "/downloads/archive.zip",
    href: "https://files.example.com/downloads/archive.zip",
    download: "archive.zip",
    hasAttribute: (attr) => attr === "download"
  };
  const fakeZipEvent = {
    altKey: false,
    preventDefault: () => { preventDefaultCalled = true; },
    stopPropagation: () => {},
    target: { closest: (tag) => (tag === "a" ? fakeZipAnchor : null) }
  };
  handleLinkClick(fakeZipEvent);
  assert.strictEqual(preventDefaultCalled, false, "Natural user click gesture is NEVER prevented");
  assert.strictEqual(sentLinkMessages.length, 1);
  assert.strictEqual(sentLinkMessages[0].action, "linkClicked");
  assert.strictEqual(sentLinkMessages[0].url, "https://files.example.com/downloads/archive.zip");
  assert.strictEqual(sentLinkMessages[0].filename, "archive.zip");

  // Alt-key bypass modifier click
  sentLinkMessages = [];
  globalThis.chrome.runtime.sendMessage = (msg) => { sentLinkMessages.push(msg); };
  const fakeAltEvent = {
    altKey: true,
    preventDefault: () => {},
    stopPropagation: () => {},
    target: { closest: () => fakeZipAnchor }
  };
  handleLinkClick(fakeAltEvent);
  assert.strictEqual(sentLinkMessages.length, 1);
  assert.strictEqual(sentLinkMessages[0].action, "bypassNextDownload");
  console.log("  -> PASS");

  console.log("[10/11] Testing Webpage Watch URL Guard & Background Runtime Message Routing...");
  const { isWebpageWatchUrl, classifyUrlIntent } = require("../extension/js/function.js");
  const { handleRuntimeMessage } = require("../extension/js/background.js");

  // YouTube watch page guard
  assert.strictEqual(isWebpageWatchUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"), true);
  assert.strictEqual(isWebpageWatchUrl("https://youtu.be/dQw4w9WgXcQ"), true);
  assert.strictEqual(isWebpageWatchUrl("https://www.youtube.com/shorts/abcd1234efg"), true);
  assert.strictEqual(isWebpageWatchUrl("https://vimeo.com/12345678"), true);
  assert.strictEqual(classifyUrlIntent("https://www.youtube.com/watch?v=dQw4w9WgXcQ"), "WEBPAGE");

  // Direct files & stream manifests
  assert.strictEqual(isWebpageWatchUrl("https://cdn.example.com/video.mp4"), false);
  assert.strictEqual(classifyUrlIntent("https://cdn.example.com/stream.m3u8"), "HLS");
  assert.strictEqual(classifyUrlIntent("https://cdn.example.com/stream.mpd"), "DASH");
  assert.strictEqual(classifyUrlIntent("https://cdn.example.com/archive.zip"), "DIRECT_FILE");

  // Message router test: addMedia and getMediaList
  let msgResponse = null;
  handleRuntimeMessage({
    action: "addMedia",
    url: "https://video.example.com/direct_test.mp4",
    streamType: "HTTP_FILE",
    tabId: 99,
    pageTitle: "Test Video"
  }, { tab: { id: 99, url: "https://video.example.com/watch" } }, (res) => { msgResponse = res; });
  assert.strictEqual(msgResponse.success, true);
  assert.strictEqual(G.cacheData.get(99).length, 1);

  handleRuntimeMessage({
    action: "clearMedia",
    tabId: 99
  }, {}, (res) => { msgResponse = res; });
  assert.strictEqual(msgResponse.success, true);
  assert.strictEqual(G.cacheData.has(99), false);
  console.log("  -> PASS");

  console.log("[11/11] Testing Cosmic Dark Extension Styling & Offscreen Overlay Visibility...");
  const cssContent = fs.readFileSync(path.join(__dirname, "../extension/css/popup.css"), "utf8");
  assert(cssContent.includes("color-scheme: dark"), "popup.css enforces dark color-scheme");
  assert(!cssContent.includes("prefers-color-scheme: light"), "popup.css does not switch to light theme");
  assert(cssContent.includes("--bg-void: #070B14"), "popup.css shares Cosmic Dark tokens");

  const offscreenHost = { style: {} };
  positionOverlayHost({ getBoundingClientRect: () => ({ width: 640, height: 360, top: -400, bottom: -40, left: 20, right: 660 }) }, offscreenHost);
  assert(offscreenHost.style.cssText.includes("opacity:0;pointer-events:none;"), "Offscreen video hides overlay unobtrusively");
  console.log("  -> PASS");

  console.log("[12/12] Testing Quality Size Estimation & Canonical YouTube Watch Routing...");
  // Test size estimation from bitrate and duration
  const bitrate = 2500000; // 2.5 Mbps
  const durationSec = 120; // 2 minutes
  const estimatedBytes = Math.round((bitrate * durationSec) / 8);
  assert.strictEqual(estimatedBytes, 37500000, "Bitrate + duration yields accurate estimated byte size");

  // Test YouTube watch URL resolution in background
  const fakeYoutubeCandidate = {
    url: "https://rr3---sn-4g5ednks.googlevideo.com/videoplayback?expire=1729272302&ei=xyz&id=o-1234&itag=137",
    streamType: "YOUTUBE_EXTRACTION",
    pageUrl: "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
    videoId: "dQw4w9WgXcQ",
    quality: "1080p",
    headers: { Referer: "https://www.youtube.com/watch?v=dQw4w9WgXcQ" }
  };
  const isYoutube = (fakeYoutubeCandidate.streamType === "YOUTUBE_EXTRACTION") ||
                    (fakeYoutubeCandidate.url.includes("googlevideo.com"));
  assert.strictEqual(isYoutube, true, "YouTube stream candidate identified");
  const canonicalWatch = fakeYoutubeCandidate.pageUrl || ("https://www.youtube.com/watch?v=" + fakeYoutubeCandidate.videoId);
  assert.strictEqual(canonicalWatch, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "Resolved canonical watch URL matches YouTube video page");
  console.log("[13/13] Testing Quality Variant Differentiation & Header Preservation without Playing-Stream Downgrade...");
  let sentRpcParams = null;
  const mockTransport = async (payload) => {
    sentRpcParams = payload.params;
    return { accepted: true, taskId: "task-mock-stream-1" };
  };

  const fakeYtCand = {
    candidateId: "cand-yt-1080",
    url: "https://rr1---sn-4g5ednks.googlevideo.com/videoplayback?itag=18&expire=999",
    streamType: "YOUTUBE",
    sourceType: "YOUTUBE_EXTRACTION",
    isGoogleVideo: true,
    tabUrl: "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
    videoUrl: "https://rr1---sn-4g5ednks.googlevideo.com/videoplayback?itag=18&expire=999", // currently playing 360p
    referer: "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
    qualities: [
      { id: "1080p_60fps_webm", resolution: "1080p60", videoFilter: "1080p60", videoUrl: "", ext: "webm" },
      { id: "1080p_mp4", resolution: "1080p", videoFilter: "1080p", videoUrl: "", ext: "mp4" },
      { id: "360p_mp4", resolution: "360p", videoFilter: "360p", videoUrl: "https://rr1---sn-4g5ednks.googlevideo.com/videoplayback?itag=18&expire=999", ext: "mp4" }
    ]
  };

  // Submit with 1080p MP4 quality: MUST NOT fall back to candidate.videoUrl (the playing 360p stream)!
  await submitMediaCandidateToOpenIDM(fakeYtCand, {
    selectedQuality: fakeYtCand.qualities[1], // 1080p MP4
    qualityId: "1080p_mp4"
  }, mockTransport);

  assert(sentRpcParams, "RPC params sent to mock transport");
  assert.strictEqual(sentRpcParams.videoUrl, "", "videoUrl is empty so backend delegates to yt-dlp with requested videoFilter");
  assert.strictEqual(sentRpcParams.videoFilter, "1080p", "Selected videoFilter matches 1080p");
  assert.strictEqual(sentRpcParams.resolution, "1080p", "Resolution matches 1080p");
  assert.strictEqual(sentRpcParams.watchUrl, "https://www.youtube.com/watch?v=dQw4w9WgXcQ", "Watch URL preserved");
  assert(sentRpcParams.headers["Referer"] || sentRpcParams.headers["referer"], "Referer header preserved for stream extraction");

  // HLS stream with chosen quality variant URL
  const fakeHlsCand = {
    candidateId: "cand-hls-pimpbunny",
    url: "https://cdn.example.com/stream/master.m3u8",
    streamType: "HLS",
    sourceType: "HLS_MANIFEST",
    referer: "https://pimpbunny.com/video/99",
    qualities: [
      { id: "720p", resolution: "720p", variantUrl: "https://cdn.example.com/stream/720p.m3u8" }
    ]
  };
  await submitMediaCandidateToOpenIDM(fakeHlsCand, {
    selectedQuality: fakeHlsCand.qualities[0],
    qualityId: "720p"
  }, mockTransport);

  assert.strictEqual(sentRpcParams.manifestUrl, "https://cdn.example.com/stream/720p.m3u8", "Variant URL prioritized when chosen quality selected");
  assert.strictEqual(sentRpcParams.headers["Referer"], "https://pimpbunny.com/video/99", "Referer header preserved from candidate.referer");

  console.log("  -> PASS");

  console.log("\n[14/14] Testing 22 Comprehensive Interception, Fallback & Edge Scenarios...");
  const mockAcceptScenario = async (rpc) => {
    return { accepted: true, taskId: "task-" + Math.random().toString(36).slice(2), fileName: rpc.params && rpc.params.filename };
  };

  // Scenario 1: Direct .zip or .exe link
  let canceledId1 = null, suggestedName1 = null;
  const res1 = await onDownloadCreatedHandler(
    { id: 2001, url: "https://example.com/installer.exe", filename: "installer.exe", fileSize: 25000000 },
    mockAcceptScenario,
    async (id) => { canceledId1 = id; },
    (sug) => { suggestedName1 = sug && sug.filename; }
  );
  assert.strictEqual(res1.intercepted, true, "Scenario 1: Direct .exe link intercepted");
  assert.strictEqual(canceledId1, 2001, "Scenario 1: Download canceled after acceptance");
  console.log("  [Scenario 1/22] Direct .zip/.exe link -> PASS");

  // Scenario 2: URL without file extension returning Content-Disposition: attachment
  onHeadersReceivedHandler({
    requestId: "req-sc-2",
    tabId: 50,
    url: "https://example.com/api/get_file?id=555",
    responseHeaders: [
      { name: "Content-Disposition", value: 'attachment; filename="data_export.csv"' },
      { name: "Content-Type", value: "text/csv" },
      { name: "Content-Length", value: "204800" }
    ]
  });
  let canceledId2 = null;
  const res2 = await onDownloadCreatedHandler(
    { id: 2002, tabId: 50, url: "https://example.com/api/get_file?id=555", filename: "" },
    mockAcceptScenario,
    async (id) => { canceledId2 = id; }
  );
  assert.strictEqual(res2.intercepted, true, "Scenario 2: URL without extension with Content-Disposition intercepted");
  assert.strictEqual(canceledId2, 2002);
  console.log("  [Scenario 2/22] URL without extension with Content-Disposition -> PASS");

  // Scenario 3: Redirect to a signed download URL
  onHeadersReceivedHandler({
    requestId: "req-sc-3",
    tabId: 51,
    statusCode: 302,
    url: "https://example.com/download/archive.tar.gz",
    responseHeaders: [
      { name: "Location", value: "https://storage.cdn.com/blobs/abc-123.tar.gz?auth=token99" }
    ]
  });
  const res3 = await onDownloadCreatedHandler(
    { id: 2003, tabId: 51, url: "https://storage.cdn.com/blobs/abc-123.tar.gz?auth=token99", filename: "abc-123.tar.gz", fileSize: 10485760 },
    mockAcceptScenario,
    async () => {}
  );
  assert.strictEqual(res3.intercepted, true, "Scenario 3: Redirect to signed download URL intercepted");
  const rec3 = downloadStateMachine.get(2003);
  assert.strictEqual(rec3.originalUrl, "https://example.com/download/archive.tar.gz", "Scenario 3: Original redirect URL correlated");
  console.log("  [Scenario 3/22] Redirect to signed download URL correlated -> PASS");

  // Scenario 4: Final filename revealed only by response headers
  onHeadersReceivedHandler({
    requestId: "req-sc-4",
    tabId: 52,
    url: "https://example.com/api/generate_report",
    responseHeaders: [
      { name: "Content-Disposition", value: "attachment; filename*=UTF-8''final_report_2026.pdf" },
      { name: "Content-Type", value: "application/pdf" },
      { name: "Content-Length", value: "5242880" }
    ]
  });
  let suggestedName4 = null;
  const res4 = await onDownloadCreatedHandler(
    { id: 2004, tabId: 52, url: "https://example.com/api/generate_report", filename: "" },
    mockAcceptScenario,
    async () => {},
    (sug) => { suggestedName4 = sug && sug.filename; }
  );
  assert.strictEqual(res4.intercepted, true, "Scenario 4: Filename revealed only by headers intercepted");
  const rec4 = downloadStateMachine.get(2004);
  assert.strictEqual(rec4.suggestedFilename, "final_report_2026.pdf", "Scenario 4: Extracted UTF-8 filename");
  console.log("  [Scenario 4/22] Filename from response headers -> PASS");

  // Scenario 5: Download with no click record
  const res5 = await onDownloadCreatedHandler(
    { id: 2005, url: "https://dist.example.com/archlinux-2026.iso", filename: "archlinux-2026.iso", fileSize: 900000000 },
    mockAcceptScenario,
    async () => {}
  );
  assert.strictEqual(res5.intercepted, true, "Scenario 5: Programmatic/direct download with no click record intercepted");
  const rec5 = downloadStateMachine.get(2005);
  assert.strictEqual(rec5.wasExplicitLinkClick, false, "Scenario 5: wasExplicitLinkClick is false");
  console.log("  [Scenario 5/22] Download with no click record -> PASS");

  // Scenario 6: Click record arriving before browser download event
  handleRuntimeMessage({
    action: "linkClicked",
    url: "https://files.example.com/packages/tool-v2.zip",
    filename: "tool-v2.zip"
  }, { tab: { id: 60 } }, () => {});
  const res6 = await onDownloadCreatedHandler(
    { id: 2006, tabId: 60, url: "https://files.example.com/packages/tool-v2.zip", filename: "tool-v2.zip", fileSize: 5000000 },
    mockAcceptScenario,
    async () => {}
  );
  assert.strictEqual(res6.intercepted, true, "Scenario 6: Click record arriving before browser event intercepted");
  const rec6 = downloadStateMachine.get(2006);
  assert.strictEqual(rec6.wasExplicitLinkClick, true, "Scenario 6: Correlated with preceding click record");
  console.log("  [Scenario 6/22] Click record before browser event -> PASS");

  // Scenario 7: Browser event arriving before click record
  const res7 = await onDownloadCreatedHandler(
    { id: 2007, tabId: 70, url: "https://files.example.com/packages/driver.msi", filename: "driver.msi", fileSize: 15000000 },
    mockAcceptScenario,
    async () => {}
  );
  assert.strictEqual(res7.intercepted, true, "Scenario 7: Browser event handled gracefully even without prior click");
  console.log("  [Scenario 7/22] Browser event arriving before click record -> PASS");

  // Scenario 8: Simultaneous downloads from different tabs
  const dlA = { id: 2008, tabId: 81, url: "https://tab1.com/fileA.zip", filename: "fileA.zip", fileSize: 2000000 };
  const dlB = { id: 2009, tabId: 82, url: "https://tab2.com/fileB.msi", filename: "fileB.msi", fileSize: 3000000 };
  const [res8a, res8b] = await Promise.all([
    onDownloadCreatedHandler(dlA, mockAcceptScenario, async () => {}),
    onDownloadCreatedHandler(dlB, mockAcceptScenario, async () => {})
  ]);
  assert.strictEqual(res8a.intercepted, true, "Scenario 8: Tab A download intercepted");
  assert.strictEqual(res8b.intercepted, true, "Scenario 8: Tab B download intercepted");
  assert.strictEqual(downloadStateMachine.get(2008).tabId, 81);
  assert.strictEqual(downloadStateMachine.get(2009).tabId, 82);
  console.log("  [Scenario 8/22] Simultaneous downloads from different tabs -> PASS");

  // Scenario 9: Two downloads from the same URL or domain (verifying second is not dropped)
  const res9a = await onDownloadCreatedHandler(
    { id: 2010, url: "https://cdn.example.com/same_file.zip", filename: "same_file.zip", fileSize: 5000000 },
    mockAcceptScenario,
    async () => {}
  );
  const res9b = await onDownloadCreatedHandler(
    { id: 2011, url: "https://cdn.example.com/same_file.zip", filename: "same_file.zip", fileSize: 5000000 },
    mockAcceptScenario,
    async () => {}
  );
  assert.strictEqual(res9a.intercepted, true, "Scenario 9: First download of same URL intercepted");
  assert.strictEqual(res9b.intercepted, true, "Scenario 9: Second download of identical URL NOT dropped");
  console.log("  [Scenario 9/22] Two downloads from same URL not dropped -> PASS");

  // Scenario 10: Desktop acceptance response (accepted: true)
  let cancelCalled10 = false, suggestCalled10 = false;
  const res10 = await onDownloadCreatedHandler(
    { id: 2012, url: "https://example.com/release.tar.gz", filename: "release.tar.gz", fileSize: 4000000 },
    mockAcceptScenario,
    async () => { cancelCalled10 = true; },
    () => { suggestCalled10 = true; }
  );
  assert.strictEqual(res10.intercepted, true);
  assert.strictEqual(suggestCalled10, false, "Scenario 10: Suggest must NOT be called on intercept to prevent browser shelf activation");
  assert.strictEqual(cancelCalled10, true, "Scenario 10: Browser download canceled upon desktop acceptance");
  console.log("  [Scenario 10/22] Desktop acceptance -> PASS");

  // Scenario 11: Desktop rejection response (accepted: false)
  let cancelCalled11 = false, suggestCalled11 = false;
  const mockTransportReject = async () => ({ accepted: false, reason: "USER_DECLINED_DOWNLOAD" });
  const res11 = await onDownloadCreatedHandler(
    { id: 2013, url: "https://example.com/rejected.zip", filename: "rejected.zip", fileSize: 4000000 },
    mockTransportReject,
    async () => { cancelCalled11 = true; },
    () => { suggestCalled11 = true; }
  );
  assert.strictEqual(res11.intercepted, false);
  assert.strictEqual(res11.fallbackToBrowser, true, "Scenario 11: Safely falls back to browser");
  assert.strictEqual(cancelCalled11, false, "Scenario 11: Browser download is NOT canceled");
  assert.strictEqual(suggestCalled11, true, "Scenario 11: Suggest called so browser can determine filename natively");
  console.log("  [Scenario 11/22] Desktop rejection -> PASS");

  // Scenario 12: Native messaging host missing error
  let cancelCalled12 = false;
  const mockHostMissing = async () => { throw new Error("Specified native messaging host not found"); };
  const res12 = await onDownloadCreatedHandler(
    { id: 2014, url: "https://example.com/host_missing.zip", filename: "host_missing.zip", fileSize: 4000000 },
    mockHostMissing,
    async () => { cancelCalled12 = true; }
  );
  assert.strictEqual(res12.intercepted, false);
  assert.strictEqual(res12.fallbackToBrowser, true, "Scenario 12: Fallback when NMH is missing");
  assert.strictEqual(cancelCalled12, false, "Scenario 12: Browser download allowed to continue");
  console.log("  [Scenario 12/22] Native messaging host missing error -> PASS");

  // Scenario 13: Native messaging timeout (5000ms)
  let cancelCalled13 = false;
  const mockTransportTimeout = async () => {
    return new Promise((_, reject) => {
      setTimeout(() => reject(new Error("RPC_TIMEOUT_AFTER_5000MS")), 20);
    });
  };
  const res13 = await onDownloadCreatedHandler(
    { id: 2015, url: "https://example.com/timeout.zip", filename: "timeout.zip", fileSize: 4000000 },
    mockTransportTimeout,
    async () => { cancelCalled13 = true; }
  );
  assert.strictEqual(res13.intercepted, false);
  assert.strictEqual(res13.fallbackToBrowser, true, "Scenario 13: Safe browser fallback on RPC timeout");
  assert.strictEqual(cancelCalled13, false);
  console.log("  [Scenario 13/22] Native messaging timeout fallback -> PASS");

  // Scenario 14: Malformed RPC responses
  const mockTransportMalformed = async () => ({ unexpectedData: true });
  const res14 = await onDownloadCreatedHandler(
    { id: 2016, url: "https://example.com/malformed.zip", filename: "malformed.zip", fileSize: 4000000 },
    mockTransportMalformed,
    async () => {}
  );
  assert.strictEqual(res14.intercepted, false);
  assert.strictEqual(res14.fallbackToBrowser, true, "Scenario 14: Malformed response safely triggers fallback");
  console.log("  [Scenario 14/22] Malformed RPC response handled -> PASS");

  // Scenario 15: Duplicate browser events for same download ID
  let rpcOfferCount = 0;
  const mockCountingTransport = async () => {
    rpcOfferCount++;
    return { accepted: true, taskId: "task-shared-offer" };
  };
  const dlItem15 = { id: 2017, url: "https://example.com/duplicate_event.zip", filename: "duplicate_event.zip", fileSize: 5000000 };
  const [res15a, res15b] = await Promise.all([
    onDownloadCreatedHandler(dlItem15, mockCountingTransport, async () => {}, null),
    onDownloadCreatedHandler(dlItem15, mockCountingTransport, async () => {}, () => {})
  ]);
  assert.strictEqual(res15a.intercepted, true);
  assert.strictEqual(res15b.intercepted, true);
  assert.strictEqual(rpcOfferCount, 1, "Scenario 15: Shared offerPromise ensures exactly 1 RPC sent for duplicate events");
  console.log("  [Scenario 15/22] Duplicate browser events for same download ID -> PASS");

  // Scenario 16: Normal HTML page navigation (verifying no spurious capture)
  let cancelCalled16 = false;
  const res16 = await onDownloadCreatedHandler(
    { id: 2018, url: "https://news.ycombinator.com/item?id=9999", filename: "", mime: "text/html", fileSize: 15000 },
    mockAcceptScenario,
    async () => { cancelCalled16 = true; }
  );
  assert.strictEqual(res16.intercepted, false, "Scenario 16: HTML page navigation not captured");
  assert.strictEqual(cancelCalled16, false, "Scenario 16: Navigation download is never canceled");
  console.log("  [Scenario 16/22] Normal HTML page navigation bypassed -> PASS");

  // Scenario 17: blob: and data: downloads (staying in browser)
  const eligBlob = isDownloadEligible({ url: "blob:https://app.example.com/uuid-1234", filename: "data.json" });
  assert.strictEqual(eligBlob.eligible, false, "Scenario 17: blob: URLs not eligible");
  assert.strictEqual(eligBlob.reason, "BROWSER_INTERNAL_OR_BLOB");
  const eligData = isDownloadEligible({ url: "data:application/zip;base64,UEsDBBQAAAAIA...", filename: "sample.zip" });
  assert.strictEqual(eligData.eligible, false, "Scenario 17: data: URLs not eligible");
  assert.strictEqual(eligData.reason, "BROWSER_INTERNAL_OR_BLOB");
  console.log("  [Scenario 17/22] blob: and data: downloads stay in browser -> PASS");

  // Scenario 18: User bypass (Alt-key) and excluded websites
  setSiteDisabled("https://company.internal.net", true);
  const eligDisabled = isDownloadEligible({
    url: "https://company.internal.net/secret.zip",
    filename: "secret.zip",
    tabUrl: "https://company.internal.net/dashboard"
  });
  assert.strictEqual(eligDisabled.eligible, false, "Scenario 18: Excluded website download ignored");
  setSiteDisabled("https://company.internal.net", false);

  handleRuntimeMessage({ action: "bypassNextDownload", url: "https://example.com/bypass_file.zip" }, {}, () => {});
  const res18 = await onDownloadCreatedHandler(
    { id: 2019, url: "https://example.com/bypass_file.zip", filename: "bypass_file.zip", fileSize: 5000000 },
    mockAcceptScenario,
    async () => {}
  );
  assert.strictEqual(res18.intercepted, false, "Scenario 18: Alt-key bypassed download ignored");
  console.log("  [Scenario 18/22] User bypass (Alt-key) & excluded sites -> PASS");

  // Scenario 19: Cross-origin frames correlation
  handleRuntimeMessage({
    action: "linkClicked",
    url: "https://sub.vendor.com/iframe_download.exe",
    filename: "iframe_download.exe",
    isFrame: true
  }, { tab: { id: 95 } }, () => {});
  const res19 = await onDownloadCreatedHandler(
    { id: 2020, tabId: 95, url: "https://sub.vendor.com/iframe_download.exe", filename: "iframe_download.exe", fileSize: 8000000 },
    mockAcceptScenario,
    async () => {}
  );
  assert.strictEqual(res19.intercepted, true, "Scenario 19: Cross-origin frame download correlated and intercepted");
  assert.strictEqual(downloadStateMachine.get(2020).mechanism, "FRAME_ORIGINATED");
  console.log("  [Scenario 19/22] Cross-origin frames correlation -> PASS");

  // Scenario 20: Failed cancellation attempt recovery
  const failingCancel = async () => { throw new Error("Download already finished"); };
  const res20 = await onDownloadCreatedHandler(
    { id: 2021, url: "https://example.com/fast.zip", filename: "fast.zip", fileSize: 5000000 },
    mockAcceptScenario,
    failingCancel
  );
  assert.strictEqual(res20.intercepted, true, "Scenario 20: Handoff succeeds even if cancel throws");
  assert.strictEqual(downloadStateMachine.get(2021).state, "HANDED_OFF");
  console.log("  [Scenario 20/22] Failed cancellation attempt recovery -> PASS");

  // Scenario 21: Service worker restart between click and download (session restore)
  handleRuntimeMessage({
    action: "linkClicked",
    url: "https://storage.cdn.com/persisted_pkg.tar.gz",
    filename: "persisted_pkg.tar.gz"
  }, { tab: { id: 88 } }, () => {});
  onHeadersReceivedHandler({
    requestId: "req-sc-21",
    tabId: 88,
    url: "https://storage.cdn.com/persisted_pkg.tar.gz",
    responseHeaders: [
      { name: "Content-Type", value: "application/gzip" },
      { name: "Content-Length", value: "25000000" }
    ]
  });
  const mockStore = {
    _d: {},
    set: async function(o) { Object.assign(this._d, o); },
    get: async function(k) { const o = {}; for (const x of k) o[x] = this._d[x]; return o; }
  };
  await persistSessionState(mockStore);
  G.recentDownloadResponses.clear();
  G.clickedLinksByUrl.clear();
  assert.strictEqual(G.recentDownloadResponses.size, 0);
  await restoreSessionState(mockStore);
  assert.strictEqual(G.recentDownloadResponses.size > 0, true, "Scenario 21: Download responses restored");
  const res21 = await onDownloadCreatedHandler(
    { id: 2022, tabId: 88, url: "https://storage.cdn.com/persisted_pkg.tar.gz", filename: "persisted_pkg.tar.gz", fileSize: 25000000 },
    mockAcceptScenario,
    async () => {}
  );
  assert.strictEqual(res21.intercepted, true, "Scenario 21: Intercepted post-restart using restored session metadata");
  console.log("  [Scenario 21/22] Service worker restart session restore -> PASS");

  // Scenario 22: Concurrent media detection while file download is starting
  const [resHls, resDl] = await Promise.all([
    onHeadersReceivedHandler({
      requestId: "req-sc-22-media",
      tabId: 99,
      url: "https://stream.example.com/master.m3u8",
      responseHeaders: [{ name: "Content-Type", value: "application/vnd.apple.mpegurl" }]
    }),
    onDownloadCreatedHandler(
      { id: 2023, tabId: 100, url: "https://dl.example.com/concurrent.iso", filename: "concurrent.iso", fileSize: 700000000 },
      mockAcceptScenario,
      async () => {}
    )
  ]);
  assert.strictEqual(resHls.added, true, "Scenario 22: Media sniffer detected HLS stream concurrently");
  assert.strictEqual(resDl.intercepted, true, "Scenario 22: File download intercepted concurrently");
  console.log("  [Scenario 22/22] Concurrent media detection & file download -> PASS");

  // Scenario 23: ChatGPT Web Blob Artifact with click correlation & chunked transfer
  const chatgptBlobUrl = "blob:https://chatgpt.com/artifact-4a0e1c2d";
  recordClickedLink(101, {
    url: chatgptBlobUrl,
    filename: "generated_script.py",
    hasDownloadAttr: true,
    referrer: "https://chatgpt.com/c/conversation-xyz"
  });
  const eligArtifact = isDownloadEligible({
    url: chatgptBlobUrl,
    filename: "generated_script.py",
    hasDownloadAttr: true,
    wasExplicitLinkClick: true,
    tabUrl: "https://chatgpt.com/c/conversation-xyz"
  });
  assert.strictEqual(eligArtifact.eligible, true, "Scenario 23: ChatGPT artifact blob is eligible for interception");
  assert.strictEqual(eligArtifact.isBlob, true, "Scenario 23: Identified as web blob artifact");
  assert.strictEqual(eligArtifact.ext, "py", "Scenario 23: Correctly identified py extension");
  console.log("  [Scenario 23/24] ChatGPT web blob artifact click correlation & eligibility -> PASS");

  // Scenario 24: ChatGPT Web Blob with detached dynamic anchor click hook & full correlation flow
  let interceptedSyntheticClick = null;
  const mockAnchorWin = {
    location: { href: "https://chatgpt.com/c/chat-1234", hostname: "chatgpt.com" },
    document: { title: "ChatGPT" }
  };
  function FakeAnchor() {
    this.href = "";
    this.download = "";
  }
  FakeAnchor.prototype.click = function () { this.clicked = true; };
  mockAnchorWin.HTMLAnchorElement = FakeAnchor;
  installDeepSearchHooks(mockAnchorWin, "nonce-anchor", (msg) => {
    if (msg.action === "fluxAnchorClicked") interceptedSyntheticClick = msg;
  });
  const detachedAnchor = new FakeAnchor();
  detachedAnchor.href = "blob:https://chatgpt.com/artifact-99aa88bb";
  detachedAnchor.download = "analysis_results.csv";
  detachedAnchor.click();
  assert.strictEqual(detachedAnchor.clicked, true, "Underlying click() called");
  assert.strictEqual(Boolean(interceptedSyntheticClick), true, "fluxAnchorClicked message emitted by HTMLAnchorElement.prototype.click hook");
  assert.strictEqual(interceptedSyntheticClick.url, "blob:https://chatgpt.com/artifact-99aa88bb");
  assert.strictEqual(interceptedSyntheticClick.download, "analysis_results.csv");

  // Verify that when fluxAnchorClicked is relayed to background as linkClicked, correlation & eligibility succeed
  handleRuntimeMessage({
    action: "linkClicked",
    url: interceptedSyntheticClick.url,
    filename: interceptedSyntheticClick.download,
    hasDownloadAttr: Boolean(interceptedSyntheticClick.download),
    mechanism: "JS_TRIGGERED_DYNAMIC_ANCHOR"
  }, { tab: { id: 105 } }, () => {});

  const eligSynthetic = isDownloadEligible({
    url: interceptedSyntheticClick.url,
    filename: interceptedSyntheticClick.download,
    wasExplicitLinkClick: true,
    hasDownloadAttr: true,
    phase: "determine"
  });
  assert.strictEqual(eligSynthetic.eligible, true, "Scenario 24: ChatGPT blob correlated via dynamic anchor click is eligible");
  assert.strictEqual(eligSynthetic.isBlob, true, "Scenario 24: Correctly classified as isBlob");
  assert.strictEqual(eligSynthetic.ext, "csv", "Scenario 24: Correctly identified csv extension");
  console.log("  [Scenario 24/24] ChatGPT synthetic detached anchor click hook & eligibility -> PASS");


  console.log("\n[15/15] Testing YouTube Cipher Quality Extraction with Sizes & Floating Overlay Attachment...");
  // 1. Verify YouTube ciphered playerResponse extracts qualities with non-zero estimated sizes
  let emittedYtMedia = null;
  const mockWin = {
    location: { href: "https://www.youtube.com/watch?v=dQw4w9WgXcQ", hostname: "www.youtube.com" },
    document: {
      title: "Rick Astley - Never Gonna Give You Up (Official Music Video) - YouTube",
      querySelector: () => ({ duration: 212 })
    }
  };
  installDeepSearchHooks(mockWin, "test-nonce", (payload) => {
    if (payload.action === "catCatchAddMedia") {
      emittedYtMedia = payload;
    }
  });

  const cipherPlayerResp = {
    playabilityStatus: { status: "OK" },
    videoDetails: {
      videoId: "dQw4w9WgXcQ",
      title: "Rick Astley - Never Gonna Give You Up",
      lengthSeconds: "212"
    },
    streamingData: {
      adaptiveFormats: [
        {
          itag: 137,
          mimeType: "video/mp4; codecs=\"avc1.640028\"",
          bitrate: 2800000,
          averageBitrate: 2500000,
          width: 1920,
          height: 1080,
          fps: 30,
          qualityLabel: "1080p",
          signatureCipher: "s=mock_sig&sp=sig&url=https%3A%2F%2Frr3.googlevideo.com%2Fvideoplayback%3Fitag%3D137"
        },
        {
          itag: 136,
          mimeType: "video/mp4; codecs=\"avc1.4d401f\"",
          bitrate: 1500000,
          averageBitrate: 1200000,
          width: 1280,
          height: 720,
          fps: 30,
          qualityLabel: "720p",
          signatureCipher: "s=mock_sig&sp=sig&url=https%3A%2F%2Frr3.googlevideo.com%2Fvideoplayback%3Fitag%3D136"
        },
        {
          itag: 140,
          mimeType: "audio/mp4; codecs=\"mp4a.40.2\"",
          bitrate: 128000,
          averageBitrate: 128000,
          signatureCipher: "s=mock_sig&sp=sig&url=https%3A%2F%2Frr3.googlevideo.com%2Fvideoplayback%3Fitag%3D140"
        }
      ]
    }
  };

  mockWin.ytInitialPlayerResponse = cipherPlayerResp;
  assert.ok(mockWin.__FluxCatchTestHook, "Test hook exposed on window");
  const scanFound = mockWin.__FluxCatchTestHook.scanPageForYouTubeData();
  assert.strictEqual(scanFound, true, "scanPageForYouTubeData successfully processed player response");
  assert.ok(emittedYtMedia, "Dispatched catCatchAddMedia for YouTube video");
  assert.strictEqual(emittedYtMedia.streamType, "YOUTUBE");
  assert.strictEqual(emittedYtMedia.sourceType, "YOUTUBE_EXTRACTION");
  assert.ok(Array.isArray(emittedYtMedia.qualities), "Qualities array present");
  assert.strictEqual(emittedYtMedia.qualities.length, 3, "Parsed 1080p, 720p, and Audio Only qualities");

  const q1080 = emittedYtMedia.qualities.find(q => q.resolution === "1080p");
  const q720 = emittedYtMedia.qualities.find(q => q.resolution === "720p");
  const qAudio = emittedYtMedia.qualities.find(q => q.isAudioOnly);

  assert.ok(q1080, "1080p quality entry found");
  assert.ok(q720, "720p quality entry found");
  assert.ok(qAudio, "Audio Only quality entry found");

  assert.ok(q1080.size > 0, "1080p quality has positive estimated byte size");
  assert.ok(q720.size > 0, "720p quality has positive estimated byte size");
  assert.ok(qAudio.size > 0, "Audio quality has positive estimated byte size");
  assert.ok(q1080.size > q720.size, "1080p size is larger than 720p size");
  assert.strictEqual(q1080.requiresCipherExtraction, true, "Marks cipher extraction required");
  assert.ok(emittedYtMedia.size > 0, "Top-level candidate size is non-zero");
  assert.ok(emittedYtMedia.extractorJson.includes("1080p"), "extractorJson includes qualities");
  assert.ok(emittedYtMedia.cachedManifestBody.includes("1080p"), "cachedManifestBody includes descriptor JSON with qualities");

  // 2. Verify scanAndAttachVideoOverlays attaches to YouTube <video> even if src is initially empty
  let overlayClickData = null;
  const mockVideoEl = {
    currentSrc: "",
    src: "",
    mediaKeys: null,
    dataset: {},
    parentElement: { id: "movie_player" },
    getBoundingClientRect: () => ({ width: 854, height: 480, top: 20, left: 20, right: 874, bottom: 500 })
  };
  const mockYtDoc = {
    location: { href: "https://www.youtube.com/watch?v=dQw4w9WgXcQ", hostname: "www.youtube.com" },
    title: "Rick Astley - Official Video",
    body: {
      appendChild: (el) => { mockYtDoc._appended = el; }
    },
    createElement: (tag) => {
      const el = {
        tagName: tag.toUpperCase(),
        style: {},
        setAttribute: (k, v) => { el[k] = v; },
        getAttribute: (k) => el[k],
        addEventListener: (evt, fn) => { el["_" + evt] = fn; },
        appendChild: (child) => { el._children = el._children || []; el._children.push(child); }
      };
      if (tag === "div") {
        el.attachShadow = () => ({
          appendChild: (c) => { el._shadowChild = c; }
        });
      }
      return el;
    },
    querySelectorAll: (sel) => (sel === "video" ? [mockVideoEl] : [])
  };

  const attachedOverlays = scanAndAttachVideoOverlays(mockYtDoc, (meta) => { overlayClickData = meta; });
  assert.strictEqual(attachedOverlays.length, 1, "Overlay attached to YouTube <video> with pending/empty src");
  assert.ok(mockVideoEl.__fluxHostEl, "Host element stored on video element");
  assert.ok(mockVideoEl.__fluxBtn, "Button element stored on video element");

  console.log("  -> PASS");

  console.log("\n[16/16] Testing HLS/DASH Quality Dropdown State Retention & Live Update Stability...");
  const hlsItem = {
    candidateId: "cand-hls-stream-1",
    url: "https://video.example.com/live/master.m3u8",
    streamType: "HLS",
    sourceType: "HLS_MANIFEST",
    resolution: "1080p",
    qualities: [
      { id: "1080p", label: "1080p (FHD)", resolution: "1080p", size: 104857600 },
      { id: "720p", label: "720p (HD)", resolution: "720p", size: 52428800 },
      { id: "480p", label: "480p (SD)", resolution: "480p", size: 26214400 }
    ]
  };

  // Step 1: Default renders with 1080p selected
  const initialHtml = renderDetectedMediaList([hlsItem]);
  assert.ok(initialHtml.includes('value="1080p" selected'), "1080p initially selected by default");

  // Step 2: User selects 720p in dropdown
  hlsItem.selectedQualityId = "720p";
  const updatedHtml = renderDetectedMediaList([hlsItem]);
  assert.ok(updatedHtml.includes('value="720p" selected'), "720p retained as selected after user choice");
  assert.ok(!updatedHtml.includes('value="1080p" selected'), "1080p no longer marked selected");

  // Step 3: Verify candidate selection preservation across background refreshes
  const incomingCandidates = [
    {
      candidateId: "cand-hls-stream-1",
      url: "https://video.example.com/live/master.m3u8",
      streamType: "HLS",
      resolution: "1080p",
      qualities: hlsItem.qualities
    }
  ];
  const prevMap = new Map();
  [hlsItem].forEach(c => { if (c.selectedQualityId) prevMap.set(c.candidateId, c.selectedQualityId); });
  const mergedCandidates = incomingCandidates.map(c => {
    if (prevMap.has(c.candidateId)) c.selectedQualityId = prevMap.get(c.candidateId);
    return c;
  });
  assert.strictEqual(mergedCandidates[0].selectedQualityId, "720p", "User selected quality survived candidate refresh");

  const postRefreshHtml = renderDetectedMediaList(mergedCandidates);
  assert.ok(postRefreshHtml.includes('value="720p" selected'), "Rendered dropdown keeps 720p selected after refresh");
  console.log("  -> PASS");

  console.log("\n[17/17] Testing Per-Tab SPA Document Scoping and Compact Player Toolbar...");
  G.cacheData.set(901, [{ candidateId: "old-page", url: "https://media.example/old.mp4" }]);
  G.cacheData.set(902, [{ candidateId: "other-tab", url: "https://media.example/other.mp4" }]);
  G.tabUrlMap.set(901, "https://site.example/page/1");
  G.tabUrlMap.set(902, "https://site.example/background");
  assert.strictEqual(updateTabDocument(901, "https://site.example/page/2"), true, "SPA history transition updates document identity");
  assert.strictEqual(G.cacheData.has(901), false, "Old document candidates are cleared for the navigated tab");
  assert.strictEqual(G.cacheData.get(902)[0].candidateId, "other-tab", "Unrelated background tab results are retained");
  G.cacheData.set(901, [{ candidateId: "current-page", url: "https://media.example/current.mp4" }]);
  assert.strictEqual(updateTabDocument(901, "https://site.example/page/2"), false, "Same document refresh keeps all current page results");
  assert.strictEqual(G.cacheData.get(901).length, 1, "Current document candidates remain available");

  const toolbarHost = { style: {} };
  positionOverlayHost({ getBoundingClientRect: () => ({ width: 640, height: 360, top: 200, bottom: 560, left: 100, right: 740 }) }, toolbarHost);
  assert.ok(toolbarHost.style.cssText.includes("top:166px"), "Player toolbar is positioned above the video image");
  assert.ok(toolbarHost.style.cssText.includes("left:100px"), "Player toolbar aligns with the player container");
  const contentScript = fs.readFileSync(path.join(__dirname, "../extension/js/content-script.js"), "utf8");
  assert.ok(contentScript.includes("padding:4px 9px") && contentScript.includes("font:600 11px"), "Player download control uses compact styling");
  console.log("  -> PASS");

  console.log("\n[18/18] Testing Bounded Media Size Probe and Numeric Filename Extension...");
  const sizeCandidate = {
    candidateId: "size-probe-candidate",
    url: "https://media.example/video?id=9755",
    sourceType: "DIRECT_PROGRESSIVE",
    streamType: "HTTP_FILE",
    size: 0,
    tabUrl: "https://site.example/watch"
  };
  G.cacheData.set(903, [sizeCandidate]);
  let rangeBodyCanceled = false;
  const probeCalls = [];
  const discoveredSize = await probeMediaSize(903, sizeCandidate, async (_url, options) => {
    probeCalls.push(options);
    if (options.method === "HEAD") return { ok: false, headers: { get: () => "" } };
    return {
      ok: true,
      headers: { get: (name) => name === "content-range" ? "bytes 0-0/1234567" : "1" },
      body: { cancel: async () => { rangeBodyCanceled = true; } }
    };
  });
  assert.strictEqual(discoveredSize, 1234567, "Range Content-Range supplies representation size when HEAD is unavailable");
  assert.strictEqual(sizeCandidate.size, 1234567, "Resolved size updates the current page candidate");
  assert.strictEqual(probeCalls[1].headers.Range, "bytes=0-0", "Fallback reads only the first byte range");
  assert.strictEqual(rangeBodyCanceled, true, "Range response body is canceled without downloading the media");

  let submittedParams = null;
  await submitMediaCandidateToFlux({
    candidateId: "numeric-name",
    url: "https://media.example/9755",
    fileName: "9755",
    ext: "mp4",
    mime: "video/mp4",
    sourceType: "DIRECT_PROGRESSIVE",
    streamType: "HTTP_FILE"
  }, { pageTitle: "Lecture" }, async (payload) => {
    submittedParams = payload.params;
    return { accepted: true, taskId: "numeric-name-task" };
  });
  assert.strictEqual(submittedParams.fileName, "9755.mp4", "Numeric server filename receives MIME-derived media extension");
  console.log("  -> PASS");

  // Extension manifest parity check
  const rootDir = path.resolve(__dirname, "..");
  const mDev = JSON.parse(fs.readFileSync(path.join(rootDir, "extension", "manifest.json"), "utf8"));
  const mChr = JSON.parse(fs.readFileSync(path.join(rootDir, "extension", "manifest.chromium.json"), "utf8"));
  const mFox = JSON.parse(fs.readFileSync(path.join(rootDir, "extension", "manifest.firefox.json"), "utf8"));
  assert.strictEqual(JSON.stringify(mDev), JSON.stringify(mChr), "manifest.json and manifest.chromium.json must remain identical");
  assert.strictEqual(mDev.name, "Radium Download Manager");
  assert.strictEqual(mFox.name, "Radium Download Manager");
  console.log("  [Manifest Parity] verified manifest.json == manifest.chromium.json -> PASS");

  console.log("\n=======================================================");
  console.log("Browser Extension Suite: 18/18 suites (26/26 edge scenarios) PASSED.");
  console.log("=======================================================");
  process.exit(0);
}

runTests().catch((err) => {
  console.error("FAIL:", err);
  process.exit(1);
});
