// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from xifangczy/cat-catch (js/background.js) & Brisk Browser Extension for Radium Download Manager.
// Handles Service Worker module initialization, webRequest header & redirect correlation,
// explicit typed media detection (HLS_MANIFEST, DASH_MANIFEST, DIRECT_PROGRESSIVE, YOUTUBE_EXTRACTION, UNSUPPORTED),
// ordinary download interception with per-tab/per-URL click tracking and attachment support,
// message routing with popup/content scripts, tab badge synchronization, and Native Messaging RPC handoff.

if (typeof importScripts === "function" && typeof globalThis.G === "undefined") {
  try {
    importScripts("/js/init.js", "/js/function.js");
  } catch (_) {
    importScripts("init.js", "function.js");
  }
} else if (typeof require !== "undefined") {
  Object.assign(globalThis, require("./init.js"), require("./function.js"));
}
var G = globalThis.G;
var SourceType = globalThis.SourceType || (G && G.SourceType) || {
  HLS_MANIFEST: "HLS_MANIFEST",
  DASH_MANIFEST: "DASH_MANIFEST",
  DIRECT_PROGRESSIVE: "DIRECT_PROGRESSIVE",
  YOUTUBE_EXTRACTION: "YOUTUBE_EXTRACTION",
  UNSUPPORTED: "UNSUPPORTED"
};

let rpcRequestCounter = 1;
let sessionRestorePromise = null;

function getStorageStore(customStore = null) {
  if (customStore) return customStore;
  if (typeof chrome !== "undefined" && chrome.storage) {
    if (chrome.storage.session) return chrome.storage.session;
    if (chrome.storage.local) return chrome.storage.local;
  }
  return null;
}

// Persist & restore tab media candidates across Manifest V3 Service Worker suspension
async function persistSessionState(storageArea = null) {
  const store = getStorageStore(storageArea);
  if (!store || typeof store.set !== "function") return false;
  const serializedTabs = {};
  for (const [tabId, list] of G.cacheData.entries()) {
    serializedTabs[String(tabId)] = list;
  }

  // Persist tabUrlMap (active tab URLs)
  const serializedTabUrls = {};
  if (G.tabUrlMap) {
    let count = 0;
    for (const [tid, url] of G.tabUrlMap.entries()) {
      if (count++ >= 50) break;
      serializedTabUrls[String(tid)] = url;
    }
  }

  // Persist recent clicked links ring buffer (up to 50 most recent clicks for correlation)
  const recentClicks = [];
  if (G.clickedLinksByUrl) {
    const now = Date.now();
    for (const item of G.clickedLinksByUrl.values()) {
      const ts = item && (item.time || item.timestamp);
      if (item && ts && (now - ts) < 1800000) {
        recentClicks.push(item);
        if (recentClicks.length >= 50) break;
      }
    }
  }

  // Persist recent download network responses for Content-Disposition & MIME correlation
  const recentResponses = [];
  if (G.recentDownloadResponses) {
    const now = Date.now();
    for (const [url, meta] of G.recentDownloadResponses.entries()) {
      if (meta && (now - meta.timestamp) < 300000) {
        recentResponses.push({ url, meta });
        if (recentResponses.length >= 50) break;
      }
    }
  }

  // Persist redirect chains
  const redirectChains = [];
  if (G.redirectChainMap) {
    let count = 0;
    for (const [from, to] of G.redirectChainMap.entries()) {
      if (count++ >= 50) break;
      redirectChains.push([from, to]);
    }
  }
  const reverseRedirects = [];
  if (G.reverseRedirectMap) {
    let count = 0;
    for (const [to, root] of G.reverseRedirectMap.entries()) {
      if (count++ >= 50) break;
      reverseRedirects.push([to, root]);
    }
  }

  await store.set({
    flux_cacheData: serializedTabs,
    flux_disabledHosts: Array.from(G.disabledHosts),
    flux_tabUrls: serializedTabUrls,
    flux_recentClicks: recentClicks,
    flux_recentResponses: recentResponses,
    flux_redirectChains: redirectChains,
    flux_reverseRedirects: reverseRedirects
  });
  return true;
}

async function restoreSessionState(storageArea = null) {
  const store = getStorageStore(storageArea);
  if (!store || typeof store.get !== "function") return false;
  const data = await store.get([
    "flux_cacheData",
    "flux_disabledHosts",
    "flux_tabUrls",
    "flux_recentClicks",
    "flux_recentResponses",
    "flux_redirectChains",
    "flux_reverseRedirects"
  ]);
  const cacheSource = data ? data.flux_cacheData : null;
  if (cacheSource) {
    for (const [k, v] of Object.entries(cacheSource)) {
      if (Array.isArray(v)) {
        const tid = Number(k);
        const existing = G.cacheData.get(tid) || [];
        if (existing.length === 0) {
          G.cacheData.set(tid, v);
        } else {
          for (const item of v) {
            addOrDeduplicateMediaCandidate(tid, item);
          }
        }
      }
    }
  }
  const disabledSource = data ? data.flux_disabledHosts : null;
  if (Array.isArray(disabledSource)) {
    for (const h of disabledSource) {
      G.disabledHosts.add(h);
    }
  }
  const tabUrlsSource = data ? data.flux_tabUrls : null;
  if (tabUrlsSource && G.tabUrlMap) {
    for (const [k, v] of Object.entries(tabUrlsSource)) {
      const tid = Number(k);
      if (!G.tabUrlMap.has(tid) && v) {
        G.tabUrlMap.set(tid, v);
      }
    }
  }
  const clicksSource = data ? data.flux_recentClicks : null;
  if (Array.isArray(clicksSource) && G.clickedLinksByUrl) {
    const now = Date.now();
    for (const item of clicksSource) {
      const ts = item && (item.time || item.timestamp);
      if (item && ts && (now - ts) < 1800000) {
        if (item.url && !G.clickedLinksByUrl.has(item.url)) {
          G.clickedLinksByUrl.set(item.url, item);
        }
        if (item.tabId && G.clickedLinksByTab) {
          const tabList = G.clickedLinksByTab.get(item.tabId);
          if (Array.isArray(tabList)) {
            tabList.push(item);
          } else {
            G.clickedLinksByTab.set(item.tabId, [item]);
          }
        }
      }
    }
  }
  const respSource = data ? data.flux_recentResponses : null;
  if (Array.isArray(respSource) && G.recentDownloadResponses) {
    const now = Date.now();
    for (const entry of respSource) {
      if (entry && entry.url && entry.meta && (now - entry.meta.timestamp) < 300000) {
        G.recentDownloadResponses.set(entry.url, entry.meta);
      }
    }
  }
  const chainSource = data ? data.flux_redirectChains : null;
  if (Array.isArray(chainSource) && G.redirectChainMap) {
    for (const [from, to] of chainSource) {
      if (from && to) G.redirectChainMap.set(from, to);
    }
  }
  const revSource = data ? data.flux_reverseRedirects : null;
  if (Array.isArray(revSource) && G.reverseRedirectMap) {
    for (const [to, root] of revSource) {
      if (to && root) G.reverseRedirectMap.set(to, root);
    }
  }
  return true;
}

function ensureSessionRestored(storageArea = null) {
  if (!sessionRestorePromise) {
    sessionRestorePromise = restoreSessionState(storageArea).catch(() => false);
  }
  return sessionRestorePromise;
}

// Keep media results scoped to the current document URL for each tab. This is
// also used for SPA history changes, where tabs.onUpdated does not fire.
function updateTabDocument(tabId, url) {
  if (!Number.isInteger(tabId) || tabId <= 0 || typeof url !== "string" || !url) return false;
  const previousUrl = G.tabUrlMap.get(tabId) || "";
  if (previousUrl && previousUrl !== url) {
    G.cacheData.delete(tabId);
    if (G.playerErrors) G.playerErrors.delete(tabId);
    if (G.blobMediaMap) G.blobMediaMap.delete(tabId);
    updateTabBadge(tabId);
    persistSessionState().catch(() => {});
    notifyPopupMediaUpdated(tabId);
  }
  G.tabUrlMap.set(tabId, url);
  return previousUrl !== url;
}

// Categorizes native messaging host errors for clear diagnostic logging
function categorizeNativeError(msg) {
  if (!msg) return "UNKNOWN_NATIVE_ERROR";
  const lower = String(msg).toLowerCase();
  if (lower.includes("forbidden") || lower.includes("not allowed") || lower.includes("unauthorized")) {
    return "UNAUTHORIZED_EXTENSION_ORIGIN";
  }
  if (lower.includes("not found") || lower.includes("specified native messaging host not found")) {
    return "MISSING_HOST_REGISTRATION";
  }
  if (lower.includes("exited") || lower.includes("crashed")) {
    return "HOST_PROCESS_TERMINATED";
  }
  if (lower.includes("err_ipc_desktop_unavailable") || lower.includes("desktop_unavailable")) {
    return "DESKTOP_APP_NOT_RUNNING";
  }
  if (lower.includes("timed out") || lower.includes("timeout")) {
    return "RPC_TIMEOUT";
  }
  return "RPC_COMMUNICATION_ERROR";
}

async function sendNativeRpc(method, params = {}, customTransport = null) {
  const correlationId = params.correlationId || generateCorrelationId("rpc");
  const payload = {
    jsonrpc: "2.0",
    id: "ext-" + (rpcRequestCounter++),
    method,
    params: Object.assign({}, params, { correlationId })
  };

  logPipelineStage(correlationId, "NATIVE_RPC_SENT", { method, rpcId: payload.id });

  if (customTransport) {
    const res = await customTransport(payload);
    logPipelineStage(correlationId, "NATIVE_RPC_RESULT", {
      method,
      accepted: res ? Boolean(res.accepted !== false) : true,
      taskId: res ? (res.taskId || "") : ""
    });
    return res;
  }

  if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.sendNativeMessage) {
    const hostsToTry = [G.nativeHostName, ...(G.fallbackNativeHostNames || [])];
    const callingExtId = (chrome.runtime && chrome.runtime.id) ? chrome.runtime.id : "unknown";
    let lastErrorMsg = "Native Messaging runtime unavailable";

    for (const hostName of hostsToTry) {
      if (!hostName) continue;
      try {
        const res = await new Promise((resolve, reject) => {
          let settled = false;
          const timer = setTimeout(() => {
            if (!settled) {
              settled = true;
              reject(new Error("Native Messaging RPC timed out after 5000ms"));
            }
          }, 5000);
          try {
            chrome.runtime.sendNativeMessage(hostName, payload, (response) => {
              if (settled) return;
              settled = true;
              clearTimeout(timer);
              if (chrome.runtime.lastError) {
                reject(new Error(chrome.runtime.lastError.message));
              } else if (response && response.error) {
                const rpcErr = new Error(response.error.message || "RPC Error");
                rpcErr.isRpcError = true;
                rpcErr.code = response.error.code;
                reject(rpcErr);
              } else {
                resolve(response ? response.result : null);
              }
            });
          } catch (e) {
            if (!settled) {
              settled = true;
              clearTimeout(timer);
              reject(e);
            }
          }
        });
        logPipelineStage(correlationId, "NATIVE_RPC_RESULT", {
          method,
          hostName,
          extensionId: callingExtId,
          accepted: res ? Boolean(res.accepted !== false) : true,
          taskId: res ? (res.taskId || "") : ""
        });
        return res;
      } catch (err) {
        if (err && err.isRpcError) {
          logPipelineStage(correlationId, "NATIVE_RPC_ERROR", { method, hostName, error: err.message, code: err.code });
          throw err;
        }
        lastErrorMsg = err ? err.message : String(err);
        const errType = categorizeNativeError(lastErrorMsg);
        logPipelineStage(correlationId, "NATIVE_HOST_ATTEMPT_FAILED", {
          hostName,
          extensionId: callingExtId,
          error: lastErrorMsg,
          errorCategory: errType
        });
      }
    }
    const finalCategory = categorizeNativeError(lastErrorMsg);
    logPipelineStage(correlationId, "NATIVE_RPC_UNAVAILABLE", {
      method,
      extensionId: callingExtId,
      error: lastErrorMsg,
      errorCategory: finalCategory
    });
    const finalErr = new Error(lastErrorMsg);
    finalErr.category = finalCategory;
    finalErr.extensionId = callingExtId;
    throw finalErr;
  }
  throw new Error("Native Messaging runtime unavailable");
}

function updateTabBadge(tabId) {
  if (typeof chrome === "undefined" || !chrome.action || !chrome.action.setBadgeText) return;
  const list = G.cacheData.get(tabId) || [];
  const count = list.length;
  const text = count > 0 ? String(count) : "";
  try {
    chrome.action.setBadgeText({ text, tabId: (typeof tabId === "number" && tabId > 0) ? tabId : undefined });
    chrome.action.setBadgeBackgroundColor({ color: "#FF4F57", tabId: (typeof tabId === "number" && tabId > 0) ? tabId : undefined });
  } catch (_) {}
}

async function collectCookiesForDownload(targetUrl, referrerUrl = "") {
  const cookieMap = new Map();
  const urlsToQuery = [];
  const domainsToQuery = new Set();

  const addTarget = (u) => {
    if (u && !u.startsWith("blob:") && !u.startsWith("data:")) {
      urlsToQuery.push(u);
      try {
        const h = new URL(u).hostname;
        if (h) {
          domainsToQuery.add(h);
          const parts = h.split(".");
          if (parts.length >= 2) {
            domainsToQuery.add("." + parts.slice(-2).join("."));
          }
        }
      } catch (_) {}
    }
  };

  addTarget(targetUrl);
  if (referrerUrl) addTarget(referrerUrl);

  if (typeof chrome !== "undefined" && chrome.cookies && typeof chrome.cookies.getAll === "function") {
    for (const u of urlsToQuery) {
      try {
        const list = await new Promise((resolve) => {
          let settled = false;
          const timer = setTimeout(() => {
            if (!settled) { settled = true; resolve([]); }
          }, 1500);
          try {
            chrome.cookies.getAll({ url: u }, (cList) => {
              if (!settled) {
                settled = true;
                clearTimeout(timer);
                if (chrome.runtime) void chrome.runtime.lastError;
                resolve(cList || []);
              }
            });
          } catch (_) {
            if (!settled) { settled = true; clearTimeout(timer); resolve([]); }
          }
        });
        if (Array.isArray(list)) {
          for (const c of list) {
            if (c && c.name && !cookieMap.has(c.name)) {
              cookieMap.set(c.name, c.value);
            }
          }
        }
      } catch (_) {}
    }

    for (const dom of domainsToQuery) {
      try {
        const list = await new Promise((resolve) => {
          let settled = false;
          const timer = setTimeout(() => {
            if (!settled) { settled = true; resolve([]); }
          }, 1000);
          try {
            chrome.cookies.getAll({ domain: dom }, (cList) => {
              if (!settled) {
                settled = true;
                clearTimeout(timer);
                if (chrome.runtime) void chrome.runtime.lastError;
                resolve(cList || []);
              }
            });
          } catch (_) {
            if (!settled) { settled = true; clearTimeout(timer); resolve([]); }
          }
        });
        if (Array.isArray(list)) {
          for (const c of list) {
            if (c && c.name && !cookieMap.has(c.name)) {
              cookieMap.set(c.name, c.value);
            }
          }
        }
      } catch (_) {}
    }
  }

  if (cookieMap.size === 0) return "";
  return Array.from(cookieMap.entries()).map(([k, v]) => `${k}=${v}`).join("; ");
}

