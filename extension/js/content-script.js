// SPDX-License-Identifier: Apache-2.0
// Adapted from xifangczy/cat-catch (js/content-script.js) & Brisk Browser Extension.
// Observes <video> elements (including dynamic insertion & resize), positions the closed Shadow DOM
// "Download with Radium" cosmic pill badge, provides real-time status/error feedback on click, intercepts link clicks
// (with Alt-key bypass), captures media events, and relays page MAIN world messages.

function positionOverlayHost(video, hostEl) {
  if (!video || !hostEl) return;
  const doc = (video && video.ownerDocument) ? video.ownerDocument : (typeof document !== "undefined" ? document : null);
  if (doc) {
    const isFullscreen = Boolean(doc.fullscreenElement || doc.webkitFullscreenElement);
    if (isFullscreen && doc.fullscreenElement && hostEl.parentElement !== doc.fullscreenElement) {
      try { doc.fullscreenElement.appendChild(hostEl); } catch (_) {}
    } else if (!isFullscreen && doc.body && hostEl.parentElement !== doc.body) {
      try { doc.body.appendChild(hostEl); } catch (_) {}
    }
  }

  const rect = typeof video.getBoundingClientRect === "function"
    ? video.getBoundingClientRect()
    : { width: video.width || 640, height: video.height || 360, top: 20, left: 20, right: 660, bottom: 380 };

  // Hide unobtrusively if the video player is collapsed or scrolled out of the visible viewport
  const isCollapsed = (rect.width !== undefined && rect.width < 120) || (rect.height !== undefined && rect.height < 80);
  const isOffscreen = (rect.bottom !== undefined && rect.bottom < 28) || (rect.right !== undefined && rect.right < 48);

  // Keep the compact control in a small toolbar just above the player, outside
  // the video image and aligned to the player's left edge.
  const topPx = Math.max(8, Math.round((Number.isFinite(rect.top) ? rect.top : 20) - 34));
  const leftPx = Math.max(8, Math.round(Number.isFinite(rect.left) ? rect.left : 20));

  const visCss = (isCollapsed || isOffscreen)
    ? "opacity:0;pointer-events:none;"
    : "pointer-events:auto;";
  const nextCss = `position:fixed;top:${topPx}px;left:${leftPx}px;z-index:2147483647;${visCss}`;
  if (hostEl.__fluxLastCss !== nextCss) {
    hostEl.__fluxLastCss = nextCss;
    hostEl.style.cssText = nextCss;
  }
}

function extractVideoElementSrc(video) {
  if (!video) return "";
  let src = video.currentSrc || video.src || "";
  if (!src && typeof video.querySelector === "function") {
    const sourceEl = video.querySelector("source[src]");
    if (sourceEl) src = sourceEl.src || sourceEl.getAttribute("src") || "";
  }
  return src;
}

let g_contentScriptCleanedUp = false;
let g_videoObserver = null;
let g_messageListener = null;

function isExtensionContextValid() {
  try {
    if (typeof chrome === "undefined") {
      // In test/Node.js unit test environments where chrome is undefined, allow DOM testing
      return typeof module !== "undefined";
    }
    if (!chrome.runtime) return false;
    // In test/sandbox environments where chrome.runtime.id is not mocked, check sendMessage
    if (typeof chrome.runtime.id === "undefined") {
      return typeof chrome.runtime.sendMessage === "function";
    }
    return Boolean(chrome.runtime.id);
  } catch (_) {
    return false;
  }
}

function cleanupContentScript() {
  if (g_contentScriptCleanedUp) return;
  g_contentScriptCleanedUp = true;
  try {
    if (typeof window !== "undefined") {
      window.removeEventListener("click", handleLinkClick, { capture: true, passive: false });
      if (g_messageListener) {
        window.removeEventListener("message", g_messageListener, { capture: true });
        g_messageListener = null;
      }
    }
    if (typeof document !== "undefined") {
      document.removeEventListener("play", handleMediaPlaybackEvent, true);
      document.removeEventListener("playing", handleMediaPlaybackEvent, true);
      document.removeEventListener("canplay", handleMediaPlaybackEvent, true);
      document.removeEventListener("loadedmetadata", handleMediaPlaybackEvent, true);
    }
    if (g_videoObserver && typeof g_videoObserver.disconnect === "function") {
      g_videoObserver.disconnect();
      g_videoObserver = null;
    }
    if (typeof document !== "undefined" && document.querySelectorAll) {
      const badges = document.querySelectorAll("[data-flux-shadow-host]");
      badges.forEach(b => { try { b.remove(); } catch (_) {} });
    }
  } catch (_) {}
}

function safeSendRuntimeMessage(msg, callback) {
  if (!isExtensionContextValid()) {
    cleanupContentScript();
    if (typeof callback === "function") callback(null);
    return;
  }
  try {
    chrome.runtime.sendMessage(msg, (response) => {
      const err = (typeof chrome !== "undefined" && chrome.runtime) ? chrome.runtime.lastError : null;
      if (err) {
        const msgStr = err.message || String(err);
        if (msgStr.includes("context invalidated") || msgStr.includes("Extension context invalidated")) {
          cleanupContentScript();
        }
      }
      if (typeof callback === "function") {
        callback(response);
      }
    });
  } catch (ex) {
    const exStr = ex ? ex.message : String(ex);
    if (exStr.includes("context invalidated") || exStr.includes("Extension context invalidated")) {
      cleanupContentScript();
    }
    if (typeof callback === "function") {
      callback(null);
    }
  }
}

