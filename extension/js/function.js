// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from xifangczy/cat-catch (js/function.js) for Radium Download Manager.
// Implements explicit typed resource classification (HLS_MANIFEST, DASH_MANIFEST,
// DIRECT_PROGRESSIVE, YOUTUBE_EXTRACTION, UNSUPPORTED), webpage watch URL filtering,
// Content-Range & Content-Disposition parsing, googlevideo.com adaptive vs. progressive
// classification, per-tab/per-URL click & redirect correlation, and correlation ID logging.

if (typeof globalThis.G === "undefined" && typeof importScripts === "function") {
  try {
    importScripts("init.js");
  } catch (_) {
    try { importScripts("/js/init.js"); } catch (_) {}
  }
}

var initMod = (typeof require !== "undefined") ? require("./init.js") : globalThis;
var G = initMod.G || globalThis.G;
var SourceType = initMod.SourceType || (G && G.SourceType) || {
  HLS_MANIFEST: "HLS_MANIFEST",
  DASH_MANIFEST: "DASH_MANIFEST",
  DIRECT_PROGRESSIVE: "DIRECT_PROGRESSIVE",
  YOUTUBE_EXTRACTION: "YOUTUBE_EXTRACTION",
  UNSUPPORTED: "UNSUPPORTED"
};

