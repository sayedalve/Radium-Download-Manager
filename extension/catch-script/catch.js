// SPDX-License-Identifier: Apache-2.0
// Adapted from xifangczy/cat-catch (catch-script/search.js & catch.js) for Radium Download Manager.
// Runs in page MAIN world at document_start. Hooks window.fetch, window.XMLHttpRequest,
// window.URL.createObjectURL, MediaSource, HTMLAnchorElement.prototype.click, and inspects
// YouTube player responses (ytInitialPlayerResponse / #movie_player) with explicit typed
// descriptors (never passing JSON as an HLS/DASH manifest).

(function () {
  function installDeepSearchHooks(win, sessionNonce, emitEvent) {
    if (!win || win.__fluxCatchScriptInstalled) return;
    win.__fluxCatchScriptInstalled = true;

    const seenUrls = new Set();
    const blobRegistry = new Map();
    let lastInspectedYtVideoId = "";

    function makeCorrelationId(prefix) {
      return `${prefix || "catch"}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
    }

    function postToExtension(payload) {
      if (typeof emitEvent === "function") {
        emitEvent(Object.assign({ nonce: sessionNonce }, payload));
      } else if (typeof win.postMessage === "function") {
        try {
          win.postMessage(Object.assign({ nonce: sessionNonce }, payload), "*");
        } catch (_) {}
      }
    }

    function sendMedia(data) {
      if (!data || !data.url) return;
      const dedupKey = (data.streamType || "") + "|" + (data.sourceType || "") + "|" + data.url + "|" + (data.videoUrl || "");
      if (seenUrls.has(dedupKey) && !data.cachedManifestBody && !data.extractorJson) return;
      seenUrls.add(dedupKey);
      postToExtension(Object.assign({
        action: "catCatchAddMedia",
        correlationId: data.correlationId || makeCorrelationId("media")
      }, data));
    }

    function sendKey(key, url) {
      if (!key) return;
      postToExtension({ action: "catCatchAddKey", key, url });
    }

    function sendPlayerError(reason, url) {
      if (!reason) return;
      postToExtension({ action: "catCatchPlayerError", reason, url });
    }

    function normalizeGooglevideoUrl(rawUrl) {
      try {
        const u = new URL(rawUrl, win.location ? win.location.href : "https://www.youtube.com");
        for (const p of ["range", "rn", "rbuf", "ump", "srfvp", "alr", "cpn"]) {
          u.searchParams.delete(p);
        }
        return u.toString();
      } catch (_) {
        return rawUrl;
      }
    }

    const PROGRESSIVE_YOUTUBE_ITAGS = new Set(["18", "22", "37", "43", "59", "78"]);

    function inspectYouTubePlayerObject(playerResp, pageUrl) {
      if (!playerResp || typeof playerResp !== "object") return false;
      const playStatus = playerResp.playabilityStatus || {};
      const videoDetails = playerResp.videoDetails || {};
      const streamingData = playerResp.streamingData || null;

      if (playStatus.status && playStatus.status !== "OK" && !streamingData) {
        const reason = playStatus.reason || ("YouTube stream unavailable (" + playStatus.status + ")");
        sendPlayerError(reason, pageUrl || (win.location ? win.location.href : ""));
        return false;
      }

      if (!streamingData) return false;

      const videoId = videoDetails.videoId || "";
      const rawTitle = videoDetails.title || (win.document ? win.document.title : "") || "youtube_video";
      const cleanTitle = rawTitle.replace(/\s*-\s*YouTube$/i, "").trim() || "youtube_video";
      const canonicalWatchUrl = videoId
        ? ("https://www.youtube.com/watch?v=" + videoId)
        : (pageUrl || (win.location ? win.location.href : ""));

      // 1. Check if actual HLS or DASH manifests are directly exposed by YouTube
      if (streamingData.hlsManifestUrl) {
        sendMedia({
          streamType: "HLS",
          sourceType: "HLS_MANIFEST",
          ext: "m3u8",
          url: streamingData.hlsManifestUrl,
          pageTitle: cleanTitle,
          referer: canonicalWatchUrl
        });
      }
      if (streamingData.dashManifestUrl) {
        sendMedia({
          streamType: "DASH",
          sourceType: "DASH_MANIFEST",
          ext: "mpd",
          url: streamingData.dashManifestUrl,
          pageTitle: cleanTitle,
          referer: canonicalWatchUrl
        });
      }

      // 2. Inspect progressive formats (combined audio + video) and adaptiveFormats (separate video & audio)
      const formats = Array.isArray(streamingData.formats) ? streamingData.formats : [];
      const adaptive = Array.isArray(streamingData.adaptiveFormats) ? streamingData.adaptiveFormats : [];
      const allFormats = formats.concat(adaptive);

      // Check if all formats are DRM protected
      if (allFormats.length > 0 && allFormats.every(f => f && (f.drmFamilies || (f.signatureCipher && String(f.signatureCipher).includes("drm"))))) {
        sendPlayerError("YouTube stream is DRM protected (Unsupported)", canonicalWatchUrl);
        return false;
      }

      let bestProgressive = null;
      for (const f of formats) {
        if (f && f.url && !f.drmFamilies) {
          const h = Number(f.height || 0);
          if (!bestProgressive || h > Number(bestProgressive.height || 0)) {
            bestProgressive = f;
          }
        }
      }

      let bestVideo = null;
      let bestAudio = null;
      let bestAudioMp4 = null;
      for (const f of adaptive) {
        if (!f || f.drmFamilies) continue;
        const mime = String(f.mimeType || "").toLowerCase();
        const bw = Number(f.bitrate || f.averageBitrate || 0);
        if (mime.startsWith("video/")) {
          if (!bestVideo || bw > Number(bestVideo.bitrate || bestVideo.averageBitrate || 0)) {
            bestVideo = f;
          }
        } else if (mime.startsWith("audio/")) {
          if (!bestAudio || bw > Number(bestAudio.bitrate || bestAudio.averageBitrate || 0)) {
            bestAudio = f;
          }
          if (mime.includes("mp4") && (!bestAudioMp4 || bw > Number(bestAudioMp4.bitrate || bestAudioMp4.averageBitrate || 0))) {
            bestAudioMp4 = f;
          }
        }
      }
      if (bestAudioMp4) bestAudio = bestAudioMp4;

      // Extract duration in seconds across videoDetails, format metadata, and DOM video element
      let durSec = parseFloat(videoDetails.lengthSeconds || "0") || 0;
      if (durSec <= 0) {
        for (const f of allFormats) {
          if (f && f.approxDurationMs) {
            const ms = parseFloat(f.approxDurationMs);
            if (ms > 0) { durSec = ms / 1000; break; }
          }
        }
      }
      if (durSec <= 0 && win.document) {
        const vidEl = win.document.querySelector("video");
        if (vidEl && vidEl.duration && !isNaN(vidEl.duration) && isFinite(vidEl.duration) && vidEl.duration > 0) {
          durSec = vidEl.duration;
        }
      }

      // Compute audio track byte size for accurate adaptive mux size estimation
      const rawAudioBytes = bestAudio ? parseInt(bestAudio.contentLength || "0", 10) : 0;
      const audioBw = bestAudio ? Number(bestAudio.bitrate || bestAudio.averageBitrate || 128000) : 128000;
      const audioByteLen = rawAudioBytes > 0 ? rawAudioBytes : (durSec > 0 ? Math.round((audioBw / 8) * durSec) : 0);
      const cleanBestAudioUrl = (bestAudio && bestAudio.url) ? normalizeGooglevideoUrl(bestAudio.url) : "";
      const qualityMap = new Map();

      const addVideoFormatToQualities = (f, isAdaptiveFmt) => {
        if (!f || f.drmFamilies) return;
        const mime = String(f.mimeType || "").toLowerCase();
        if (mime.startsWith("audio/")) return;
        const height = Number(f.height || 0);
        const width = Number(f.width || 0);
        const fps = Number(f.fps || 30);
        const bw = Number(f.bitrate || f.averageBitrate || 0);
        const resLabel = f.qualityLabel || (height > 0 ? `${height}p` : "HD");
        const isMp4 = mime.includes("mp4");
        const codecKey = isMp4 ? "mp4" : "webm";
        const fpsSuffix = fps > 30 ? `${fps}` : "";
        const fpsKey = fpsSuffix ? `${fpsSuffix}fps_` : "";
        const key = `${resLabel.toLowerCase()}_${fpsKey}${codecKey}`;
        const displayRes = `${resLabel}${fpsSuffix ? fpsSuffix : ""}`;
        const displayLabel = `${displayRes} (${isMp4 ? "MP4" : "WebM"})`;
        const vFilter = height > 0 ? (fps > 30 ? `${height}p${fps}` : `${height}p`) : resLabel;

        const hasDirectUrl = Boolean(f.url);
        const cleanVUrl = hasDirectUrl ? normalizeGooglevideoUrl(f.url) : "";
        const needsMux = Boolean(isAdaptiveFmt);
        const unpairedAdaptive = Boolean(isAdaptiveFmt && hasDirectUrl && !cleanBestAudioUrl);
        const requiresCipher = Boolean(!hasDirectUrl || unpairedAdaptive);

        const vSize = parseInt(f.contentLength || "0", 10);
        const estVBytes = (vSize === 0 && bw > 0 && durSec > 0) ? Math.round((bw / 8) * durSec) : 0;
        const effectiveVBytes = vSize > 0 ? vSize : estVBytes;
        const totalSize = isAdaptiveFmt
          ? (effectiveVBytes + (audioByteLen > 0 ? audioByteLen : (durSec > 0 ? Math.round((128000 / 8) * durSec) : 0)))
          : (effectiveVBytes || vSize);
        const isSizeEstimated = Boolean(vSize === 0 || (isAdaptiveFmt && rawAudioBytes === 0));

        const entry = {
          id: key,
          label: displayLabel,
          resolution: displayRes,
          width,
          height,
          fps,
          itag: f.itag || 0,
          bandwidth: bw,
          size: totalSize,
          sizeEstimated: isSizeEstimated,
          ext: isMp4 ? "mp4" : "webm",
          mime: mime.split(";")[0] || (isMp4 ? "video/mp4" : "video/webm"),
          videoUrl: unpairedAdaptive ? "" : cleanVUrl,
          pairedAudioUrl: needsMux ? cleanBestAudioUrl : "",
          requiresAudioMux: needsMux,
          requiresCipherExtraction: requiresCipher,
          videoFilter: vFilter
        };

        const existing = qualityMap.get(key);
        if (!existing) {
          qualityMap.set(key, entry);
        } else {
          // Prefer direct URL over cipher, and higher bitrate
          const existingDirect = Boolean(existing.videoUrl);
          if ((hasDirectUrl && !existingDirect) ||
              (hasDirectUrl === existingDirect && bw > existing.bandwidth)) {
            qualityMap.set(key, entry);
          }
        }
      };

      for (const f of adaptive) addVideoFormatToQualities(f, true);
      for (const f of formats) addVideoFormatToQualities(f, false);

      const qualities = Array.from(qualityMap.values()).sort((a, b) => {
        if ((b.height || 0) !== (a.height || 0)) return (b.height || 0) - (a.height || 0);
        if ((b.fps || 0) !== (a.fps || 0)) return (b.fps || 0) - (a.fps || 0);
        if (a.ext === "mp4" && b.ext !== "mp4") return -1;
        if (a.ext !== "mp4" && b.ext === "mp4") return 1;
        return (b.bandwidth || 0) - (a.bandwidth || 0);
      });

      if (bestAudio) {
        const audioBwKbps = `${Math.round(audioBw / 1000)} Kbps`;
        qualities.push({
          id: "audio-only",
          label: `Audio Only (${audioBwKbps})`,
          resolution: "Audio",
          width: 0,
          height: 0,
          itag: bestAudio.itag || 140,
          bandwidth: audioBw,
          size: audioByteLen,
          sizeEstimated: rawAudioBytes === 0,
          isAudioOnly: true,
          ext: "m4a",
          mime: String(bestAudio.mimeType || "audio/mp4").split(";")[0],
          videoUrl: cleanBestAudioUrl,
          pairedAudioUrl: "",
          requiresAudioMux: false,
          requiresCipherExtraction: !bestAudio.url,
          videoFilter: "audio"
        });
      }

      const topVid = qualities.find(q => !q.isAudioOnly) || qualities[0];
      const primarySize = topVid ? (topVid.size || 0) : 0;
      const primaryRes = topVid ? (topVid.resolution || topVid.label || "1080p") : "1080p";

      // Adaptive video tracks on YouTube have no audio; only use adaptive bestVideo if bestAudio is also available,
      // otherwise prefer bestProgressive or mark requiresCipherExtraction so yt-dlp extracts & merges both tracks.
      const canMuxAdaptive = Boolean(bestVideo && bestVideo.url && bestAudio && bestAudio.url);
      const chosenVideo = canMuxAdaptive ? bestVideo : (bestProgressive || bestVideo);

      if (chosenVideo && chosenVideo.url) {
        const cleanVideoUrl = normalizeGooglevideoUrl(chosenVideo.url);
        const cleanAudioUrl = canMuxAdaptive ? normalizeGooglevideoUrl(bestAudio.url) : "";
        const isUnpairedAdaptiveVideo = Boolean(chosenVideo === bestVideo && !cleanAudioUrl);
        // Do NOT downgrade candidate resolution to 360p progressive! Always advertise the highest detected video resolution!
        const resolution = primaryRes;
        const sizeBytes = primarySize ||
                          (parseInt(chosenVideo.contentLength || "0", 10) + (canMuxAdaptive ? audioByteLen : 0));

        const descriptor = {
          fluxType: "YOUTUBE_STREAM",
          sourceType: "YOUTUBE_EXTRACTION",
          videoId,
          title: cleanTitle,
          watchUrl: canonicalWatchUrl,
          resolution: primaryRes,
          videoUrl: isUnpairedAdaptiveVideo ? "" : (canMuxAdaptive ? cleanVideoUrl : ""),
          audioUrl: cleanAudioUrl,
          requiresAudioMux: canMuxAdaptive,
          requiresCipherExtraction: isUnpairedAdaptiveVideo || !canMuxAdaptive,
          videoMime: String(chosenVideo.mimeType || "video/mp4").split(";")[0],
          audioMime: (canMuxAdaptive && bestAudio) ? String(bestAudio.mimeType || "audio/mp4").split(";")[0] : "",
          hlsManifestUrl: streamingData.hlsManifestUrl || "",
          dashManifestUrl: streamingData.dashManifestUrl || "",
          size: sizeBytes > 0 ? sizeBytes : 0,
          qualities
        };

        lastInspectedYtVideoId = videoId || canonicalWatchUrl;
        sendMedia({
          streamType: "YOUTUBE",
          sourceType: "YOUTUBE_EXTRACTION",
          ext: "mp4",
          url: canonicalWatchUrl,
          videoId,
          pageTitle: cleanTitle,
          resolution: primaryRes,
          size: sizeBytes > 0 ? sizeBytes : 0,
          videoUrl: isUnpairedAdaptiveVideo ? "" : (canMuxAdaptive ? cleanVideoUrl : ""),
          pairedAudioUrl: cleanAudioUrl,
          requiresAudioMux: canMuxAdaptive,
          requiresCipherExtraction: isUnpairedAdaptiveVideo || !canMuxAdaptive,
          qualities,
          referer: canonicalWatchUrl,
          extractorJson: JSON.stringify(descriptor),
          cachedManifestBody: JSON.stringify(descriptor)
        });
        return true;
      }

      // If formats use signatureCipher / cipher or serverAbrStreamingUrl, register the YouTube extraction target for yt-dlp
      if (formats.length > 0 || adaptive.length > 0 || streamingData.serverAbrStreamingUrl) {
        const resolution = primaryRes;
        const descriptor = {
          fluxType: "YOUTUBE_STREAM",
          sourceType: "YOUTUBE_EXTRACTION",
          videoId,
          title: cleanTitle,
          watchUrl: canonicalWatchUrl,
          resolution: primaryRes,
          requiresCipherExtraction: true,
          size: primarySize,
          qualities
        };
        lastInspectedYtVideoId = videoId || canonicalWatchUrl;
        sendMedia({
          streamType: "YOUTUBE",
          sourceType: "YOUTUBE_EXTRACTION",
          ext: "mp4",
          url: canonicalWatchUrl,
          videoId,
          pageTitle: cleanTitle,
          resolution: primaryRes,
          size: primarySize,
          requiresCipherExtraction: true,
          qualities,
          referer: canonicalWatchUrl,
          extractorJson: JSON.stringify(descriptor),
          cachedManifestBody: JSON.stringify(descriptor)
        });
        return true;
      }

      return false;
    }

    function scanPageForYouTubeData() {
      try {
        if (!win.location || !win.location.hostname) return false;
        const host = win.location.hostname.toLowerCase();
        if (!host.includes("youtube.com") && host !== "youtu.be") return false;

        // 1. Check #movie_player API (most up-to-date during SPA navigation)
        if (win.document && typeof win.document.getElementById === "function") {
          const playerEl = win.document.getElementById("movie_player");
          if (playerEl && typeof playerEl.getPlayerResponse === "function") {
            const pr = playerEl.getPlayerResponse();
            if (pr && inspectYouTubePlayerObject(pr, win.location.href)) {
              return true;
            }
          }
        }

        // 2. Check window.ytInitialPlayerResponse
        if (win.ytInitialPlayerResponse) {
          if (inspectYouTubePlayerObject(win.ytInitialPlayerResponse, win.location.href)) {
            return true;
          }
        }

        // 2b. Check window.ytplayer?.config?.args?.raw_player_response
        if (win.ytplayer && win.ytplayer.config && win.ytplayer.config.args) {
          const rawPr = win.ytplayer.config.args.raw_player_response;
          if (rawPr) {
            const prObj = (typeof rawPr === "string") ? JSON.parse(rawPr) : rawPr;
            if (prObj && inspectYouTubePlayerObject(prObj, win.location.href)) {
              return true;
            }
          }
        }

        // 3. Fallback: if on a valid YouTube watch/shorts/embed URL and #movie_player or <video> exists,
        // register a site extraction candidate with standard presets & duration-based estimated sizes
        const href = win.location.href || "";
        const u = new URL(href);
        const path = u.pathname.toLowerCase();
        const isWatchPage = (path === "/watch" && u.searchParams.get("v")) ||
                            path.startsWith("/shorts/") ||
                            path.startsWith("/embed/") ||
                            path.startsWith("/live/") ||
                            (host === "youtu.be" && path.length > 1);
        if (isWatchPage && win.document && win.document.querySelector && win.document.querySelector("video")) {
          const vid = u.searchParams.get("v") || path.split("/").filter(Boolean).pop() || "";
          const rawTitle = (win.document.title || "youtube_video").replace(/\s*-\s*YouTube$/i, "").trim() || "youtube_video";
          const vidEl = win.document.querySelector("video");
          const fallbackDur = (vidEl && vidEl.duration && !isNaN(vidEl.duration) && isFinite(vidEl.duration) && vidEl.duration > 0)
            ? vidEl.duration
            : 0;

          const standardPresets = [
            { id: "1080p_mp4", res: "1080p", label: "1080p Full HD (MP4)", bw: 4200000, h: 1080, ext: "mp4" },
            { id: "720p_mp4", res: "720p", label: "720p HD (MP4)", bw: 2200000, h: 720, ext: "mp4" },
            { id: "480p_mp4", res: "480p", label: "480p SD (MP4)", bw: 1100000, h: 480, ext: "mp4" },
            { id: "360p_mp4", res: "360p", label: "360p Standard (MP4)", bw: 650000, h: 360, ext: "mp4" },
            { id: "audio-only", res: "Audio", label: "Audio Only (128 Kbps)", bw: 128000, h: 0, ext: "m4a", isAudioOnly: true }
          ];

          const fallbackQualities = standardPresets.map(p => {
            const sz = fallbackDur > 0 ? Math.round((p.bw / 8) * fallbackDur) : 0;
            return {
              id: p.id,
              label: p.label,
              resolution: p.res,
              width: p.h > 0 ? Math.round((p.h * 16) / 9) : 0,
              height: p.h,
              bandwidth: p.bw,
              size: sz,
              sizeEstimated: true,
              isAudioOnly: Boolean(p.isAudioOnly),
              ext: p.ext,
              requiresCipherExtraction: true,
              videoFilter: p.h > 0 ? `${p.h}p` : "audio"
            };
          });

          const primaryFallbackSize = fallbackQualities[0] ? fallbackQualities[0].size : 0;
          const descriptor = {
            fluxType: "YOUTUBE_STREAM",
            sourceType: "YOUTUBE_EXTRACTION",
            videoId: vid,
            title: rawTitle,
            watchUrl: href,
            resolution: "1080p",
            requiresCipherExtraction: true,
            size: primaryFallbackSize,
            qualities: fallbackQualities
          };
          sendMedia({
            streamType: "YOUTUBE",
            sourceType: "YOUTUBE_EXTRACTION",
            ext: "mp4",
            url: href,
            videoId: vid,
            pageTitle: rawTitle,
            resolution: "1080p",
            size: primaryFallbackSize,
            requiresCipherExtraction: true,
            qualities: fallbackQualities,
            referer: href,
            extractorJson: JSON.stringify(descriptor),
            cachedManifestBody: JSON.stringify(descriptor)
          });
          return true;
        }
      } catch (_) {}
      return false;
    }

    function inspectManifestText(url, text, referer) {
      if (typeof text !== "string" || !url) return false;
      const trimmed = text.trimStart();
      const effectiveUrl = url.startsWith("blob:")
        ? (referer || (win.location ? win.location.href : url))
        : url;

      if (trimmed.startsWith("#EXTM3U") || trimmed.includes("#EXT-X-STREAM-INF") || trimmed.includes("#EXT-X-TARGETDURATION")) {
        const hlsQualities = [];
        const lines = text.split(/\r?\n/);
        for (let i = 0; i < lines.length; ++i) {
          const ln = lines[i].trim();
          if (!ln.startsWith("#EXT-X-STREAM-INF:")) continue;
          const bwM = ln.match(/BANDWIDTH=(\d+)/i);
          const resM = ln.match(/RESOLUTION=(\d+)x(\d+)/i);
          let vUri = "";
          for (let j = i + 1; j < lines.length; ++j) {
            const c = lines[j].trim();
            if (c && !c.startsWith("#")) { vUri = c; i = j; break; }
          }
          if (!vUri) continue;
          let resolvedVUri = vUri;
          try { resolvedVUri = new URL(vUri, effectiveUrl).href; } catch (_) {}
          const w = resM ? parseInt(resM[1], 10) : 0;
          const h = resM ? parseInt(resM[2], 10) : 0;
          const bw = bwM ? parseInt(bwM[1], 10) : 0;
          const resLabel = h > 0 ? `${h}p` : "Auto";
          const bwStr = bw >= 1000000 ? `${(bw / 1000000).toFixed(1)} Mbps` : (bw >= 1000 ? `${Math.round(bw / 1000)} Kbps` : "");
          hlsQualities.push({
            id: h > 0 ? `${h}p` : `hls-${hlsQualities.length}`,
            label: `${resLabel}${bwStr ? " (" + bwStr + ")" : ""}`,
            resolution: resLabel,
            width: w,
            height: h,
            bandwidth: bw,
            ext: "mp4",
            variantUrl: resolvedVUri,
            videoFilter: h > 0 ? `${h}p` : "best"
          });
        }
        hlsQualities.sort((a, b) => ((b.height || 0) - (a.height || 0)) || ((b.bandwidth || 0) - (a.bandwidth || 0)));
        sendMedia({
          streamType: "HLS",
          sourceType: "HLS_MANIFEST",
          ext: "m3u8",
          url: effectiveUrl,
          blobUrl: url.startsWith("blob:") ? url : "",
          resolution: hlsQualities.length > 0 ? hlsQualities[0].resolution : "",
          qualities: hlsQualities,
          referer: referer || (win.location ? win.location.href : ""),
          cachedManifestBody: text
        });

        // Parse AES-128 key URI if present: #EXT-X-KEY:METHOD=AES-128,URI="key_url"
        const keyMatch = text.match(/#EXT-X-KEY:METHOD=([^,\r\n]+)(?:,URI="([^"\r\n]+)")?/i);
        if (keyMatch && keyMatch[2]) {
          try {
            const resolvedKeyUrl = new URL(keyMatch[2], effectiveUrl).href;
            sendKey(resolvedKeyUrl, effectiveUrl);
          } catch (_) {}
        }
        return true;
      } else if ((trimmed.startsWith("<?xml") && trimmed.includes("<MPD")) || trimmed.startsWith("<MPD")) {
        const dashQualities = [];
        const repRe = /<Representation\b([^>]*)>/gi;
        let rm;
        while ((rm = repRe.exec(text)) !== null) {
          const attrs = rm[1] || "";
          const idM = attrs.match(/\bid="([^"]+)"/i);
          const wM = attrs.match(/\bwidth="(\d+)"/i);
          const hM = attrs.match(/\bheight="(\d+)"/i);
          const bwM = attrs.match(/\bbandwidth="(\d+)"/i);
          const w = wM ? parseInt(wM[1], 10) : 0;
          const h = hM ? parseInt(hM[1], 10) : 0;
          const bw = bwM ? parseInt(bwM[1], 10) : 0;
          if (h <= 0 && w <= 0 && bw <= 0) continue;
          const resLabel = h > 0 ? `${h}p` : (idM ? idM[1] : "Video");
          const bwStr = bw >= 1000000 ? `${(bw / 1000000).toFixed(1)} Mbps` : (bw >= 1000 ? `${Math.round(bw / 1000)} Kbps` : "");
          dashQualities.push({
            id: h > 0 ? `${h}p` : (idM ? idM[1] : `dash-${dashQualities.length}`),
            label: `${resLabel}${bwStr ? " (" + bwStr + ")" : ""}`,
            resolution: resLabel,
            width: w,
            height: h,
            bandwidth: bw,
            ext: "mp4",
            variantUrl: effectiveUrl,
            videoFilter: h > 0 ? `${h}p` : (idM ? idM[1] : "best")
          });
        }
        dashQualities.sort((a, b) => ((b.height || 0) - (a.height || 0)) || ((b.bandwidth || 0) - (a.bandwidth || 0)));
        sendMedia({
          streamType: "DASH",
          sourceType: "DASH_MANIFEST",
          ext: "mpd",
          url: effectiveUrl,
          blobUrl: url.startsWith("blob:") ? url : "",
          resolution: dashQualities.length > 0 ? dashQualities[0].resolution : "",
          qualities: dashQualities,
          referer: referer || (win.location ? win.location.href : ""),
          cachedManifestBody: text
        });
        return true;
      } else if (url.includes("/youtubei/v1/player") && trimmed.startsWith("{")) {
        try {
          const ytObj = JSON.parse(trimmed);
          return inspectYouTubePlayerObject(ytObj, win.location ? win.location.href : url);
        } catch (_) {}
      } else if (/^https:\/\/[^.]*\.vimeocdn\.com\/.*playlist\.json/i.test(url)) {
        try {
          const vimeoObj = JSON.parse(text);
          if (vimeoObj && vimeoObj.clip_id) {
            sendMedia({
              streamType: "VIMEO",
              sourceType: "YOUTUBE_EXTRACTION",
              ext: "mp4",
              url,
              referer: referer || (win.location ? win.location.href : ""),
              extractorJson: text,
              cachedManifestBody: ""
            });
            return true;
          }
        } catch (_) {}
      }
      return false;
    }

    function inspectUrl(url, referer) {
      if (!url || typeof url !== "string") return;
      if (url.startsWith("blob:") || url.startsWith("data:") || url.startsWith("javascript:")) return;

      let resolvedUrl = url;
      try {
        if (win.location && win.location.href) {
          resolvedUrl = new URL(url, win.location.href).href;
        }
      } catch (_) {}

      const lower = resolvedUrl.toLowerCase();
      const ref = referer || (win.location ? win.location.href : "");

      if (lower.includes(".m3u8")) {
        sendMedia({
          streamType: "HLS",
          sourceType: "HLS_MANIFEST",
          ext: "m3u8",
          url: resolvedUrl,
          referer: ref
        });
      } else if (lower.includes(".mpd")) {
        sendMedia({
          streamType: "DASH",
          sourceType: "DASH_MANIFEST",
          ext: "mpd",
          url: resolvedUrl,
          referer: ref
        });
      } else if (lower.includes("googlevideo.com/videoplayback") || lower.includes("/videoplayback?")) {
        // Do NOT pretend an adaptive or chunked googlevideo URL is a standalone HTTP_FILE
        const cleanUrl = normalizeGooglevideoUrl(resolvedUrl);
        try {
          const rawU = new URL(resolvedUrl);
          const u = new URL(cleanUrl);
          const mime = (u.searchParams.get("mime") || "video/mp4").toLowerCase();
          const clen = parseInt(u.searchParams.get("clen") || "0", 10);
          const itag = u.searchParams.get("itag") || "";
          const hasUmp = rawU.searchParams.get("ump") === "1" || rawU.searchParams.has("srfvp");
          const hadRange = rawU.searchParams.has("range") || rawU.searchParams.has("rbuf");
          const isAudio = mime.startsWith("audio/");
          const isProgressive = PROGRESSIVE_YOUTUBE_ITAGS.has(itag) && !isAudio && !hasUmp && !hadRange;

          if (!scanPageForYouTubeData()) {
            const watchRef = (ref && (ref.includes("youtube.com/watch") || ref.includes("youtube.com/shorts/") || ref.includes("youtu.be/")))
              ? ref
              : cleanUrl;
            sendMedia({
              streamType: isProgressive ? "HTTP_FILE" : "YOUTUBE",
              sourceType: isProgressive ? "DIRECT_PROGRESSIVE" : "YOUTUBE_EXTRACTION",
              ext: isProgressive ? (mime.includes("webm") ? "webm" : "mp4") : "mp4",
              mime,
              size: clen > 0 ? clen : 0,
              itag,
              isAudioOnly: isAudio,
              isGoogleVideo: true,
              isAdaptiveTrack: !isProgressive,
              videoUrl: (!isAudio && !hasUmp) ? cleanUrl : "",
              pairedAudioUrl: (isAudio && !hasUmp) ? cleanUrl : "",
              requiresCipherExtraction: hasUmp,
              url: isProgressive ? cleanUrl : watchRef,
              referer: ref
            });
          }
        } catch (_) {
          scanPageForYouTubeData();
        }
      } else if (/\.(mp4|webm|mkv|mov|flv)(\?.*)?$/i.test(lower)) {
        // Suppress init segments
        if (/\/(init|header)[-_0-9]*\.(mp4|m4s)$/i.test(lower)) return;
        const extMatch = lower.match(/\.(mp4|webm|mkv|mov|flv)(\?.*)?$/i);
        sendMedia({
          streamType: "HTTP_FILE",
          sourceType: "DIRECT_PROGRESSIVE",
          ext: extMatch ? extMatch[1].toLowerCase() : "mp4",
          url: resolvedUrl,
          referer: ref
        });
      }
    }

    function scanPerformanceResources() {
      try {
        if (!win.performance || typeof win.performance.getEntriesByType !== "function") return;
        const entries = win.performance.getEntriesByType("resource") || [];
        for (const entry of entries) {
          if (entry && entry.name) {
            inspectUrl(entry.name, win.location ? win.location.href : "");
          }
        }
      } catch (_) {}
    }

    // 1. Hook window.fetch
    if (typeof win.fetch === "function") {
      const origFetch = win.fetch;
      win.fetch = function (...args) {
        let reqUrl = "";
        try {
          const req = args[0];
          reqUrl = (typeof req === "string") ? req : (req && req.url) ? req.url : "";
          if (reqUrl) inspectUrl(reqUrl);
        } catch (_) {}

        return origFetch.apply(this, args).then((response) => {
          try {
            if (!response) return response;
            const finalUrl = (response && response.url) ? response.url : reqUrl;
            if (finalUrl && finalUrl !== reqUrl) inspectUrl(finalUrl);

            const lowerUrl = (finalUrl || "").toLowerCase();
            const contentType = ((response.headers && typeof response.headers.get === "function" && response.headers.get("content-type")) || "").toLowerCase();

            const isManifestOrPlayer =
              lowerUrl.includes(".m3u8") ||
              lowerUrl.includes(".mpd") ||
              lowerUrl.includes("manifest") ||
              lowerUrl.includes("playlist") ||
              lowerUrl.includes("/youtubei/v1/player") ||
              contentType.includes("mpegurl") ||
              contentType.includes("dash") ||
              !response.headers;

            if (isManifestOrPlayer && typeof response.clone === "function" && !response.bodyUsed) {
              try {
                const cloned = response.clone();
                if (cloned && typeof cloned.text === "function") {
                  cloned.text().then(text => {
                    if (text && typeof text === "string" && text.length < 5000000) {
                      inspectManifestText(finalUrl, text, win.location ? win.location.href : "");
                    }
                  }).catch(() => {});
                }
              } catch (_) {}
            }
          } catch (_) {}
          return response;
        });
      };
    }

    // 2. Hook window.XMLHttpRequest
    if (typeof win.XMLHttpRequest === "function" && win.XMLHttpRequest.prototype) {
      const origOpen = win.XMLHttpRequest.prototype.open;
      const origSend = win.XMLHttpRequest.prototype.send;

      win.XMLHttpRequest.prototype.open = function (method, url, ...rest) {
        try {
          this.__fluxUrl = (typeof url === "string") ? url : String(url);
          inspectUrl(this.__fluxUrl);
        } catch (_) {}
        return origOpen.apply(this, [method, url, ...rest]);
      };

      win.XMLHttpRequest.prototype.send = function (...bodyArgs) {
        if (typeof this.addEventListener === "function") {
          this.addEventListener("load", function () {
            try {
              const url = this.responseURL || this.__fluxUrl;
              if (!url) return;
              inspectUrl(url);
              const rType = this.responseType;
              if (rType === "" || rType === "text") {
                const lower = url.toLowerCase();
                if (lower.includes(".m3u8") || lower.includes(".mpd") || lower.includes("/youtubei/v1/player")) {
                  if (typeof this.responseText === "string") {
                    inspectManifestText(url, this.responseText, win.location ? win.location.href : "");
                  }
                }
              }
            } catch (_) {}
          }, { passive: true });
        }
        return origSend.apply(this, bodyArgs);
      };
    }

    // 3. Hook URL.createObjectURL & URL.revokeObjectURL (Blob tracking with delayed revocation)
    if (win.URL && typeof win.URL.createObjectURL === "function") {
      const origCreateObjectUrl = win.URL.createObjectURL;
      win.URL.createObjectURL = function (obj) {
        const blobUrl = origCreateObjectUrl.apply(this, arguments);
        try {
          if (blobRegistry.size > 200) {
            const cutoff = Date.now() - 600000;
            for (const [k, v] of blobRegistry.entries()) {
              if (v.createdAt < cutoff) blobRegistry.delete(k);
            }
          }
          const isMediaSource = (typeof MediaSource !== "undefined" && obj instanceof MediaSource) ||
                                (typeof ManagedMediaSource !== "undefined" && obj instanceof ManagedMediaSource);
          const isBlobLike = obj && (
            (typeof Blob !== "undefined" && obj instanceof Blob) ||
            (typeof File !== "undefined" && obj instanceof File) ||
            Object.prototype.toString.call(obj) === "[object Blob]" ||
            Object.prototype.toString.call(obj) === "[object File]" ||
            (typeof obj.arrayBuffer === "function" && typeof obj.size === "number")
          );
          if (isMediaSource) {
            blobRegistry.set(blobUrl, {
              blobUrl,
              kind: "MediaSource",
              mimeType: "video/mp4",
              size: 0,
              createdAt: Date.now()
            });
            scanPerformanceResources();
            scanPageForYouTubeData();
          } else if (isBlobLike) {
            const type = (obj.type || "").toLowerCase();
            blobRegistry.set(blobUrl, {
              blobUrl,
              blob: obj,
              kind: "Blob",
              mimeType: type,
              filename: (obj && typeof obj.name === "string") ? obj.name : "",
              size: obj.size || 0,
              createdAt: Date.now()
            });
            if (type.includes("mpegurl") || type.includes("dash") || (type.includes("xml") && obj.size < 524288)) {
              if (typeof obj.text === "function") {
                obj.text().then(text => {
                  inspectManifestText(blobUrl, text, win.location ? win.location.href : "");
                }).catch(() => {});
              }
            }
          }
          scanPageForYouTubeData();
        } catch (_) {}
        return blobUrl;
      };
    }

    if (win.URL && typeof win.URL.revokeObjectURL === "function") {
      const origRevokeObjectUrl = win.URL.revokeObjectURL;
      win.URL.revokeObjectURL = function (url) {
        if (typeof url === "string" && (url.startsWith("blob:") || blobRegistry.has(url))) {
          setTimeout(() => {
            try { origRevokeObjectUrl.call(win.URL, url); } catch (_) {}
            blobRegistry.delete(url);
          }, 60000);
          return;
        }
        return origRevokeObjectUrl.apply(this, arguments);
      };
    }

    function arrayBufferToBase64(buffer) {
      const bytes = new Uint8Array(buffer);
      const chunkSize = 32768;
      let binary = "";
      for (let i = 0; i < bytes.length; i += chunkSize) {
        binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunkSize));
      }
      return win.btoa(binary);
    }

    function tryInterceptBlobClick(anchor, href, downloadAttr, fallbackFn) {
      if (!href || (!href.startsWith("blob:http://") && !href.startsWith("blob:https://"))) {
        return false;
      }
      if (anchor.__radiumBypass === true || anchor.__fluxBypass === true) {
        delete anchor.__radiumBypass;
        delete anchor.__fluxBypass;
        return false;
      }
      if (win.event && (win.event.altKey || win.event.metaKey)) {
        return false;
      }

      // In unit test / mock environments without a real DOM createElement, allow mock click to proceed
      if (!win.document || typeof win.document.createElement !== "function") {
        return false;
      }

      const host = (win.location && win.location.hostname) ? win.location.hostname.toLowerCase() : "";
      const isAiSite = host.includes("chatgpt.com") || host.includes("openai.com") ||
                       host.includes("oaiusercontent.com") || host.includes("oaistatic.com") ||
                       host.includes("claude.ai") || host.includes("anthropic.com");

      const entry = blobRegistry.get(href);
      const blobObj = entry ? entry.blob : null;
      let targetFilename = downloadAttr || (entry && entry.filename) || "";
      const mimeType = (blobObj && blobObj.type) || (entry && entry.mimeType) || "";

      // Deduce file extension from MIME type if filename lacks one
      if (!targetFilename || !targetFilename.includes(".")) {
        let ext = "";
        if (mimeType.includes("python") || mimeType.includes("x-python")) ext = "py";
        else if (mimeType.includes("csv")) ext = "csv";
        else if (mimeType.includes("markdown") || mimeType.includes("md")) ext = "md";
        else if (mimeType.includes("json")) ext = "json";
        else if (mimeType.includes("png")) ext = "png";
        else if (mimeType.includes("jpeg") || mimeType.includes("jpg")) ext = "jpg";
        else if (mimeType.includes("pdf")) ext = "pdf";
        else if (mimeType.includes("zip")) ext = "zip";
        else if (mimeType.includes("html")) ext = "html";
        else if (mimeType.includes("text/plain")) ext = "txt";

        if (ext) {
          targetFilename = targetFilename ? `${targetFilename}.${ext}` : `artifact_${Date.now().toString(36)}.${ext}`;
        }
      }

      const extMatch = (targetFilename || "").match(/\.([a-zA-Z0-9_-]{1,16})$/);
      const isEligible = Boolean(downloadAttr || extMatch || isAiSite);

      if (!isEligible) {
        return false;
      }

      const effectiveFilename = targetFilename || `download_${Date.now().toString(36)}.bin`;
      const correlationId = "blob-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 8);

      const triggerFallback = () => {
        try {
          if (anchor && typeof fallbackFn === "function") {
            anchor.__radiumBypass = true;
            fallbackFn.call(anchor);
          } else if (win.document) {
            const fb = win.document.createElement("a");
            fb.href = href;
            fb.download = effectiveFilename;
            fb.__radiumBypass = true;
            (win.document.body || win.document.documentElement).appendChild(fb);
            fb.click();
            fb.remove();
          }
        } catch (_) {}
      };

      const deliverPayload = (b64, bType = (mimeType || "application/octet-stream"), bSize = 0) => {
        postToExtension({
          action: "radiumDirectBlobDownload",
          url: href,
          filename: effectiveFilename,
          mimeType: bType,
          size: bSize,
          dataBase64: b64,
          correlationId,
          pageTitle: win.document ? win.document.title : "",
          pageUrl: win.location ? win.location.href : ""
        });
      };

      const processAndSend = (b) => {
        const bType = (b && b.type) || mimeType || "application/octet-stream";
        const bSize = (b && b.size) || 0;

        if (typeof win.FileReader === "function") {
          const reader = new win.FileReader();
          reader.onload = () => {
            const res = String(reader.result || "");
            const commaIdx = res.indexOf(",");
            const b64 = commaIdx >= 0 ? res.slice(commaIdx + 1) : res;
            deliverPayload(b64, bType, bSize);
          };
          reader.onerror = () => {
            if (typeof b.arrayBuffer === "function") {
              b.arrayBuffer().then(buf => deliverPayload(arrayBufferToBase64(buf), bType, bSize)).catch(triggerFallback);
            } else {
              triggerFallback();
            }
          };
          reader.readAsDataURL(b);
        } else if (typeof b.arrayBuffer === "function") {
          b.arrayBuffer().then(buf => deliverPayload(arrayBufferToBase64(buf), bType, bSize)).catch(triggerFallback);
        } else {
          triggerFallback();
        }
      };

      if (blobObj) {
        processAndSend(blobObj);
        return true; // Intercepted! Native browser download shelf suppressed!
      } else {
        // Blob object not in local registry (e.g. created inside Web Worker, sandboxed frame, or pre-script).
        // DO NOT call win.fetch() here! Sites with strict CSP (such as ChatGPT's default-src 'none')
        // will block win.fetch(blobUrl). Extension background Service Worker has host permissions
        // and can fetch the blobUrl without CSP restrictions.
        deliverPayload("");
        return true; // Intercepted! Native browser download shelf suppressed!
      }
    }

    // 4. Hook HTMLAnchorElement.prototype.click & dispatchEvent (Synthetic / programmatic downloads on detached or attached anchors)
    const anchorProto = (typeof win.HTMLAnchorElement !== "undefined" && win.HTMLAnchorElement.prototype) ? win.HTMLAnchorElement.prototype : null;
    if (anchorProto && typeof anchorProto.click === "function") {
      const origAnchorClick = anchorProto.click;
      anchorProto.click = function () {
        try {
          let href = this.href || (typeof this.getAttribute === "function" ? (this.getAttribute("href") || this.getAttribute("data-download-url")) : "") || "";
          const downloadAttr = this.download || (typeof this.getAttribute === "function" ? (this.getAttribute("download") || "") : "") || "";

          if (href && !href.startsWith("blob:") && !href.startsWith("data:") && !href.startsWith("http://") && !href.startsWith("https://")) {
            try {
              href = new URL(href, win.location ? win.location.href : "").href;
            } catch (_) {}
          }

          if (href && (href.startsWith("blob:") || href.startsWith("http://") || href.startsWith("https://") || href.startsWith("data:"))) {
            postToExtension({
              action: "fluxAnchorClicked",
              url: href,
              download: downloadAttr,
              pageTitle: win.document ? win.document.title : "",
              pageUrl: win.location ? win.location.href : ""
            });
          }

          if (tryInterceptBlobClick(this, href, downloadAttr, origAnchorClick)) {
            return;
          }
        } catch (_) {}
        return origAnchorClick.apply(this, arguments);
      };
    }

    if (anchorProto && typeof anchorProto.dispatchEvent === "function") {
      const origDispatchEvent = anchorProto.dispatchEvent;
      anchorProto.dispatchEvent = function (ev) {
        try {
          if (ev && (ev.type === "click" || ev.type === "pointerup")) {
            let href = this.href || (typeof this.getAttribute === "function" ? (this.getAttribute("href") || this.getAttribute("data-download-url")) : "") || "";
            const downloadAttr = this.download || (typeof this.getAttribute === "function" ? (this.getAttribute("download") || "") : "") || "";

            if (href && !href.startsWith("blob:") && !href.startsWith("data:") && !href.startsWith("http://") && !href.startsWith("https://")) {
              try {
                href = new URL(href, win.location ? win.location.href : "").href;
              } catch (_) {}
            }

            if (href && (href.startsWith("blob:") || href.startsWith("http://") || href.startsWith("https://") || href.startsWith("data:"))) {
              postToExtension({
                action: "fluxAnchorClicked",
                url: href,
                download: downloadAttr,
                pageTitle: win.document ? win.document.title : "",
                pageUrl: win.location ? win.location.href : ""
              });
            }

            if (tryInterceptBlobClick(this, href, downloadAttr, (el) => origDispatchEvent.call(el, ev))) {
              return true;
            }
          }
        } catch (_) {}
        return origDispatchEvent.apply(this, arguments);
      };
    }

    function scanDomMediaElements() {
      try {
        if (!win.document || typeof win.document.querySelectorAll !== "function") return 0;
        const videos = Array.from(win.document.querySelectorAll("video"));
        let foundCount = 0;
        for (const v of videos) {
          const sources = typeof v.querySelectorAll === "function" ? Array.from(v.querySelectorAll("source")) : [];
          const domQualities = [];
          for (const s of sources) {
            const sUrl = s.src || (typeof s.getAttribute === "function" ? s.getAttribute("src") : "") || "";
            if (!sUrl || sUrl.startsWith("blob:") || sUrl.startsWith("data:")) continue;
            const labelAttr = (typeof s.getAttribute === "function")
              ? (s.getAttribute("label") || s.getAttribute("data-quality") || s.getAttribute("data-res") || s.getAttribute("res") || s.getAttribute("size") || "")
              : "";
            const urlResM = sUrl.match(/(?:^|[_\-/\.])(2160p|1440p|1080p|720p|480p|360p|240p|144p)(?:[_\-/\.?&]|$)/i);
            const rawRes = labelAttr
              ? (/^\d+$/.test(labelAttr) ? `${labelAttr}p` : labelAttr)
              : (urlResM ? urlResM[1].toLowerCase() : "");
            const hMatch = rawRes.match(/(\d{3,4})/);
            const h = hMatch ? parseInt(hMatch[1], 10) : 0;
            domQualities.push({
              id: (rawRes || `src-${domQualities.length}`).toLowerCase(),
              label: rawRes ? `${rawRes} (Direct)` : "Direct Stream",
              resolution: rawRes || "HD",
              height: h,
              ext: sUrl.toLowerCase().includes(".webm") ? "webm" : "mp4",
              videoUrl: sUrl,
              videoFilter: rawRes || "best"
            });
          }
          domQualities.sort((a, b) => (b.height || 0) - (a.height || 0));

          const primarySrc = v.currentSrc || v.src || (typeof v.getAttribute === "function" ? v.getAttribute("src") : "") ||
                             (domQualities.length > 0 ? domQualities[0].videoUrl : "");
          if (!primarySrc || primarySrc.startsWith("data:")) continue;
          if (primarySrc.startsWith("blob:")) {
            scanPerformanceResources();
            scanPageForYouTubeData();
            continue;
          }

          const liveH = Number(v.videoHeight || 0);
          const liveRes = liveH > 0 ? `${liveH}p` : (domQualities.length > 0 ? domQualities[0].resolution : "");
          const lower = primarySrc.toLowerCase();
          const isHls = lower.includes(".m3u8") || lower.includes(".m3u");
          const isDash = lower.includes(".mpd");
          sendMedia({
            streamType: isHls ? "HLS" : (isDash ? "DASH" : "HTTP_FILE"),
            sourceType: isHls ? "HLS_MANIFEST" : (isDash ? "DASH_MANIFEST" : "DIRECT_PROGRESSIVE"),
            ext: isHls ? "m3u8" : (isDash ? "mpd" : (lower.includes(".webm") ? "webm" : "mp4")),
            url: primarySrc,
            resolution: liveRes,
            qualities: domQualities,
            pageTitle: (win.document ? win.document.title : "") || "video",
            referer: win.location ? win.location.href : ""
          });
          foundCount++;
        }
        return foundCount;
      } catch (_) {
        return 0;
      }
    }

    win.__RadiumCatchTestHook = win.__FluxCatchTestHook = {
      scanDomMediaElements,
      scanPageForYouTubeData,
      scanPerformanceResources
    };

    // 5. Listen for active blob/MSE & page media discovery requests from content-script.js
    if (typeof win.addEventListener === "function") {
      win.addEventListener("message", (ev) => {
        if (!ev || !ev.data) return;
        if (ev.data.action === "radiumBlobFallback") {
          try {
            const fallbackUrl = ev.data.url;
            const fallbackName = ev.data.filename || "download";
            if (fallbackUrl && win.document) {
              const fb = win.document.createElement("a");
              fb.href = fallbackUrl;
              fb.download = fallbackName;
              fb.__radiumBypass = true;
              (win.document.body || win.document.documentElement).appendChild(fb);
              fb.click();
              fb.remove();
            }
          } catch (_) {}
          return;
        }
        if (ev.data.action === "radiumScanPageMedia" || ev.data.action === "fluxScanPageMedia") {
          scanPerformanceResources();
          scanPageForYouTubeData();
          scanDomMediaElements();
          return;
        }
        if (ev.data.action === "fluxGetBlobData") {
          const blobUrl = ev.data.blobUrl || "";
          const requestId = ev.data.requestId || "";
          const blobEntry = blobRegistry.get(blobUrl);

          const arrayBufferToBase64 = (buffer) => {
            const bytes = new Uint8Array(buffer);
            const chunkSize = 32768;
            let binary = "";
            for (let i = 0; i < bytes.length; i += chunkSize) {
              binary += String.fromCharCode.apply(null, bytes.subarray(i, i + chunkSize));
            }
            return win.btoa(binary);
          };

          const processBlob = (blobObj, mimeType) => {
            if (!blobObj) {
              postToExtension({ action: "fluxBlobDataResponse", requestId, blobUrl, success: false, error: "INVALID_BLOB" });
              return;
            }
            if (typeof win.FileReader === "function") {
              const reader = new win.FileReader();
              reader.onload = () => {
                const res = String(reader.result || "");
                const commaIdx = res.indexOf(",");
                const b64 = commaIdx >= 0 ? res.slice(commaIdx + 1) : res;
                postToExtension({
                  action: "fluxBlobDataResponse",
                  requestId,
                  blobUrl,
                  success: true,
                  dataBase64: b64,
                  mimeType: mimeType || blobObj.type || "application/octet-stream",
                  size: blobObj.size || 0
                });
              };
              reader.onerror = () => {
                if (typeof blobObj.arrayBuffer === "function") {
                  blobObj.arrayBuffer().then(buf => {
                    const b64 = arrayBufferToBase64(buf);
                    postToExtension({
                      action: "fluxBlobDataResponse",
                      requestId,
                      blobUrl,
                      success: true,
                      dataBase64: b64,
                      mimeType: mimeType || blobObj.type || "application/octet-stream",
                      size: blobObj.size || 0
                    });
                  }).catch(err => {
                    postToExtension({ action: "fluxBlobDataResponse", requestId, blobUrl, success: false, error: String(err) });
                  });
                } else {
                  postToExtension({ action: "fluxBlobDataResponse", requestId, blobUrl, success: false, error: "READER_ERROR" });
                }
              };
              reader.readAsDataURL(blobObj);
              return;
            }
            if (typeof blobObj.arrayBuffer === "function") {
              blobObj.arrayBuffer().then(buf => {
                const b64 = arrayBufferToBase64(buf);
                postToExtension({
                  action: "fluxBlobDataResponse",
                  requestId,
                  blobUrl,
                  success: true,
                  dataBase64: b64,
                  mimeType: mimeType || blobObj.type || "application/octet-stream",
                  size: blobObj.size || 0
                });
              }).catch(err => {
                postToExtension({ action: "fluxBlobDataResponse", requestId, blobUrl, success: false, error: String(err) });
              });
            } else {
              postToExtension({ action: "fluxBlobDataResponse", requestId, blobUrl, success: false, error: "CANNOT_READ_BLOB" });
            }
          };

          if (blobEntry && blobEntry.blob) {
            processBlob(blobEntry.blob, blobEntry.mimeType);
          } else {
            postToExtension({ action: "fluxBlobDataResponse", requestId, blobUrl, success: false, error: "BLOB_NOT_IN_REGISTRY" });
          }
          return;
        }
        if (ev.data.action !== "fluxDiscoverBlobSource") return;
        try {
          const blobUrl = ev.data.blobUrl || "";
          const correlationId = ev.data.correlationId || makeCorrelationId("blob");
          scanPerformanceResources();
          const ytFound = scanPageForYouTubeData();
          scanDomMediaElements();
          const blobMeta = blobRegistry.get(blobUrl) || null;
          postToExtension({
            action: "catCatchBlobDiscoveryResult",
            blobUrl,
            correlationId,
            ytFound,
            blobMeta,
            pageUrl: win.location ? win.location.href : ""
          });
        } catch (_) {}
      });
    }

    // 6. Listen for YouTube SPA & DOM lifecycle events (automatic video + quality capture on play)
    if (win.document && typeof win.document.addEventListener === "function") {
      win.document.addEventListener("DOMContentLoaded", () => {
        scanPerformanceResources();
        scanPageForYouTubeData();
        scanDomMediaElements();
      });
      win.document.addEventListener("yt-navigate-finish", scanPageForYouTubeData);
      win.document.addEventListener("yt-page-data-updated", scanPageForYouTubeData);
      win.document.addEventListener("loadedmetadata", () => {
        scanPerformanceResources();
        scanPageForYouTubeData();
        scanDomMediaElements();
      }, true);
      win.document.addEventListener("play", () => {
        scanPerformanceResources();
        scanPageForYouTubeData();
        scanDomMediaElements();
      }, true);
      win.document.addEventListener("playing", () => {
        scanPerformanceResources();
        scanPageForYouTubeData();
        scanDomMediaElements();
      }, true);
    }
    if (typeof win.addEventListener === "function") {
      win.addEventListener("load", () => {
        scanPerformanceResources();
        scanPageForYouTubeData();
        scanDomMediaElements();
      });
    }

    scanPageForYouTubeData();
    scanDomMediaElements();
  }

  // Execute in browser window
  if (typeof window !== "undefined") {
    installDeepSearchHooks(window, "flux-deepsearch", null);
  }

  if (typeof module !== "undefined" && module.exports) {
    module.exports = { installDeepSearchHooks };
  }
})();
