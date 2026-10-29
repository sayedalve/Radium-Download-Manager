// SPDX-License-Identifier: Apache-2.0
// OpenIDM — N_m3u8DL-RE Stream Download Adapter, YouTube Extractor & Managed Worker Process Supervisor
// Integrates nilaoda/N_m3u8DL-RE (v0.3.0-beta), yt-dlp & FFmpeg via Job Objects / Process Groups
// and ACL-restricted @job.rsp response files.
// Implements FR-019, FR-020, FR-021, FR-022, NFR-015, THREAT-002, THREAT-004, and Defect B Fix.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "common/types.hpp"

namespace openidm::stream {

struct StreamProbeResult {
    std::string manifest_url;
    bool is_live{false};
    bool drm_protected{false};
    std::string drm_system;
    std::vector<StreamTrack> tracks;
};

struct StreamProgress {
    std::string task_id;
    TaskState phase{TaskState::kDownloading}; // kDownloading vs kMuxing vs kVerifying vs kFinalizing
    double percent{0.0};
    uint64_t downloaded_bytes{0};
    uint64_t total_bytes{0};
    uint64_t speed_bps{0};
    uint32_t completed_segments{0};
    uint32_t total_segments{0};
    std::string status_line;
};

struct MediaQualityExpectations {
    int expected_height{0};             // e.g. 1080 for 1080p, 720 for 720p, etc.
    std::string expected_container;     // "mp4", "mkv", "webm", "m4a"
    std::string expected_video_codec;   // e.g. "av01", "vp9", "h264", "avc1" (optional substring)
    std::string expected_audio_codec;   // e.g. "aac", "opus", "mp4a" (optional substring)
    double expected_duration_sec{0.0};  // from video details (if known)
    uint64_t expected_min_bytes{0};     // min size expectation (if known)
    bool expect_video{true};            // false for audio-only
    bool expect_audio{true};            // false for video-only
};

struct MediaQualityResult {
    bool ok{false};
    std::string error_code;
    std::string error_message;
    int probed_width{0};
    int probed_height{0};
    std::string probed_video_codec;
    std::string probed_audio_codec;
    double probed_duration_sec{0.0};
    uint64_t probed_file_size{0};
};

struct StreamAdapterConfig {
    std::filesystem::path nm3u8dl_binary_path;
    std::filesystem::path ffmpeg_binary_path;
    std::filesystem::path temp_root_dir;
    bool allow_loopback_manifests{false};
    std::filesystem::path fallback_worker_path{};
    std::filesystem::path ffprobe_binary_path{};
};

class Nm3u8dlReAdapter {
public:
    explicit Nm3u8dlReAdapter(StreamAdapterConfig cfg);

    // Verifies that the configured N_m3u8DL-RE worker binary exists and responds to --version
    Result<std::string> VerifyWorkerBinary() const;

    // Validates manifest URL or cached manifest body against SSRF / local file inclusion (THREAT-004)
    // and rejects arbitrary JSON descriptors masquerading as HLS/DASH manifests (Defect B fix).
    Result<void> ValidateManifestSafety(std::string_view url, std::string_view body) const;

    // Writes ACL-restricted @job.rsp response file (0600 permissions) so Cookie/Authorization
    // never appear in process command-line arguments (FR-021, THREAT-002).
    static Result<std::filesystem::path> WriteRestrictedResponseFile(
        const std::filesystem::path& task_tmp_dir,
        const std::string& filename,
        const std::vector<std::string>& lines);

    // Writes ACL-restricted Netscape-formatted cookie file for yt-dlp / external workers
    static Result<std::filesystem::path> WriteNetscapeCookieFile(
        const std::filesystem::path& task_tmp_dir,
        const std::string& filename,
        const std::string& cookie_str,
        const std::string& domain_hint);

    // Parses N_m3u8DL-RE meta.json output and detects unsupported DRM (NFR-015)
    static Result<StreamProbeResult> ParseMetaJson(const std::filesystem::path& meta_json_path);

    // Validates that output stream file is non-empty and contains valid container magic bytes
    // (MP4/M4A/MOV, MKV/WebM, MP3, AAC, FLAC, Ogg/Opus, TS).
    static bool VerifyContainerMagicBytes(const std::filesystem::path& file_path, const std::string& fmt);

    // Verifies downloaded media file using ffprobe (or container magic/header fallback)
    // against expected quality (resolution, codecs, duration, min size).
    static Result<MediaQualityResult> VerifyMediaQuality(
        const std::filesystem::path& file_path,
        const MediaQualityExpectations& expectations,
        const std::filesystem::path& ffprobe_path = {});

    // Constructs strongly typed yt-dlp format selector without silent downgrade to 360p
    static std::string BuildYtDlpFormatSelector(
        std::string_view requested_filter,
        int target_height,
        bool wants_audio_only,
        std::string_view preferred_container);

    // Phase 1: Probes a detected HLS/DASH manifest using the actual N_m3u8DL-RE worker (--skip-download --write-meta-json)
    Result<StreamProbeResult> ProbeManifest(
        const std::string& manifest_url,
        const std::map<std::string, std::string>& headers,
        const std::map<std::string, std::string>& auth_headers,
        const std::string& cached_manifest_body = "");

    // Phase 2: Executes HLS/DASH stream downloading and FFmpeg muxing via N_m3u8DL-RE worker process
    using ProgressCallback = std::function<void(const StreamProgress&)>;
    Result<std::filesystem::path> ExecuteStreamDownload(
        TaskModel& task,
        std::atomic<bool>& pause_requested,
        std::atomic<bool>& cancel_requested,
        uint64_t speed_limit_bps,
        ProgressCallback on_progress);

    // Phase 3: Executes YouTube / site-specific extraction, adaptive video+audio track download, and FFmpeg muxing
    // without passing JSON descriptors as .m3u8 manifests to N_m3u8DL-RE (Defect B fix).
    Result<std::filesystem::path> ExecuteYouTubeExtraction(
        TaskModel& task,
        std::atomic<bool>& pause_requested,
        std::atomic<bool>& cancel_requested,
        uint64_t speed_limit_bps,
        ProgressCallback on_progress);

private:
    StreamAdapterConfig m_cfg;
};

} // namespace openidm::stream
