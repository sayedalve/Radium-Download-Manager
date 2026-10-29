// SPDX-License-Identifier: Apache-2.0
// Adapted from xifangczy/cat-catch (js/popup.js) for Radium Cosmic Dark Popup UI.
// Supports media type filtering (All / Streams / Direct), live count badges, search filtering,
// resolution/size formatting, real-time background query, and batch/single download handoff.

function formatMediaSize(bytes) {
  if (!bytes || bytes <= 0) return "Live / Stream";
  if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(1) + " GiB";
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + " MiB";
  if (bytes >= 1024) return (bytes / 1024).toFixed(1) + " KiB";
  return bytes + " B";
}

function extractSourceHost(urlStr) {
  try {
    return new URL(urlStr).hostname;
  } catch (_) {
    return "web";
  }
}

function escapeHtml(str) {
  return String(str || "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function isStreamCandidate(item) {
  if (!item) return false;
  return item.streamType === "HLS" ||
         item.streamType === "DASH" ||
         item.streamType === "MSS" ||
         item.streamType === "YOUTUBE" ||
         item.streamType === "SITE_EXTRACTION" ||
         item.streamType === "VIMEO";
}

function filterPopupCandidates(candidates, filterMode = "all", searchQuery = "") {
  if (!Array.isArray(candidates)) return [];
  const q = (searchQuery || "").trim().toLowerCase();
  return candidates.filter(item => {
    const isStream = isStreamCandidate(item);
    if (filterMode === "stream" && !isStream) return false;
    if (filterMode === "direct" && isStream) return false;
    if (q) {
      const title = (item.pageTitle || item.fileName || item.url || "").toLowerCase();
      const badge = (item.streamType || item.ext || "").toLowerCase();
      const res = (item.resolution || "").toLowerCase();
      return title.includes(q) || badge.includes(q) || res.includes(q);
    }
    return true;
  });
}

function renderDetectedMediaList(candidates, options = {}) {
  const {
    filterMode = "all",
    searchQuery = "",
    loading = false,
    desktopConnected = true,
    errorMessage = ""
  } = options;

  if (loading) {
    return '<p class="empty" data-state="loading">Probing tab media &amp; connecting to Radium...</p>';
  }
  if (errorMessage) {
    return `<div class="error-banner" data-state="error">${escapeHtml(errorMessage)}</div>`;
  }

  const connBanner = desktopConnected
    ? ""
    : '<div class="error-banner" data-state="disconnected">Radium Desktop is offline. Direct files will fall back to browser download.</div>';

  if (filterMode === "links") {
    const pageLinks = options.pageLinks || [];
    const q = (searchQuery || "").trim().toLowerCase();
    const filteredLinks = pageLinks.filter(l => !q || (l.filename && l.filename.toLowerCase().includes(q)) || (l.url && l.url.toLowerCase().includes(q)) || (l.category && l.category.toLowerCase().includes(q)));
    if (filteredLinks.length === 0) {
      return '<p class="empty" data-state="empty">No downloadable links found on this page.</p>';
    }
    const linksHtml = filteredLinks.map(l => {
      const catBadge = escapeHtml(l.category || "File");
      const safeName = escapeHtml(l.filename || "file");
      const safeUrl = escapeHtml(l.url);
      const extStr = escapeHtml(String(l.ext || "file").toUpperCase());
      return `<div class="media-item link-item" data-url="${safeUrl}" data-stream="false">
        <div class="media-meta">
          <div class="media-title-row">
            <span class="badge badge-stream">${catBadge}</span>
            <strong class="title" title="${safeName}">${safeName}</strong>
          </div>
          <div class="media-sub-row">
            <span class="meta-mono">${extStr}</span>
            <span class="item-variants" title="${safeUrl}">${safeUrl.length > 50 ? safeUrl.slice(0, 48) + "..." : safeUrl}</span>
          </div>
        </div>
        <div class="media-actions">
          <button class="dl-btn dl-link-btn" data-url="${safeUrl}" data-name="${safeName}" aria-label="${desktopConnected ? 'Download ' + safeName + ' in Radium' : 'Download ' + safeName + ' in browser'}">${desktopConnected ? "Download in Radium" : "Browser Download"}</button>
        </div>
      </div>`;
    }).join("\n");
    return connBanner + linksHtml;
  }

  const filtered = filterPopupCandidates(candidates, filterMode, searchQuery);
  if (filtered.length === 0) {
    return '<p class="empty" data-state="empty">No media detected on this tab yet. Play a video or start a download to capture streams.</p>';
  }

  const itemsHtml = filtered.map(item => {
    const isStream = isStreamCandidate(item);
    const badge = isStream
      ? item.streamType
      : (item.ext ? item.ext.toUpperCase() : (item.streamType || "MEDIA"));
    const badgeClass = isStream ? "badge badge-stream" : "badge";
    const rawName = item.pageTitle || item.fileName || (item.url ? item.url.split("?")[0].split("/").pop() : "media");
    const safeName = escapeHtml(rawName);
    const sourceHost = escapeHtml(extractSourceHost(item.tabUrl || item.url));
    const activeQuality = (Array.isArray(item.qualities) && item.qualities.length > 0)
      ? (item.qualities.find(q =>
          String(q.id || "").toLowerCase() === String(item.resolution || "").toLowerCase() ||
          String(q.resolution || "").toLowerCase() === String(item.resolution || "").toLowerCase() ||
          String(q.videoFilter || "").toLowerCase() === String(item.resolution || "").toLowerCase()
        ) || item.qualities[0])
      : null;
    const effSize = activeQuality ? (activeQuality.size || item.size || 0) : (item.size || 0);
    const effEstimated = activeQuality ? Boolean(activeQuality.sizeEstimated) : false;
    const szPrefix = effEstimated ? " &middot; ~" : " &middot; ";
    const unknownSizeLabel = item.sizeStatus === "checking" ? "Checking size…" : "Size unknown";
    const sizeLabel = effSize > 0 ? (effEstimated ? "~" : "") + formatMediaSize(effSize) : unknownSizeLabel;
    const qualityTag = item.resolution
      ? `<span class="meta-mono">${escapeHtml(item.resolution)}${szPrefix}${escapeHtml(sizeLabel)}</span>`
      : `<span class="meta-mono">${escapeHtml(sizeLabel)}</span>`;
    const variantNote = (item.variants && item.variants.length > 0)
      ? `<span class="item-variants">(+${item.variants.length} sub-playlists grouped)</span>`
      : "";
    const statusText = escapeHtml(item.handoffStatus || "Ready");
    const isAlreadyQueued = (item.handoffStatus === "Queued in Radium");
    const actionLabel = isAlreadyQueued
      ? "Queued in Radium"
      : ((!desktopConnected && !isStream) ? "Browser Download" : "Download in Radium");
    const btnClass = isAlreadyQueued ? "dl-btn dl-queued" : "dl-btn";

    let qualityDropdownHtml = "";
    if (Array.isArray(item.qualities) && item.qualities.length > 1) {
      const topNonAudio = item.qualities.find(q => !q.isAudioOnly) || item.qualities[0];
      const defaultId = String((topNonAudio && (topNonAudio.id || topNonAudio.resolution)) || "").toLowerCase();
      const selKey = String(item.selectedQualityId || item.resolution || "").toLowerCase();
      let hasSel = false;

      const optsHtml = item.qualities.map(q => {
        const qid = escapeHtml(q.id || q.resolution || "");
        const szText = q.size > 0 ? (` · ${q.sizeEstimated ? "~" : ""}${formatMediaSize(q.size)}`) : "";
        const qlbl = escapeHtml((q.label || q.resolution || qid) + szText);
        let isSelected = false;
        if (!hasSel && selKey && (
          String(q.id || "").toLowerCase() === selKey ||
          String(q.resolution || "").toLowerCase() === selKey ||
          String(q.videoFilter || "").toLowerCase() === selKey
        )) {
          isSelected = true;
          hasSel = true;
        }
        return { qid, qlbl, isSelected, isDefault: (String(q.id || "").toLowerCase() === defaultId) };
      });

      if (!hasSel) {
        for (const opt of optsHtml) {
          if (opt.isDefault) { opt.isSelected = true; hasSel = true; break; }
        }
        if (!hasSel && optsHtml.length > 0) optsHtml[0].isSelected = true;
      }

      const optsStr = optsHtml.map(o => `<option value="${o.qid}" ${o.isSelected ? "selected" : ""}>${o.qlbl}</option>`).join("");
      qualityDropdownHtml = `<select class="quality-select" data-id="${escapeHtml(item.candidateId)}" aria-label="Select stream quality">${optsStr}</select>`;
    } else if (Array.isArray(item.qualities) && item.qualities.length === 1) {
      const singleQ = item.qualities[0];
      const singleSz = singleQ.size > 0 ? (` · ${singleQ.sizeEstimated ? "~" : ""}${formatMediaSize(singleQ.size)}`) : "";
      const singleLbl = escapeHtml((singleQ.label || singleQ.resolution || item.resolution || "Single stream") + singleSz);
      qualityDropdownHtml = `<span class="meta-mono" style="margin-right:6px;" title="Detected quality">${singleLbl}</span>`;
    }

    return `<div class="media-item" data-id="${escapeHtml(item.candidateId)}" data-stream="${isStream}">
      <div class="media-meta">
        <div class="media-title-row">
          <span class="${badgeClass}">${escapeHtml(badge)}</span>
          <strong class="title" title="${safeName}">${safeName}</strong>
        </div>
        <div class="media-sub-row">
          <span>${sourceHost}</span>
          ${qualityTag}
          <span class="item-status">${statusText}</span>
          ${variantNote}
        </div>
      </div>
      <div class="media-actions">
        ${qualityDropdownHtml}
        <button class="${btnClass}" data-url="${escapeHtml(item.url)}" data-id="${escapeHtml(item.candidateId)}" aria-label="${escapeHtml(actionLabel)}: ${safeName}">${actionLabel}</button>
      </div>
    </div>`;
  }).join("\n");

  return connBanner + itemsHtml;
}

// Interactive DOM Controller for Popup
function initPopupUI() {
  if (typeof document === "undefined") return;

  let currentTabId = 0;
  let currentTabUrl = "";
  let currentCandidates = [];
  let currentTabLinks = [];
  let activeFilterMode = "all";
  let activeSearchQuery = "";
  let isDesktopConnected = false;
  let activeErrorMessage = "";
  let isSelectDropdownOpen = false;
  let activeSelectCandId = null;
  let pendingRefreshRequested = false;

  const mediaListEl = document.getElementById("mediaList");
  const connectionBadge = document.getElementById("connectionBadge");
  const siteToggle = document.getElementById("siteToggle");
  const searchInput = document.getElementById("searchInput");
  const clearListBtn = document.getElementById("clearListBtn");
  const batchDownloadBtn = document.getElementById("batchDownloadBtn");
  const footerSummary = document.getElementById("footerSummary");
  const countAllEl = document.getElementById("countAll");
  const countStreamEl = document.getElementById("countStream");
  const countDirectEl = document.getElementById("countDirect");
  const countLinksEl = document.getElementById("countLinks");
  const filterTabs = document.querySelectorAll(".tab-btn");

  function fetchPageLinks() {
    if (typeof chrome !== "undefined" && chrome.tabs && typeof chrome.tabs.sendMessage === "function" && currentTabId > 0) {
      try {
        chrome.tabs.sendMessage(currentTabId, { action: "extractPageLinks" }, (resp) => {
          if (typeof chrome !== "undefined" && chrome.runtime) void chrome.runtime.lastError;
          if (resp && Array.isArray(resp.links)) {
            currentTabLinks = resp.links;
            if (countLinksEl) countLinksEl.textContent = String(currentTabLinks.length);
            if (activeFilterMode === "links") refreshListDisplay();
          }
        });
      } catch (_) {}
    }
  }

  function updateCounters() {
    const allCount = Array.isArray(currentCandidates) ? currentCandidates.length : 0;
    const streamCount = filterPopupCandidates(currentCandidates, "stream", "").length;
    const directCount = filterPopupCandidates(currentCandidates, "direct", "").length;
    const linksCount = currentTabLinks.length;

    let filteredCount = 0;
    if (activeFilterMode === "links") {
      const q = (activeSearchQuery || "").trim().toLowerCase();
      filteredCount = currentTabLinks.filter(l => !q || (l.filename && l.filename.toLowerCase().includes(q)) || (l.url && l.url.toLowerCase().includes(q)) || (l.category && l.category.toLowerCase().includes(q))).length;
    } else {
      filteredCount = filterPopupCandidates(currentCandidates, activeFilterMode, activeSearchQuery).length;
    }

    if (countAllEl) countAllEl.textContent = String(allCount);
    if (countStreamEl) countStreamEl.textContent = String(streamCount);
    if (countDirectEl) countDirectEl.textContent = String(directCount);
    if (countLinksEl) countLinksEl.textContent = String(linksCount);
    if (footerSummary) {
      footerSummary.textContent = filteredCount === 1 ? "1 item" : `${filteredCount} items`;
    }
    if (batchDownloadBtn) {
      batchDownloadBtn.disabled = (filteredCount === 0);
    }
    if (clearListBtn) {
      clearListBtn.disabled = (allCount === 0 && linksCount === 0);
    }
  }

  function refreshListDisplay() {
    updateCounters();
    if (!mediaListEl) return;

    // Guard: Never destroy DOM while a quality dropdown is active/expanded!
    const activeEl = document.activeElement;
    const isSelectActive = isSelectDropdownOpen || (activeEl && (activeEl.classList?.contains("quality-select") || activeEl.tagName === "SELECT"));
    if (isSelectActive) {
      pendingRefreshRequested = true;
      return;
    }

    const newHtml = renderDetectedMediaList(currentCandidates, {
      filterMode: activeFilterMode,
      searchQuery: activeSearchQuery,
      desktopConnected: isDesktopConnected,
      errorMessage: activeErrorMessage,
      pageLinks: currentTabLinks
    });

    // Zero DOM churn if HTML output is unchanged
    if (mediaListEl.innerHTML === newHtml) {
      return;
    }

    const prevScrollTop = mediaListEl.scrollTop;
    mediaListEl.innerHTML = newHtml;
    mediaListEl.scrollTop = prevScrollTop;
    attachItemClickListeners();
  }

  function attachItemClickListeners() {
    if (!mediaListEl) return;
    const qualitySelects = mediaListEl.querySelectorAll(".quality-select");
    qualitySelects.forEach(sel => {
      sel.addEventListener("mousedown", () => {
        isSelectDropdownOpen = true;
        activeSelectCandId = sel.getAttribute("data-id");
      });
      sel.addEventListener("pointerdown", () => {
        isSelectDropdownOpen = true;
        activeSelectCandId = sel.getAttribute("data-id");
      });
      sel.addEventListener("focus", () => {
        activeSelectCandId = sel.getAttribute("data-id");
      });
      sel.addEventListener("change", (ev) => {
        isSelectDropdownOpen = false;
        activeSelectCandId = null;
        const selTarget = ev.currentTarget || ev.target;
        const candId = selTarget.getAttribute("data-id");
        const cand = currentCandidates.find(c => c.candidateId === candId);
        if (cand && Array.isArray(cand.qualities)) {
          const val = String(selTarget.value || "").toLowerCase();
          const chosen = cand.qualities.find(q =>
            String(q.id || "").toLowerCase() === val ||
            String(q.resolution || "").toLowerCase() === val ||
            String(q.videoFilter || "").toLowerCase() === val
          );
          if (chosen) {
            cand.resolution = chosen.resolution || chosen.id;
            cand.selectedQualityId = chosen.id;
            cand.videoFilter = chosen.videoFilter || chosen.resolution || chosen.id;
            cand.size = chosen.size || cand.size;
            const itemBox = selTarget.closest ? selTarget.closest(".media-item") : null;
            const metaMono = itemBox ? itemBox.querySelector(".meta-mono") : null;
            if (metaMono) {
              const chosenSize = chosen.size || cand.size || 0;
              const szPrefix = chosen.sizeEstimated ? " · ~" : " · ";
              metaMono.textContent = cand.resolution + (chosenSize > 0 ? szPrefix + formatMediaSize(chosenSize) : "");
            }
            if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.sendMessage) {
              chrome.runtime.sendMessage({
                action: "updateCandidateQuality",
                tabId: currentTabId,
                candidateId: cand.candidateId,
                qualityId: chosen.id,
                resolution: chosen.resolution
              }, () => {
                if (chrome.runtime) void chrome.runtime.lastError;
              });
            }
          }
        }
        if (pendingRefreshRequested) {
          pendingRefreshRequested = false;
          setTimeout(refreshListDisplay, 40);
        }
      });
      sel.addEventListener("blur", () => {
        setTimeout(() => {
          if (document.activeElement !== sel) {
            isSelectDropdownOpen = false;
            activeSelectCandId = null;
            if (pendingRefreshRequested) {
              pendingRefreshRequested = false;
              refreshListDisplay();
            }
          }
        }, 100);
      });
    });

    const buttons = mediaListEl.querySelectorAll(".dl-btn");
    buttons.forEach(btn => {
      btn.addEventListener("click", (ev) => {
        const targetBtn = ev.currentTarget || ev.target;
        const candidateId = targetBtn.getAttribute("data-id");
        const url = targetBtn.getAttribute("data-url");

        const itemContainer = targetBtn.closest ? targetBtn.closest(".media-item") : null;
        const qualitySelect = itemContainer ? itemContainer.querySelector(".quality-select") : null;
        const itemCand = currentCandidates.find(c => c.candidateId === candidateId);
        const selectedQualityId = String(qualitySelect ? qualitySelect.value : (itemCand ? (itemCand.selectedQualityId || itemCand.resolution) : "")).toLowerCase();
        let chosenQ = null;
        if (itemCand && Array.isArray(itemCand.qualities)) {
          chosenQ = itemCand.qualities.find(q =>
            String(q.id || "").toLowerCase() === selectedQualityId ||
            String(q.resolution || "").toLowerCase() === selectedQualityId ||
            String(q.videoFilter || "").toLowerCase() === selectedQualityId
          ) || itemCand.qualities[0];
          if (chosenQ) {
            itemCand.resolution = chosenQ.resolution || chosenQ.id;
            itemCand.selectedQualityId = chosenQ.id;
          }
        }

        targetBtn.textContent = "Connecting...";
        targetBtn.disabled = true;

        if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.sendMessage) {
          chrome.runtime.sendMessage({
            action: "downloadMedia",
            candidateId,
            qualityId: selectedQualityId,
            resolution: chosenQ ? chosenQ.resolution : (itemCand ? itemCand.resolution : ""),
            videoFilter: chosenQ ? (chosenQ.videoFilter || chosenQ.resolution) : "best",
            selectedQuality: chosenQ,
            url,
            tabId: currentTabId,
            tabUrl: currentTabUrl
          }, (resp) => {
            const lastErr = chrome.runtime.lastError ? chrome.runtime.lastError.message : null;
            if (!lastErr && resp && resp.success) {
              const isFallback = Boolean(resp.result && resp.result.fallbackToBrowser);
              targetBtn.textContent = isFallback ? "Browser Download" : "Queued in Radium";
              targetBtn.classList.add("dl-queued");
              const taskId = (resp.result && resp.result.taskId) ? resp.result.taskId : "";
              if (taskId) targetBtn.setAttribute("data-task-id", taskId);
              const itemCand = currentCandidates.find(c => c.candidateId === candidateId);
              if (itemCand) itemCand.handoffStatus = isFallback ? "Browser Fallback" : "Queued in Radium";
            } else {
              const errMsg = lastErr || (resp && resp.error) || "Failed";
              targetBtn.textContent = "Failed";
              targetBtn.title = errMsg;
              targetBtn.disabled = false;
            }
          });
        }
      });
    });

    const linkBtns = mediaListEl.querySelectorAll(".dl-link-btn");
    linkBtns.forEach(btn => {
      btn.addEventListener("click", (ev) => {
        const targetBtn = ev.currentTarget || ev.target;
        const linkUrl = targetBtn.getAttribute("data-url");
        const linkName = targetBtn.getAttribute("data-name");
        targetBtn.textContent = "Connecting...";
        targetBtn.disabled = true;

        if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.sendMessage) {
          chrome.runtime.sendMessage({
            action: "downloadMedia",
            url: linkUrl,
            pageTitle: linkName,
            isMseBlob: false,
            tabId: currentTabId,
            tabUrl: currentTabUrl
          }, (resp) => {
            const lastErr = chrome.runtime.lastError ? chrome.runtime.lastError.message : null;
            if (!lastErr && resp && resp.success) {
              const isFallback = Boolean(resp.result && resp.result.fallbackToBrowser);
              targetBtn.textContent = isFallback ? "Browser Download" : "Queued in Radium";
              targetBtn.classList.add("dl-queued");
            } else {
              targetBtn.textContent = "Failed";
              targetBtn.disabled = false;
            }
          });
        }
      });
    });
  }

  function updateConnectionBadge(connected, errCategory = "") {
    isDesktopConnected = Boolean(connected);
    if (!connectionBadge) return;
    if (isDesktopConnected) {
      connectionBadge.textContent = "Desktop Connected";
      connectionBadge.className = "conn-badge conn-connected";
      connectionBadge.title = "Radium desktop application is running and communicating via Native Messaging.";
    } else if (errCategory === "UNAUTHORIZED_EXTENSION_ORIGIN") {
      connectionBadge.textContent = "Origin Unauthorized";
      connectionBadge.className = "conn-badge conn-warning";
      connectionBadge.title = "This extension ID is not authorized in the Native Messaging Host manifest. Check registry or manifest allowed_origins.";
    } else if (errCategory === "MISSING_HOST_REGISTRATION") {
      connectionBadge.textContent = "Host Misconfigured";
      connectionBadge.className = "conn-badge conn-warning";
      connectionBadge.title = "Native Messaging Host is not registered in the Windows registry (HKCU\\Software\\Google\\Chrome\\NativeMessagingHosts).";
    } else {
      connectionBadge.textContent = "Desktop Offline";
      connectionBadge.className = "conn-badge conn-disconnected";
      connectionBadge.title = "Radium desktop application is not running or named pipe IPC is unreachable.";
    }
  }

  function fetchTabData(explicitTabId = null) {
    if (typeof chrome === "undefined" || !chrome.tabs || !chrome.runtime) return;

    const queryAndLoad = (tabId, tabUrl) => {
      currentTabId = tabId;
      currentTabUrl = tabUrl || "";
      chrome.runtime.sendMessage({
        action: "getMediaList",
        tabId: currentTabId,
        tabUrl: currentTabUrl
      }, (res) => {
        const lastErr = chrome.runtime.lastError ? chrome.runtime.lastError.message : null;
        if (lastErr || !res) {
          updateConnectionBadge(false);
          activeErrorMessage = lastErr ? ("Extension service worker error: " + lastErr) : "";
          refreshListDisplay();
          return;
        }
        updateConnectionBadge(res.desktopConnected, res.nativeErrorCategory);
        if (siteToggle) {
          siteToggle.checked = Boolean(res.captureEnabled && !res.siteDisabled);
        }
        // Preserve user's active quality selections across refreshes
        const prevSelections = new Map();
        (currentCandidates || []).forEach(c => {
          if (c.selectedQualityId) prevSelections.set(c.candidateId, c.selectedQualityId);
        });
        currentCandidates = (res.candidates || []).map(c => {
          if (prevSelections.has(c.candidateId)) {
            c.selectedQualityId = prevSelections.get(c.candidateId);
          }
          return c;
        });
        activeErrorMessage = (currentCandidates.length === 0 && res.playerError) ? res.playerError : "";
        fetchPageLinks();
        refreshListDisplay();
      });
    };

    const urlParams = (typeof window !== "undefined" && window.location)
      ? new URLSearchParams(window.location.search)
      : null;
    const paramTabId = explicitTabId || (urlParams && urlParams.get("tabId") ? Number(urlParams.get("tabId")) : null);
    const paramTabUrl = (urlParams && urlParams.get("tabUrl")) || "";

    if (paramTabId && paramTabId > 0) {
      queryAndLoad(paramTabId, paramTabUrl);
      return;
    }

    chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
      if (tabs && tabs[0]) {
        let targetTab = tabs[0];
        if (targetTab.url && targetTab.url.startsWith("chrome-extension://")) {
          chrome.tabs.query({ currentWindow: true }, (allTabs) => {
            const nonExt = (allTabs || []).find(t => t.url && !t.url.startsWith("chrome-extension://"));
            if (nonExt) targetTab = nonExt;
            queryAndLoad(targetTab.id, targetTab.url || "");
          });
          return;
        }
        queryAndLoad(targetTab.id, targetTab.url || "");
      } else {
        chrome.tabs.query({ active: true, lastFocusedWindow: true }, (lastTabs) => {
          if (lastTabs && lastTabs[0]) {
            let targetTab = lastTabs[0];
            if (targetTab.url && targetTab.url.startsWith("chrome-extension://")) {
              chrome.tabs.query({ lastFocusedWindow: true }, (allTabs) => {
                const nonExt = (allTabs || []).find(t => t.url && !t.url.startsWith("chrome-extension://"));
                if (nonExt) targetTab = nonExt;
                queryAndLoad(targetTab.id, targetTab.url || "");
              });
              return;
            }
            queryAndLoad(targetTab.id, targetTab.url || "");
          } else {
            queryAndLoad(0, "");
          }
        });
      }
    });
  }

  // Listen for live updates from background service worker while popup is open
  if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.onMessage) {
    let updateDebounceTimer = null;
    chrome.runtime.onMessage.addListener((msg) => {
      if (msg && msg.action === "mediaUpdated" && (!msg.tabId || msg.tabId === currentTabId || (Array.isArray(msg.tabs) && msg.tabs.includes(currentTabId)))) {
        if (updateDebounceTimer) clearTimeout(updateDebounceTimer);
        updateDebounceTimer = setTimeout(() => {
          updateDebounceTimer = null;
          fetchTabData(currentTabId);
        }, 80);
      }
    });
  }

  // Dismiss dropdown interaction guard when clicking anywhere outside quality select
  document.addEventListener("click", (ev) => {
    if (!ev.target || !ev.target.classList || !ev.target.classList.contains("quality-select")) {
      isSelectDropdownOpen = false;
      activeSelectCandId = null;
      if (pendingRefreshRequested) {
        pendingRefreshRequested = false;
        refreshListDisplay();
      }
    }
  });

  // Filter tab buttons with WAI-ARIA arrow navigation
  function setActiveTab(tab, moveFocus = false) {
    if (!tab) return;
    filterTabs.forEach(t => {
      t.classList.remove("active");
      t.setAttribute("aria-selected", "false");
      t.setAttribute("tabindex", "-1");
    });
    tab.classList.add("active");
    tab.setAttribute("aria-selected", "true");
    tab.setAttribute("tabindex", "0");
    if (moveFocus) tab.focus();

    activeFilterMode = tab.getAttribute("data-filter") || "all";
    if (activeFilterMode === "links" && currentTabLinks.length === 0) {
      fetchPageLinks();
    }
    refreshListDisplay();
  }

  filterTabs.forEach((tab, idx) => {
    tab.addEventListener("click", () => {
      setActiveTab(tab, false);
    });

    tab.addEventListener("keydown", (ev) => {
      let targetIdx = -1;
      if (ev.key === "ArrowRight" || ev.key === "ArrowDown") {
        targetIdx = (idx + 1) % filterTabs.length;
      } else if (ev.key === "ArrowLeft" || ev.key === "ArrowUp") {
        targetIdx = (idx - 1 + filterTabs.length) % filterTabs.length;
      } else if (ev.key === "Home") {
        targetIdx = 0;
      } else if (ev.key === "End") {
        targetIdx = filterTabs.length - 1;
      }
      if (targetIdx >= 0) {
        ev.preventDefault();
        setActiveTab(filterTabs[targetIdx], true);
      }
    });
  });

  // Search input
  if (searchInput) {
    searchInput.addEventListener("input", (ev) => {
      activeSearchQuery = ev.target.value;
      refreshListDisplay();
    });
  }

  // Site capture toggle
  if (siteToggle) {
    siteToggle.addEventListener("change", (ev) => {
      if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.sendMessage) {
        chrome.runtime.sendMessage({
          action: "setSiteCapture",
          tabUrl: currentTabUrl,
          enabled: ev.target.checked
        });
      }
    });
  }

  // Clear list button
  if (clearListBtn) {
    clearListBtn.addEventListener("click", () => {
      currentCandidates = [];
      if (activeFilterMode === "links") {
        currentTabLinks = [];
      }
      activeErrorMessage = "";
      refreshListDisplay();
      if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.sendMessage) {
        chrome.runtime.sendMessage({
          action: "clearMedia",
          tabId: currentTabId
        });
      }
    });
  }

  // Download all in Radium button
  if (batchDownloadBtn) {
    batchDownloadBtn.addEventListener("click", () => {
      if (activeFilterMode === "links") {
        const q = (activeSearchQuery || "").trim().toLowerCase();
        const toDl = currentTabLinks.filter(l => !q || (l.filename && l.filename.toLowerCase().includes(q)) || (l.url && l.url.toLowerCase().includes(q)));
        if (toDl.length === 0) return;
        batchDownloadBtn.textContent = "Submitting " + toDl.length + "...";
        batchDownloadBtn.disabled = true;
        toDl.forEach(l => {
          if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.sendMessage) {
            chrome.runtime.sendMessage({
              action: "downloadMedia",
              url: l.url,
              pageTitle: l.filename,
              isMseBlob: false,
              tabId: currentTabId,
              tabUrl: currentTabUrl
            });
          }
        });
        setTimeout(() => {
          batchDownloadBtn.textContent = "Queued " + toDl.length + " links";
          refreshListDisplay();
        }, 600);
        return;
      }

      const filtered = filterPopupCandidates(currentCandidates, activeFilterMode, activeSearchQuery);
      if (filtered.length === 0) return;
      batchDownloadBtn.textContent = "Submitting " + filtered.length + "...";
      batchDownloadBtn.disabled = true;

      filtered.forEach(item => {
        if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.sendMessage) {
          const itemEl = mediaListEl ? mediaListEl.querySelector(`.media-item[data-id="${item.candidateId}"]`) : null;
          const qSelect = itemEl ? itemEl.querySelector(".quality-select") : null;
          const qId = String(qSelect ? qSelect.value : (item.selectedQualityId || item.resolution || "")).toLowerCase();
          let chosenQ = null;
          if (Array.isArray(item.qualities)) {
            chosenQ = item.qualities.find(q =>
              String(q.id || "").toLowerCase() === qId ||
              String(q.resolution || "").toLowerCase() === qId ||
              String(q.videoFilter || "").toLowerCase() === qId
            ) || item.qualities[0];
          }
          chrome.runtime.sendMessage({
            action: "downloadMedia",
            candidateId: item.candidateId,
            qualityId: qId,
            resolution: chosenQ ? chosenQ.resolution : (item.resolution || ""),
            videoFilter: chosenQ ? (chosenQ.videoFilter || chosenQ.resolution) : "best",
            selectedQuality: chosenQ,
            url: item.url,
            tabId: currentTabId,
            tabUrl: currentTabUrl
          });
        }
      });

      setTimeout(() => {
        batchDownloadBtn.textContent = "Queued " + filtered.length + " items";
        refreshListDisplay();
      }, 600);
    });
  }

  // Initial load
  fetchTabData();
}

if (typeof document !== "undefined") {
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initPopupUI);
  } else {
    initPopupUI();
  }
}

if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    formatMediaSize,
    filterPopupCandidates,
    renderDetectedMediaList,
    initPopupUI
  };
}