function notifyDirectVideoElementDiscovered(video, doc) {
  if (!isExtensionContextValid() || g_contentScriptCleanedUp) return;
  try {
    const src = extractVideoElementSrc(video);
    if (!src || src.startsWith("blob:") || src.startsWith("data:")) return;
    if (video.__fluxLastReportedSrc === src) return;
    video.__fluxLastReportedSrc = src;

    const lower = src.toLowerCase();
    const streamType = lower.includes(".m3u8") ? "HLS" : (lower.includes(".mpd") ? "DASH" : "HTTP_FILE");
    safeSendRuntimeMessage({
      action: "addMedia",
      url: src,
      streamType,
      pageTitle: (doc && doc.title) ? doc.title : "video",
      tabUrl: (doc && doc.location) ? doc.location.href : ""
    });
  } catch (_) {}
}

function scanAndAttachVideoOverlays(doc, onDownloadClick, minWidth = 240, minHeight = 135) {
  if (!isExtensionContextValid() || g_contentScriptCleanedUp) return [];
  if (!doc || !doc.querySelectorAll) return [];
  const videos = Array.from(doc.querySelectorAll("video"));
  const attached = [];

  for (const video of videos) {
    notifyDirectVideoElementDiscovered(video, doc);

    const rect = typeof video.getBoundingClientRect === "function"
      ? video.getBoundingClientRect()
      : { width: video.width || 640, height: video.height || 360, top: 20, left: 20, right: 660, bottom: 380 };

    const isYouTube = Boolean(doc.location && doc.location.hostname && (doc.location.hostname.includes("youtube.com") || doc.location.hostname === "youtu.be"));
    const isPlayerCandidate = (rect.width >= minWidth && rect.height >= minHeight) ||
      (isYouTube && (rect.width >= 160 || (video.parentElement && video.parentElement.id === "movie_player")));

    if (!isPlayerCandidate) {
      continue;
    }

    const src = extractVideoElementSrc(video);
    // On video sites like YouTube, video elements initially stream via MSE blob: or delayed src.
    // Never skip attaching the overlay button to an active video player just because src is pending.
    if (!src && !isYouTube && (!video.readyState || video.readyState === 0 || rect.width < 320 || rect.height < 180)) {
      continue;
    }
    const isBlob = Boolean(src && src.startsWith("blob:"));
    const isDrm = Boolean(video.mediaKeys);

    // If host element is already attached and in DOM, ensure it's positioned correctly
    if (video.__fluxHostEl && video.__fluxHostEl.isConnected) {
      positionOverlayHost(video, video.__fluxHostEl);
      continue;
    }

    // If host element was previously created but detached during DOM updates/SPA navigation, re-append it
    if (video.__fluxHostEl) {
      const targetContainer = doc.fullscreenElement || doc.body;
      if (targetContainer && targetContainer.appendChild) {
        targetContainer.appendChild(video.__fluxHostEl);
        positionOverlayHost(video, video.__fluxHostEl);
        attached.push({
          video,
          hostEl: video.__fluxHostEl,
          btn: video.__fluxBtn,
          isBlob,
          isDrm,
          src,
          reposition: () => positionOverlayHost(video, video.__fluxHostEl)
        });
        continue;
      }
    }

    video.dataset.radiumOverlayAttached = "true";
    video.dataset.fluxOverlayAttached = "true";
    video.dataset.openidmOverlayAttached = "true";

    const hostEl = doc.createElement("div");
    hostEl.setAttribute("data-radium-shadow-host", "true");
    hostEl.setAttribute("data-flux-shadow-host", "true");
    hostEl.setAttribute("data-openidm-shadow-host", "true");
    if (isDrm) hostEl.setAttribute("data-flux-drm", "true");
    positionOverlayHost(video, hostEl);

    // Closed Shadow DOM ensures zero CSS leakage into or from the host webpage
    const shadow = hostEl.attachShadow ? hostEl.attachShadow({ mode: "closed" }) : hostEl;

    const styleEl = doc.createElement("style");
    styleEl.textContent = `
      button {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        background: #25262A;
        color: #F2F2F3;
        border: 1px solid #414248;
        border-radius: 6px;
        padding: 4px 9px;
        font: 600 11px "Segoe UI Variable Text", "Segoe UI", -apple-system, BlinkMacSystemFont, sans-serif;
        letter-spacing: 0.01em;
        cursor: pointer;
        opacity: 0.94;
        box-shadow: 0 2px 6px rgba(0, 0, 0, 0.35);
        transition: border-color 0.12s ease, background 0.12s ease;
        white-space: nowrap;
      }
      button::before {
        content: "";
        width: 6px;
        height: 6px;
        border-radius: 50%;
        background: #FF4F57;
        box-shadow: none;
        flex-shrink: 0;
      }
      button:hover:not(:disabled), button:focus-visible {
        opacity: 1;
        border-color: #FF4F57;
        background: #37383D;
        color: #FFFFFF;
        outline: none;
      }
      button:active:not(:disabled) {
        transform: scale(0.98);
      }
      button:disabled {
        opacity: 0.72;
        border-color: #414248;
        color: #A4A5AC;
        cursor: not-allowed;
      }
      button:disabled::before {
        background: #64748B;
        box-shadow: none;
      }
      @media (prefers-reduced-motion: reduce) {
        button { transition: none; }
      }
    `;
    if (shadow && typeof shadow.appendChild === "function") {
      shadow.appendChild(styleEl);
    }

    const lastCand = (typeof window !== "undefined" && window.__fluxLastCandidate) ? window.__fluxLastCandidate : null;
    const topQ = (lastCand && Array.isArray(lastCand.qualities) && lastCand.qualities.length > 0)
      ? (lastCand.qualities.find(q => !q.isAudioOnly && String(q.resolution || "").toLowerCase().includes("1080")) ||
         lastCand.qualities.find(q => !q.isAudioOnly) ||
         lastCand.qualities[0])
      : null;
    const qSuffix = (topQ && topQ.resolution) ? ` [${topQ.resolution}]` : "";

    const btn = doc.createElement("button");
    btn.type = "button";
    btn.textContent = isDrm ? "DRM Protected (Unsupported)" : `Download with Radium${qSuffix}`;
    btn.disabled = isDrm;
    const baseBtnStyle =
      "background:#2D2E32;color:#F2F2F3;border:1px solid #414248;border-radius:7px;" +
      "padding:4px 9px;font:600 11px 'Segoe UI Variable Text','Segoe UI',sans-serif;cursor:pointer;box-shadow:0 2px 6px rgba(0,0,0,0.35);white-space:nowrap;";
    if (btn.style) {
      btn.style.cssText = baseBtnStyle;
    }

    const invokeClick = (ev) => {
      if (ev && typeof ev.stopPropagation === "function") ev.stopPropagation();
      if (ev && typeof ev.preventDefault === "function") ev.preventDefault();

      const liveDrm = Boolean(video.mediaKeys) || isDrm;
      if (liveDrm) {
        btn.textContent = "DRM Protected (Unsupported)";
        btn.disabled = true;
        return;
      }

      const liveSrc = extractVideoElementSrc(video);
      const liveIsBlob = liveSrc.startsWith("blob:");

      if (onDownloadClick) {
        onDownloadClick({
          videoSrc: liveIsBlob ? "" : liveSrc,
          isMseBlob: liveIsBlob,
          pageTitle: doc.title || "video",
          frameUrl: (doc.location ? doc.location.href : ""),
          tabUrl: (doc.location ? doc.location.href : ""),
          isIframe: (typeof window !== "undefined" && window.self !== window.top),
          buttonEl: btn,
          hostEl,
          baseBtnStyle
        });
      }
    };

    btn.addEventListener("click", invokeClick);
    if (hostEl && typeof hostEl.addEventListener === "function") {
      hostEl.addEventListener("click", invokeClick);
    }

    shadow.appendChild(btn);
    const targetContainer = doc.fullscreenElement || doc.body;
    if (targetContainer && targetContainer.appendChild) {
      targetContainer.appendChild(hostEl);
    }

    video.__fluxHostEl = hostEl;
    video.__fluxBtn = btn;
    hostEl.__fluxVideo = video;
    hostEl.__fluxBtn = btn;

    attached.push({
      video,
      hostEl,
      btn,
      isBlob,
      isDrm,
      src,
      reposition: () => positionOverlayHost(video, hostEl)
    });
  }
  return attached;
}