function findRecentRequestHeadersForUrl(url) {
  if (!url || !G.requestMetaMap) return {};
  try {
    const norm = normalizeCorrelationUrl(url);
    for (const meta of G.requestMetaMap.values()) {
      if (meta && meta.headers) {
        const curNorm = meta.currentUrl ? normalizeCorrelationUrl(meta.currentUrl) : "";
        const initNorm = meta.initialUrl ? normalizeCorrelationUrl(meta.initialUrl) : "";
        if (curNorm === norm || initNorm === norm) {
          return Object.assign({}, meta.headers);
        }
      }
    }
  } catch (_) {}
  return {};
}

let popupNotifyTimer = null;
const pendingNotifyTabs = new Set();

function notifyPopupMediaUpdated(tabId) {
  if (typeof tabId === "number" && tabId > 0) {
    pendingNotifyTabs.add(tabId);
  }
  if (popupNotifyTimer) clearTimeout(popupNotifyTimer);
  popupNotifyTimer = setTimeout(() => {
    popupNotifyTimer = null;
    const tabsToNotify = Array.from(pendingNotifyTabs);
    pendingNotifyTabs.clear();
    const targetTab = tabsToNotify.length === 1 ? tabsToNotify[0] : (typeof tabId === "number" ? tabId : null);

    if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.sendMessage) {
      try {
        chrome.runtime.sendMessage({ action: "mediaUpdated", tabId: targetTab, tabs: tabsToNotify }, () => {
          if (chrome.runtime) void chrome.runtime.lastError;
        });
      } catch (_) {}
    }
    for (const tid of tabsToNotify) {
      if (typeof chrome !== "undefined" && chrome.tabs && typeof chrome.tabs.sendMessage === "function") {
        try {
          chrome.tabs.sendMessage(tid, { action: "mediaUpdated", tabId: tid }, () => {
            if (chrome.runtime) void chrome.runtime.lastError;
          });
        } catch (_) {}
      }
    }
  }, 100);
}

async function autoProbeManifestQualities(tabId, candidate) {
  if (!candidate || !candidate.url) return false;
  if (candidate.cachedManifestBody && Array.isArray(candidate.qualities) && candidate.qualities.length > 0) {
    return true;
  }
  const isHls = candidate.sourceType === SourceType.HLS_MANIFEST || candidate.streamType === "HLS";
  const isDash = candidate.sourceType === SourceType.DASH_MANIFEST || candidate.streamType === "DASH";
  if (!isHls && !isDash) return false;
  if (!candidate.url.startsWith("http://") && !candidate.url.startsWith("https://")) return false;
  if (typeof fetch !== "function") return false;
  if (candidate.__probingQualities) return false;
  candidate.__probingQualities = true;

  try {
    const fetchHeaders = {};
    if (candidate.referer || candidate.tabUrl) {
      fetchHeaders["Referer"] = candidate.referer || candidate.tabUrl;
    }
    if (candidate.tabUrl) {
      try {
        const origin = new URL(candidate.tabUrl).origin;
        fetchHeaders["Origin"] = origin;
      } catch (_) {}
    }
    if (typeof collectCookiesForDownload === "function") {
      const cookieHeader = await collectCookiesForDownload(candidate.url, candidate.tabUrl || candidate.referer);
      if (cookieHeader) fetchHeaders["Cookie"] = cookieHeader;
    }

    const resp = await fetch(candidate.url, {
      method: "GET",
      credentials: "include",
      headers: fetchHeaders
    });
    if (!resp || !resp.ok) return false;
    const bodyText = await resp.text();
    if (!bodyText) return false;
    if (isHls && isValidHlsManifestText(bodyText)) {
      candidate.cachedManifestBody = bodyText;
      const parsed = parseHlsMasterQualities(bodyText, candidate.url);
      if (parsed.length > 0) {
        candidate.qualities = mergeQualityLists(candidate.qualities || [], parsed);
        if (!candidate.resolution || candidate.resolution === "Auto") {
          candidate.resolution = candidate.qualities[0].resolution;
        }
        notifyPopupMediaUpdated(tabId);
        return true;
      }
    } else if (isDash && isValidDashManifestText(bodyText)) {
      candidate.cachedManifestBody = bodyText;
      const parsed = parseDashManifestQualities(bodyText, candidate.url);
      if (parsed.length > 0) {
        candidate.qualities = mergeQualityLists(candidate.qualities || [], parsed);
        if (!candidate.resolution || candidate.resolution === "Auto") {
          candidate.resolution = candidate.qualities[0].resolution;
        }
        notifyPopupMediaUpdated(tabId);
        return true;
      }
    }
  } catch (_) {
  } finally {
    candidate.__probingQualities = false;
  }
  return false;
}

