// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from xifangczy/cat-catch (js/init.js) for Radium Browser Integration.
// Preserves Cat Catch's G configuration model, Ext/Type/Regex rule tables, header cache,
// per-tab/per-URL click correlation tables, redirect chains, and diagnostic pipeline log ring buffer.

var SourceType = {
  HLS_MANIFEST: "HLS_MANIFEST",
  DASH_MANIFEST: "DASH_MANIFEST",
  DIRECT_PROGRESSIVE: "DIRECT_PROGRESSIVE",
  YOUTUBE_EXTRACTION: "YOUTUBE_EXTRACTION",
  UNSUPPORTED: "UNSUPPORTED"
};

var G = {
  version: "0.3.1",
  nativeHostName: "com.flux.downloadmanager",
  fallbackNativeHostNames: ["org.flux.native", "org.openidm.native"],
  enableCapture: true,
  enableVideoOverlay: true,
  minFileSizeBytes: 1048576, // 1 MiB minimum for size-only fallback file interception
  disabledHosts: new Set(),
  bypassNextDownload: false,
  lastClickedLink: null,
  handledDownloadIds: new Set(),
  handledDownloadUrls: new Map(),
  tabUrlMap: new Map(),
  sizeProbeInFlight: new Map(),
  playerErrors: new Map(),

  // Multi-tab and per-URL link click correlation & Alt-bypass tracking (Defect C fix)
  clickedLinksByTab: new Map(),      // tabId -> Array<ClickRecord>
  clickedLinksByUrl: new Map(),      // normalizedUrl -> ClickRecord
  bypassTabs: new Map(),             // tabId -> timestampMs
  bypassUrls: new Map(),             // normalizedUrl -> timestampMs
  bypassDownloadIds: new Set(),      // downloadItem.id values that were Alt-bypassed
  redirectChainMap: new Map(),       // fromUrl -> toUrl
  reverseRedirectMap: new Map(),     // toUrl -> initialUrl
  requestMetaMap: new Map(),         // requestId -> { initialUrl, currentUrl, tabId, referrer, correlationId }
  recentDownloadResponses: new Map(),// url -> { url, originalUrl, filename, isAttachment, mime, size, tabId, headers, timestamp, correlationId }
  activeDownloadOffers: new Map(),   // dedupKey -> timestampMs

  // Per-tab discovered blob: video URLs and their resolution status (Defect A fix)
  blobMediaMap: new Map(),           // tabId -> Map<blobUrl, { blobUrl, resolvedCandidateId, status, timestamp }>

  // Bounded diagnostic trace log ring buffer with correlation IDs (Requirement 3)
  diagnosticLogs: [],

  // Explicit typed source constants (Defect B fix)
  SourceType,

  // Adapted from Cat Catch default media extension rules (G.Ext)
  // Minimum video threshold 64 KiB filters out tiny UI hover icons (< 64 KiB) while capturing real video streams
  Ext: [
    { ext: "m3u8", size: 0, state: true, streamType: "HLS", sourceType: SourceType.HLS_MANIFEST },
    { ext: "m3u", size: 0, state: true, streamType: "HLS", sourceType: SourceType.HLS_MANIFEST },
    { ext: "mpd", size: 0, state: true, streamType: "DASH", sourceType: SourceType.DASH_MANIFEST },
    { ext: "ism", size: 0, state: true, streamType: "MSS", sourceType: SourceType.DASH_MANIFEST },
    { ext: "mp4", size: 65536, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "webm", size: 65536, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "mkv", size: 65536, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "flv", size: 65536, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "mov", size: 65536, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "m4v", size: 65536, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "avi", size: 65536, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "mp3", size: 32768, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "m4a", size: 32768, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "flac", size: 32768, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "ogg", size: 32768, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "opus", size: 32768, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "wav", size: 32768, state: true, streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "vtt", size: 0, state: true, streamType: "SUBTITLE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { ext: "srt", size: 0, state: true, streamType: "SUBTITLE", sourceType: SourceType.DIRECT_PROGRESSIVE }
  ],

  // Adapted from Cat Catch MIME type rules (G.Type)
  Type: [
    { type: "application/vnd.apple.mpegurl", size: 0, state: true, ext: "m3u8", streamType: "HLS", sourceType: SourceType.HLS_MANIFEST },
    { type: "application/x-mpegurl", size: 0, state: true, ext: "m3u8", streamType: "HLS", sourceType: SourceType.HLS_MANIFEST },
    { type: "audio/mpegurl", size: 0, state: true, ext: "m3u8", streamType: "HLS", sourceType: SourceType.HLS_MANIFEST },
    { type: "audio/x-mpegurl", size: 0, state: true, ext: "m3u8", streamType: "HLS", sourceType: SourceType.HLS_MANIFEST },
    { type: "video/mpegurl", size: 0, state: true, ext: "m3u8", streamType: "HLS", sourceType: SourceType.HLS_MANIFEST },
    { type: "video/x-mpegurl", size: 0, state: true, ext: "m3u8", streamType: "HLS", sourceType: SourceType.HLS_MANIFEST },
    { type: "application/dash+xml", size: 0, state: true, ext: "mpd", streamType: "DASH", sourceType: SourceType.DASH_MANIFEST },
    { type: "video/vnd.mpeg.dash.mpd", size: 0, state: true, ext: "mpd", streamType: "DASH", sourceType: SourceType.DASH_MANIFEST },
    { type: "application/vnd.ms-sstr+xml", size: 0, state: true, ext: "ism", streamType: "MSS", sourceType: SourceType.DASH_MANIFEST },
    { type: "video/mp4", size: 65536, state: true, ext: "mp4", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "video/webm", size: 65536, state: true, ext: "webm", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "video/x-matroska", size: 65536, state: true, ext: "mkv", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "video/quicktime", size: 65536, state: true, ext: "mov", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "video/x-flv", size: 65536, state: true, ext: "flv", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "video/x-msvideo", size: 65536, state: true, ext: "avi", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "video/ogg", size: 65536, state: true, ext: "ogv", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "audio/mpeg", size: 32768, state: true, ext: "mp3", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "audio/mp4", size: 32768, state: true, ext: "m4a", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "audio/webm", size: 32768, state: true, ext: "webm", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "audio/flac", size: 32768, state: true, ext: "flac", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE },
    { type: "audio/ogg", size: 32768, state: true, ext: "ogg", streamType: "HTTP_FILE", sourceType: SourceType.DIRECT_PROGRESSIVE }
  ],

  // Adapted from Cat Catch URL Regex rules (G.Regex)
  Regex: [
    { type: "i", regex: "https?://[^\\s\"']+\\.m3u8(\\?[^\\s\"']*)?$", ext: "m3u8", streamType: "HLS", sourceType: SourceType.HLS_MANIFEST, state: true },
    { type: "i", regex: "https?://[^\\s\"']+\\.mpd(\\?[^\\s\"']*)?$", ext: "mpd", streamType: "DASH", sourceType: SourceType.DASH_MANIFEST, state: true },
    { type: "i", regex: "https?://[^\\s\"']+\\.ism/[Mm]anifest(\\?[^\\s\"']*)?$", ext: "ism", streamType: "MSS", sourceType: SourceType.DASH_MANIFEST, state: true }
  ],

  // Ordinary file extensions intercepted by DownloadInterceptor (comprehensive suite)
  InterceptFileExts: new Set([
    // Archives & Compressed
    "zip", "rar", "7z", "tar", "gz", "bz2", "xz", "tgz", "tbz2", "txz", "z", "lz", "lzma", "cab", "iso", "img", "bin",
    // Installers & Executables & Packages
    "exe", "msi", "dmg", "pkg", "deb", "rpm", "apk", "appimage", "jar", "war", "ipa", "crx", "xpi", "whl", "nupkg",
    // Documents & E-books & Data
    "pdf", "docx", "xlsx", "pptx", "doc", "xls", "ppt", "odt", "ods", "odp", "rtf", "csv", "tsv", "epub", "mobi", "azw3", "torrent",
    // Code & Scripts & Artifacts (ChatGPT, Claude, IDE exports)
    "py", "ipynb", "js", "mjs", "cjs", "ts", "tsx", "jsx", "html", "htm", "css", "scss", "sass", "json", "jsonl",
    "xml", "yaml", "yml", "toml", "ini", "cfg", "conf", "sql", "sh", "bash", "zsh", "bat", "cmd", "ps1",
    "c", "cpp", "cc", "cxx", "h", "hpp", "hxx", "cs", "rs", "go", "java", "kt", "rb", "php", "lua", "r", "m", "swift",
    "txt", "md", "markdown", "log", "svg", "webp",
    // Media & Progressive Audio/Video
    "mp4", "mkv", "webm", "mov", "avi", "flv", "wmv", "m4v", "3gp", "ts",
    "mp3", "m4a", "flac", "wav", "aac", "ogg", "opus", "wma",
    // Virtual Disk Images
    "vhd", "vhdx", "vmdk", "ova", "ovf"
  ]),

  // Ordinary file MIME types eligible for automatic download interception
  InterceptMimeTypes: new Set([
    "application/pdf",
    "application/zip",
    "application/x-zip-compressed",
    "application/x-rar-compressed",
    "application/vnd.rar",
    "application/x-7z-compressed",
    "application/x-7z",
    "application/gzip",
    "application/x-gzip",
    "application/x-tar",
    "application/x-bzip",
    "application/x-bzip2",
    "application/x-xz",
    "application/x-iso9660-image",
    "application/x-msdownload",
    "application/x-msi",
    "application/x-dosexec",
    "application/vnd.microsoft.portable-executable",
    "application/x-executable",
    "application/x-sharedlib",
    "application/x-apple-diskimage",
    "application/x-debian-package",
    "application/x-deb",
    "application/x-redhat-package-manager",
    "application/x-rpm",
    "application/vnd.android.package-archive",
    "application/octet-stream",
    "application/binary",
    "application/force-download",
    "application/x-download",
    "application/msword",
    "application/vnd.ms-excel",
    "application/vnd.ms-powerpoint",
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    "application/vnd.openxmlformats-officedocument.presentationml.presentation",
    "application/x-bittorrent",
    "application/json",
    "application/xml",
    "text/xml",
    "text/csv",
    "text/plain",
    "text/markdown",
    "text/x-python",
    "image/svg+xml",
    "video/mp4",
    "video/webm",
    "video/x-matroska",
    "video/quicktime",
    "video/x-msvideo",
    "audio/mpeg",
    "audio/mp4",
    "audio/x-m4a",
    "audio/flac",
    "audio/wav",
    "audio/ogg"
  ]),

  // Known YouTube progressive (combined video+audio) itags; all other videoplayback itags are adaptive split tracks
  YouTubeProgressiveItags: new Set(["18", "22", "37", "43", "59", "78"]),

  // Segment extensions that must be suppressed when an HLS/DASH manifest is active on the tab
  SegmentExts: new Set(["ts", "m4s", "cmfv", "cmfa", "m4f", "aac"]),

  // Short-lived requestId -> { Referer, Origin, Cookie, User-Agent, Authorization } map
  requestHeadersMap: new Map(),

  // Per-tab detected media candidates: tabId -> Array<MediaCandidate>
  cacheData: new Map(),

  // Captured AES-128 keys from catch-script/catch.js: tabId -> Set<string>
  capturedKeys: new Map()
};

globalThis.G = G;
globalThis.SourceType = SourceType;

if (typeof module !== "undefined" && module.exports) {
  module.exports = { G, SourceType };
}