function observeDynamicVideosAndFullscreen(doc, win, onDownloadClick) {
  let activeOverlays = scanAndAttachVideoOverlays(doc, onDownloadClick);

  const refreshPositions = () => {
    activeOverlays = activeOverlays.filter(item => {
      if (item.video && item.video.isConnected === false) {
        if (item.hostEl && typeof item.hostEl.remove === "function") {
          item.hostEl.remove();
        }
        return false;
      }
      if (item.hostEl && item.hostEl.isConnected === false) {
        const targetContainer = (doc && doc.fullscreenElement) || (doc && doc.body);
        if (targetContainer && targetContainer.appendChild) {
          targetContainer.appendChild(item.hostEl);
        }
      }
      item.reposition();
      return true;
    });
  };

  const rescanAndRefresh = () => {
    const newlyAttached = scanAndAttachVideoOverlays(doc, onDownloadClick);
    if (newlyAttached.length > 0) {
      activeOverlays = activeOverlays.concat(newlyAttached);
    }
    refreshPositions();
  };

  if (win && typeof win.addEventListener === "function") {
    win.addEventListener("scroll", refreshPositions, { passive: true });
    win.addEventListener("resize", refreshPositions, { passive: true });
  }
  if (doc && typeof doc.addEventListener === "function") {
    doc.addEventListener("fullscreenchange", () => {
      refreshPositions();
      rescanAndRefresh();
    });
    doc.addEventListener("webkitfullscreenchange", () => {
      refreshPositions();
      rescanAndRefresh();
    });
    doc.addEventListener("loadedmetadata", rescanAndRefresh, true);
    doc.addEventListener("loadeddata", rescanAndRefresh, true);
    doc.addEventListener("canplay", rescanAndRefresh, true);
    doc.addEventListener("play", rescanAndRefresh, true);
    doc.addEventListener("playing", rescanAndRefresh, true);
    doc.addEventListener("timeupdate", refreshPositions, { passive: true, capture: true });
    doc.addEventListener("yt-navigate-finish", rescanAndRefresh);
    doc.addEventListener("yt-page-data-updated", rescanAndRefresh);
  }

  // Periodic gentle sweep ensuring dynamic SPA player transitions never leave the overlay detached
  const pollTimer = (typeof setInterval === "function") ? setInterval(() => {
    if (g_contentScriptCleanedUp) {
      clearInterval(pollTimer);
      return;
    }
    rescanAndRefresh();
  }, 1500) : null;

  if (typeof MutationObserver !== "undefined" && doc && doc.documentElement) {
    const observer = new MutationObserver((mutations) => {
      if (Array.isArray(mutations)) {
        const onlyOverlayChange = mutations.every(m =>
          m.target && m.target.getAttribute && m.target.getAttribute("data-flux-shadow-host") === "true"
        );
        if (onlyOverlayChange) return;
      }
      rescanAndRefresh();
    });
    observer.observe(doc.documentElement, { childList: true, subtree: true });
    g_videoObserver = observer;
    return { activeOverlays, observer, refreshPositions, pollTimer };
  }
  return { activeOverlays, observer: null, refreshPositions, pollTimer };
}