// Resolve a direct media representation's size without reading its payload.
// HEAD is preferred; a single-byte range request is the fallback for servers
// that reject HEAD or omit Content-Length. Range bodies are canceled immediately.
async function probeMediaSize(tabId, candidate, fetchImpl = (typeof fetch === "function" ? fetch : null)) {
  if (!candidate || !candidate.url || Number(candidate.size) > 0) return Number(candidate && candidate.size) || 0;
  const isDirectMedia = candidate.sourceType === SourceType.DIRECT_PROGRESSIVE || candidate.streamType === "HTTP_FILE";
  if (!isDirectMedia || !/^https?:\/\//i.test(candidate.url) || typeof fetchImpl !== "function") return 0;
  const now = Date.now();
  if (candidate.sizeProbeAt && now - candidate.sizeProbeAt < 300000) return 0;
  const key = candidate.url;
  if (G.sizeProbeInFlight.has(key)) return G.sizeProbeInFlight.get(key);

  candidate.sizeStatus = "checking";
  candidate.sizeProbeAt = now;
  const probe = (async () => {
    let resolvedSize = 0;
    const headers = {};
    const auth = candidate.headers && (candidate.headers.Authorization || candidate.headers.authorization);
    if (auth) headers.Authorization = auth;
    const referrer = /^https?:\/\//i.test(candidate.tabUrl || "") ? candidate.tabUrl : undefined;
    const requestOptions = { credentials: "include", cache: "no-store", redirect: "follow", referrer };
    const readSize = (response) => {
      if (!response || !response.headers) return 0;
      const range = response.headers.get("content-range") || "";
      const rangeMatch = range.match(/bytes\s+\d+-\d+\/(\d+)/i);
      if (rangeMatch) return Number(rangeMatch[1]) || 0;
      const length = Number(response.headers.get("content-length")) || 0;
      return Number.isSafeInteger(length) && length > 0 ? length : 0;
    };
    const request = async (method, extraHeaders = {}) => {
      const controller = typeof AbortController === "function" ? new AbortController() : null;
      const timer = controller ? setTimeout(() => controller.abort(), 4500) : null;
      try {
        return await fetchImpl(candidate.url, Object.assign({}, requestOptions, {
          method,
          headers: Object.assign({}, headers, extraHeaders),
          signal: controller ? controller.signal : undefined
        }));
      } finally {
        if (timer) clearTimeout(timer);
      }
    };
    try {
      const head = await request("HEAD");
      if (head && head.ok) resolvedSize = readSize(head);
    } catch (_) {}
    if (!resolvedSize) {
      try {
        const ranged = await request("GET", { Range: "bytes=0-0" });
        if (ranged && ranged.ok) resolvedSize = readSize(ranged);
        if (ranged && ranged.body && typeof ranged.body.cancel === "function") {
          try { await ranged.body.cancel(); } catch (_) {}
        }
      } catch (_) {}
    }
    const current = G.cacheData.get(tabId) || [];
    const stillCurrent = current.some(item => item === candidate || (item.candidateId && item.candidateId === candidate.candidateId));
    if (stillCurrent) {
      candidate.sizeStatus = resolvedSize > 0 ? "known" : "unknown";
      if (resolvedSize > 0) {
        candidate.size = resolvedSize;
        for (const quality of (candidate.qualities || [])) {
          if (!Number(quality.size)) quality.size = resolvedSize;
        }
      }
      persistSessionState().catch(() => {});
      notifyPopupMediaUpdated(tabId);
    }
    return resolvedSize;
  })().catch(() => 0).finally(() => G.sizeProbeInFlight.delete(key));
  G.sizeProbeInFlight.set(key, probe);
  return probe;
}

function resolveEffectiveTabId(details) {
  if (details && typeof details.tabId === "number" && details.tabId > 0) {
    return details.tabId;
  }
  const sourceUrl = (details && (details.documentUrl || details.initiator)) || "";
  if (sourceUrl) {
    for (const [tid, turl] of G.tabUrlMap.entries()) {
      if (turl && (turl === sourceUrl || turl.startsWith(sourceUrl))) {
        return tid;
      }
    }
  }
  return (details && typeof details.tabId === "number") ? details.tabId : 0;
}

// 1. Record outgoing request headers (Referer, Origin, Cookie, User-Agent, Authorization) per requestId
function onBeforeSendHeadersHandler(details) {
  if (!details || !details.requestId) return;
  const captured = {};
  if (Array.isArray(details.requestHeaders)) {
    for (const h of details.requestHeaders) {
      if (!h || !h.name) continue;
      const lower = h.name.toLowerCase();
      if (lower === "referer" || lower === "origin" || lower === "cookie" ||
          lower === "user-agent" || lower === "authorization") {
        captured[h.name] = h.value || "";
      }
    }
  }
  G.requestHeadersMap.set(details.requestId, captured);
  if (G.requestHeadersMap.size > 2000) {
    const oldestKey = G.requestHeadersMap.keys().next().value;
    G.requestHeadersMap.delete(oldestKey);
  }

  if (G.requestMetaMap && details.url) {
    const prev = G.requestMetaMap.get(details.requestId) || {};
    G.requestMetaMap.set(details.requestId, Object.assign(prev, {
      initialUrl: prev.initialUrl || details.url,
      currentUrl: details.url,
      tabId: resolveEffectiveTabId(details),
      headers: captured,
      timestamp: Date.now()
    }));
    if (G.requestMetaMap.size > 2000) {
      G.requestMetaMap.delete(G.requestMetaMap.keys().next().value);
    }
  }
}

// 2. Inspect incoming response headers to track redirects, attachment downloads, and media resources
function onHeadersReceivedHandler(details) {
  if (!details || !details.url) return null;
  const effectiveTabId = resolveEffectiveTabId(details);
  const hmap = parseResponseHeaderMap(details.responseHeaders);
  const reqHeaders = Object.assign({}, G.requestHeadersMap.get(details.requestId) || {});
  const tabUrl = G.tabUrlMap.get(effectiveTabId) || details.documentUrl || details.initiator || details.url;
  if (!reqHeaders.Referer && !reqHeaders.referer && tabUrl && !tabUrl.startsWith("chrome-extension:")) {
    reqHeaders.Referer = tabUrl;
  }

  // 2a. Track HTTP Redirect chains (301, 302, 303, 307, 308) for ordinary download correlation
  const loc = hmap["location"] || "";
  const code = Number(details.statusCode);
  const isRedirect = (code >= 300 && code <= 308) || (loc && (!details.statusCode || (code >= 300 && code < 400)));
  if (isRedirect && loc) {
    let resolvedTarget = loc;
    try { resolvedTarget = new URL(loc, details.url).href; } catch (_) {}
    recordRedirectHop(details.url, resolvedTarget, details.requestId, effectiveTabId);
    if (details.requestId && G.requestMetaMap) {
      const meta = G.requestMetaMap.get(details.requestId) || { initialUrl: details.url };
      meta.currentUrl = resolvedTarget;
      G.requestMetaMap.set(details.requestId, meta);
    }
    return null;
  }

  // 2b. Record eligible attachment / binary download response metadata for correlation with chrome.downloads
  const cdHeader = hmap["content-disposition"] || "";
  const cdFilename = extractFilenameFromContentDisposition(cdHeader);
  const isAttachment = isAttachmentContentDisposition(cdHeader);
  const contentType = (hmap["content-type"] || "").split(";")[0].trim().toLowerCase();
  const totalSize = parseTotalSizeFromHeaders(hmap, details.url);
  const ext = extractUrlExtension(cdFilename) || extractUrlExtension(details.url);
  const reqMeta = (details.requestId && G.requestMetaMap && G.requestMetaMap.get(details.requestId)) || null;
  const originalUrl = (reqMeta && reqMeta.initialUrl) || resolveInitialUrlFromRedirects(details.url) || details.url;

  if (isAttachment || G.InterceptFileExts.has(ext) || (G.InterceptMimeTypes && G.InterceptMimeTypes.has(contentType))) {
    const respMeta = {
      url: details.url,
      originalUrl,
      filename: cdFilename || "",
      isAttachment,
      mime: contentType,
      size: totalSize,
      ext,
      tabId: effectiveTabId,
      tabUrl,
      headers: reqHeaders,
      timestamp: Date.now(),
      correlationId: generateCorrelationId("netdl")
    };
    if (G.recentDownloadResponses) {
      const normCur = normalizeCorrelationUrl(details.url);
      G.recentDownloadResponses.set(normCur, respMeta);
      if (normCur.includes("?")) {
        G.recentDownloadResponses.set(normCur.split("?")[0], respMeta);
      }
      if (originalUrl !== details.url) {
        const normOrig = normalizeCorrelationUrl(originalUrl);
        G.recentDownloadResponses.set(normOrig, respMeta);
        if (normOrig.includes("?")) {
          G.recentDownloadResponses.set(normOrig.split("?")[0], respMeta);
        }
      }
      if (G.recentDownloadResponses.size > 500) {
        G.recentDownloadResponses.delete(G.recentDownloadResponses.keys().next().value);
      }
    }
    logPipelineStage(respMeta.correlationId, "NETWORK_DOWNLOAD_CLASSIFIED", {
      tabId: effectiveTabId,
      url: details.url,
      originalUrl,
      filename: cdFilename,
      isAttachment,
      mime: contentType,
      size: totalSize
    });
  }

  // 2c. Classify streaming/video media resources for Popup & Video Overlay
  const enrichedDetails = Object.assign({}, details, { tabId: effectiveTabId });
  const classified = classifyNetworkResource(enrichedDetails);
  if (!classified) return null;

  const candidate = {
    url: classified.url,
    videoUrl: classified.videoUrl || "",
    pairedAudioUrl: classified.pairedAudioUrl || "",
    ext: classified.ext,
    streamType: classified.streamType,
    sourceType: classified.sourceType || classifyMediaSourceType(classified, classified.mime),
    mime: classified.mime,
    size: classified.size,
    itag: classified.itag || "",
    isAudioOnly: Boolean(classified.isAudioOnly),
    isGoogleVideo: Boolean(classified.isGoogleVideo),
    isAdaptiveTrack: Boolean(classified.isAdaptiveTrack),
    requiresAudioMux: Boolean(classified.requiresAudioMux),
    requiresCipherExtraction: Boolean(classified.requiresCipherExtraction),
    videoId: classified.videoId || "",
    fileName: classified.fileName || cdFilename || "",
    tabId: effectiveTabId,
    frameId: details.frameId || 0,
    tabUrl,
    pageTitle: details.pageTitle || "",
    headers: reqHeaders,
    correlationId: generateCorrelationId("netmedia")
  };

  const res = addOrDeduplicateMediaCandidate(effectiveTabId, candidate);
  if (res && res.candidate && (!res.candidate.qualities || res.candidate.qualities.length === 0)) {
    autoProbeManifestQualities(effectiveTabId, res.candidate).catch(() => {});
  }
  if (res && res.candidate && !Number(res.candidate.size) && typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.id) {
    probeMediaSize(effectiveTabId, res.candidate).catch(() => {});
  }
  updateTabBadge(effectiveTabId);
  persistSessionState().catch(() => {});
  notifyPopupMediaUpdated(effectiveTabId);
  return res;
}

function findCorrelatedNetworkResponse(downloadItem) {
  if (!downloadItem || !G.recentDownloadResponses) return null;
  const now = Date.now();
  const urls = [
    normalizeCorrelationUrl(downloadItem.finalUrl || ""),
    normalizeCorrelationUrl(downloadItem.url || ""),
    resolveInitialUrlFromRedirects(downloadItem.finalUrl || downloadItem.url || "")
  ].filter(Boolean);

  for (const u of urls) {
    const meta = G.recentDownloadResponses.get(u);
    if (meta && (now - meta.timestamp) < 60000) {
      return meta;
    }
    const uNoQuery = u.split("?")[0];
    const metaNoQuery = G.recentDownloadResponses.get(uNoQuery);
    if (metaNoQuery && (now - metaNoQuery.timestamp) < 60000) {
      return metaNoQuery;
    }
  }
  return null;
}

// 3. Authoritative Per-Download State Machine & Robust Interception (ADR-0003 & Priority 1 Requirement)
class DownloadStateMachine {
  constructor() {
    this.records = new Map();         // downloadId -> DownloadCorrelationRecord
    this.inFlightOffers = new Map();  // downloadId -> Promise<Result>
  }

  getOrCreateRecord(downloadId, initialItem = {}) {
    const id = Number(downloadId);
    let rec = this.records.get(id);
    if (!rec) {
      rec = {
        downloadId: id,
        url: initialItem.url || "",
        finalUrl: initialItem.finalUrl || initialItem.url || "",
        originalUrl: initialItem.url || "",
        redirectChain: [],
        tabId: Number(initialItem.tabId) || 0,
        frameId: 0,
        filename: initialItem.filename || "",
        suggestedFilename: "",
        mime: initialItem.mime || "",
        contentLength: initialItem.fileSize || initialItem.totalBytes || 0,
        headers: {},
        state: "INIT", // INIT, OFFERING, ACCEPTED, HANDED_OFF, DECLINED, FALLBACK, IGNORED, BYPASS
        stage: "STAGE_INITIALIZATION",
        correlationId: generateCorrelationId("dl"),
        suggestCallback: null,
        wasExplicitLinkClick: false,
        isAttachment: false,
        mechanism: "ORDINARY_LINK",
        createdAt: Date.now(),
        updatedAt: Date.now()
      };
      this.records.set(id, rec);
      if (this.records.size > 200) {
        const oldestKey = this.records.keys().next().value;
        this.records.delete(oldestKey);
      }
    } else {
      if (initialItem.finalUrl) rec.finalUrl = initialItem.finalUrl;
      if (initialItem.url && !rec.url) rec.url = initialItem.url;
      if (initialItem.filename && !rec.filename) rec.filename = initialItem.filename;
      if (initialItem.mime && !rec.mime) rec.mime = initialItem.mime;
      if (initialItem.fileSize) rec.contentLength = initialItem.fileSize;
      else if (initialItem.totalBytes) rec.contentLength = initialItem.totalBytes;
      if (initialItem.tabId && !rec.tabId) rec.tabId = Number(initialItem.tabId) || 0;
      rec.updatedAt = Date.now();
    }
    return rec;
  }

  getRecord(downloadId) {
    return this.records.get(Number(downloadId));
  }

  get(downloadId) {
    return this.getRecord(downloadId);
  }

  removeRecord(downloadId) {
    this.records.delete(Number(downloadId));
    this.inFlightOffers.delete(Number(downloadId));
  }
}

const downloadStateMachine = new DownloadStateMachine();


async function transferBlobToFlux(tabId, blobUrl, filename, mime, correlationId, customTransport = null) {
  let directDataB64 = "";
  let directTotalBytes = 0;
  let directMime = mime || "application/octet-stream";

  // 1a. Direct data: URL parsing (RFC 2397)
  if (blobUrl && blobUrl.startsWith("data:")) {
    try {
      const commaIdx = blobUrl.indexOf(",");
      if (commaIdx >= 0) {
        const metaPart = blobUrl.slice(5, commaIdx);
        const dataPart = blobUrl.slice(commaIdx + 1);
        if (metaPart.includes(";base64")) {
          directDataB64 = dataPart;
          directTotalBytes = Math.round(dataPart.length * 0.75);
        } else {
          const decoded = decodeURIComponent(dataPart);
          directTotalBytes = decoded.length;
          directDataB64 = btoa(unescape(encodeURIComponent(decoded)));
        }
        const mimeMatch = metaPart.match(/^([^;]+)/);
        if (mimeMatch) directMime = mimeMatch[1];
      }
    } catch (_) {}
  }

  // 1b. Direct Background Service Worker fetch for blob: URLs (immune to webpage CSP)
  if (!directDataB64 && blobUrl && blobUrl.startsWith("blob:") && typeof fetch === "function") {
    try {
      const resp = await fetch(blobUrl);
      if (resp && resp.ok) {
        const b = await resp.blob();
        directTotalBytes = b.size;
        if (b.type) directMime = b.type;
        const arrayBuf = await b.arrayBuffer();
        const bytes = new Uint8Array(arrayBuf);
        let binary = "";
        const chunk = 32768;
        for (let i = 0; i < bytes.length; i += chunk) {
          binary += String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i + chunk, bytes.length)));
        }
        directDataB64 = btoa(binary);
      }
    } catch (_) {}
  }

  if (directDataB64) {
    const transferId = "blob-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 8);
    const startRes = await sendNativeRpc("download.startChunkedTransfer", {
      taskId: transferId,
      filename: filename || "artifact_download",
      totalBytes: directTotalBytes || Math.round(directDataB64.length * 0.75),
      mimeType: directMime,
      correlationId
    }, customTransport);

    if (!startRes || !startRes.accepted) {
      throw new Error((startRes && startRes.reason) || "Chunked transfer rejected by Radium");
    }

    const effectiveTaskId = startRes.taskId || transferId;
    const chunkSize = 262144; // 256 KiB base64 chunk
    let chunkIndex = 0;
    for (let offset = 0; offset < directDataB64.length; offset += chunkSize) {
      const chunkStr = directDataB64.slice(offset, offset + chunkSize);
      await sendNativeRpc("download.transferChunk", {
        taskId: effectiveTaskId,
        chunkIndex,
        dataBase64: chunkStr
      }, customTransport);
      chunkIndex++;
    }

    const completeRes = await sendNativeRpc("download.completeChunkedTransfer", {
      taskId: effectiveTaskId,
      totalBytes: directTotalBytes || Math.round(directDataB64.length * 0.75)
    }, customTransport);

    if (!completeRes || !completeRes.success) {
      throw new Error((completeRes && completeRes.error) || "Complete chunked transfer failed");
    }

    return { success: true, taskId: effectiveTaskId, filename: completeRes.fileName || filename };
  }

  let resolvedTabId = Number(tabId) || 0;
  if (resolvedTabId <= 0 && typeof chrome !== "undefined" && chrome.tabs && typeof chrome.tabs.query === "function") {
    try {
      if (blobUrl && (blobUrl.startsWith("blob:http://") || blobUrl.startsWith("blob:https://"))) {
        const origin = new URL(blobUrl.replace(/^blob:/i, "")).origin;
        if (origin && origin !== "null") {
          const originTabs = await new Promise(r => chrome.tabs.query({ url: origin + "/*" }, r));
          if (Array.isArray(originTabs) && originTabs.length > 0) {
            const activeOriginTab = originTabs.find(t => t.active) || originTabs[0];
            if (activeOriginTab && activeOriginTab.id > 0) {
              resolvedTabId = activeOriginTab.id;
            }
          }
        }
      }
      if (resolvedTabId <= 0) {
        const activeTabs = await new Promise(r => chrome.tabs.query({ active: true, lastFocusedWindow: true }, r));
        if (Array.isArray(activeTabs) && activeTabs[0] && activeTabs[0].id > 0) {
          resolvedTabId = activeTabs[0].id;
        }
      }
    } catch (_) {}
  }

  return new Promise((resolve, reject) => {
    if (!resolvedTabId || resolvedTabId <= 0) {
      return reject(new Error("No tab available for blob data retrieval"));
    }
    if (typeof chrome === "undefined" || !chrome.tabs || typeof chrome.tabs.sendMessage !== "function") {
      return reject(new Error("chrome.tabs.sendMessage not available in environment"));
    }
    chrome.tabs.sendMessage(resolvedTabId, {
      action: "fluxFetchBlobData",
      blobUrl,
      filename,
      correlationId
    }, async (response) => {
      const lastErr = (chrome.runtime && chrome.runtime.lastError) ? chrome.runtime.lastError.message : "";
      if (lastErr || !response || !response.success || !response.dataBase64) {
        return reject(new Error(lastErr || (response && response.error) || "Failed to fetch blob from tab"));
      }
      try {
        const transferId = "blob-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 8);
        const dataB64 = response.dataBase64;
        const totalBytes = response.size || Math.round(dataB64.length * 0.75);

        const startRes = await sendNativeRpc("download.startChunkedTransfer", {
          taskId: transferId,
          filename: filename || "artifact_download",
          totalBytes,
          mimeType: response.mimeType || mime || "application/octet-stream",
          correlationId
        }, customTransport);

        if (!startRes || !startRes.accepted) {
          return reject(new Error((startRes && startRes.reason) || "Chunked transfer rejected by Radium"));
        }

        const effectiveTaskId = startRes.taskId || transferId;
        const chunkSize = 262144; // 256 KiB base64 chunk
        let chunkIndex = 0;
        for (let offset = 0; offset < dataB64.length; offset += chunkSize) {
          const chunkStr = dataB64.slice(offset, offset + chunkSize);
          await sendNativeRpc("download.transferChunk", {
            taskId: effectiveTaskId,
            chunkIndex,
            dataBase64: chunkStr
          }, customTransport);
          chunkIndex++;
        }

        const completeRes = await sendNativeRpc("download.completeChunkedTransfer", {
          taskId: effectiveTaskId,
          totalBytes
        }, customTransport);

        if (!completeRes || !completeRes.success) {
          return reject(new Error((completeRes && completeRes.error) || "Complete chunked transfer failed"));
        }

        resolve({ accepted: true, taskId: effectiveTaskId, fileName: completeRes.fileName || filename });
      } catch (err) {
        reject(err);
      }
    });
  });
}