function generateCorrelationId(prefix = "corr") {
  return `${prefix}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
}

function logPipelineStage(correlationId, stage, details = {}) {
  const entry = Object.assign({
    correlationId: correlationId || generateCorrelationId("auto"),
    stage,
    timestamp: Date.now()
  }, details);

  if (G && Array.isArray(G.diagnosticLogs)) {
    G.diagnosticLogs.push(entry);
    if (G.diagnosticLogs.length > 500) {
      G.diagnosticLogs.shift();
    }
  }

  try {
    console.log(`[Radium Trace] [${stage}] [${entry.correlationId}]`, details);
  } catch (_) {}

  return entry;
}

function extractHostname(url) {
  try {
    return new URL(url).hostname.toLowerCase();
  } catch (_) {
    return "";
  }
}

function isSiteDisabled(tabUrl) {
  const host = extractHostname(tabUrl);
  if (!host) return false;
  if (G.disabledHosts.has(host)) return true;
  for (const pattern of G.disabledHosts) {
    if (pattern.startsWith("*.") && host.endsWith(pattern.slice(1))) {
      return true;
    }
  }
  return false;
}

function setSiteDisabled(tabUrl, disabled) {
  const host = extractHostname(tabUrl);
  if (!host) return false;
  if (disabled) {
    G.disabledHosts.add(host);
  } else {
    G.disabledHosts.delete(host);
  }
  return true;
}

function extractFilenameFromUrlQuery(rawUrl) {
  if (!rawUrl || typeof rawUrl !== "string" || !rawUrl.includes("?")) return "";
  try {
    const u = new URL(rawUrl, "https://example.invalid");
    for (const [key, val] of u.searchParams.entries()) {
      const kLower = key.toLowerCase();
      if (kLower === "rscd" || kLower === "response-content-disposition") {
        const fromCd = extractFilenameFromContentDisposition(val);
        if (fromCd) return fromCd;
      }
      if (kLower === "filename" || kLower === "file" || kLower === "fn") {
        if (val && val.includes(".")) return sanitizeFilename(val, "");
      }
    }
  } catch (_) {}
  return "";
}

function extractUrlExtension(url) {
  if (!url || typeof url !== "string") return "";
  try {
    const u = new URL(url, "https://example.invalid");
    const pathname = u.pathname || "";
    const lastDot = pathname.lastIndexOf(".");
    const lastSlash = pathname.lastIndexOf("/");
    if (lastDot > lastSlash && lastDot !== -1) {
      return pathname.slice(lastDot + 1).toLowerCase();
    }
    // Also check query parameter if pathname has no extension (e.g. files.oaiusercontent.com, S3 presigned URLs)
    for (const [key, val] of u.searchParams.entries()) {
      const kLower = key.toLowerCase();
      if (kLower === "rscd" || kLower === "response-content-disposition" || kLower === "filename" || kLower === "file") {
        const fn = extractFilenameFromContentDisposition(val) || val;
        const fnDot = fn.lastIndexOf(".");
        if (fnDot !== -1) {
          const fnExt = fn.slice(fnDot + 1).toLowerCase().replace(/[^a-z0-9]/g, "");
          if (fnExt) return fnExt;
        }
      }
    }
  } catch (_) {
    const clean = url.split("?")[0].split("#")[0];
    const lastDot = clean.lastIndexOf(".");
    const lastSlash = Math.max(clean.lastIndexOf("/"), clean.lastIndexOf("\\"));
    if (lastDot > lastSlash && lastDot !== -1) {
      return clean.slice(lastDot + 1).toLowerCase();
    }
  }
  return "";
}

function extractFilenameFromContentDisposition(cdHeader) {
  if (!cdHeader || typeof cdHeader !== "string") return "";
  const starMatch = cdHeader.match(/filename\*\s*=\s*(?:UTF-8''|utf-8'')?([^;\r\n]+)/i);
  if (starMatch && starMatch[1]) {
    let raw = starMatch[1].trim().replace(/^["']|["']$/g, "");
    try { return decodeURIComponent(raw); } catch (_) { return raw; }
  }
  const fnMatch = cdHeader.match(/filename\s*=\s*(?:"([^"]+)"|([^;\r\n]+))/i);
  if (fnMatch) {
    let raw = (fnMatch[1] || fnMatch[2] || "").trim().replace(/^["']|["']$/g, "");
    try { return decodeURIComponent(raw); } catch (_) { return raw; }
  }
  return "";
}

function isAttachmentContentDisposition(cdHeader) {
  if (!cdHeader || typeof cdHeader !== "string") return false;
  return /^\s*attachment\b/i.test(cdHeader) || /;\s*filename\*?=/i.test(cdHeader);
}

function sanitizeFilename(filename, fallback = "download") {
  if (!filename || typeof filename !== "string") return fallback;
  let clean = filename.trim();
  try { clean = decodeURIComponent(clean); } catch (_) {}
  // Strip control chars (0x00 - 0x1F, 0x7F)
  clean = clean.replace(/[\x00-\x1F\x7F]/g, "");
  // Strip Windows invalid filename characters: \ / : * ? " < > |
  clean = clean.replace(/[\\/:*?"<>|]/g, "_");
  // Strip path traversal sequences
  clean = clean.replace(/\.{2,}/g, ".");
  // Strip leading/trailing dots and spaces
  clean = clean.replace(/^[.\s]+|[.\s]+$/g, "");
  // Check reserved Windows DOS device names: CON, PRN, AUX, NUL, COM1-9, LPT1-9
  const baseName = clean.split(".")[0].toUpperCase();
  const reserved = /^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$/i;
  if (reserved.test(baseName)) {
    clean = "_" + clean;
  }
  return clean || fallback;
}

function isDownloadEligible(params = {}) {
  const {
    url = "",
    finalUrl = "",
    filename = "",
    mime = "",
    fileSize = 0,
    isAttachment = false,
    wasExplicitLinkClick = false,
    hasDownloadAttr = false,
    tabUrl = "",
    phase = "determine"
  } = params;

  const targetUrl = finalUrl || url;
  const isWebBlob = targetUrl.startsWith("blob:http://") || targetUrl.startsWith("blob:https://");
  if (!targetUrl) return { eligible: false, reason: "EMPTY_URL" };

  const isAiWebBlob = isWebBlob && (
    targetUrl.includes("chatgpt.com") ||
    targetUrl.includes("oaistatic.com") ||
    targetUrl.includes("oaiusercontent.com") ||
    targetUrl.includes("claude.ai") ||
    targetUrl.includes("anthropic.com") ||
    (tabUrl && (tabUrl.includes("chatgpt.com") || tabUrl.includes("claude.ai") || tabUrl.includes("oaiusercontent.com")))
  );

  // 1. Exclude browser extension/internal schemes
  if (targetUrl.startsWith("chrome:") || targetUrl.startsWith("chrome-extension:") ||
      targetUrl.startsWith("brave:") || targetUrl.startsWith("about:") || targetUrl.startsWith("javascript:")) {
    return { eligible: false, reason: "BROWSER_INTERNAL_PAGE" };
  }

  // 2. Exclude disabled/blocked hostnames
  if (isSiteDisabled(tabUrl || targetUrl)) {
    return { eligible: false, reason: "SITE_EXCLUDED" };
  }

  const queryFilename = extractFilenameFromUrlQuery(targetUrl);
  const cleanMime = (mime || "").split(";")[0].trim().toLowerCase();
  const cleanFilename = sanitizeFilename(filename || queryFilename, "");
  const ext = (extractUrlExtension(cleanFilename) || extractUrlExtension(targetUrl) || extractUrlExtension(url)).toLowerCase();
  const webCodeExts = new Set(["html", "htm", "xhtml", "css", "js", "mjs", "jsx", "ts", "tsx"]);
  const webDocExts = new Set(["html", "htm", "xhtml"]);
  const hasDownloadExt = Boolean(ext && (!webDocExts.has(ext) || hasDownloadAttr || isAttachment));

  // Handle blob: URLs (web-origin blobs, worker-generated blobs, canvas/pdf exports)
  if (targetUrl.startsWith("blob:")) {
    if (isAiWebBlob || ((isWebBlob || wasExplicitLinkClick || hasDownloadAttr) && (wasExplicitLinkClick || hasDownloadAttr || isAiWebBlob))) {
      return { eligible: true, reason: "WEB_BLOB_ARTIFACT", ext, mime: cleanMime, filename: cleanFilename, isBlob: true };
    }
    return { eligible: false, reason: "BROWSER_INTERNAL_OR_BLOB" };
  }

  // Handle data: URLs (RFC 2397 base64 or encoded payloads, e.g. generated text/csv/pdf/images)
  if (targetUrl.startsWith("data:")) {
    if (wasExplicitLinkClick || hasDownloadAttr) {
      return { eligible: true, reason: "DATA_URL_DOWNLOAD", ext, mime: cleanMime, filename: cleanFilename, isData: true };
    }
    return { eligible: false, reason: "BROWSER_INTERNAL_OR_BLOB" };
  }

  // 3. Exclude webpage watch URLs only when the request is an ordinary HTML page navigation
  // (Never disqualify an explicit attachment, a downloadable file extension, or download attribute)
  if (!isAttachment && !hasDownloadAttr && !hasDownloadExt &&
      (cleanMime === "" || cleanMime === "text/html" || cleanMime === "application/xhtml+xml") &&
      isWebpageWatchUrl(targetUrl)) {
    return { eligible: false, reason: "WEBPAGE_URL" };
  }

  // Exclude plain HTML page navigations unless explicitly marked as attachment, download attribute, or recognized file
  if ((cleanMime === "text/html" || cleanMime === "application/xhtml+xml") && !isAttachment && !hasDownloadAttr && !hasDownloadExt) {
    return { eligible: false, reason: "HTML_PAGE_NAVIGATION" };
  }

  // 4. Content-Disposition: attachment explicitly declares intent to download
  if (isAttachment) {
    return { eligible: true, reason: "CONTENT_DISPOSITION_ATTACHMENT", ext, mime: cleanMime, filename: cleanFilename };
  }

  // 5. Recognized file extension in InterceptFileExts
  if (ext && G.InterceptFileExts && G.InterceptFileExts.has(ext)) {
    return { eligible: true, reason: "MATCHED_FILE_EXTENSION", ext, mime: cleanMime, filename: cleanFilename };
  }

  // 6. Recognized MIME type in InterceptMimeTypes
  if (cleanMime && G.InterceptMimeTypes && G.InterceptMimeTypes.has(cleanMime)) {
    return { eligible: true, reason: "MATCHED_MIME_TYPE", ext, mime: cleanMime, filename: cleanFilename };
  }

  // 7. General media MIME types
  if (cleanMime.startsWith("video/") || cleanMime.startsWith("audio/")) {
    return { eligible: true, reason: "MEDIA_MIME_TYPE", ext, mime: cleanMime, filename: cleanFilename };
  }

  // 8. Explicit user link click (with download attribute or user-clicked anchor)
  if (wasExplicitLinkClick || hasDownloadAttr) {
    if (fileSize > 0 || ext || (cleanMime && cleanMime !== "text/html") || hasDownloadAttr) {
      return { eligible: true, reason: "EXPLICIT_LINK_CLICK", ext, mime: cleanMime, filename: cleanFilename };
    }
  }

  // 9. Non-web-code file extensions on the determined filename or URL
  if (ext && !webCodeExts.has(ext)) {
    return { eligible: true, reason: "VALID_DOWNLOAD_EXTENSION", ext, mime: cleanMime, filename: cleanFilename };
  }

  // 10. Size threshold fallback (>= 1 MiB and not HTML/CSS/JS)
  const sizeNum = Number(fileSize) || 0;
  if (sizeNum >= G.minFileSizeBytes &&
      cleanMime !== "text/html" && cleanMime !== "text/css" && cleanMime !== "application/javascript") {
    return { eligible: true, reason: "FILE_SIZE_THRESHOLD", ext, mime: cleanMime, filename: cleanFilename };
  }

  // 11. Authoritative fallback for browser-confirmed downloads (onDeterminingFilename)
  // When Chromium/Brave fires onDeterminingFilename for an HTTP/HTTPS resource,
  // the browser has already verified it is a downloadable file, not a page navigation.
  if (phase === "determine" &&
      cleanMime !== "text/html" && cleanMime !== "application/xhtml+xml" &&
      cleanMime !== "text/css" && cleanMime !== "application/javascript") {
    return { eligible: true, reason: "BROWSER_DETERMINED_DOWNLOAD", ext, mime: cleanMime, filename: cleanFilename };
  }

  return { eligible: false, reason: "BELOW_SIZE_OR_EXT_FILTER" };
}

function isYouTubeWatchOrShortsUrl(url) {
  if (!url || typeof url !== "string") return false;
  try {
    const u = new URL(url);
    const host = u.hostname.toLowerCase();
    const path = u.pathname.toLowerCase();
    if ((host === "youtube.com" || host.endsWith(".youtube.com")) &&
        (path === "/watch" || path.startsWith("/watch/") || path.startsWith("/shorts/") || path.startsWith("/embed/") || path.startsWith("/live/"))) {
      if (path === "/watch" && !u.searchParams.get("v")) return false;
      return true;
    }
    if (host === "youtu.be" && path.length > 1) return true;
  } catch (_) {}
  return false;
}

function extractYouTubeVideoId(url) {
  if (!url || typeof url !== "string") return "";
  try {
    const u = new URL(url);
    const host = u.hostname.toLowerCase();
    const path = u.pathname;
    if (host === "youtu.be" && path.length > 1) {
      return path.slice(1).split("/")[0];
    }
    if (host === "youtube.com" || host.endsWith(".youtube.com")) {
      const v = u.searchParams.get("v");
      if (v) return v;
      const m = path.match(/^\/(?:shorts|embed|live|watch)\/([^/?#]+)/i);
      if (m && m[1]) return m[1];
    }
  } catch (_) {}
  return "";
}

function isWebpageWatchUrl(url) {
  if (!url || typeof url !== "string") return false;
  try {
    const u = new URL(url);
    const host = u.hostname.toLowerCase();
    const path = u.pathname.toLowerCase();

    // YouTube watch pages and shorts
    if (host.includes("youtube.com") && (path.includes("/watch") || path.includes("/shorts/") || path.includes("/embed/") || path.includes("/live/"))) return true;
    if (path === "/watch" && u.searchParams.get("v")) return true;
    if (host === "youtu.be") return true;

    // Vimeo watch pages (e.g. vimeo.com/12345678)
    if (host.includes("vimeo.com") && /^\/\d+$/.test(path)) return true;

    // Twitch stream/video pages
    if (host.includes("twitch.tv") && (path.includes("/videos/") || path.split("/").filter(Boolean).length === 1)) {
      if (!path.endsWith(".m3u8") && !path.endsWith(".mpd") && !path.endsWith(".mp4")) return true;
    }

    // Generic HTML pages (Note: never include server-side endpoints like .php, .asp, .aspx which commonly serve downloads)
    if (path.endsWith(".html") || path.endsWith(".htm")) {
      return true;
    }
  } catch (_) {}
  return false;
}

function isValidHlsManifestText(text) {
  if (!text || typeof text !== "string") return false;
  const trimmed = text.trimStart();
  if (trimmed.startsWith("{") || trimmed.startsWith("[")) return false;
  return trimmed.startsWith("#EXTM3U") ||
         trimmed.includes("#EXT-X-STREAM-INF") ||
         trimmed.includes("#EXT-X-TARGETDURATION");
}

function isValidDashManifestText(text) {
  if (!text || typeof text !== "string") return false;
  const trimmed = text.trimStart();
  if (trimmed.startsWith("{") || trimmed.startsWith("[")) return false;
  return (trimmed.startsWith("<?xml") && trimmed.includes("<MPD")) ||
         trimmed.startsWith("<MPD") ||
         trimmed.includes("<MPD ");
}

// Explicit 5-category media source classification (Defect B fix)
function classifyMediaSourceType(candidateOrUrl, mime = "", cachedBody = "") {
  const isObj = candidateOrUrl && typeof candidateOrUrl === "object";
  const url = isObj ? (candidateOrUrl.url || candidateOrUrl.manifestUrl || "") : String(candidateOrUrl || "");
  const streamType = isObj ? (candidateOrUrl.streamType || "") : "";
  const effectiveMime = (isObj ? (candidateOrUrl.mime || mime) : mime || "").toLowerCase();
  const body = isObj ? (candidateOrUrl.cachedManifestBody || cachedBody || "") : (cachedBody || "");

  if (isObj && candidateOrUrl.isDrm) {
    return SourceType.UNSUPPORTED;
  }

  // 1. Check actual manifest body if provided
  if (body) {
    if (isValidHlsManifestText(body)) return SourceType.HLS_MANIFEST;
    if (isValidDashManifestText(body)) return SourceType.DASH_MANIFEST;
    // Arbitrary JSON in cachedManifestBody is NOT a valid HLS or DASH manifest
    if (body.trimStart().startsWith("{")) {
      if (streamType === "YOUTUBE" || streamType === "SITE_EXTRACTION" || isYouTubeWatchOrShortsUrl(url)) {
        return SourceType.YOUTUBE_EXTRACTION;
      }
      return SourceType.UNSUPPORTED;
    }
  }

  // 2. Check explicit YouTube / site extraction flags
  if (streamType === "YOUTUBE" ||
      streamType === "SITE_EXTRACTION" ||
      isYouTubeWatchOrShortsUrl(url) ||
      (isObj && (candidateOrUrl.requiresCipherExtraction || candidateOrUrl.pairedAudioUrl || candidateOrUrl.isAdaptiveTrack))) {
    return SourceType.YOUTUBE_EXTRACTION;
  }

  // 3. Reject unresolved blob: or data: URLs that lack a valid manifest body
  if (!url || url.startsWith("blob:") || url.startsWith("data:") || url.startsWith("javascript:")) {
    return SourceType.UNSUPPORTED;
  }

  const ext = (isObj && candidateOrUrl.ext) ? candidateOrUrl.ext.toLowerCase() : extractUrlExtension(url);

  // 4. Actual HLS Manifests
  if (streamType === "HLS" || ext === "m3u8" || ext === "m3u" ||
      effectiveMime.includes("mpegurl")) {
    if (isYouTubeWatchOrShortsUrl(url)) return SourceType.YOUTUBE_EXTRACTION;
    return SourceType.HLS_MANIFEST;
  }

  // 5. Actual DASH Manifests
  if (streamType === "DASH" || streamType === "MSS" || ext === "mpd" || ext === "ism" ||
      effectiveMime.includes("dash+xml") || effectiveMime.includes("vnd.mpeg.dash.mpd")) {
    if (isYouTubeWatchOrShortsUrl(url)) return SourceType.YOUTUBE_EXTRACTION;
    return SourceType.DASH_MANIFEST;
  }

  // 6. Reject raw HTML watch pages as direct progressive media
  if (isWebpageWatchUrl(url) || effectiveMime === "text/html") {
    return SourceType.UNSUPPORTED;
  }

  // 7. Direct progressive media URLs
  if (streamType === "HTTP_FILE" || streamType === "DIRECT_MEDIA" ||
      G.InterceptFileExts.has(ext) ||
      effectiveMime.startsWith("video/") || effectiveMime.startsWith("audio/")) {
    return SourceType.DIRECT_PROGRESSIVE;
  }

  return SourceType.UNSUPPORTED;
}

function classifyUrlIntent(url) {
  if (!url || typeof url !== "string") return "UNKNOWN";
  if (isWebpageWatchUrl(url)) return "WEBPAGE";

  const ext = extractUrlExtension(url);
  if (ext === "m3u8" || ext === "m3u") return "HLS";
  if (ext === "mpd") return "DASH";
  if (ext === "ism") return "MSS";
  if (G.SegmentExts.has(ext)) return "SEGMENT";
  if (G.InterceptFileExts.has(ext)) return "DIRECT_FILE";
  return "UNKNOWN";
}

function parseResponseHeaderMap(responseHeaders) {
  const map = {};
  if (!Array.isArray(responseHeaders)) return map;
  for (const h of responseHeaders) {
    if (h && h.name) {
      map[h.name.toLowerCase()] = h.value || "";
    }
  }
  return map;
}

function parseTotalSizeFromHeaders(hmap, url) {
  const contentRange = hmap["content-range"] || "";
  if (contentRange) {
    const m = contentRange.match(/bytes\s+\d+-\d+\/(\d+)/i);
    if (m && m[1]) {
      const total = parseInt(m[1], 10);
      if (total > 0) return total;
    }
  }
  try {
    const u = new URL(url);
    const clen = u.searchParams.get("clen");
    if (clen) {
      const n = parseInt(clen, 10);
      if (n > 0) return n;
    }
  } catch (_) {}
  const cl = parseInt(hmap["content-length"] || "0", 10);
  return isNaN(cl) ? 0 : cl;
}

function normalizeMediaUrl(url) {
  if (!url || typeof url !== "string") return url;
  try {
    const u = new URL(url);
    if (u.hostname.toLowerCase().includes("googlevideo.com") || u.pathname.includes("/videoplayback")) {
      // Strip per-chunk MSE range/buffer parameters so signed googlevideo URLs point to the full representation
      const stripParams = ["range", "rn", "rbuf", "ump", "srfvp", "alr", "cpn"];
      for (const p of stripParams) {
        u.searchParams.delete(p);
      }
      return u.toString();
    }
  } catch (_) {}
  return url;
}

function isMediaSegmentRequest(url, ext, responseHeaderMap, tabId) {
  if (G.SegmentExts.has(ext)) return true;
  try {
    const u = new URL(url);
    if (/\/(seg|segment|chunk|frag)[-_0-9]+/i.test(u.pathname) && ext !== "m3u8" && ext !== "mpd") {
      return true;
    }
    // Suppress init segments when an HLS/DASH manifest is active
    if (/\/(init|header)[-_0-9]*\.(mp4|m4s|cmfv|cmfa)$/i.test(u.pathname)) {
      return true;
    }
  } catch (_) {}

  // Suppress small (< 4 MiB) 206 byte-range requests if an HLS/DASH manifest already exists on this tab
  const contentRange = responseHeaderMap["content-range"] || "";
  if (contentRange.startsWith("bytes ")) {
    const tabItems = G.cacheData.get(tabId) || [];
    const hasManifest = tabItems.some(item => item.streamType === "HLS" || item.streamType === "DASH");
    const len = parseInt(responseHeaderMap["content-length"] || "0", 10);
    if (hasManifest && len > 0 && len < 4 * 1048576) {
      return true;
    }
  }
  return false;
}

// Core network resource classifier (findMedia logic)
function classifyNetworkResource(details) {
  if (!details || !details.url) return null;
  const rawUrl = details.url;
  if (rawUrl.startsWith("blob:") || rawUrl.startsWith("data:") || rawUrl.startsWith("chrome-extension:")) {
    return null;
  }

  if (typeof details.statusCode === "number" &&
      details.statusCode !== 200 && details.statusCode !== 206 && details.statusCode !== 304) {
    return null;
  }

  // Reject webpage URLs (e.g. YouTube watch page HTML responses)
  if (isWebpageWatchUrl(rawUrl)) {
    return null;
  }

  const tabUrl = details.initiator || details.documentUrl || (G.tabUrlMap && G.tabUrlMap.get(details.tabId)) || rawUrl;
  if (isSiteDisabled(tabUrl)) return null;

  const hmap = parseResponseHeaderMap(details.responseHeaders);
  const contentType = (hmap["content-type"] || "").split(";")[0].trim().toLowerCase();
  if (contentType === "text/html" || contentType === "text/css" || contentType === "application/javascript") {
    return null;
  }

  const totalSize = parseTotalSizeFromHeaders(hmap, rawUrl);
  const cdFilename = extractFilenameFromContentDisposition(hmap["content-disposition"] || "");
  const url = normalizeMediaUrl(rawUrl);
  let ext = extractUrlExtension(cdFilename) || extractUrlExtension(url);

  if (isMediaSegmentRequest(url, ext, hmap, details.tabId)) {
    return null;
  }

  // Special handling for googlevideo.com / videoplayback streams (IDM FUN_00592d50 pattern + Defect B fix)
  // Do NOT pretend a raw adaptive video-only or UMP googlevideo URL is a complete standalone progressive video.
  try {
    const rawParsedU = new URL(rawUrl);
    const parsedU = new URL(url);
    if (parsedU.hostname.toLowerCase().includes("googlevideo.com") || parsedU.pathname.includes("/videoplayback")) {
      const mimeParam = (parsedU.searchParams.get("mime") || contentType || "").toLowerCase();
      const itag = parsedU.searchParams.get("itag") || "";
      const hasUmp = rawParsedU.searchParams.get("ump") === "1" || rawParsedU.searchParams.has("srfvp") || contentType === "application/vnd.yt-ump";
      const hadRangeParam = rawParsedU.searchParams.has("range") || rawParsedU.searchParams.has("rbuf");
      const isProgressiveItag = G.YouTubeProgressiveItags ? G.YouTubeProgressiveItags.has(itag) : (itag === "18" || itag === "22");

      if (mimeParam.startsWith("video/") || mimeParam.startsWith("audio/")) {
        const isAudioOnly = mimeParam.startsWith("audio/");
        const resolvedExt = mimeParam.includes("webm") ? "webm" : (isAudioOnly ? "m4a" : "mp4");
        const isAdaptiveTrack = !isProgressiveItag || isAudioOnly || hasUmp || hadRangeParam;

        // If this is an adaptive track (video-only, audio-only, or UMP/range chunk), classify as YOUTUBE_EXTRACTION
        // so it is paired/muxed or routed through the YouTube extractor rather than downloaded as a broken/silent file.
        if (isAdaptiveTrack || isYouTubeWatchOrShortsUrl(tabUrl)) {
          const watchUrl = isYouTubeWatchOrShortsUrl(tabUrl) ? tabUrl : url;
          return {
            url: watchUrl,
            videoUrl: !isAudioOnly ? url : "",
            pairedAudioUrl: isAudioOnly ? url : "",
            ext: "mp4",
            streamType: "YOUTUBE",
            sourceType: SourceType.YOUTUBE_EXTRACTION,
            mime: mimeParam,
            size: totalSize,
            itag,
            isAudioOnly,
            isGoogleVideo: true,
            isAdaptiveTrack: true,
            requiresAudioMux: !isAudioOnly,
            requiresCipherExtraction: false,
            videoId: extractYouTubeVideoId(watchUrl),
            matchedBy: "googlevideo_adaptive"
          };
        }

        return {
          url,
          ext: resolvedExt,
          streamType: "YOUTUBE",
          sourceType: isYouTubeWatchOrShortsUrl(tabUrl) ? SourceType.YOUTUBE_EXTRACTION : SourceType.DIRECT_PROGRESSIVE,
          mime: mimeParam,
          size: totalSize,
          itag,
          isAudioOnly: false,
          isGoogleVideo: true,
          isAdaptiveTrack: false,
          matchedBy: "googlevideo_progressive"
        };
      }
    }
  } catch (_) {}

  // 1. Highest priority: Regex match (Cat Catch G.Regex)
  for (const rule of G.Regex) {
    if (!rule.state) continue;
    const re = new RegExp(rule.regex, rule.type.replace("g", ""));
    if (re.test(url)) {
      return {
        url,
        ext: rule.ext,
        streamType: rule.streamType,
        sourceType: rule.sourceType || classifyMediaSourceType(url, contentType),
        mime: contentType || "application/x-mpegurl",
        size: totalSize,
        matchedBy: "regex"
      };
    }
  }

  // 2. MIME Type match (Cat Catch G.Type)
  for (const rule of G.Type) {
    if (!rule.state) continue;
    if (contentType === rule.type) {
      if (rule.size > 0 && totalSize > 0 && totalSize < rule.size) {
        return null;
      }
      const knownMediaExtensions = new Set(["mp4", "m4v", "m4a", "webm", "mkv", "mov", "flv", "avi", "ogv", "mp3", "flac", "ogg", "wav", "aac", "opus", "wma"]);
      return {
        url,
        ext: knownMediaExtensions.has(ext) ? ext : rule.ext,
        streamType: rule.streamType,
        sourceType: rule.sourceType || classifyMediaSourceType(url, contentType),
        mime: contentType,
        size: totalSize,
        fileName: cdFilename || "",
        matchedBy: "mime"
      };
    }
  }

  // 3. Extension match (Cat Catch G.Ext)
  for (const rule of G.Ext) {
    if (!rule.state) continue;
    if (ext === rule.ext) {
      if (rule.size > 0 && totalSize > 0 && totalSize < rule.size) {
        return null;
      }
      return {
        url,
        ext: rule.ext,
        streamType: rule.streamType,
        sourceType: rule.sourceType || classifyMediaSourceType(url, contentType),
        mime: contentType,
        size: totalSize,
        fileName: cdFilename || "",
        matchedBy: "ext"
      };
    }
  }

  return null;
}

function getParentUrlPrefix(url) {
  try {
    const u = new URL(url);
    const idx = u.pathname.lastIndexOf("/");
    return idx >= 0 ? (u.origin + u.pathname.slice(0, idx + 1)) : u.origin;
  } catch (_) {
    return "";
  }
}

function isLikelyMasterManifestUrl(url, cachedBody) {
  if (cachedBody && cachedBody.includes("#EXT-X-STREAM-INF")) return true;
  return /(master|manifest|playlist|index)\.m3u8/i.test(url) && !/(1080|720|480|360|240|audio|sub|chunklist)/i.test(url);
}

function inferResolutionLabelFromUrl(urlStr) {
  if (!urlStr || typeof urlStr !== "string") return "";
  const m = urlStr.match(/(?:^|[_\-/\.])(2160p|1440p|1080p|720p|480p|360p|240p|144p|4k|8k)(?:[_\-/\.?&]|$)/i);
  if (m && m[1]) {
    const tok = m[1].toLowerCase();
    if (tok === "4k") return "2160p";
    if (tok === "8k") return "4320p";
    return tok;
  }
  const dim = urlStr.match(/(?:^|[_\-/\.])(\d{3,4})x(\d{3,4})(?:[_\-/\.?&]|$)/i);
  if (dim && dim[2]) {
    return `${dim[2]}p`;
  }
  return "";
}

function heightFromResolutionLabel(res) {
  if (!res || typeof res !== "string") return 0;
  const pMatch = res.match(/(\d{3,4})p/i);
  if (pMatch) return parseInt(pMatch[1], 10);
  const dimMatch = res.match(/(\d{3,4})x(\d{3,4})/i);
  if (dimMatch) return parseInt(dimMatch[2], 10);
  if (/4k/i.test(res)) return 2160;
  if (/2k/i.test(res)) return 1440;
  if (/hd/i.test(res)) return 720;
  return 0;
}

function parseHlsMasterQualities(manifestText, baseUrl = "") {
  if (!isValidHlsManifestText(manifestText)) return [];
  const lines = manifestText.split(/\r?\n/);
  const qualities = [];
  for (let i = 0; i < lines.length; ++i) {
    const line = lines[i].trim();
    if (!line.startsWith("#EXT-X-STREAM-INF:")) continue;
    const attrs = line.slice("#EXT-X-STREAM-INF:".length);
    const bwMatch = attrs.match(/BANDWIDTH=(\d+)/i);
    const resMatch = attrs.match(/RESOLUTION=(\d+)x(\d+)/i);
    const fpsMatch = attrs.match(/FRAME-RATE=([0-9.]+)/i);
    const nameMatch = attrs.match(/NAME="([^"]+)"/i);
    const codecsMatch = attrs.match(/CODECS="([^"]+)"/i);

    let uriLine = "";
    for (let j = i + 1; j < lines.length; ++j) {
      const cand = lines[j].trim();
      if (!cand) continue;
      if (cand.startsWith("#")) continue;
      uriLine = cand;
      i = j;
      break;
    }
    if (!uriLine) continue;

    let resolvedUri = uriLine;
    if (baseUrl) {
      try { resolvedUri = new URL(uriLine, baseUrl).href; } catch (_) {}
    }

    const width = resMatch ? parseInt(resMatch[1], 10) : 0;
    const height = resMatch ? parseInt(resMatch[2], 10) : 0;
    const bandwidth = bwMatch ? parseInt(bwMatch[1], 10) : 0;
    const fps = fpsMatch ? Math.round(parseFloat(fpsMatch[1])) : 0;
    const inferredRes = height > 0
      ? `${height}p${fps > 30 ? fps : ""}`
      : (nameMatch ? nameMatch[1] : (inferResolutionLabelFromUrl(uriLine) || "Auto"));
    const bwLabel = bandwidth >= 1000000
      ? `${(bandwidth / 1000000).toFixed(1)} Mbps`
      : (bandwidth >= 1000 ? `${Math.round(bandwidth / 1000)} Kbps` : "");

    qualities.push({
      id: height > 0 ? `${height}p` : (inferredRes || `hls-${qualities.length}`),
      label: `${inferredRes}${bwLabel ? " (" + bwLabel + ")" : ""}`,
      resolution: height > 0 ? `${height}p` : inferredRes,
      width,
      height: height || heightFromResolutionLabel(inferredRes),
      bandwidth,
      fps,
      codecs: codecsMatch ? codecsMatch[1] : "",
      ext: "mp4",
      variantUrl: resolvedUri,
      videoFilter: height > 0 ? `${height}p` : (width && height ? `${width}x${height}` : inferredRes)
    });
  }
  return mergeQualityLists([], qualities);
}

function parseDashManifestQualities(mpdText, baseUrl = "") {
  if (!isValidDashManifestText(mpdText)) return [];
  const qualities = [];
  const repRegex = /<Representation\b([^>]*)>/gi;
  let match;
  while ((match = repRegex.exec(mpdText)) !== null) {
    const attrs = match[1] || "";
    const idMatch = attrs.match(/\bid="([^"]+)"/i);
    const wMatch = attrs.match(/\bwidth="(\d+)"/i);
    const hMatch = attrs.match(/\bheight="(\d+)"/i);
    const bwMatch = attrs.match(/\bbandwidth="(\d+)"/i);
    const codecsMatch = attrs.match(/\bcodecs="([^"]+)"/i);
    const mimeMatch = attrs.match(/\bmimeType="([^"]+)"/i);

    const width = wMatch ? parseInt(wMatch[1], 10) : 0;
    const height = hMatch ? parseInt(hMatch[1], 10) : 0;
    const bandwidth = bwMatch ? parseInt(bwMatch[1], 10) : 0;
    const codecs = codecsMatch ? codecsMatch[1] : "";
    const mime = mimeMatch ? mimeMatch[1].toLowerCase() : "";
    const repId = idMatch ? idMatch[1] : `rep-${qualities.length}`;

    const isAudio = mime.startsWith("audio/") || (!height && (codecs.includes("mp4a") || codecs.includes("opus") || codecs.includes("vorbis")));
    if (isAudio) {
      const bwKbps = bandwidth >= 1000 ? `${Math.round(bandwidth / 1000)} Kbps` : "";
      qualities.push({
        id: "audio-only",
        label: `Audio Only${bwKbps ? " (" + bwKbps + ")" : ""}`,
        resolution: "Audio",
        width: 0,
        height: 0,
        bandwidth,
        codecs,
        isAudioOnly: true,
        ext: "m4a",
        videoFilter: "audio"
      });
      continue;
    }

    if (height <= 0 && width <= 0 && bandwidth <= 0) continue;
    const resLabel = height > 0 ? `${height}p` : (repId || "Video");
    const bwLabel = bandwidth >= 1000000
      ? `${(bandwidth / 1000000).toFixed(1)} Mbps`
      : (bandwidth >= 1000 ? `${Math.round(bandwidth / 1000)} Kbps` : "");

    qualities.push({
      id: height > 0 ? `${height}p` : repId,
      label: `${resLabel}${bwLabel ? " (" + bwLabel + ")" : ""}`,
      resolution: resLabel,
      width,
      height,
      bandwidth,
      codecs,
      ext: "mp4",
      variantUrl: baseUrl || "",
      videoFilter: height > 0 ? `${height}p` : repId
    });
  }
  return mergeQualityLists([], qualities);
}

function mergeQualityLists(existingList = [], incomingList = []) {
  const map = new Map();
  const combined = []
    .concat(Array.isArray(existingList) ? existingList : [])
    .concat(Array.isArray(incomingList) ? incomingList : []);

  for (const q of combined) {
    if (!q || typeof q !== "object") continue;
    const res = q.resolution || q.label || q.id || "Auto";
    const key = String(q.id || res).toLowerCase();
    const prev = map.get(key);
    if (!prev) {
      map.set(key, Object.assign({
        id: q.id || res,
        label: q.label || res,
        resolution: res,
        height: q.height !== undefined ? Number(q.height) : heightFromResolutionLabel(res),
        bandwidth: Number(q.bandwidth || 0),
        size: Number(q.size || 0),
        ext: q.ext || "mp4",
        videoFilter: q.videoFilter || res
      }, q));
    } else {
      map.set(key, Object.assign({}, prev, q, {
        videoUrl: q.videoUrl || prev.videoUrl || "",
        pairedAudioUrl: q.pairedAudioUrl || prev.pairedAudioUrl || "",
        variantUrl: q.variantUrl || prev.variantUrl || "",
        size: Math.max(Number(prev.size || 0), Number(q.size || 0)),
        sizeEstimated: (q.sizeEstimated !== undefined) ? Boolean(q.sizeEstimated) : Boolean(prev.sizeEstimated),
        bandwidth: Math.max(Number(prev.bandwidth || 0), Number(q.bandwidth || 0)),
        height: Math.max(Number(prev.height || 0), Number(q.height || 0))
      }));
    }
  }

  const out = Array.from(map.values());
  out.sort((a, b) => {
    if (Boolean(a.isAudioOnly) !== Boolean(b.isAudioOnly)) {
      return a.isAudioOnly ? 1 : -1;
    }
    const hDiff = (Number(b.height) || 0) - (Number(a.height) || 0);
    if (hDiff !== 0) return hDiff;
    const bwDiff = (Number(b.bandwidth) || 0) - (Number(a.bandwidth) || 0);
    if (bwDiff !== 0) return bwDiff;
    return (Number(b.size) || 0) - (Number(a.size) || 0);
  });
  return out;
}

// Adds a candidate to G.cacheData[tabId] with duplicate suppression and Master/Child HLS + YouTube grouping
function addOrDeduplicateMediaCandidate(tabId, candidate) {
  if (!G.cacheData.has(tabId)) {
    G.cacheData.set(tabId, []);
  }
  const list = G.cacheData.get(tabId);

  if (candidate.url) {
    candidate.url = normalizeMediaUrl(candidate.url);
  }

  // Sanitize cachedManifestBody: never keep arbitrary JSON inside cachedManifestBody
  if (candidate.cachedManifestBody && candidate.cachedManifestBody.trimStart().startsWith("{")) {
    if (!candidate.extractorJson) {
      candidate.extractorJson = candidate.cachedManifestBody;
    }
    candidate.cachedManifestBody = "";
  }

  candidate.sourceType = candidate.sourceType || classifyMediaSourceType(candidate, candidate.mime, candidate.cachedManifestBody);
  candidate.correlationId = candidate.correlationId || generateCorrelationId("media");

  if (!candidate.resolution && candidate.url) {
    const inferred = inferResolutionLabelFromUrl(candidate.url);
    if (inferred) candidate.resolution = inferred;
  }

  // Extract available qualities from manifest body, extractorJson, or candidate properties
  let autoQualities = Array.isArray(candidate.qualities) ? candidate.qualities.slice() : [];
  if (candidate.cachedManifestBody) {
    if (candidate.sourceType === SourceType.HLS_MANIFEST || candidate.streamType === "HLS") {
      autoQualities = mergeQualityLists(autoQualities, parseHlsMasterQualities(candidate.cachedManifestBody, candidate.url));
    } else if (candidate.sourceType === SourceType.DASH_MANIFEST || candidate.streamType === "DASH") {
      autoQualities = mergeQualityLists(autoQualities, parseDashManifestQualities(candidate.cachedManifestBody, candidate.url));
    }
  }
  if (candidate.extractorJson) {
    try {
      const exObj = JSON.parse(candidate.extractorJson);
      if (exObj && Array.isArray(exObj.qualities)) {
        autoQualities = mergeQualityLists(autoQualities, exObj.qualities);
      }
    } catch (_) {}
  }
  if (autoQualities.length === 0 && candidate.resolution && candidate.resolution !== "Auto") {
    autoQualities.push({
      id: candidate.resolution.toLowerCase(),
      label: `${candidate.resolution}${candidate.ext ? " (" + String(candidate.ext).toUpperCase() + ")" : ""}`,
      resolution: candidate.resolution,
      height: heightFromResolutionLabel(candidate.resolution),
      size: Number(candidate.size || 0),
      ext: candidate.ext || "mp4",
      videoUrl: candidate.videoUrl || (!isYouTubeWatchOrShortsUrl(candidate.url) ? candidate.url : ""),
      pairedAudioUrl: candidate.pairedAudioUrl || "",
      videoFilter: candidate.resolution
    });
  }
  candidate.qualities = mergeQualityLists([], autoQualities);
  if ((!candidate.resolution || candidate.resolution === "Auto") && candidate.qualities.length > 0) {
    const topVid = candidate.qualities.find(q => !q.isAudioOnly) || candidate.qualities[0];
    if (topVid && topVid.resolution) candidate.resolution = topVid.resolution;
  }

  // If this is an audio-only googlevideo stream and we already have a YouTube/video candidate on this tab,
  // attach the audio stream to the existing video candidate rather than cluttering the primary list
  if (candidate.isGoogleVideo && candidate.isAudioOnly) {
    const audioTargetUrl = candidate.pairedAudioUrl || candidate.url;
    const videoCand = list.find(item => item.streamType === "YOUTUBE" || item.sourceType === SourceType.YOUTUBE_EXTRACTION || (item.isGoogleVideo && !item.isAudioOnly));
    if (videoCand) {
      if (audioTargetUrl && !isYouTubeWatchOrShortsUrl(audioTargetUrl)) {
        videoCand.pairedAudioUrl = audioTargetUrl;
        if (Array.isArray(videoCand.qualities)) {
          for (const q of videoCand.qualities) {
            if (!q.isAudioOnly && !q.pairedAudioUrl) q.pairedAudioUrl = audioTargetUrl;
          }
          videoCand.qualities = mergeQualityLists(videoCand.qualities, [{
            id: "audio-only",
            label: "Audio Only (M4A)",
            resolution: "Audio",
            height: 0,
            size: Number(candidate.size || 0),
            isAudioOnly: true,
            ext: "m4a",
            videoUrl: audioTargetUrl,
            pairedAudioUrl: "",
            videoFilter: "audio"
          }]);
        }
      }
      videoCand.sourceType = SourceType.YOUTUBE_EXTRACTION;
      return { added: false, groupedUnder: videoCand.candidateId, candidate: videoCand };
    }
  }

  // If this is a video googlevideo stream and an audio-only stream was already observed, pair it and prune standalone audio
  if (candidate.isGoogleVideo && !candidate.isAudioOnly) {
    const audioIdx = list.findIndex(item => item.isGoogleVideo && item.isAudioOnly);
    if (audioIdx !== -1) {
      const audioCand = list[audioIdx];
      if (!candidate.pairedAudioUrl) {
        candidate.pairedAudioUrl = audioCand.pairedAudioUrl || audioCand.url;
      }
      if (candidate.pairedAudioUrl) {
        candidate.qualities = mergeQualityLists(candidate.qualities || [], [{
          id: "audio-only",
          label: "Audio Only (M4A)",
          resolution: "Audio",
          height: 0,
          size: Number(audioCand.size || 0),
          isAudioOnly: true,
          ext: "m4a",
          videoUrl: candidate.pairedAudioUrl,
          pairedAudioUrl: "",
          videoFilter: "audio"
        }]);
      }
      list.splice(audioIdx, 1);
    }
  }

  // If this is a YouTube extraction stream, prune any standalone googlevideo audio-only items
  if (candidate.streamType === "YOUTUBE" || candidate.sourceType === SourceType.YOUTUBE_EXTRACTION) {
    for (let i = list.length - 1; i >= 0; --i) {
      if (list[i].isGoogleVideo && list[i].isAudioOnly) {
        if (!candidate.pairedAudioUrl) {
          candidate.pairedAudioUrl = list[i].pairedAudioUrl || list[i].url;
        }
        list.splice(i, 1);
      }
    }
  }

  // Exact URL deduplication (or matching YouTube videoId / googlevideo itag)
  const existing = list.find(item => {
    if (item.url === candidate.url) return true;
    if ((item.streamType === "YOUTUBE" || item.sourceType === SourceType.YOUTUBE_EXTRACTION) &&
        (candidate.streamType === "YOUTUBE" || candidate.sourceType === SourceType.YOUTUBE_EXTRACTION)) {
      if (item.videoId && candidate.videoId && item.videoId === candidate.videoId) return true;
      if (isYouTubeWatchOrShortsUrl(item.url) && isYouTubeWatchOrShortsUrl(candidate.url) &&
          extractYouTubeVideoId(item.url) === extractYouTubeVideoId(candidate.url)) {
        return true;
      }
    }
    if (item.isGoogleVideo && candidate.isGoogleVideo &&
        item.itag && candidate.itag && item.itag === candidate.itag) {
      return true;
    }
    return false;
  });

  if (existing) {
    if (candidate.cachedManifestBody && (!existing.cachedManifestBody || candidate.cachedManifestBody.length > existing.cachedManifestBody.length)) {
      existing.cachedManifestBody = candidate.cachedManifestBody;
    }
    if (candidate.extractorJson) {
      if (!existing.extractorJson) {
        existing.extractorJson = candidate.extractorJson;
      } else {
        try {
          const exExisting = JSON.parse(existing.extractorJson);
          const exIncoming = JSON.parse(candidate.extractorJson);
          const existingQCount = (exExisting && Array.isArray(exExisting.qualities)) ? exExisting.qualities.length : 0;
          const incomingQCount = (exIncoming && Array.isArray(exIncoming.qualities)) ? exIncoming.qualities.length : 0;
          if (incomingQCount >= existingQCount) {
            existing.extractorJson = candidate.extractorJson;
          }
        } catch (_) {
          existing.extractorJson = candidate.extractorJson;
        }
      }
    }
    if (candidate.videoUrl && !existing.videoUrl) {
      existing.videoUrl = candidate.videoUrl;
    }
    if (candidate.pairedAudioUrl && !existing.pairedAudioUrl) {
      existing.pairedAudioUrl = candidate.pairedAudioUrl;
    }
    if (candidate.requiresCipherExtraction !== undefined) {
      existing.requiresCipherExtraction = Boolean(candidate.requiresCipherExtraction);
    }
    if (candidate.resolution) {
      const incH = heightFromResolutionLabel(candidate.resolution);
      const curH = heightFromResolutionLabel(existing.resolution);
      if (incH > curH || !existing.resolution || existing.resolution === "Auto") {
        existing.resolution = candidate.resolution;
      }
    }
    if (candidate.size && (!existing.size || existing.size === 0 || candidate.size > existing.size)) {
      existing.size = candidate.size;
    }
    if (candidate.pageTitle && (!existing.pageTitle || existing.pageTitle === "video" || existing.pageTitle === "youtube_video")) {
      existing.pageTitle = candidate.pageTitle;
    }
    if (candidate.headers && Object.keys(candidate.headers).length > 0) {
      existing.headers = Object.assign({}, existing.headers, candidate.headers);
    }
    if (Array.isArray(candidate.qualities) && candidate.qualities.length > 0) {
      existing.qualities = mergeQualityLists(existing.qualities || [], candidate.qualities);
      const topVid = existing.qualities.find(q => !q.isAudioOnly) || existing.qualities[0];
      if (topVid && topVid.resolution) {
        const topH = heightFromResolutionLabel(topVid.resolution);
        const curH = heightFromResolutionLabel(existing.resolution);
        if (topH > curH || !existing.resolution || existing.resolution === "Auto") {
          existing.resolution = topVid.resolution;
        }
        if (topVid.size && (!existing.size || existing.size === 0 || topH > curH)) {
          existing.size = topVid.size;
        }
      }
    }
    return { added: false, candidate: existing };
  }

  candidate.candidateId = candidate.candidateId || ("cand-" + Math.random().toString(36).slice(2, 10));
  candidate.timestamp = Date.now();
  candidate.variants = candidate.variants || [];

  if (candidate.streamType === "HLS" || candidate.sourceType === SourceType.HLS_MANIFEST) {
    const isMaster = isLikelyMasterManifestUrl(candidate.url, candidate.cachedManifestBody);
    candidate.isMaster = isMaster;
    candidate.confidence = isMaster ? 95 : 80;
    const prefix = getParentUrlPrefix(candidate.url);

    if (!isMaster) {
      const parentMaster = list.find(item =>
        item.streamType === "HLS" &&
        item.isMaster &&
        prefix.startsWith(getParentUrlPrefix(item.url))
      );
      if (parentMaster) {
        parentMaster.variants.push(candidate);
        const childRes = candidate.resolution || inferResolutionLabelFromUrl(candidate.url) || `Variant ${parentMaster.variants.length}`;
        parentMaster.qualities = mergeQualityLists(parentMaster.qualities || [], [{
          id: childRes.toLowerCase(),
          label: `${childRes} (HLS)`,
          resolution: childRes,
          height: heightFromResolutionLabel(childRes),
          ext: "mp4",
          variantUrl: candidate.url,
          videoFilter: childRes
        }]);
        return { added: false, groupedUnder: parentMaster.candidateId, candidate: parentMaster };
      }
    } else {
      for (let i = list.length - 1; i >= 0; --i) {
        const item = list[i];
        if (item.streamType === "HLS" && !item.isMaster && getParentUrlPrefix(item.url).startsWith(prefix)) {
          candidate.variants.push(item);
          const childRes = item.resolution || inferResolutionLabelFromUrl(item.url) || `Variant ${candidate.variants.length}`;
          candidate.qualities = mergeQualityLists(candidate.qualities || [], [{
            id: childRes.toLowerCase(),
            label: `${childRes} (HLS)`,
            resolution: childRes,
            height: heightFromResolutionLabel(childRes),
            ext: "mp4",
            variantUrl: item.url,
            videoFilter: childRes
          }]);
          list.splice(i, 1);
        }
      }
    }
  } else if (candidate.streamType === "DASH" || candidate.sourceType === SourceType.DASH_MANIFEST) {
    candidate.isMaster = true;
    candidate.confidence = 95;
  } else if (candidate.streamType === "YOUTUBE" || candidate.streamType === "SITE_EXTRACTION" || candidate.sourceType === SourceType.YOUTUBE_EXTRACTION) {
    candidate.isMaster = true;
    candidate.confidence = 96;
  } else if (candidate.isGoogleVideo && candidate.isAudioOnly) {
    candidate.confidence = 50;
  } else {
    candidate.confidence = 90;
  }

  list.push(candidate);
  list.sort((a, b) => (b.confidence || 0) - (a.confidence || 0));
  logPipelineStage(candidate.correlationId, "CANDIDATE_CLASSIFIED", {
    tabId,
    candidateId: candidate.candidateId,
    streamType: candidate.streamType,
    sourceType: candidate.sourceType,
    url: candidate.url
  });
  return { added: true, candidate };
}

// ---------------------------------------------------------------------------
// Ordinary Download Click, Redirect & Attachment Correlation Helpers (Defect C)
// ---------------------------------------------------------------------------
function normalizeCorrelationUrl(rawUrl) {
  if (!rawUrl || typeof rawUrl !== "string") return "";
  try {
    const u = new URL(rawUrl);
    u.hash = "";
    return u.toString();
  } catch (_) {
    return rawUrl.split("#")[0];
  }
}

function recordClickedLink(tabId, msg) {
  if (!msg || !msg.url) return null;
  const now = Date.now();
  const normUrl = normalizeCorrelationUrl(msg.url);
  const record = {
    tabId: Number(tabId) || 0,
    url: msg.url,
    normalizedUrl: normUrl,
    filename: msg.filename || "",
    hasDownloadAttr: Boolean(msg.hasDownloadAttr),
    referrer: msg.referrer || "",
    pageTitle: msg.pageTitle || "",
    correlationId: msg.correlationId || generateCorrelationId("click"),
    isFrame: Boolean(msg.isFrame),
    time: now
  };

  G.lastClickedLink = record;

  if (G.clickedLinksByUrl) {
    G.clickedLinksByUrl.set(normUrl, record);
    if (G.clickedLinksByUrl.size > 500) {
      const oldestKey = G.clickedLinksByUrl.keys().next().value;
      G.clickedLinksByUrl.delete(oldestKey);
    }
  }

  const tid = Number(tabId) || 0;
  if (G.clickedLinksByTab) {
    const arr = (G.clickedLinksByTab.get(tid) || []).filter(item => (now - item.time) < 60000);
    arr.unshift(record);
    if (arr.length > 25) arr.length = 25;
    G.clickedLinksByTab.set(tid, arr);
  }

  if (msg.bypass) {
    recordBypassIntent(tid, msg.url);
  }

  logPipelineStage(record.correlationId, "CLICK_CAPTURED", {
    tabId: tid,
    url: record.url,
    filename: record.filename,
    hasDownloadAttr: record.hasDownloadAttr
  });
  return record;
}

function recordBypassIntent(tabId, url = "") {
  const now = Date.now();
  G.bypassNextDownload = true;
  if (G.bypassTabs && tabId !== undefined && tabId !== null) {
    G.bypassTabs.set(Number(tabId) || 0, now);
  }
  if (G.bypassUrls && url) {
    G.bypassUrls.set(normalizeCorrelationUrl(url), now);
  }
}

function consumeBypassIfActive(tabId, downloadItem) {
  if (!downloadItem) return false;
  if (downloadItem.id !== undefined && G.bypassDownloadIds && G.bypassDownloadIds.has(downloadItem.id)) {
    return true;
  }

  const now = Date.now();
  const urlsToCheck = [
    normalizeCorrelationUrl(downloadItem.url || ""),
    normalizeCorrelationUrl(downloadItem.finalUrl || ""),
    resolveInitialUrlFromRedirects(downloadItem.finalUrl || downloadItem.url || "")
  ].filter(Boolean);

  let matched = false;
  if (G.bypassNextDownload) {
    G.bypassNextDownload = false;
    matched = true;
  }

  if (G.bypassUrls) {
    for (const u of urlsToCheck) {
      const ts = G.bypassUrls.get(u);
      if (ts !== undefined) {
        G.bypassUrls.delete(u);
        if ((now - ts) < 30000) {
          matched = true;
        }
      }
    }
  }

  const tid = Number(tabId || downloadItem.tabId || 0);
  if (G.bypassTabs && G.bypassTabs.has(tid)) {
    const ts = G.bypassTabs.get(tid);
    G.bypassTabs.delete(tid);
    if (ts && (now - ts) < 15000) {
      matched = true;
    }
  }

  if (matched && downloadItem.id !== undefined && G.bypassDownloadIds) {
    G.bypassDownloadIds.add(downloadItem.id);
  }
  return matched;
}

function recordRedirectHop(fromUrl, toUrl, requestId = "", tabId = 0) {
  if (!fromUrl || !toUrl) return;
  const normFrom = normalizeCorrelationUrl(fromUrl);
  const normTo = normalizeCorrelationUrl(toUrl);
  if (!normFrom || !normTo || normFrom === normTo) return;

  if (G.redirectChainMap) {
    G.redirectChainMap.set(normFrom, normTo);
    if (G.redirectChainMap.size > 1000) {
      G.redirectChainMap.delete(G.redirectChainMap.keys().next().value);
    }
  }
  if (G.reverseRedirectMap) {
    const rootUrl = G.reverseRedirectMap.get(normFrom) || normFrom;
    G.reverseRedirectMap.set(normTo, rootUrl);
    if (G.reverseRedirectMap.size > 1000) {
      G.reverseRedirectMap.delete(G.reverseRedirectMap.keys().next().value);
    }
  }
  // Propagate clicked link metadata across redirect hops
  if (G.clickedLinksByUrl && G.clickedLinksByUrl.has(normFrom) && !G.clickedLinksByUrl.has(normTo)) {
    const clickRec = G.clickedLinksByUrl.get(normFrom);
    G.clickedLinksByUrl.set(normTo, Object.assign({}, clickRec, { redirectedTo: normTo }));
  }
  if (G.bypassUrls && G.bypassUrls.has(normFrom)) {
    G.bypassUrls.set(normTo, G.bypassUrls.get(normFrom));
  }
  void requestId;
  void tabId;
}

function resolveInitialUrlFromRedirects(targetUrl) {
  const norm = normalizeCorrelationUrl(targetUrl);
  if (!norm) return "";
  if (G.reverseRedirectMap && G.reverseRedirectMap.has(norm)) {
    return G.reverseRedirectMap.get(norm);
  }
  return norm;
}

function findCorrelatedClick(tabId, downloadItem) {
  if (!downloadItem) return null;
  const now = Date.now();
  const maxAgeMs = 30000;
  const candidateUrls = [
    normalizeCorrelationUrl(downloadItem.url || ""),
    normalizeCorrelationUrl(downloadItem.finalUrl || ""),
    resolveInitialUrlFromRedirects(downloadItem.url || ""),
    resolveInitialUrlFromRedirects(downloadItem.finalUrl || "")
  ].filter(Boolean);

  // 1. Direct or redirect-resolved URL match in G.clickedLinksByUrl (including path without query)
  if (G.clickedLinksByUrl) {
    for (const u of candidateUrls) {
      const rec = G.clickedLinksByUrl.get(u);
      if (rec && (now - rec.time) < maxAgeMs) {
        return rec;
      }
      const uNoQuery = u.split("?")[0];
      const recNoQuery = G.clickedLinksByUrl.get(uNoQuery);
      if (recNoQuery && (now - recNoQuery.time) < maxAgeMs) {
        return recNoQuery;
      }
    }
  }

  // 2. Match in per-tab clicked link list (handles dynamic redirect endpoints on the same tab)
  const effectiveTabId = Number(tabId || downloadItem.tabId || 0);
  if (G.clickedLinksByTab && effectiveTabId > 0) {
    const tabClicks = G.clickedLinksByTab.get(effectiveTabId) || [];
    for (const rec of tabClicks) {
      if ((now - rec.time) >= maxAgeMs) continue;
      if (candidateUrls.includes(rec.normalizedUrl)) return rec;
      if (candidateUrls.some(u => u.split("?")[0] === rec.normalizedUrl.split("?")[0])) return rec;
      if (downloadItem.referrer && rec.referrer && normalizeCorrelationUrl(downloadItem.referrer) === normalizeCorrelationUrl(rec.referrer)) {
        return rec;
      }
    }
  }

  // 2b. If effectiveTabId is not available (e.g., cross-origin or sandboxed iframe), search recent clicks across all tabs
  if (effectiveTabId <= 0 && G.clickedLinksByTab) {
    for (const tabClicks of G.clickedLinksByTab.values()) {
      for (const rec of tabClicks) {
        if ((now - rec.time) >= maxAgeMs) continue;
        if (candidateUrls.includes(rec.normalizedUrl)) return rec;
        if (candidateUrls.some(u => u.split("?")[0] === rec.normalizedUrl.split("?")[0])) return rec;
        if (downloadItem.referrer && rec.referrer && normalizeCorrelationUrl(downloadItem.referrer) === normalizeCorrelationUrl(rec.referrer)) {
          return rec;
        }
      }
    }
  }

  // 3. Fallback to G.lastClickedLink if matching candidate URL or exact referrer
  if (G.lastClickedLink && (now - G.lastClickedLink.time) < 15000) {
    const lastNorm = normalizeCorrelationUrl(G.lastClickedLink.url);
    if (candidateUrls.includes(lastNorm) ||
        candidateUrls.some(u => u.split("?")[0] === lastNorm.split("?")[0]) ||
        (downloadItem.referrer && G.lastClickedLink.referrer && normalizeCorrelationUrl(downloadItem.referrer) === normalizeCorrelationUrl(G.lastClickedLink.referrer))) {
      return G.lastClickedLink;
    }
  }

  return null;
}

function isBlobOrDataUrl(url) {
  if (!url || typeof url !== "string") return false;
  return url.startsWith("blob:") || url.startsWith("data:");
}

function classifyDownloadMechanism(context = {}) {
  const url = context.url || (context.downloadItem && context.downloadItem.url) || "";
  const finalUrl = context.finalUrl || (context.downloadItem && context.downloadItem.finalUrl) || "";
  const isFrame = Boolean(context.isFrame || (context.downloadItem && context.downloadItem.tabId === -1) || (context.details && context.details.frameId > 0));
  const hasAuth = Boolean(context.headers && (context.headers.Authorization || context.headers.Cookie));
  const isAttachment = Boolean(context.isAttachment || (context.netMeta && context.netMeta.isAttachment));
  const isRedirect = Boolean((finalUrl && url && finalUrl !== url) || context.hasRedirectHop);

  if (url.startsWith("blob:") || url.startsWith("data:") || finalUrl.startsWith("blob:") || finalUrl.startsWith("data:")) {
    return "BROWSER_BLOB_OR_DATA";
  }
  if (isFrame) {
    return "FRAME_ORIGINATED";
  }
  if (isRedirect) {
    return "REDIRECT_CHAIN";
  }
  if (hasAuth) {
    return "AUTHENTICATED_SESSION";
  }
  if (context.isDynamicAnchor || context.wasSyntheticClick) {
    return "JS_TRIGGERED_DYNAMIC_ANCHOR";
  }
  if (isAttachment || context.filenameFromContentDisposition) {
    return "UNKNOWN_FILENAME_DYNAMIC";
  }
  return "ORDINARY_LINK";
}

var exportedFns = {
  SourceType,
  generateCorrelationId,
  logPipelineStage,
  extractHostname,
  isSiteDisabled,
  setSiteDisabled,
  extractUrlExtension,
  extractFilenameFromUrlQuery,
  extractFilenameFromContentDisposition,
  isAttachmentContentDisposition,
  isYouTubeWatchOrShortsUrl,
  extractYouTubeVideoId,
  isWebpageWatchUrl,
  isValidHlsManifestText,
  isValidDashManifestText,
  classifyMediaSourceType,
  classifyUrlIntent,
  parseResponseHeaderMap,
  parseTotalSizeFromHeaders,
  normalizeMediaUrl,
  classifyNetworkResource,
  inferResolutionLabelFromUrl,
  heightFromResolutionLabel,
  parseHlsMasterQualities,
  parseDashManifestQualities,
  mergeQualityLists,
  addOrDeduplicateMediaCandidate,
  normalizeCorrelationUrl,
  recordClickedLink,
  recordBypassIntent,
  consumeBypassIfActive,
  recordRedirectHop,
  resolveInitialUrlFromRedirects,
  findCorrelatedClick,
  isBlobOrDataUrl,
  classifyDownloadMechanism,
  sanitizeFilename,
  isDownloadEligible
};

Object.assign(globalThis, exportedFns);

if (typeof module !== "undefined" && module.exports) {
  module.exports = exportedFns;
}