function findAnchorFromEvent(ev) {
  if (!ev) return null;
  if (typeof ev.composedPath === "function") {
    try {
      const path = ev.composedPath();
      for (const el of path) {
        if (el && el.nodeType === 1 && typeof el.matches === "function") {
          if (el.matches("a[href], [data-download-url], [data-href]")) {
            return el;
          }
        }
      }
    } catch (_) {}
  }
  let target = ev.target;
  if (target && target.nodeType === 3) {
    target = target.parentElement;
  }
  if (target && typeof target.closest === "function") {
    return target.closest("a[href], [data-download-url], [data-href]") || target.closest("a");
  }
  return null;
}

function triggerBrowserFallback(anchor, href) {
  try {
    if (anchor && anchor.dataset) {
      anchor.dataset.fluxBypass = "true";
    }
    safeSendRuntimeMessage({ action: "bypassNextDownload", url: href });
    if (anchor && typeof anchor.click === "function") {
      anchor.click();
    } else if (typeof location !== "undefined" && location.assign) {
      location.assign(href);
    }
  } catch (_) {}
}

function handleLinkClick(ev) {
  if (!isExtensionContextValid() || g_contentScriptCleanedUp) {
    cleanupContentScript();
    return;
  }
  try {
    const correlationId = "click-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 8);

    const anchor = findAnchorFromEvent(ev);
    if (!anchor) return;

    if (anchor.dataset && (anchor.dataset.fluxBypass === "true" || anchor.dataset.fluxBypass === true)) {
      delete anchor.dataset.fluxBypass;
      return;
    }

    let href = anchor.href || (typeof anchor.getAttribute === "function" ? (anchor.getAttribute("data-download-url") || anchor.getAttribute("data-href")) : "") || "";
    if (!href) return;

    let isFrame = false;
    try { isFrame = (window !== window.top); } catch (_) { isFrame = true; }
    let referrer = "";
    try { referrer = location.href; } catch (_) {}
    let pageTitle = "";
    try { pageTitle = document.title; } catch (_) {}

    const isBlobOrData = href.startsWith("blob:") || href.startsWith("data:");
    let mechanism = isBlobOrData ? "BROWSER_BLOB_OR_DATA" : (isFrame ? "FRAME_ORIGINATED" : "ORDINARY_LINK");
    if (!ev.isTrusted) {
      mechanism = "JS_TRIGGERED_DYNAMIC_ANCHOR";
    }

    console.log(`[Radium Trace] [CLICK_SEEN] [${correlationId}]`, {
      url: isBlobOrData ? (href.slice(0, 48) + "...") : href,
      target: anchor.tagName,
      button: ev.button,
      altKey: ev.altKey,
      mechanism,
      isFrame
    });

    if (ev.altKey) {
      // IDM Alt-key bypass behavior: user explicitly requested browser-native download
      console.log(`[Radium Trace] [CLICK_IGNORED] [${correlationId}]`, { url: isBlobOrData ? (href.slice(0, 48) + "...") : href, reason: "ALT_KEY_BYPASS", mechanism });
      safeSendRuntimeMessage({ action: "bypassNextDownload", url: href, correlationId });
      return;
    }

    // 1. Browser-generated in-memory Blob and Data URLs:
    // Native browser download pipeline handles client-side files reliably without disruption.
    const isWebBlob = href.startsWith("blob:http://") || href.startsWith("blob:https://");
    if (isBlobOrData && !isWebBlob) {
      console.log(`[Radium Trace] [BROWSER_BLOB_OR_DATA] [${correlationId}]`, {
        url: href.slice(0, 48) + "...",
        filename: anchor.download || "",
        mechanism
      });
      return;
    }

    if (ev.button !== undefined && ev.button !== 0) {
      console.log(`[Radium Trace] [CLICK_IGNORED] [${correlationId}]`, { url: href, reason: "UNSUPPORTED_BUTTON", mechanism });
      return;
    }

    if (href.startsWith("javascript:") || href.startsWith("#") || href.startsWith("mailto:")) {
      console.log(`[Radium Trace] [CLICK_IGNORED] [${correlationId}]`, { url: href, reason: "NON_HTTP_PROTOCOL", mechanism });
      return;
    }

    let pathname = anchor.pathname || "";
    if (!pathname && typeof URL !== "undefined") {
      try { pathname = new URL(href, (typeof location !== "undefined" ? location.href : undefined)).pathname; } catch (_) {}
    }
    const lastDot = pathname.lastIndexOf(".");
    const ext = lastDot !== -1 ? pathname.slice(lastDot + 1).toLowerCase() : "";
    const hasDownloadAttr = typeof anchor.hasAttribute === "function" && anchor.hasAttribute("download");

    const interceptExts = new Set([
      "zip", "rar", "7z", "tar", "gz", "bz2", "xz", "tgz", "iso", "exe", "msi", "dmg", "pdf",
      "apk", "bin", "deb", "rpm", "docx", "xlsx", "pptx", "csv", "epub",
      "mp4", "mkv", "webm", "mov", "avi", "flv", "mp3", "m4a", "flac", "wav",
      "py", "json", "txt", "md", "sql", "yaml", "xml", "html", "js", "ts", "cpp", "c", "rs", "go", "java"
    ]);

    const isAiFileUrl = href.includes("oaiusercontent.com") || href.includes("rscd=") ||
                        href.includes("response-content-disposition=") || href.includes("/backend-api/files/");
    let isEligible = isWebBlob || hasDownloadAttr || isAiFileUrl || interceptExts.has(ext) ||
      pathname.includes("/redirect") || pathname.includes("/download") || pathname.includes("/dl/");

    if (!isEligible && typeof URL !== "undefined") {
      try {
        const parsed = new URL(href, (typeof location !== "undefined" ? location.href : undefined));
        for (const [key, val] of parsed.searchParams.entries()) {
          const kLower = key.toLowerCase();
          if (kLower.includes("download") || kLower === "dl" || kLower === "file" || kLower === "filename") {
            const vDot = val.lastIndexOf(".");
            const vExt = vDot !== -1 ? val.slice(vDot + 1).toLowerCase() : "";
            if (interceptExts.has(vExt) || val === "1" || val === "true" || kLower.includes("download")) {
              isEligible = true;
              break;
            }
          }
          const vDot = val.lastIndexOf(".");
          if (vDot !== -1 && interceptExts.has(val.slice(vDot + 1).toLowerCase())) {
            isEligible = true;
            break;
          }
        }
      } catch (_) {}
    }

    if (!isEligible) {
      console.log(`[Radium Trace] [CLICK_IGNORED] [${correlationId}]`, {
        url: href,
        reason: "NOT_DOWNLOAD_ELIGIBLE",
        mechanism: "UNKNOWN_FILENAME_DYNAMIC",
        note: "Deferred to browser network response for Content-Disposition determination"
      });
      return;
    }

    // Prevent duplicate click recordings from rapid multi-clicks (brief debounce)
    if (anchor.__fluxClickInFlight) {
      return;
    }
    anchor.__fluxClickInFlight = true;
    setTimeout(() => { delete anchor.__fluxClickInFlight; }, 800);

    // Record link click metadata for correlation in chrome.downloads.onDeterminingFilename.
    // User gesture is strictly preserved: we NEVER call ev.preventDefault() or ev.stopPropagation()
    // on natural anchor clicks, ensuring browser cookies, session state, and redirects proceed uninterrupted.
    const targetFilename = anchor.download ||
      (typeof extractFilenameFromUrlQuery === "function" ? extractFilenameFromUrlQuery(href) : "") || "";
    safeSendRuntimeMessage({
      action: "linkClicked",
      url: href,
      filename: targetFilename,
      hasDownloadAttr: Boolean(anchor.download || isAiFileUrl || hasDownloadAttr),
      referrer,
      pageTitle,
      bypass: false,
      mechanism,
      isFrame,
      correlationId
    });

    console.log(`[Radium Trace] [CLICK_OBSERVED] [${correlationId}]`, {
      url: href,
      filename: anchor.download || "",
      hasDownloadAttr,
      mechanism,
      note: "Click observed and correlated with chrome.downloads.onDeterminingFilename; user gesture preserved"
    });
    console.log(`[Radium Trace] [CLICK_RECORDED] [${correlationId}]`, {
      url: href,
      filename: anchor.download || "",
      hasDownloadAttr,
      mechanism,
      note: "Click intent correlated with chrome.downloads.onDeterminingFilename; user gesture preserved"
    });
  } catch (err) {
    console.error("[Radium] Error in handleLinkClick:", err);
  }
}