async function onDownloadCreatedHandler(
  downloadItem,
  customTransport = null,
  cancelBrowserDownloadFn = null,
  suggestCallback = null,
  phase = "determine"
) {
  if (!downloadItem || (!downloadItem.url && !downloadItem.finalUrl && downloadItem.id === undefined)) {
    if (typeof suggestCallback === "function") try { suggestCallback(); } catch (_) {}
    return { intercepted: false, reason: "INVALID_DOWNLOAD_ITEM" };
  }

  const rec = downloadStateMachine.getOrCreateRecord(downloadItem.id, downloadItem);
  if (typeof suggestCallback === "function") {
    rec.suggestCallback = suggestCallback;
  }

  const invokeSuggest = (suggestPayload) => {
    if (typeof rec.suggestCallback === "function") {
      try { rec.suggestCallback(suggestPayload); } catch (_) {}
      rec.suggestCallback = null;
    }
  };

  // 1. Interception globally disabled check
  if (!G.enableCapture) {
    rec.state = "IGNORED";
    invokeSuggest();
    return { intercepted: false, reason: "CAPTURE_DISABLED" };
  }

  // 2. Alt-key bypass check
  if (consumeBypassIfActive(rec.tabId || downloadItem.tabId, downloadItem)) {
    rec.state = "BYPASS";
    invokeSuggest();
    logPipelineStage(rec.correlationId, "DOWNLOAD_BYPASSED_BY_USER", { downloadId: rec.downloadId, url: rec.url });
    return { intercepted: false, reason: "BYPASS_MODIFIER_ACTIVE" };
  }

  const targetUrl = downloadItem.finalUrl || downloadItem.url || rec.finalUrl || rec.url;
  rec.finalUrl = targetUrl;
  const isBlob = targetUrl.startsWith("blob:");
  const isData = targetUrl.startsWith("data:");
  const isInternal = downloadItem.byExtensionId || targetUrl.startsWith("chrome-extension:") || targetUrl.startsWith("chrome:") || targetUrl.startsWith("brave:");
  const isWebBlob = targetUrl.startsWith("blob:http://") || targetUrl.startsWith("blob:https://");

  // 3. Extension-internal URLs must stay in browser memory without disrupting user gestures.
  if (isInternal) {
    rec.state = "IGNORED";
    invokeSuggest();
    if (!customTransport && typeof chrome !== "undefined" && chrome.downloads && typeof chrome.downloads.resume === "function" && downloadItem.id !== undefined) {
      try { chrome.downloads.resume(downloadItem.id, () => { if (chrome.runtime) void chrome.runtime.lastError; }); } catch (_) {}
    }
    const mechanism = "BROWSER_INTERNAL_OR_EXTENSION";
    logPipelineStage(rec.correlationId, "DOWNLOAD_DELEGATED_TO_BROWSER", {
      downloadId: rec.downloadId,
      mechanism,
      stage: "STAGE_DELEGATION",
      filename: downloadItem.filename,
      url: targetUrl.slice(0, 64),
      reason: "URL_IS_INTERNAL_OR_EXTENSION_TRANSFERRING_VIA_BROWSER"
    });
    logPipelineStage(rec.correlationId, "DELEGATED_TO_BROWSER", {
      downloadId: rec.downloadId,
      mechanism,
      stage: "STAGE_DELEGATION",
      filename: downloadItem.filename,
      url: targetUrl.slice(0, 64),
      reason: "URL_IS_INTERNAL_OR_EXTENSION_TRANSFERRING_VIA_BROWSER"
    });
    return { intercepted: false, reason: "BROWSER_INTERNAL_PAGE", mechanism };
  }

  // 4. In Chromium/Brave/Edge, onCreated fires first. Pause the download immediately so no bytes transfer
  // before filename determination and Flux eligibility check!
  if (phase === "created") {
    if (!isBlob && G.enableCapture && typeof chrome !== "undefined" && chrome.downloads && typeof chrome.downloads.pause === "function" && downloadItem.id !== undefined) {
      try { chrome.downloads.pause(downloadItem.id, () => { if (chrome.runtime) void chrome.runtime.lastError; }); } catch (_) {}
    }
    if (typeof chrome !== "undefined" && chrome.downloads && chrome.downloads.onDeterminingFilename) {
      return { intercepted: false, reason: "DEFERRED_TO_FILENAME_DETERMINATION" };
    }
  }

  // 5. Deduplication & State Synchronization: if an offer for this exact downloadId is currently in flight, await it!
  if (downloadStateMachine.inFlightOffers.has(rec.downloadId)) {
    try {
      const existingRes = await downloadStateMachine.inFlightOffers.get(rec.downloadId);
      if (!existingRes || !existingRes.intercepted) {
        invokeSuggest();
      }
      return existingRes;
    } catch (_) {
      invokeSuggest();
      return { intercepted: false, reason: "ALREADY_HANDLED" };
    }
  }

  // If already accepted and handed off to Flux:
  if (rec.state === "HANDED_OFF" || rec.state === "ACCEPTED") {
    return { intercepted: true, reason: "ALREADY_ACCEPTED", taskId: rec.taskId };
  }
  if (rec.state === "DECLINED" || rec.state === "FALLBACK" || rec.state === "IGNORED") {
    invokeSuggest();
    return { intercepted: false, reason: rec.state };
  }

  // 6. Gather all correlation metadata across click, network response headers, and redirect chains
  const netMeta = findCorrelatedNetworkResponse(downloadItem);
  let effectiveTabId = downloadItem.tabId || (netMeta && netMeta.tabId) || rec.tabId || 0;

  const clickedInfo = findCorrelatedClick(effectiveTabId, downloadItem);
  if (effectiveTabId <= 0 && clickedInfo && clickedInfo.tabId > 0) {
    effectiveTabId = clickedInfo.tabId;
  }

  // If effectiveTabId is still unknown, attempt async resolution from matching web origin or active tab
  if (effectiveTabId <= 0 && typeof chrome !== "undefined" && chrome.tabs && typeof chrome.tabs.query === "function") {
    try {
      let originPattern = "";
      if (isWebBlob) {
        try {
          const stripped = targetUrl.replace(/^blob:/i, "");
          const origin = new URL(stripped).origin;
          if (origin && origin !== "null") originPattern = origin + "/*";
        } catch (_) {}
      }
      if (originPattern) {
        const originTabs = await new Promise(r => chrome.tabs.query({ url: originPattern }, r));
        if (Array.isArray(originTabs) && originTabs.length > 0) {
          const activeOriginTab = originTabs.find(t => t.active) || originTabs[0];
          if (activeOriginTab && activeOriginTab.id > 0) {
            effectiveTabId = activeOriginTab.id;
          }
        }
      }
      if (effectiveTabId <= 0) {
        const activeTabs = await new Promise(r => chrome.tabs.query({ active: true, lastFocusedWindow: true }, r));
        if (Array.isArray(activeTabs) && activeTabs[0] && activeTabs[0].id > 0) {
          effectiveTabId = activeTabs[0].id;
        }
      }
    } catch (_) {}
  }
  rec.tabId = effectiveTabId;

  rec.originalUrl = (clickedInfo && clickedInfo.url) ||
                    (netMeta && netMeta.originalUrl) ||
                    resolveInitialUrlFromRedirects(downloadItem.url || targetUrl) ||
                    downloadItem.url || targetUrl;

  const pageReferrer = downloadItem.referrer ||
                       (clickedInfo ? clickedInfo.referrer : "") ||
                       (netMeta ? netMeta.tabUrl : "") ||
                       (rec.tabId && G.tabUrlMap && G.tabUrlMap.get(rec.tabId)) || "";

  rec.correlationId = (clickedInfo && clickedInfo.correlationId) ||
                      (netMeta && netMeta.correlationId) ||
                      rec.correlationId;

  // Build redirect chain representation
  const chain = [];
  let curr = rec.originalUrl;
  if (G.redirectChainMap) {
    while (curr && G.redirectChainMap.has(normalizeCorrelationUrl(curr)) && chain.length < 10) {
      const nextHop = G.redirectChainMap.get(normalizeCorrelationUrl(curr));
      if (!nextHop || chain.includes(nextHop)) break;
      chain.push(nextHop);
      curr = nextHop;
    }
  }
  rec.redirectChain = chain;

  const rawFilename = downloadItem.filename ||
                      (netMeta && netMeta.filename) ||
                      (clickedInfo && clickedInfo.filename) ||
                      (typeof extractFilenameFromUrlQuery === "function" ? extractFilenameFromUrlQuery(targetUrl) : "") || "";
  const cleanFilename = sanitizeFilename(rawFilename ? rawFilename.split(/[/\\]/).pop() : "");
  rec.suggestedFilename = cleanFilename;

  const mime = ((downloadItem.mime || (netMeta && netMeta.mime) || "").split(";")[0]).trim().toLowerCase();
  rec.mime = mime;
  const fileSize = downloadItem.fileSize || downloadItem.totalBytes || (netMeta && netMeta.size) || 0;
  rec.contentLength = fileSize;
  const isAttachment = Boolean(netMeta && netMeta.isAttachment);
  rec.isAttachment = isAttachment;
  const wasExplicitLinkClick = Boolean(clickedInfo);
  rec.wasExplicitLinkClick = wasExplicitLinkClick;

  const isFrame = Boolean(downloadItem.tabId === -1 || (clickedInfo && clickedInfo.isFrame));
  const hasRedirectHop = Boolean((downloadItem.finalUrl && downloadItem.url && downloadItem.finalUrl !== downloadItem.url) || chain.length > 0);
  rec.mechanism = classifyDownloadMechanism({
    url: downloadItem.url,
    finalUrl: downloadItem.finalUrl,
    downloadItem,
    netMeta,
    clickedInfo,
    isFrame,
    hasRedirectHop,
    filenameFromContentDisposition: Boolean(netMeta && netMeta.filename)
  });

  // 7. Check eligibility with unified isDownloadEligible
  logPipelineStage(rec.correlationId, "BROWSER_DOWNLOAD_OBSERVED", {
    downloadId: rec.downloadId,
    url: targetUrl,
    filename: cleanFilename,
    fileSize,
    mime,
    mechanism: rec.mechanism,
    stage: "OBSERVED"
  });

  const eligibility = isDownloadEligible({
    url: downloadItem.url,
    finalUrl: targetUrl,
    filename: cleanFilename,
    mime,
    fileSize,
    isAttachment,
    wasExplicitLinkClick,
    hasDownloadAttr: Boolean(clickedInfo && clickedInfo.hasDownloadAttr),
    tabUrl: pageReferrer,
    phase
  });

  logPipelineStage(rec.correlationId, "DOWNLOAD_CLASSIFIED", {
    downloadId: rec.downloadId,
    eligible: eligibility.eligible,
    reason: eligibility.reason,
    filename: cleanFilename,
    mime,
    stage: "CLASSIFIED"
  });

  if (!eligibility.eligible) {
    rec.state = "IGNORED";
    invokeSuggest();
    if (!customTransport && typeof chrome !== "undefined" && chrome.downloads && typeof chrome.downloads.resume === "function" && downloadItem.id !== undefined) {
      try { chrome.downloads.resume(downloadItem.id, () => { if (chrome.runtime) void chrome.runtime.lastError; }); } catch (_) {}
    }
    logPipelineStage(rec.correlationId, "DOWNLOAD_SKIPPED_NOT_ELIGIBLE", {
      downloadId: rec.downloadId,
      url: targetUrl,
      reason: eligibility.reason,
      mechanism: rec.mechanism
    });
    return { intercepted: false, reason: eligibility.reason };
  }

  // 8. In Chromium/Brave, pause the browser download shelf so no bytes transfer while dialog is showing!
  // CRITICAL: We hold the onDeterminingFilename suggestCallback until the user makes a choice or fallback occurs.
  // Calling suggest() prematurely allows Chromium to start writing bytes to disk in the browser's download shelf.
  if (!customTransport && typeof chrome !== "undefined" && chrome.downloads && typeof chrome.downloads.pause === "function" && downloadItem.id !== undefined) {
    try { chrome.downloads.pause(downloadItem.id, () => { if (chrome.runtime) void chrome.runtime.lastError; }); } catch (_) {}
  }

  // 9. Execute Offer Flow with safe handoff & browser fallback
  rec.state = "PROMPT_REQUESTED";
  logPipelineStage(rec.correlationId, "PROMPT_REQUESTED", {
    downloadId: rec.downloadId,
    url: targetUrl,
    filename: cleanFilename,
    fileSize,
    mechanism: rec.mechanism,
    stage: "PROMPT_REQUESTED"
  });
  logPipelineStage(rec.correlationId, "DOWNLOAD_OFFER_INITIATED", {
    downloadId: rec.downloadId,
    url: targetUrl,
    originalUrl: rec.originalUrl,
    filename: cleanFilename,
    fileSize,
    mime,
    wasExplicitLinkClick,
    isAttachment,
    mechanism: rec.mechanism,
    stage: "STAGE_ELIGIBILITY"
  });

  const offerAction = async () => {
    try {
      // Collect request headers and session cookies
      const recentHeaders = findRecentRequestHeadersForUrl(targetUrl);
      const headers = Object.assign({}, recentHeaders, (netMeta && netMeta.headers) || {}, downloadItem.headers || {});
      if (pageReferrer && !headers.Referer && !headers.referer) {
        headers.Referer = pageReferrer;
      }
      const cookieHeader = await collectCookiesForDownload(targetUrl, pageReferrer);
      if (cookieHeader && !headers.Cookie && !headers.cookie) {
        headers.Cookie = cookieHeader;
      }
      rec.headers = headers;

      logPipelineStage(rec.correlationId, "NATIVE_OFFER_SENT", {
        downloadId: rec.downloadId,
        url: targetUrl,
        filename: cleanFilename,
        fileSize,
        mechanism: rec.mechanism
      });
      logPipelineStage(rec.correlationId, "DOWNLOAD_OFFER_SENT", {
        url: targetUrl,
        filename: cleanFilename,
        fileSize,
        mechanism: rec.mechanism,
        stage: "STAGE_NATIVE_RPC"
      });

      let result;
      if (isBlob || isData) {
        logPipelineStage(rec.correlationId, isData ? "DATA_URL_TRANSFER_INITIATED" : "BLOB_CHUNKED_TRANSFER_INITIATED", {
          downloadId: rec.downloadId,
          tabId: effectiveTabId,
          url: targetUrl.slice(0, 80),
          filename: cleanFilename
        });
        result = await transferBlobToFlux(effectiveTabId, targetUrl, cleanFilename, mime, rec.correlationId, customTransport);
      } else {
        result = await sendNativeRpc("download.offer", {
          url: targetUrl,
          originalUrl: rec.originalUrl,
          filename: cleanFilename,
          fileSize: fileSize > 0 ? fileSize : 0,
          mime,
          tabUrl: pageReferrer,
          bypassModifier: false,
          correlationId: rec.correlationId,
          headers,
          autoStart: false // Browser intercepted downloads must NEVER start transferring silently!
        }, customTransport);
      }

      logPipelineStage(rec.correlationId, "NATIVE_OFFER_RESPONSE", {
        downloadId: rec.downloadId,
        accepted: result ? Boolean(result.accepted) : false,
        taskId: result ? (result.taskId || "") : ""
      });

      if (result && result.accepted) {
        rec.state = "PROMPT_SHOWN";
        rec.taskId = result.taskId;

        logPipelineStage(rec.correlationId, "PROMPT_SHOWN", {
          downloadId: rec.downloadId,
          taskId: result.taskId,
          fileName: result.fileName || cleanFilename,
          stage: "PROMPT_SHOWN"
        });
        logPipelineStage(rec.correlationId, "NEW_DOWNLOAD_DIALOG_REQUESTED", {
          downloadId: rec.downloadId,
          taskId: result.taskId,
          fileName: result.fileName || cleanFilename
        });
        logPipelineStage(rec.correlationId, "NEW_DOWNLOAD_DIALOG_SHOWN", {
          downloadId: rec.downloadId,
          taskId: result.taskId,
          fileName: result.fileName || cleanFilename
        });
        logPipelineStage(rec.correlationId, "DOWNLOAD_OFFER_ACCEPTED", {
          downloadId: rec.downloadId,
          taskId: result.taskId,
          fileName: result.fileName || cleanFilename,
          mechanism: rec.mechanism,
          stage: "STAGE_DESKTOP_ACCEPTANCE"
        });
        logPipelineStage(rec.correlationId, "DOWNLOAD_ACCEPTED_BY_FLUX", {
          downloadId: rec.downloadId,
          taskId: result.taskId,
          fileName: result.fileName || cleanFilename,
          mechanism: rec.mechanism,
          stage: "STAGE_DESKTOP_ACCEPTANCE"
        });

        // Record target & original URLs in handled set with timestamp
        if (!G.handledDownloadUrls || !(G.handledDownloadUrls instanceof Map)) {
          G.handledDownloadUrls = new Map();
        }
        G.handledDownloadUrls.set(normalizeCorrelationUrl(targetUrl), Date.now());
        G.handledDownloadUrls.set(normalizeCorrelationUrl(rec.originalUrl), Date.now());

        // Hand off to Flux: cancel browser download cleanly and immediately now that Flux has accepted the task
        // and is presenting the Win32 New Download dialog on the desktop.
        rec.state = "HANDED_OFF";
        logPipelineStage(rec.correlationId, "BROWSER_HANDOFF", {
          downloadId: rec.downloadId,
          taskId: rec.taskId,
          status: "HANDED_OFF_TO_FLUX"
        });
        logPipelineStage(rec.correlationId, "HANDED_OFF", {
          downloadId: rec.downloadId,
          taskId: rec.taskId,
          status: "HANDED_OFF_TO_FLUX"
        });

        // Cancel browser download shelf cleanly.
        // Cancel browser download shelf cleanly without activating browser shelf.
        rec.suggestCallback = null;

        if (cancelBrowserDownloadFn) {
          try {
            await cancelBrowserDownloadFn(downloadItem.id);
          } catch (cancelErr) {
            logPipelineStage(rec.correlationId, "BROWSER_CANCEL_ERROR_RECOVERED", {
              downloadId: downloadItem.id,
              error: cancelErr ? cancelErr.message : String(cancelErr)
            });
          }
        } else if (typeof chrome !== "undefined" && chrome.downloads && chrome.downloads.cancel) {
          // Retry cancel without resuming in browser (calling resume causes Brave to download a duplicate copy!)
          const tryCancel = (retries = 4) => {
            chrome.downloads.cancel(downloadItem.id, () => {
              const err = (chrome.runtime && chrome.runtime.lastError) ? chrome.runtime.lastError.message : "";
              if (err && retries > 0) {
                setTimeout(() => tryCancel(retries - 1), 50);
              } else if (typeof chrome.downloads.erase === "function") {
                chrome.downloads.erase({ id: downloadItem.id }, () => {
                  if (chrome.runtime) void chrome.runtime.lastError;
                });
              }
            });
          };
          tryCancel();
        }

        return { intercepted: true, taskId: rec.taskId, fileName: result.fileName || cleanFilename, correlationId: rec.correlationId };
      }

      // Flux declined: allow native browser download to continue seamlessly
      rec.state = "BROWSER_FALLBACK";
      logPipelineStage(rec.correlationId, "FALLBACK", {
        downloadId: rec.downloadId,
        reason: (result && result.reason) || "DECLINED_BY_DESKTOP"
      });
      logPipelineStage(rec.correlationId, "DOWNLOAD_OFFER_REJECTED", {
        downloadId: rec.downloadId,
        reason: (result && result.reason) || "DECLINED_BY_DESKTOP",
        mechanism: rec.mechanism,
        stage: "STAGE_DESKTOP_ACCEPTANCE"
      });
      logPipelineStage(rec.correlationId, "DOWNLOAD_DECLINED_BY_FLUX", {
        downloadId: rec.downloadId,
        reason: (result && result.reason) || "DECLINED_BY_DESKTOP",
        mechanism: rec.mechanism,
        stage: "STAGE_DESKTOP_ACCEPTANCE"
      });
      invokeSuggest({ filename: cleanFilename });
      if (!customTransport && typeof chrome !== "undefined" && chrome.downloads && typeof chrome.downloads.resume === "function" && downloadItem.id !== undefined) {
        try { chrome.downloads.resume(downloadItem.id, () => { if (chrome.runtime) void chrome.runtime.lastError; }); } catch (_) {}
      }
      return { intercepted: false, fallbackToBrowser: true, reason: (result && result.reason) || "DECLINED_BY_DESKTOP", mechanism: rec.mechanism, correlationId: rec.correlationId };

    } catch (err) {
      // Flux unreachable or RPC error: resume native browser download safely
      rec.state = "BROWSER_FALLBACK";
      logPipelineStage(rec.correlationId, "FALLBACK", {
        downloadId: rec.downloadId,
        error: err ? err.message : String(err),
        mechanism: rec.mechanism
      });
      logPipelineStage(rec.correlationId, "INTERCEPTION_FAILED", {
        downloadId: rec.downloadId,
        error: err ? err.message : String(err),
        mechanism: rec.mechanism,
        stage: "STAGE_FAILURE_RECOVERY"
      });
      logPipelineStage(rec.correlationId, "DOWNLOAD_FALLBACK_TO_BROWSER", {
        downloadId: rec.downloadId,
        error: err ? err.message : String(err),
        mechanism: rec.mechanism,
        stage: "STAGE_FAILURE_RECOVERY"
      });
      invokeSuggest({ filename: cleanFilename });
      if (!customTransport && typeof chrome !== "undefined" && chrome.downloads && typeof chrome.downloads.resume === "function" && downloadItem.id !== undefined) {
        try { chrome.downloads.resume(downloadItem.id, () => { if (chrome.runtime) void chrome.runtime.lastError; }); } catch (_) {}
      }
      return { intercepted: false, fallbackToBrowser: true, error: err ? err.message : String(err), correlationId: rec.correlationId };
    } finally {
      downloadStateMachine.inFlightOffers.delete(rec.downloadId);
    }
  };

  const offerPromise = offerAction();
  downloadStateMachine.inFlightOffers.set(rec.downloadId, offerPromise);
  return await offerPromise;
}