function handleMediaPlaybackEvent(ev) {
  if (!isExtensionContextValid() || g_contentScriptCleanedUp) {
    cleanupContentScript();
    return;
  }
  try {
    const target = ev.target;
    if (!target) return;
    const tagName = (target.tagName || "").toLowerCase();
    if (tagName !== "video" && tagName !== "audio") return;

    const src = extractVideoElementSrc(target);
    if (src && !src.startsWith("blob:") && !src.startsWith("data:")) {
      const lower = src.toLowerCase();
      const streamType = lower.includes(".m3u8") ? "HLS" : (lower.includes(".mpd") ? "DASH" : "HTTP_FILE");
      safeSendRuntimeMessage({
        action: "addMedia",
        url: src,
        streamType,
        pageTitle: (typeof document !== "undefined" ? document.title : "video"),
        tabUrl: (typeof location !== "undefined" ? location.href : "")
      });
    }
  } catch (_) {}
}

function extractEligiblePageLinks(doc) {
  if (!doc || !doc.querySelectorAll) return [];
  const anchors = Array.from(doc.querySelectorAll("a[href]"));
  const seen = new Set();
  const results = [];

  const extCategories = {
    zip: "Compressed", rar: "Compressed", "7z": "Compressed", tar: "Compressed", gz: "Compressed", bz2: "Compressed", xz: "Compressed",
    pdf: "Documents", doc: "Documents", docx: "Documents", xls: "Documents", xlsx: "Documents", ppt: "Documents", pptx: "Documents", epub: "Documents", txt: "Documents", csv: "Documents",
    exe: "Applications", msi: "Applications", dmg: "Applications", pkg: "Applications", deb: "Applications", rpm: "Applications", apk: "Applications",
    mp4: "Video", mkv: "Video", webm: "Video", mov: "Video", avi: "Video", flv: "Video",
    mp3: "Music", m4a: "Music", flac: "Music", wav: "Music", ogg: "Music",
    jpg: "Images", jpeg: "Images", png: "Images", gif: "Images", webp: "Images", svg: "Images"
  };

  for (const a of anchors) {
    const rawHref = a.getAttribute("href") || "";
    if (!rawHref || rawHref.startsWith("#") || rawHref.startsWith("javascript:") || rawHref.startsWith("mailto:") || rawHref.startsWith("data:")) {
      continue;
    }
    let fullUrl = a.href;
    try { fullUrl = new URL(rawHref, doc.location ? doc.location.href : undefined).href; } catch (_) {}
    if (!fullUrl.startsWith("http://") && !fullUrl.startsWith("https://")) continue;
    if (seen.has(fullUrl)) continue;
    seen.add(fullUrl);

    let pathname = "";
    try { pathname = new URL(fullUrl).pathname; } catch (_) {}
    const rawFilename = (a.download || pathname.split("/").pop() || "download").split("?")[0];
    const lastDot = rawFilename.lastIndexOf(".");
    const ext = lastDot !== -1 ? rawFilename.slice(lastDot + 1).toLowerCase() : "";
    const category = extCategories[ext] || "Other";
    const linkText = (a.textContent || a.title || rawFilename).trim().replace(/\s+/g, " ");

    results.push({
      url: fullUrl,
      filename: rawFilename || "file",
      ext: ext || "file",
      category,
      text: linkText.slice(0, 80),
      isDirectFile: Boolean(extCategories[ext])
    });
  }
  return results;
}