// 4. Submit a detected video or stream candidate to Flux with explicit 5-way typed source handling (Defect B fix)
async function submitMediaCandidateToFlux(candidate, options = {}, customTransport = null, browserDownloadFn = null) {
  if (!candidate || typeof candidate !== "object") {
    throw new Error("Invalid media candidate");
  }

  const correlationId = options.correlationId || candidate.correlationId || generateCorrelationId("stream");
  const sourceType = candidate.sourceType || classifyMediaSourceType(candidate, candidate.mime, candidate.cachedManifestBody);

  // 5. Unsupported or unresolved media source must fail fast with a clear error
  if (sourceType === SourceType.UNSUPPORTED || candidate.isDrm || candidate.drmProtected) {
    const defaultMsg = (candidate.isDrm || candidate.drmProtected)
      ? "DRM protected streams are unsupported"
      : "Unsupported or unresolved media source";
    const err = new Error(candidate.errorMessage || defaultMsg);
    logPipelineStage(correlationId, "MEDIA_SUBMIT_REJECTED", {
      reason: err.message,
      url: candidate.url || ""
    });
    throw err;
  }

  // Resolve selected video quality (from Popup quality dropdown, Video Overlay dropdown, or options.qualityId)
  const qualitiesList = Array.isArray(candidate.qualities) ? candidate.qualities : [];
  let chosenQuality = options.selectedQuality || null;
  const reqQ = String(options.qualityId || options.resolution || "").toLowerCase();
  if (!chosenQuality && qualitiesList.length > 0) {
    if (reqQ && reqQ !== "best" && reqQ !== "auto") {
      chosenQuality = qualitiesList.find(q =>
        String(q.id || "").toLowerCase() === reqQ ||
        String(q.resolution || "").toLowerCase() === reqQ ||
        String(q.videoFilter || "").toLowerCase() === reqQ
      ) || qualitiesList.find(q =>
        (reqQ.includes("1080") && String(q.resolution || "").toLowerCase().includes("1080")) ||
        (reqQ.includes("720") && String(q.resolution || "").toLowerCase().includes("720")) ||
        (reqQ.includes("480") && String(q.resolution || "").toLowerCase().includes("480")) ||
        (reqQ.includes("360") && String(q.resolution || "").toLowerCase().includes("360"))
      ) || null;
    }
    if (!chosenQuality) {
      chosenQuality = qualitiesList.find(q => !q.isAudioOnly && String(q.resolution || "").toLowerCase().includes("1080")) ||
                      qualitiesList.find(q => !q.isAudioOnly) ||
                      qualitiesList[0];
    }
  }

  const effectiveVideoUrl = chosenQuality
    ? (chosenQuality.videoUrl || "")
    : (candidate.videoUrl || "");
  const effectiveAudioUrl = (chosenQuality && chosenQuality.pairedAudioUrl !== undefined)
    ? chosenQuality.pairedAudioUrl
    : (candidate.pairedAudioUrl || "");
  const effectiveRequiresCipher = (chosenQuality && chosenQuality.requiresCipherExtraction !== undefined)
    ? Boolean(chosenQuality.requiresCipherExtraction)
    : Boolean(candidate.requiresCipherExtraction);
  const effectiveVideoFilter = (options.videoFilter && options.videoFilter !== "best")
    ? options.videoFilter
    : ((chosenQuality && chosenQuality.videoFilter) ? chosenQuality.videoFilter : (options.videoFilter || "best"));
  const effectiveResolution = (chosenQuality && chosenQuality.resolution)
    ? chosenQuality.resolution
    : (options.resolution || candidate.resolution || "");

  // Sanitize cachedManifestBody: ONLY pass actual HLS (#EXTM3U) or DASH (<MPD) manifest text!
  // Never pass a YouTube JSON descriptor inside cachedManifestBody.
  let validCachedManifest = "";
  let extractorJson = candidate.extractorJson || "";
  if (candidate.cachedManifestBody) {
    if (sourceType === SourceType.HLS_MANIFEST && isValidHlsManifestText(candidate.cachedManifestBody)) {
      validCachedManifest = candidate.cachedManifestBody;
    } else if (sourceType === SourceType.DASH_MANIFEST && isValidDashManifestText(candidate.cachedManifestBody)) {
      validCachedManifest = candidate.cachedManifestBody;
    } else if (candidate.cachedManifestBody.trimStart().startsWith("{") && !extractorJson) {
      extractorJson = candidate.cachedManifestBody;
    }
  }

  if (sourceType === SourceType.YOUTUBE_EXTRACTION) {
    let baseDesc = {};
    if (extractorJson) {
      try { baseDesc = JSON.parse(extractorJson) || {}; } catch (_) { baseDesc = {}; }
    }
    const updatedDesc = Object.assign({}, baseDesc, {
      fluxType: "YOUTUBE_STREAM",
      sourceType: SourceType.YOUTUBE_EXTRACTION,
      videoId: candidate.videoId || baseDesc.videoId || extractYouTubeVideoId(candidate.url || candidate.tabUrl || ""),
      title: options.pageTitle || candidate.pageTitle || baseDesc.title || "youtube_video",
      watchUrl: candidate.tabUrl || baseDesc.watchUrl || candidate.url,
      resolution: effectiveResolution || baseDesc.resolution || "Auto",
      videoUrl: effectiveVideoUrl || (!chosenQuality && candidate.isGoogleVideo ? candidate.url : ""),
      audioUrl: effectiveAudioUrl,
      videoFilter: effectiveVideoFilter,
      requiresCipherExtraction: effectiveRequiresCipher,
      qualities: qualitiesList,
      selectedQuality: chosenQuality,
      selectedItag: (chosenQuality && chosenQuality.itag) ? chosenQuality.itag : 0
    });
    extractorJson = JSON.stringify(updatedDesc);
  }

  let effectiveStreamType = "HLS";
  if (sourceType === SourceType.HLS_MANIFEST) {
    effectiveStreamType = "HLS";
  } else if (sourceType === SourceType.DASH_MANIFEST) {
    effectiveStreamType = "DASH";
  } else if (sourceType === SourceType.YOUTUBE_EXTRACTION) {
    effectiveStreamType = "YOUTUBE";
  } else if (sourceType === SourceType.DIRECT_PROGRESSIVE) {
    effectiveStreamType = "HTTP_FILE";
  }

  const defaultExt = (chosenQuality && chosenQuality.ext)
    ? chosenQuality.ext
    : ((sourceType === SourceType.DIRECT_PROGRESSIVE && candidate.ext) ? candidate.ext : "mp4");

  let targetManifestUrl = candidate.url;
  if (sourceType === SourceType.YOUTUBE_EXTRACTION) {
    const isRawGoogleVideo = (candidate.url && (candidate.url.includes("googlevideo.com") || candidate.url.includes("videoplayback")));
    targetManifestUrl = (!isRawGoogleVideo && candidate.url)
      ? candidate.url
      : ((candidate.tabUrl && !candidate.tabUrl.includes("googlevideo.com"))
          ? candidate.tabUrl
          : (candidate.videoId ? `https://www.youtube.com/watch?v=${candidate.videoId}` : candidate.url));
  } else if (sourceType === SourceType.HLS_MANIFEST && chosenQuality && chosenQuality.variantUrl && !validCachedManifest) {
    targetManifestUrl = chosenQuality.variantUrl;
  } else if (sourceType === SourceType.DIRECT_PROGRESSIVE && effectiveVideoUrl) {
    targetManifestUrl = effectiveVideoUrl;
  }

  let baseTitle = options.pageTitle || candidate.pageTitle || candidate.fileName || "";
  // Strip any old bracketed resolution tag (e.g. [360p], [480p], [720p]) so quality upgrades are clean
  baseTitle = baseTitle.replace(/\s*\[\s*\d{3,4}p[_\w]*\s*\]/gi, "").trim();
  if (effectiveResolution && effectiveResolution !== "Auto" && qualitiesList.length > 1) {
    baseTitle = `${baseTitle} [${effectiveResolution}]`.trim();
  }

  let effectiveFileName = candidate.fileName || "";
  if (effectiveFileName) {
    effectiveFileName = effectiveFileName.replace(/\s*\[\s*\d{3,4}p[_\w]*\s*\]/gi, "").trim();
    const suffix = effectiveFileName.match(/\.([a-zA-Z0-9]+)$/);
    if (!suffix || /^\d+$/.test(suffix[1])) {
      const stem = suffix ? effectiveFileName.slice(0, -suffix[0].length) : effectiveFileName;
      effectiveFileName = `${stem || "video"}.${defaultExt}`;
    }
    if (effectiveResolution && effectiveResolution !== "Auto") {
      const extM = effectiveFileName.match(/\.([a-zA-Z0-9]+)$/);
      const curExt = extM ? extM[1] : defaultExt;
      const fStem = extM ? effectiveFileName.slice(0, -extM[0].length) : effectiveFileName;
      effectiveFileName = `${fStem} [${effectiveResolution}].${curExt}`;
    }
  }

  const headers = Object.assign({}, candidate.headers || {});
  if (!headers.Referer && !headers.referer) {
    if (candidate.referer) headers.Referer = candidate.referer;
    else if (candidate.tabUrl) headers.Referer = candidate.tabUrl;
  }
  if (!headers["User-Agent"] && !headers["user-agent"] && typeof navigator !== "undefined" && navigator.userAgent) {
    headers["User-Agent"] = navigator.userAgent;
  }
  if (!headers.Cookie && !headers.cookie && typeof collectCookiesForDownload === "function") {
    try {
      const cookieHeader = await collectCookiesForDownload(targetManifestUrl, candidate.referer || candidate.tabUrl || "");
      if (cookieHeader) headers.Cookie = cookieHeader;
    } catch (_) {}
  }

  const isYtTarget = (sourceType === SourceType.YOUTUBE_EXTRACTION || sourceType === "YOUTUBE" || effectiveStreamType === "YOUTUBE" || isYouTubeWatchOrShortsUrl(targetManifestUrl));

  const params = {
    candidateId: candidate.candidateId || "",
    correlationId,
    streamType: effectiveStreamType,
    sourceType,
    manifestUrl: targetManifestUrl,
    url: targetManifestUrl,
    watchUrl: (candidate.tabUrl && !candidate.tabUrl.includes("googlevideo.com"))
      ? candidate.tabUrl
      : (candidate.videoId ? `https://www.youtube.com/watch?v=${candidate.videoId}` : ""),
    videoUrl: effectiveVideoUrl,
    pairedAudioUrl: effectiveAudioUrl,
    videoId: candidate.videoId || "",
    resolution: effectiveResolution,
    qualityId: (chosenQuality && chosenQuality.id) ? chosenQuality.id : (options.qualityId || ""),
    itag: (chosenQuality && chosenQuality.itag) ? chosenQuality.itag : 0,
    selectedItag: (chosenQuality && chosenQuality.itag) ? chosenQuality.itag : 0,
    requiresCipherExtraction: effectiveRequiresCipher,
    extractorJson,
    pageTitle: baseTitle,
    fileName: effectiveFileName,
    tabUrl: candidate.tabUrl || "",
    outputFormat: (options.audioOnly || (chosenQuality && chosenQuality.isAudioOnly))
      ? (options.outputFormat || (defaultExt === "mp3" || defaultExt === "m4a" ? defaultExt : "m4a"))
      : (options.outputFormat || defaultExt),
    videoFilter: (options.audioOnly || (chosenQuality && chosenQuality.isAudioOnly)) ? "none" : effectiveVideoFilter,
    audioFilter: options.videoOnly ? "none" : (options.audioFilter || (chosenQuality && chosenQuality.isAudioOnly ? "best" : "all")),
    subtitleFilter: options.subtitleFilter || "all",
    customKeyHex: options.customKeyHex || "",
    cachedManifestBody: validCachedManifest || (isYtTarget ? extractorJson : "") || "",
    headers
  };

  logPipelineStage(correlationId, "MEDIA_SUBMIT_INITIATED", {
    candidateId: params.candidateId,
    streamType: effectiveStreamType,
    sourceType,
    url: candidate.url,
    hasManifestBody: Boolean(validCachedManifest),
    hasExtractorJson: Boolean(extractorJson)
  });

  try {
    const res = await sendNativeRpc("stream.add", params, customTransport);
    candidate.handoffStatus = "Queued in Radium";
    if (res && typeof res === "object") {
      res.taskType = res.taskType || sourceType;
    }
    return res;
  } catch (err) {
    const isDirectFile = sourceType === SourceType.DIRECT_PROGRESSIVE && !isWebpageWatchUrl(candidate.url);
    if (isDirectFile) {
      if (browserDownloadFn) {
        await browserDownloadFn({ url: candidate.url });
        candidate.handoffStatus = "Browser Fallback";
        return { accepted: false, fallbackToBrowser: true, url: candidate.url };
      } else if (typeof chrome !== "undefined" && chrome.downloads && chrome.downloads.download) {
        chrome.downloads.download({ url: candidate.url });
        candidate.handoffStatus = "Browser Fallback";
        return { accepted: false, fallbackToBrowser: true, url: candidate.url };
      }
    }
    throw err;
  }
}

var submitMediaCandidateToRadium = submitMediaCandidateToFlux;
var submitMediaCandidateToOpenIDM = submitMediaCandidateToFlux;

function resolveCandidateForDownloadRequest(tabId, msg, sender) {
  let tabList = G.cacheData.get(tabId) || [];
  const effectiveTabUrl = msg.tabUrl || (sender && sender.tab && sender.tab.url) || G.tabUrlMap.get(tabId) || "";
  const effectiveTitle = (msg.pageTitle || (sender && sender.tab && sender.tab.title) || "video").replace(/\s*-\s*YouTube$/i, "").trim();

  // 1. Explicit candidateId match (e.g. from Popup UI or resolved video overlay)
  if (msg.candidateId) {
    let byId = tabList.find(c => c.candidateId === msg.candidateId);
    if (!byId) {
      for (const [tId, cList] of G.cacheData.entries()) {
        if (Array.isArray(cList)) {
          byId = cList.find(c => c.candidateId === msg.candidateId);
          if (byId) break;
        }
      }
    }
    if (byId) return { candidate: byId };
  }

  // 2. Direct non-blob URL match
  if (msg.url && !msg.url.startsWith("blob:") && !isWebpageWatchUrl(msg.url)) {
    const normUrl = normalizeMediaUrl(msg.url);

    // If downloading on a YouTube watch page or via googlevideo URL, prefer the rich YouTube extraction candidate
    if (isYouTubeWatchOrShortsUrl(effectiveTabUrl) || normUrl.includes("googlevideo.com") || normUrl.includes("videoplayback")) {
      const ytCand = tabList.find(c => (c.streamType === "YOUTUBE" || c.sourceType === SourceType.YOUTUBE_EXTRACTION) && Array.isArray(c.qualities) && c.qualities.length > 0);
      if (ytCand) {
        if (effectiveTitle && (!ytCand.pageTitle || ytCand.pageTitle === "video" || ytCand.pageTitle === "youtube_video")) {
          ytCand.pageTitle = effectiveTitle;
        }
        return { candidate: ytCand };
      }
    }

    const existing = tabList.find(c => c.url === normUrl || c.url === msg.url);
    if (existing) {
      if (effectiveTitle && (!existing.pageTitle || existing.pageTitle === "video")) {
        existing.pageTitle = effectiveTitle;
      }
      return { candidate: existing };
    }
    const isHls = normUrl.toLowerCase().includes(".m3u8") || normUrl.toLowerCase().includes(".m3u");
    const isDash = normUrl.toLowerCase().includes(".mpd");
    return {
      candidate: {
        url: normUrl,
        ext: extractUrlExtension(normUrl) || "mp4",
        streamType: isHls ? "HLS" : (isDash ? "DASH" : "HTTP_FILE"),
        sourceType: isHls ? SourceType.HLS_MANIFEST : (isDash ? SourceType.DASH_MANIFEST : SourceType.DIRECT_PROGRESSIVE),
        pageTitle: effectiveTitle,
        tabUrl: effectiveTabUrl,
        headers: effectiveTabUrl ? { Referer: effectiveTabUrl } : {}
      }
    };
  }

  // 3. Fallback candidate search across all cached tabs if current tabList is empty
  if (tabList.length === 0 && effectiveTabUrl) {
    for (const [tId, cList] of G.cacheData.entries()) {
      if (tId !== tabId && Array.isArray(cList)) {
        const matching = cList.filter(c => c.tabUrl === effectiveTabUrl);
        if (matching.length > 0) {
          tabList = matching;
          break;
        }
      }
    }
  }

  // 4. Highest-confidence detected candidate on this tab (prioritizing video / streams over stray audio)
  if (tabList.length > 0) {
    const nonAudio = tabList.filter(c => !c.isAudioOnly);
    const pool = nonAudio.length > 0 ? nonAudio : tabList;
    const streams = pool.filter(c => c.sourceType === SourceType.YOUTUBE_EXTRACTION || c.streamType === "YOUTUBE" || c.streamType === "HLS" || c.streamType === "DASH");
    const best = (streams.length > 0 ? streams[0] : pool[0]);
    if (effectiveTitle && (!best.pageTitle || best.pageTitle === "video")) {
      best.pageTitle = effectiveTitle;
    }
    return { candidate: best };
  }

  // 5. Check if the page reported a specific player error (e.g. YouTube unplayable / private / DRM video)
  if (G.playerErrors && G.playerErrors.has(tabId)) {
    return { error: G.playerErrors.get(tabId) };
  }

  // 6. If on a valid YouTube watch/shorts/embed URL, route via YOUTUBE_EXTRACTION worker
  const targetPageUrl = effectiveTabUrl || msg.frameUrl || msg.url || "";
  if (isYouTubeWatchOrShortsUrl(targetPageUrl)) {
    const videoId = extractYouTubeVideoId(targetPageUrl);
    const ytCand = {
      url: targetPageUrl,
      ext: "mp4",
      streamType: "YOUTUBE",
      sourceType: SourceType.YOUTUBE_EXTRACTION,
      videoId,
      requiresCipherExtraction: true,
      pageTitle: effectiveTitle || "youtube_video",
      tabUrl: targetPageUrl,
      headers: { Referer: targetPageUrl }
    };
    addOrDeduplicateMediaCandidate(tabId, ytCand);
    updateTabBadge(tabId);
    return { candidate: ytCand };
  }

  // 7. Unresolved blob: video with no manifest or network stream
  if (msg.isMseBlob || (msg.blobUrl && msg.blobUrl.startsWith("blob:"))) {
    return {
      error: "No supported download source found for this blob: video player."
    };
  }

  return {
    error: "No downloadable media stream detected on this tab yet. Start video playback and try again."
  };
}