function initContentScript() {
  if (typeof document === "undefined" || typeof window === "undefined") return;

  // 1. Intercept link clicks with capture phase at window level
  window.addEventListener("click", handleLinkClick, { capture: true, passive: false });

  // 2. Listen to media playback events with capture phase
  document.addEventListener("play", handleMediaPlaybackEvent, true);
  document.addEventListener("playing", handleMediaPlaybackEvent, true);
  document.addEventListener("canplay", handleMediaPlaybackEvent, true);
  document.addEventListener("loadedmetadata", handleMediaPlaybackEvent, true);

  // 3. Relay page MAIN world messages from catch-script/catch.js to background
  g_messageListener = (ev) => {
    try {
      if (!isExtensionContextValid() || g_contentScriptCleanedUp) {
        cleanupContentScript();
        return;
      }
      if (!ev || !ev.data) return;
      if (ev.source !== window && ev.origin !== window.location.origin) return;

      if (ev.data.action === "catCatchAddMedia") {
        if (typeof window !== "undefined") {
          window.__fluxLastCandidate = ev.data;
        }
        // Dynamically update overlay badge label with detected quality and estimated size
        try {
          if (typeof document !== "undefined" && document.querySelectorAll) {
            const hosts = document.querySelectorAll("[data-flux-shadow-host]");
            const topQ = (Array.isArray(ev.data.qualities) && ev.data.qualities.length > 0)
              ? (ev.data.qualities.find(q => !q.isAudioOnly && String(q.resolution || "").toLowerCase().includes("1080")) ||
                 ev.data.qualities.find(q => !q.isAudioOnly) ||
                 ev.data.qualities[0])
              : null;
            if (topQ) {
              ev.data.qualityId = topQ.id || topQ.resolution || "1080p";
              ev.data.resolution = topQ.resolution || "1080p";
              const qText = topQ.resolution || topQ.label || "";
              const szStr = topQ.size > 0 ? ` (~${(topQ.size / 1048576).toFixed(0)} MB)` : "";
              hosts.forEach(h => {
                if (h.__fluxBtn && !h.getAttribute("data-flux-status") && !h.getAttribute("data-flux-drm")) {
                  h.__fluxBtn.textContent = `Download [${qText}${szStr}]`;
                  h.__fluxBtn.title = `Download ${qText} with Radium Download Manager`;
                }
              });
            }
          }
        } catch (_) {}
        safeSendRuntimeMessage({
          action: "addMedia",
          url: ev.data.url,
          streamType: ev.data.streamType || "HLS",
          sourceType: ev.data.sourceType || "",
          ext: ev.data.ext || "mp4",
          mime: ev.data.mime || "",
          size: ev.data.size || 0,
          itag: ev.data.itag || "",
          isAudioOnly: Boolean(ev.data.isAudioOnly),
          isGoogleVideo: Boolean(ev.data.isGoogleVideo),
          isAdaptiveTrack: Boolean(ev.data.isAdaptiveTrack),
          requiresAudioMux: Boolean(ev.data.requiresAudioMux),
          requiresCipherExtraction: Boolean(ev.data.requiresCipherExtraction),
          videoId: ev.data.videoId || "",
          resolution: ev.data.resolution || "",
          videoUrl: ev.data.videoUrl || "",
          pairedAudioUrl: ev.data.pairedAudioUrl || "",
          qualities: Array.isArray(ev.data.qualities) ? ev.data.qualities : [],
          extractorJson: ev.data.extractorJson || "",
          candidateId: ev.data.candidateId || "",
          cachedManifestBody: ev.data.cachedManifestBody || "",
          tabUrl: location.href,
          pageTitle: ev.data.pageTitle || document.title || "video"
        }, (resp) => {
          if (resp && resp.candidate && resp.candidate.candidateId && typeof window !== "undefined" && window.__fluxLastCandidate) {
            window.__fluxLastCandidate.candidateId = resp.candidate.candidateId;
          }
        });
      } else if (ev.data.action === "catCatchAddKey") {
        safeSendRuntimeMessage({
          action: "addKey",
          key: ev.data.key,
          url: ev.data.url
        });
      } else if (ev.data.action === "catCatchPlayerError") {
        safeSendRuntimeMessage({
          action: "playerError",
          reason: ev.data.reason,
          url: ev.data.url || location.href
        });
      } else if (ev.data.action === "radiumDirectBlobDownload") {
        safeSendRuntimeMessage({
          action: "interceptBlobDownload",
          url: ev.data.url,
          filename: ev.data.filename,
          mimeType: ev.data.mimeType,
          size: ev.data.size,
          dataBase64: ev.data.dataBase64,
          correlationId: ev.data.correlationId,
          pageTitle: ev.data.pageTitle || (typeof document !== "undefined" ? document.title : ""),
          pageUrl: ev.data.pageUrl || (typeof location !== "undefined" ? location.href : "")
        }, (resp) => {
          if (!resp || !resp.success) {
            window.postMessage({
              action: "radiumBlobFallback",
              url: ev.data.url,
              filename: ev.data.filename
            }, "*");
          }
        });
      } else if (ev.data.action === "fluxAnchorClicked" || ev.data.action === "radiumAnchorClicked") {
        safeSendRuntimeMessage({
          action: "linkClicked",
          url: ev.data.url,
          filename: ev.data.download || "",
          hasDownloadAttr: Boolean(ev.data.download),
          referrer: ev.data.pageUrl || location.href,
          pageTitle: ev.data.pageTitle || document.title,
          bypass: false,
          mechanism: "JS_TRIGGERED_DYNAMIC_ANCHOR",
          isFrame: (window !== window.top),
          correlationId: "anchor-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 8)
        });
      }
    } catch (_) {}
  };
  window.addEventListener("message", g_messageListener, { capture: true });

  // 3b. Handle background probePageMedia & extractPageLinks RPC
  if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.onMessage) {
    chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
      if (!isExtensionContextValid() || g_contentScriptCleanedUp) {
        cleanupContentScript();
        return false;
      }
      if (request && request.action === "fluxFetchBlobData") {
        const blobUrl = request.blobUrl || "";
        const requestId = request.requestId || ("blob-req-" + Date.now().toString(36));

        const queryCatchScript = () => {
          let resolved = false;
          const handler = (ev) => {
            if (!ev || !ev.data || ev.data.action !== "fluxBlobDataResponse" || ev.data.requestId !== requestId) return;
            if (resolved) return;
            resolved = true;
            window.removeEventListener("message", handler, true);
            sendResponse(ev.data);
          };
          window.addEventListener("message", handler, true);
          window.postMessage({ action: "fluxGetBlobData", blobUrl, requestId }, "*");
          setTimeout(() => {
            if (!resolved) {
              resolved = true;
              window.removeEventListener("message", handler, true);
              sendResponse({ success: false, error: "TIMEOUT" });
            }
          }, 8000);
        };

        if (typeof fetch === "function") {
          fetch(blobUrl).then(r => r.blob()).then(blob => {
            const reader = new FileReader();
            reader.onload = () => {
              const res = String(reader.result || "");
              const commaIdx = res.indexOf(",");
              const b64 = commaIdx >= 0 ? res.slice(commaIdx + 1) : res;
              sendResponse({
                success: true,
                dataBase64: b64,
                mimeType: blob.type || "application/octet-stream",
                size: blob.size || 0
              });
            };
            reader.onerror = () => { queryCatchScript(); };
            reader.readAsDataURL(blob);
          }).catch(() => { queryCatchScript(); });
        } else {
          queryCatchScript();
        }
        return true;
      }

      if (request && request.action === "probePageMedia") {
        try {
          window.postMessage({ action: "fluxScanPageMedia" }, "*");
          sendResponse({ success: true });
        } catch (e) {
          sendResponse({ success: false, error: e.message });
        }
        return true;
      }
      if (request && request.action === "extractPageLinks") {
        try {
          const links = extractEligiblePageLinks(document);
          sendResponse({ success: true, links });
        } catch (e) {
          sendResponse({ success: false, error: e.message, links: [] });
        }
        return true;
      }
    });
  }

  // 4. Note: catch-script/catch.js is injected directly into page MAIN world at document_start by manifest.json.
  // Redundant DOM script tag injection has been removed to preserve CSP compliance across all frames.

  // 5. Observe videos and attach overlay with active visual status & error reporting
  const onDownloadClick = (mediaMeta) => {
    const btn = mediaMeta.buttonEl;
    const hostEl = mediaMeta.hostEl;
    const baseStyle = mediaMeta.baseBtnStyle || "";

    if (btn) {
      btn.textContent = "Starting in Radium...";
      btn.disabled = true;
    }
    if (hostEl && hostEl.setAttribute) {
      hostEl.setAttribute("data-flux-status", "starting");
    }

    const lastCand = (typeof window !== "undefined" && window.__fluxLastCandidate) ? window.__fluxLastCandidate : null;
    let topQ = null;
    if (lastCand && Array.isArray(lastCand.qualities)) {
      topQ = lastCand.qualities.find(q => !q.isAudioOnly && String(q.resolution || "").toLowerCase().includes("1080")) ||
             lastCand.qualities.find(q => !q.isAudioOnly) ||
             lastCand.qualities[0];
    }
    const candidateId = mediaMeta.candidateId || (lastCand && lastCand.candidateId) || "";
    const qualityId = mediaMeta.qualityId || (topQ && (topQ.id || topQ.resolution)) || (lastCand && lastCand.qualityId) || "";
    const resolution = mediaMeta.resolution || (topQ && topQ.resolution) || (lastCand && lastCand.resolution) || "";
    const videoFilter = mediaMeta.videoFilter || (topQ && (topQ.videoFilter || topQ.resolution)) || "best";
    const videoSrc = mediaMeta.videoSrc || (lastCand ? (lastCand.url || lastCand.videoUrl) : "") || "";

    safeSendRuntimeMessage({
      action: "downloadMedia",
      candidateId,
      qualityId,
      resolution,
      videoFilter,
      selectedQuality: topQ,
      url: videoSrc,
      isMseBlob: Boolean(mediaMeta.isMseBlob),
      pageTitle: mediaMeta.pageTitle || document.title || "video",
      tabUrl: mediaMeta.tabUrl || location.href,
      frameUrl: mediaMeta.frameUrl || location.href
    }, (resp) => {
      const lastErr = (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.lastError)
        ? chrome.runtime.lastError.message
        : null;

      if (lastErr || !resp || !resp.success) {
        const errText = lastErr || (resp && resp.error) || "Download failed";
        if (btn) {
          btn.textContent = "Error: " + (errText.length > 28 ? errText.slice(0, 28) + "..." : errText);
          btn.title = errText;
          btn.disabled = false;
          if (btn.style) btn.style.cssText = baseStyle + "border-color:#EF4444;color:#FCA5A5;";
        }
        if (hostEl && hostEl.setAttribute) {
          hostEl.setAttribute("data-flux-status", "error");
          hostEl.setAttribute("data-flux-error", errText);
        }
      } else {
        const isFallback = Boolean(resp.result && resp.result.fallbackToBrowser);
        const taskId = (resp.result && resp.result.taskId) ? resp.result.taskId : "";
        if (btn) {
          btn.textContent = isFallback ? "Browser Download" : "Queued in Radium \u2713";
          btn.title = taskId ? ("Task ID: " + taskId) : "Queued in Radium";
          btn.disabled = false;
          if (btn.style) btn.style.cssText = baseStyle + "border-color:#22C55E;color:#86EFAC;";
        }
        if (hostEl && hostEl.setAttribute) {
          hostEl.setAttribute("data-flux-status", isFallback ? "browser_fallback" : "queued");
          if (taskId) hostEl.setAttribute("data-flux-task-id", taskId);
        }
      }

      if (btn && typeof setTimeout === "function") {
        setTimeout(() => {
          btn.textContent = "Download with Radium";
          if (btn.style && baseStyle) btn.style.cssText = baseStyle;
        }, 3500);
      }
    });
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", () => {
      observeDynamicVideosAndFullscreen(document, window, onDownloadClick);
    }, { once: true });
  } else {
    observeDynamicVideosAndFullscreen(document, window, onDownloadClick);
  }
}

// Auto-run in browser execution
if (typeof window !== "undefined" && typeof document !== "undefined") {
  initContentScript();
}

if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    positionOverlayHost,
    scanAndAttachVideoOverlays,
    observeDynamicVideosAndFullscreen,
    findAnchorFromEvent,
    handleLinkClick,
    handleMediaPlaybackEvent,
    extractEligiblePageLinks,
    initContentScript
  };
}