// 5. Message listener handling Popup and Content Script RPCs
function handleRuntimeMessage(msg, sender, sendResponse) {
  if (!msg || !msg.action) return false;

  const tabId = (msg.tabId !== undefined && msg.tabId !== null && msg.tabId !== 0)
    ? Number(msg.tabId)
    : (sender && sender.tab && sender.tab.id ? sender.tab.id : 0);

  if (sender && sender.tab && sender.tab.id && sender.tab.url) {
    updateTabDocument(sender.tab.id, sender.tab.url);
  }

  if (msg.action === "getMediaList") {
    ensureSessionRestored().then(async () => {
      let resolvedTabId = tabId;
      if (resolvedTabId <= 0 && typeof chrome !== "undefined" && chrome.tabs && typeof chrome.tabs.query === "function") {
        try {
          const activeTabs = await new Promise((res) => chrome.tabs.query({ active: true, lastFocusedWindow: true }, res));
          if (activeTabs && activeTabs[0] && activeTabs[0].id) {
            resolvedTabId = activeTabs[0].id;
          }
        } catch (_) {}
      }
      if (resolvedTabId <= 0 && G.cacheData.size > 0) {
        for (const k of G.cacheData.keys()) {
          resolvedTabId = k;
          break;
        }
      }

      const targetTabUrl = msg.tabUrl || G.tabUrlMap.get(resolvedTabId) || "";
      if (resolvedTabId > 0 && targetTabUrl) updateTabDocument(resolvedTabId, targetTabUrl);

      // If no candidates are cached yet, probe the active tab content script (or YouTube watch URL)
      let currentList = G.cacheData.get(resolvedTabId) || [];
      if (targetTabUrl && currentList.some(item => item && item.tabUrl && item.tabUrl !== targetTabUrl)) {
        G.cacheData.delete(resolvedTabId);
        currentList = [];
        updateTabBadge(resolvedTabId);
      }
      if (currentList.length === 0 && resolvedTabId > 0 && typeof chrome !== "undefined" && chrome.tabs && typeof chrome.tabs.sendMessage === "function") {
        try {
          await new Promise((resolve) => {
            chrome.tabs.sendMessage(resolvedTabId, { action: "probePageMedia" }, () => {
              if (chrome.runtime) void chrome.runtime.lastError;
              resolve();
            });
          });
        } catch (_) {}
        currentList = G.cacheData.get(resolvedTabId) || [];
      }
      for (const candidate of currentList) {
        if (!Number(candidate.size) && typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.id) {
          probeMediaSize(resolvedTabId, candidate).catch(() => {});
        }
      }

      if (currentList.length === 0 && isYouTubeWatchOrShortsUrl(targetTabUrl) && !isSiteDisabled(targetTabUrl) && !(G.playerErrors && G.playerErrors.has(resolvedTabId))) {
        const vidId = extractYouTubeVideoId(targetTabUrl);
        const defaultQualities = [
          { id: "1080p_mp4", res: "1080p", label: "1080p Full HD (MP4)", bw: 4200000, h: 1080, ext: "mp4" },
          { id: "720p_mp4", res: "720p", label: "720p HD (MP4)", bw: 2200000, h: 720, ext: "mp4" },
          { id: "480p_mp4", res: "480p", label: "480p SD (MP4)", bw: 1100000, h: 480, ext: "mp4" },
          { id: "360p_mp4", res: "360p", label: "360p Standard (MP4)", bw: 650000, h: 360, ext: "mp4" },
          { id: "audio-only", res: "Audio", label: "Audio Only (128 Kbps)", bw: 128000, h: 0, ext: "m4a", isAudioOnly: true }
        ].map(p => ({
          id: p.id,
          label: p.label,
          resolution: p.res,
          width: p.h > 0 ? Math.round((p.h * 16) / 9) : 0,
          height: p.h,
          bandwidth: p.bw,
          size: Math.round((p.bw / 8) * 180),
          sizeEstimated: true,
          isAudioOnly: Boolean(p.isAudioOnly),
          ext: p.ext,
          requiresCipherExtraction: true,
          videoFilter: p.h > 0 ? `${p.h}p` : "audio"
        }));

        const ytDesc = {
          fluxType: "YOUTUBE_STREAM",
          sourceType: SourceType.YOUTUBE_EXTRACTION,
          videoId: vidId,
          title: "YouTube Video",
          watchUrl: targetTabUrl,
          resolution: "1080p",
          requiresCipherExtraction: true,
          size: defaultQualities[0].size,
          qualities: defaultQualities
        };

        addOrDeduplicateMediaCandidate(resolvedTabId, {
          url: targetTabUrl,
          ext: "mp4",
          streamType: "YOUTUBE",
          sourceType: SourceType.YOUTUBE_EXTRACTION,
          videoId: vidId,
          requiresCipherExtraction: true,
          pageTitle: "YouTube Video",
          tabUrl: targetTabUrl,
          size: defaultQualities[0].size,
          qualities: defaultQualities,
          extractorJson: JSON.stringify(ytDesc),
          cachedManifestBody: JSON.stringify(ytDesc),
          headers: { Referer: targetTabUrl }
        });
        updateTabBadge(resolvedTabId);
      }

      return sendNativeRpc("system.handshake", { protocolVersion: "1.0" })
        .then((hs) => {
          sendResponse({
            success: true,
            desktopConnected: Boolean(hs && hs.protocolVersion),
            workersAvailable: Boolean(hs && hs.workersAvailable),
            captureEnabled: G.enableCapture,
            siteDisabled: isSiteDisabled(targetTabUrl),
            playerError: (G.playerErrors && G.playerErrors.get(resolvedTabId)) || "",
            candidates: G.cacheData.get(resolvedTabId) || []
          });
        })
        .catch((err) => {
          const errCategory = err && err.category ? err.category : categorizeNativeError(err ? err.message : "");
          sendResponse({
            success: true,
            desktopConnected: false,
            workersAvailable: false,
            nativeErrorCategory: errCategory,
            nativeErrorMessage: err ? err.message : "",
            captureEnabled: G.enableCapture,
            siteDisabled: isSiteDisabled(targetTabUrl),
            playerError: (G.playerErrors && G.playerErrors.get(resolvedTabId)) || "",
            candidates: G.cacheData.get(resolvedTabId) || []
          });
        });
    });
    return true;
  }

  if (msg.action === "discoverBlobMedia") {
    ensureSessionRestored().then(() => {
      const sourceTabUrl = msg.tabUrl || (sender && sender.tab ? sender.tab.url : "") || G.tabUrlMap.get(tabId) || "";
      const list = G.cacheData.get(tabId) || [];
      if (list.length === 0 && isYouTubeWatchOrShortsUrl(sourceTabUrl) && !isSiteDisabled(sourceTabUrl) && !(G.playerErrors && G.playerErrors.has(tabId))) {
        const cleanTitle = (msg.pageTitle || "youtube_video").replace(/\s*-\s*YouTube$/i, "").trim();
        addOrDeduplicateMediaCandidate(tabId, {
          url: sourceTabUrl,
          ext: "mp4",
          streamType: "YOUTUBE",
          sourceType: SourceType.YOUTUBE_EXTRACTION,
          videoId: extractYouTubeVideoId(sourceTabUrl),
          requiresCipherExtraction: true,
          pageTitle: cleanTitle || "youtube_video",
          tabUrl: sourceTabUrl,
          headers: { Referer: sourceTabUrl }
        });
        updateTabBadge(tabId);
      }
      const updatedList = G.cacheData.get(tabId) || [];
      const best = updatedList.length > 0 ? updatedList[0] : null;
      if (G.blobMediaMap && msg.blobUrl) {
        if (!G.blobMediaMap.has(tabId)) G.blobMediaMap.set(tabId, new Map());
        G.blobMediaMap.get(tabId).set(msg.blobUrl, {
          blobUrl: msg.blobUrl,
          resolvedCandidateId: best ? best.candidateId : "",
          status: best ? "resolved" : "unresolved",
          timestamp: Date.now()
        });
      }
      logPipelineStage(msg.correlationId, "BLOB_MEDIA_DISCOVERY", {
        tabId,
        blobUrl: msg.blobUrl || "",
        hasCandidate: Boolean(best),
        candidateId: best ? best.candidateId : ""
      });
      sendResponse({
        success: true,
        hasCandidate: Boolean(best),
        candidate: best,
        playerError: (G.playerErrors && G.playerErrors.get(tabId)) || ""
      });
    });
    return true;
  }

  if (msg.action === "addMedia") {
    if (!msg.url || (isWebpageWatchUrl(msg.url) && msg.streamType !== "YOUTUBE" && msg.streamType !== "SITE_EXTRACTION" && msg.sourceType !== SourceType.YOUTUBE_EXTRACTION)) {
      sendResponse({ success: false, added: false, error: "Invalid or webpage URL" });
      return true;
    }
    const sourceTabUrl = msg.tabUrl || (sender && sender.tab ? sender.tab.url : "") || G.tabUrlMap.get(tabId) || "";
    if (isSiteDisabled(sourceTabUrl)) {
      sendResponse({ success: false, added: false, error: "Site capture disabled" });
      return true;
    }

    if (G.playerErrors) G.playerErrors.delete(tabId);

    const inferredStreamType = msg.streamType || (msg.url.includes(".m3u8") ? "HLS" : (msg.url.includes(".mpd") ? "DASH" : "HTTP_FILE"));
    const candidate = {
      url: normalizeMediaUrl(msg.url),
      blobUrl: msg.blobUrl || "",
      ext: msg.ext || extractUrlExtension(msg.url) || "mp4",
      streamType: inferredStreamType,
      sourceType: msg.sourceType || classifyMediaSourceType({
        url: msg.url,
        streamType: inferredStreamType,
        mime: msg.mime || "",
        cachedManifestBody: msg.cachedManifestBody || "",
        requiresCipherExtraction: msg.requiresCipherExtraction,
        pairedAudioUrl: msg.pairedAudioUrl
      }),
      mime: msg.mime || "",
      size: msg.size || 0,
      itag: msg.itag || "",
      isAudioOnly: Boolean(msg.isAudioOnly),
      isGoogleVideo: Boolean(msg.isGoogleVideo),
      isAdaptiveTrack: Boolean(msg.isAdaptiveTrack),
      requiresAudioMux: Boolean(msg.requiresAudioMux),
      requiresCipherExtraction: Boolean(msg.requiresCipherExtraction),
      videoId: msg.videoId || extractYouTubeVideoId(msg.url),
      resolution: msg.resolution || "",
      qualities: Array.isArray(msg.qualities) ? msg.qualities : [],
      videoUrl: msg.videoUrl || "",
      pairedAudioUrl: msg.pairedAudioUrl || "",
      extractorJson: msg.extractorJson || "",
      tabId,
      tabUrl: sourceTabUrl,
      pageTitle: (msg.pageTitle || (sender && sender.tab ? sender.tab.title : "video") || "video").replace(/\s*-\s*YouTube$/i, "").trim(),
      cachedManifestBody: msg.cachedManifestBody || "",
      correlationId: msg.correlationId || generateCorrelationId("addmedia"),
      headers: sourceTabUrl ? { Referer: sourceTabUrl } : {}
    };
    const res = addOrDeduplicateMediaCandidate(tabId, candidate);
    if (res && res.candidate && (!res.candidate.qualities || res.candidate.qualities.length === 0)) {
      autoProbeManifestQualities(tabId, res.candidate).catch(() => {});
    }
  if (res && res.candidate && !Number(res.candidate.size) && typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.id) {
    probeMediaSize(tabId, res.candidate).catch(() => {});
  }
    updateTabBadge(tabId);
    persistSessionState().catch(() => {});
    notifyPopupMediaUpdated(tabId);
    sendResponse({ success: true, added: res.added, count: (G.cacheData.get(tabId) || []).length, candidate: res.candidate });
    return true;
  }

  if (msg.action === "playerError") {
    if (G.playerErrors && msg.reason) {
      G.playerErrors.set(tabId, msg.reason);
    }
    sendResponse({ success: true });
    return true;
  }

  if (msg.action === "addKey") {
    if (msg.key) {
      if (!G.capturedKeys.has(tabId)) G.capturedKeys.set(tabId, new Set());
      G.capturedKeys.get(tabId).add(msg.key);
    }
    sendResponse({ success: true });
    return true;
  }

  if (msg.action === "updateCandidateQuality") {
    const list = G.cacheData.get(tabId) || [];
    const cand = list.find(c => c.candidateId === msg.candidateId);
    if (cand) {
      cand.selectedQualityId = msg.qualityId;
      if (msg.resolution) cand.resolution = msg.resolution;
      persistSessionState().catch(() => {});
    }
    sendResponse({ success: true });
    return true;
  }

  if (msg.action === "downloadMedia") {
    ensureSessionRestored().then(async () => {
      const correlationId = msg.correlationId || generateCorrelationId("dlmedia");
      const resolved = resolveCandidateForDownloadRequest(tabId, msg, sender);
      if (resolved.error || !resolved.candidate) {
        logPipelineStage(correlationId, "MEDIA_RESOLVE_FAILED", {
          tabId,
          error: resolved.error || "Media candidate not found"
        });
        sendResponse({ success: false, error: resolved.error || "Media candidate not found", correlationId });
        return;
      }

      // Prevent duplicate task submissions if a download for this candidate/quality is already in flight
      const candIdentifier = resolved.candidate.candidateId || resolved.candidate.url || "";
      const inFlightKey = `${tabId}:${candIdentifier}:${msg.qualityId || ""}`;
      G.inFlightMediaDownloads = G.inFlightMediaDownloads || new Map();
      if (G.inFlightMediaDownloads.has(inFlightKey)) {
        try {
          const prevResult = await G.inFlightMediaDownloads.get(inFlightKey);
          sendResponse({ success: true, result: prevResult, correlationId, deduplicated: true });
        } catch (err) {
          sendResponse({ success: false, error: err.message || "Failed to submit download to Radium", correlationId });
        }
        return;
      }

      const dlPromise = submitMediaCandidateToFlux(resolved.candidate, {
        pageTitle: msg.pageTitle,
        qualityId: msg.qualityId || "",
        resolution: msg.resolution || "",
        videoFilter: msg.videoFilter || "best",
        selectedQuality: msg.selectedQuality || null,
        outputFormat: msg.outputFormat || "",
        correlationId
      });

      G.inFlightMediaDownloads.set(inFlightKey, dlPromise);
      dlPromise.finally(() => {
        setTimeout(() => {
          if (G.inFlightMediaDownloads) G.inFlightMediaDownloads.delete(inFlightKey);
        }, 8000);
      });

      try {
        const result = await dlPromise;
        persistSessionState().catch(() => {});
        notifyPopupMediaUpdated(tabId);
        sendResponse({ success: true, result, correlationId });
      } catch (err) {
        sendResponse({ success: false, error: err.message || "Failed to submit download to Radium", correlationId });
      }
    });
    return true;
  }

  if (msg.action === "clearMedia") {
    G.cacheData.delete(tabId);
    if (G.playerErrors) G.playerErrors.delete(tabId);
    if (G.blobMediaMap) G.blobMediaMap.delete(tabId);
    updateTabBadge(tabId);
    persistSessionState().catch(() => {});
    sendResponse({ success: true });
    return true;
  }

  if (msg.action === "setSiteCapture") {
    if (msg.tabUrl) {
      setSiteDisabled(msg.tabUrl, !msg.enabled);
      persistSessionState().catch(() => {});
    }
    sendResponse({ success: true, enabled: msg.enabled });
    return true;
  }

  if (msg.action === "bypassNextDownload") {
    recordBypassIntent(tabId, msg.url || "");
    sendResponse({ success: true });
    return true;
  }

  if (msg.action === "linkClicked") {
    const rec = recordClickedLink(tabId, msg);
    sendResponse({ success: true, correlationId: rec ? rec.correlationId : "" });
    return true;
  }

  if (msg.action === "interceptBlobDownload") {
    const correlationId = msg.correlationId || generateCorrelationId("blob");
    const filename = sanitizeFilename(msg.filename || "artifact_download");
    let mime = msg.mimeType || "application/octet-stream";
    let dataB64 = msg.dataBase64 || "";
    let totalBytes = msg.size || 0;

    if (!G.enableCapture || isSiteDisabled(msg.pageUrl || "")) {
      sendResponse({ success: false, fallbackToBrowser: true, reason: "CAPTURE_DISABLED" });
      return true;
    }

    logPipelineStage(correlationId, "BLOB_DIRECT_INTERCEPTION_STARTED", {
      url: msg.url,
      filename,
      fileSize: totalBytes,
      mime
    });

    (async () => {
      try {
        // If dataBase64 was not supplied by page context (e.g. blob generated in Web Worker under strict CSP),
        // fetch it directly here in the Background Service Worker where page CSP does not apply.
        if (!dataB64 && msg.url && msg.url.startsWith("blob:") && typeof fetch === "function") {
          const resp = await fetch(msg.url);
          if (!resp.ok) {
            throw new Error(`Failed to fetch blob in background: ${resp.status}`);
          }
          const blob = await resp.blob();
          totalBytes = blob.size;
          if (blob.type) mime = blob.type;
          const arrayBuf = await blob.arrayBuffer();
          const bytes = new Uint8Array(arrayBuf);
          let binary = "";
          const chunk = 32768;
          for (let i = 0; i < bytes.length; i += chunk) {
            binary += String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i + chunk, bytes.length)));
          }
          dataB64 = btoa(binary);
        }

        if (!totalBytes && dataB64) {
          totalBytes = Math.round(dataB64.length * 0.75);
        }

        const transferId = "blob-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 8);
        const startRes = await sendNativeRpc("download.startChunkedTransfer", {
          taskId: transferId,
          filename,
          totalBytes,
          mimeType: mime,
          pageUrl: msg.pageUrl || "",
          correlationId
        });

        if (!startRes || !startRes.accepted) {
          throw new Error((startRes && startRes.reason) || "Chunked transfer rejected by Radium");
        }

        const effectiveTaskId = startRes.taskId || transferId;
        const chunkSize = 262144; // 256 KiB base64 chunk
        let chunkIndex = 0;
        for (let offset = 0; offset < dataB64.length; offset += chunkSize) {
          const chunkStr = dataB64.slice(offset, offset + chunkSize);
          await sendNativeRpc("download.transferChunk", {
            taskId: effectiveTaskId,
            chunkIndex,
            dataBase64: chunkStr
          });
          chunkIndex++;
        }

        const completeRes = await sendNativeRpc("download.completeChunkedTransfer", {
          taskId: effectiveTaskId,
          totalBytes
        });

        if (!completeRes || !completeRes.success) {
          throw new Error((completeRes && completeRes.error) || "Complete chunked transfer failed");
        }

        logPipelineStage(correlationId, "BLOB_DIRECT_INTERCEPTION_COMPLETED", {
          taskId: effectiveTaskId,
          filename: completeRes.fileName || filename,
          totalBytes
        });

        sendResponse({ success: true, taskId: effectiveTaskId, filename: completeRes.fileName || filename });
      } catch (err) {
        logPipelineStage(correlationId, "BLOB_DIRECT_INTERCEPTION_FAILED", {
          error: err ? err.message : String(err),
          fallbackToBrowser: true
        });
        sendResponse({ success: false, fallbackToBrowser: true, error: err ? err.message : String(err) });
      }
    })();
    return true;
  }

  if (msg.action === "startChunkedDownload") {
    const correlationId = msg.correlationId || generateCorrelationId("chunk");
    const mechanism = (msg.mimeType && msg.mimeType.includes("data")) ? "BROWSER_DATA_DOWNLOAD" : "BROWSER_BLOB_OR_DATA";

    if (!G.enableCapture || isSiteDisabled(msg.pageUrl || "")) {
      logPipelineStage(correlationId, "UNSUPPORTED", {
        mechanism,
        stage: "STAGE_ELIGIBILITY",
        reason: "CAPTURE_DISABLED_OR_SITE_DISABLED"
      });
      sendResponse({ accepted: false, reason: "CAPTURE_DISABLED" });
      return true;
    }

    logPipelineStage(correlationId, "BACKGROUND_MESSAGE_RECEIVED", {
      action: msg.action,
      filename: msg.filename,
      totalBytes: msg.totalBytes,
      mechanism,
      stage: "STAGE_HEADER_COLLECTION"
    });

    sendNativeRpc("download.startChunkedTransfer", {
      taskId: msg.transferId,
      filename: msg.filename,
      totalBytes: msg.totalBytes,
      mimeType: msg.mimeType,
      pageUrl: msg.pageUrl,
      correlationId
    }).then(res => {
      logPipelineStage(correlationId, "CHUNKED_TRANSFER_STARTED", {
        taskId: res ? res.taskId : "",
        mechanism,
        stage: "STAGE_NATIVE_RPC"
      });
      sendResponse(Object.assign({ accepted: true }, res));
    }).catch(err => {
      const errCategory = err && err.category ? err.category : categorizeNativeError(err ? err.message : "");
      logPipelineStage(correlationId, "CAPTURE_FAILED", {
        error: err ? err.message : String(err),
        errorCategory: errCategory,
        mechanism,
        stage: "STAGE_FAILURE_RECOVERY"
      });
      sendResponse({ accepted: false, error: err ? err.message : "RPC_FAILED" });
    });
    return true;
  }

  if (msg.action === "transferDownloadChunk") {
    sendNativeRpc("download.transferChunk", {
      taskId: msg.transferId,
      chunkIndex: msg.chunkIndex,
      dataBase64: msg.dataBase64
    }).then(res => {
      sendResponse(Object.assign({ received: true }, res));
    }).catch(err => {
      sendResponse({ received: false, error: err ? err.message : "CHUNK_FAILED" });
    });
    return true;
  }

  if (msg.action === "completeChunkedDownload") {
    const correlationId = msg.correlationId || generateCorrelationId("chunk");
    const mechanism = "BROWSER_BLOB_OR_DATA";

    sendNativeRpc("download.completeChunkedTransfer", {
      taskId: msg.transferId,
      totalBytes: msg.totalBytes
    }).then(res => {
      if (res && res.success) {
        logPipelineStage(correlationId, "CAPTURED_BY_FLUX", {
          taskId: res.taskId,
          fileName: res.fileName,
          saveDir: res.saveDir,
          totalBytes: res.totalBytes,
          mechanism,
          stage: "STAGE_COMPLETION"
        });
        sendResponse(Object.assign({ success: true }, res));
      } else {
        const reason = (res && res.error) || "INGEST_FAILED";
        logPipelineStage(correlationId, "CAPTURE_FAILED", {
          reason,
          mechanism,
          stage: "STAGE_FAILURE_RECOVERY"
        });
        sendResponse({ success: false, error: reason });
      }
    }).catch(err => {
      const errCategory = err && err.category ? err.category : categorizeNativeError(err ? err.message : "");
      logPipelineStage(correlationId, "CAPTURE_FAILED", {
        error: err ? err.message : String(err),
        errorCategory: errCategory,
        mechanism,
        stage: "STAGE_FAILURE_RECOVERY"
      });
      sendResponse({ success: false, error: err ? err.message : "RPC_FAILED" });
    });
    return true;
  }

  if (msg.action === "abortChunkedDownload") {
    const correlationId = msg.correlationId || generateCorrelationId("chunk");
    sendNativeRpc("download.abortChunkedTransfer", {
      taskId: msg.transferId,
      reason: msg.reason || ""
    }).then(res => {
      logPipelineStage(correlationId, "CAPTURE_FAILED", {
        reason: msg.reason || "ABORTED_BY_CLIENT",
        mechanism: "BROWSER_BLOB_OR_DATA",
        stage: "STAGE_FAILURE_RECOVERY"
      });
      sendResponse({ aborted: true });
    }).catch(() => {
      sendResponse({ aborted: false });
    });
    return true;
  }

  if (msg.action === "interceptDownloadClick") {
    const rec = recordClickedLink(tabId, msg);
    const correlationId = msg.correlationId || (rec && rec.correlationId) || generateCorrelationId("dl");

    const targetUrl = msg.url || "";
    const isBlobOrData = isBlobOrDataUrl(targetUrl);
    const mechanism = isBlobOrData ? "BROWSER_BLOB_OR_DATA" : (msg.mechanism || (msg.isFrame ? "FRAME_ORIGINATED" : "ORDINARY_LINK"));

    logPipelineStage(correlationId, "BACKGROUND_MESSAGE_RECEIVED", {
      action: msg.action,
      url: isBlobOrData ? targetUrl.slice(0, 48) + "..." : targetUrl,
      tabId,
      referrer: msg.referrer,
      mechanism,
      stage: "STAGE_HEADER_COLLECTION"
    });

    // Guard: Never pass invalid internal references or ephemeral browser URLs to the native downloader!
    if (isBlobOrData || targetUrl.startsWith("chrome-extension:") || targetUrl.startsWith("chrome:") || targetUrl.startsWith("brave:")) {
      logPipelineStage(correlationId, "DOWNLOAD_DELEGATED_TO_BROWSER", {
        mechanism,
        stage: "STAGE_DELEGATION",
        url: targetUrl.slice(0, 48) + "...",
        reason: "BLOB_AND_DATA_URLS_MUST_STAY_IN_BROWSER_FOR_BYTE_TRANSFER"
      });
      sendResponse({ accepted: false, reason: "BLOB_OR_DATA_URL_MUST_STAY_IN_BROWSER", mechanism, correlationId });
      return true;
    }

    if (!G.enableCapture || isSiteDisabled(msg.referrer || targetUrl)) {
      logPipelineStage(correlationId, "DOWNLOAD_OFFER_REJECTED", {
        url: targetUrl,
        reason: "CAPTURE_DISABLED",
        mechanism,
        stage: "STAGE_ELIGIBILITY"
      });
      sendResponse({ accepted: false, reason: "CAPTURE_DISABLED", mechanism, correlationId });
      return true;
    }

    (async () => {
      try {
        const rawFilename = msg.filename || "";
        const cleanFilename = rawFilename ? rawFilename.split(/[/\\]/).pop() : "";

        // Collect request headers (Referer, Cookie, Authorization)
        const recentHeaders = findRecentRequestHeadersForUrl(targetUrl);
        const headers = Object.assign({}, recentHeaders);
        if (msg.referrer && !headers.Referer && !headers.referer) {
          headers.Referer = msg.referrer;
        }
        const cookieHeader = await collectCookiesForDownload(targetUrl, msg.referrer);
        if (cookieHeader && !headers.Cookie && !headers.cookie) {
          headers.Cookie = cookieHeader;
        }

        // Deduplication & in-flight tracking: prevent duplicate offers on rapid double-clicks
        G.inFlightClickOffers = G.inFlightClickOffers || new Map();
        const clickKey = normalizeCorrelationUrl(targetUrl);
        if (G.inFlightClickOffers.has(clickKey)) {
          try {
            const prev = await G.inFlightClickOffers.get(clickKey);
            sendResponse(Object.assign({}, prev, { deduplicated: true, mechanism }));
            return;
          } catch (_) {}
        }

        logPipelineStage(correlationId, "DOWNLOAD_OFFER_SENT", {
          url: targetUrl,
          filename: cleanFilename,
          headersCount: Object.keys(headers).length,
          mechanism,
          stage: "STAGE_NATIVE_RPC"
        });

        const offerPromise = sendNativeRpc("download.offer", {
          url: targetUrl,
          originalUrl: targetUrl,
          filename: cleanFilename,
          fileSize: 0,
          mime: "",
          tabUrl: msg.referrer || "",
          bypassModifier: false,
          correlationId,
          headers
        });

        G.inFlightClickOffers.set(clickKey, offerPromise);
        offerPromise.finally(() => {
          setTimeout(() => {
            if (G.inFlightClickOffers) G.inFlightClickOffers.delete(clickKey);
          }, 8000);
        });

        const result = await offerPromise;

        if (result && result.accepted) {
          if (!G.handledDownloadUrls || !(G.handledDownloadUrls instanceof Map)) {
            G.handledDownloadUrls = new Map();
          }
          G.handledDownloadUrls.set(normalizeCorrelationUrl(targetUrl), Date.now());
          if (msg.url) G.handledDownloadUrls.set(normalizeCorrelationUrl(msg.url), Date.now());

          logPipelineStage(correlationId, "DOWNLOAD_OFFER_ACCEPTED", {
            url: targetUrl,
            taskId: result.taskId,
            fileName: result.fileName,
            mechanism,
            stage: "STAGE_DESKTOP_ACCEPTANCE"
          });
          logPipelineStage(correlationId, "DOWNLOAD_ACCEPTED_BY_FLUX", {
            url: targetUrl,
            taskId: result.taskId,
            fileName: result.fileName,
            mechanism,
            stage: "STAGE_DESKTOP_ACCEPTANCE"
          });
          logPipelineStage(correlationId, "CAPTURED_BY_FLUX", {
            url: targetUrl,
            taskId: result.taskId,
            fileName: result.fileName,
            mechanism,
            stage: "STAGE_COMPLETION"
          });

          sendResponse({ accepted: true, taskId: result.taskId, fileName: result.fileName, mechanism, correlationId });
        } else {
          const reason = (result && result.reason) || "DECLINED_BY_DESKTOP";
          logPipelineStage(correlationId, "DOWNLOAD_OFFER_REJECTED", {
            url: targetUrl,
            reason,
            mechanism,
            stage: "STAGE_DESKTOP_ACCEPTANCE"
          });
          logPipelineStage(correlationId, "DOWNLOAD_DECLINED_BY_FLUX", {
            url: targetUrl,
            reason,
            mechanism,
            stage: "STAGE_DESKTOP_ACCEPTANCE"
          });
          logPipelineStage(correlationId, "CAPTURE_FAILED", {
            url: targetUrl,
            reason,
            mechanism,
            stage: "STAGE_FAILURE_RECOVERY"
          });
          sendResponse({ accepted: false, reason, mechanism, correlationId });
        }
      } catch (err) {
        const errCategory = err && err.category ? err.category : categorizeNativeError(err ? err.message : "");
        logPipelineStage(correlationId, "INTERCEPTION_FAILED", {
          url: msg.url,
          error: err ? err.message : String(err),
          errorCategory: errCategory,
          mechanism,
          stage: "STAGE_FAILURE_RECOVERY"
        });
        logPipelineStage(correlationId, "CAPTURE_FAILED", {
          url: msg.url,
          error: err ? err.message : String(err),
          errorCategory: errCategory,
          mechanism,
          stage: "STAGE_FAILURE_RECOVERY"
        });
        sendResponse({
          accepted: false,
          error: err ? err.message : String(err),
          errorCategory: errCategory,
          mechanism,
          correlationId
        });
      }
    })();
    return true;
  }

  if (msg.action === "getDiagnosticLogs") {
    sendResponse({ success: true, logs: G.diagnosticLogs || [] });
    return true;
  }

  if (msg.action === "pingDesktop") {
    sendNativeRpc("system.handshake", { protocolVersion: "1.0" })
      .then((hs) => sendResponse({ connected: true, workersAvailable: Boolean(hs && hs.workersAvailable) }))
      .catch((err) => {
        const cat = err && err.category ? err.category : categorizeNativeError(err ? err.message : "");
        sendResponse({ connected: false, error: err ? err.message : "Offline", nativeErrorCategory: cat });
      });
    return true;
  }

  return false;
}

// 6. Register extension runtime listeners
if (typeof chrome !== "undefined") {
  // WebRequest Listeners
  if (chrome.webRequest) {
    if (chrome.webRequest.onBeforeRequest) {
      chrome.webRequest.onBeforeRequest.addListener(
        (details) => {
          if (details && details.type === "main_frame" && details.tabId > 0 && details.url) {
            updateTabDocument(details.tabId, details.url);
          }
        },
        { urls: ["*://*/*"] }
      );
    }

    if (chrome.webRequest.onBeforeRedirect) {
      chrome.webRequest.onBeforeRedirect.addListener(
        (details) => {
          if (details && details.url && details.redirectUrl) {
            recordRedirectHop(details.url, details.redirectUrl, details.requestId, resolveEffectiveTabId(details));
          }
        },
        { urls: ["*://*/*"] }
      );
    }

    const extraReqOpts = ["requestHeaders"];
    if (chrome.webRequest.OnBeforeSendHeadersOptions && chrome.webRequest.OnBeforeSendHeadersOptions.EXTRA_HEADERS) {
      extraReqOpts.push("extraHeaders");
    }
    chrome.webRequest.onBeforeSendHeaders.addListener(
      onBeforeSendHeadersHandler,
      { urls: ["*://*/*"] },
      extraReqOpts
    );

    const extraRespOpts = ["responseHeaders"];
    if (chrome.webRequest.OnHeadersReceivedOptions && chrome.webRequest.OnHeadersReceivedOptions.EXTRA_HEADERS) {
      extraRespOpts.push("extraHeaders");
    }
    chrome.webRequest.onHeadersReceived.addListener(
      onHeadersReceivedHandler,
      { urls: ["*://*/*"] },
      extraRespOpts
    );
  }

  // History API and fragment navigation are the page transitions used by
  // many SPA video sites. Scope these updates to the tab reported by the API.
  if (chrome.webNavigation) {
    const handleDocumentNavigation = (details) => {
      if (details && details.frameId === 0) updateTabDocument(details.tabId, details.url);
    };
    if (chrome.webNavigation.onHistoryStateUpdated) {
      chrome.webNavigation.onHistoryStateUpdated.addListener(handleDocumentNavigation, { url: [{ schemes: ["http", "https"] }] });
    }
    if (chrome.webNavigation.onReferenceFragmentUpdated) {
      chrome.webNavigation.onReferenceFragmentUpdated.addListener(handleDocumentNavigation, { url: [{ schemes: ["http", "https"] }] });
    }
  }

  // Runtime Message Listener
  if (chrome.runtime && chrome.runtime.onMessage) {
    chrome.runtime.onMessage.addListener(handleRuntimeMessage);
  }

  // Downloads API Listeners (Defect C fix: hold suggest() until download.offer completes, avoid race between onCreated and onDeterminingFilename)
  if (chrome.downloads) {
    if (chrome.downloads.onDeterminingFilename) {
      chrome.downloads.onDeterminingFilename.addListener((downloadItem, suggest) => {
        onDownloadCreatedHandler(downloadItem, null, null, suggest, "determine").catch(() => {
          try { if (typeof suggest === "function") suggest(); } catch (_) {}
        });
        return true;
      });
    }
    if (chrome.downloads.onCreated) {
      chrome.downloads.onCreated.addListener((downloadItem) => {
        onDownloadCreatedHandler(downloadItem, null, null, null, "created").catch(() => {});
      });
    }
  }

  // Tab Lifecycle Listeners
  if (chrome.tabs) {
    if (chrome.tabs.onActivated) {
      chrome.tabs.onActivated.addListener((activeInfo) => {
        updateTabBadge(activeInfo.tabId);
      });
    }
    if (chrome.tabs.onUpdated) {
      chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
        if (tab && tab.url) {
          updateTabDocument(tabId, tab.url);
        }
        if (changeInfo && changeInfo.status === "complete") {
          updateTabBadge(tabId);
        }
      });
    }
    if (chrome.tabs.onRemoved) {
      chrome.tabs.onRemoved.addListener((tabId) => {
        G.cacheData.delete(tabId);
        G.tabUrlMap.delete(tabId);
        if (G.playerErrors) G.playerErrors.delete(tabId);
        if (G.clickedLinksByTab) G.clickedLinksByTab.delete(tabId);
        if (G.capturedKeys && typeof G.capturedKeys.delete === "function") G.capturedKeys.delete(tabId);
        if (G.bypassTabs) G.bypassTabs.delete(tabId);
        if (G.blobMediaMap) G.blobMediaMap.delete(tabId);
        persistSessionState().catch(() => {});
      });
    }
  }

  // Restore state on service worker startup
  ensureSessionRestored();
}

if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    persistSessionState,
    restoreSessionState,
    ensureSessionRestored,
    sendNativeRpc,
    updateTabBadge,
    updateTabDocument,
    probeMediaSize,
    onBeforeSendHeadersHandler,
    onHeadersReceivedHandler,
    onDownloadCreatedHandler,
    submitMediaCandidateToFlux,
    submitMediaCandidateToOpenIDM,
    resolveCandidateForDownloadRequest,
    handleRuntimeMessage,
    downloadStateMachine,
    transferBlobToFlux
  };
}
