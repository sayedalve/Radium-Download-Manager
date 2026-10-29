// SPDX-License-Identifier: Apache-2.0
// OpenIDM — Unified Task Model, Application Settings & Common Types
// Adapted from Brisk (lib/download_engine/model/download_item_model.dart) & OpenIDM PRS.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openidm {

enum class TaskType {
    kHttpFile,
    kStreamHls,
    kStreamDash,
    kStreamMss,
    kFtp,
    kTorrent,
    kRtsp,
    kSiteGrabber
};

enum class TaskState {
    kQueued,
    kProbing,
    kDownloading,
    kMuxing,
    kVerifying,
    kFinalizing,
    kPaused,
    kCompleted,
    kFailed,
    kCanceled
};

enum class ConflictPolicy {
    kAutoRename,
    kOverwrite,
    kFailIfExists
};

struct Error {
    std::string code;
    std::string message;
    bool retryable{false};
    bool user_action_resumable{false};
};

template <typename T>
struct Result {
    std::optional<T> value;
    std::optional<Error> error;

    bool ok() const { return !error.has_value(); }
    static Result<T> Ok(T val) { return Result<T>{std::move(val), std::nullopt}; }
    static Result<T> Err(std::string code, std::string msg, bool retryable = false, bool resumable = false) {
        return Result<T>{std::nullopt, Error{std::move(code), std::move(msg), retryable, resumable}};
    }
};

template <>
struct Result<void> {
    std::optional<Error> error;

    bool ok() const { return !error.has_value(); }
    static Result<void> Ok() { return Result<void>{std::nullopt}; }
    static Result<void> Err(std::string code, std::string msg, bool retryable = false, bool resumable = false) {
        return Result<void>{Error{std::move(code), std::move(msg), retryable, resumable}};
    }
};

// ByteRange & Segment data model definitions
struct Segment {
    uint32_t segment_id{0};
    uint64_t start_offset{0};
    uint64_t current_offset{0};
    uint64_t end_offset{0}; // Inclusive byte index
    bool completed{false};

    uint64_t total_bytes() const {
        return (end_offset >= start_offset) ? (end_offset - start_offset + 1) : 0;
    }
    uint64_t downloaded_bytes() const {
        if (completed) return total_bytes();
        return (current_offset > start_offset) ? (current_offset - start_offset) : 0;
    }
    uint64_t remaining_bytes() const {
        if (completed || current_offset > end_offset) return 0;
        return end_offset - current_offset + 1;
    }
    bool is_valid() const {
        return start_offset <= end_offset && current_offset >= start_offset;
    }
};

struct StreamTrack {
    std::string id;
    std::string media_type; // "VIDEO", "AUDIO", "SUBTITLES"
    std::string group_id;
    std::string language;
    std::string name;
    std::string codecs;
    std::string resolution; // e.g., "1920x1080"
    uint64_t bandwidth{0};  // bps
    double frame_rate{0.0};
    uint32_t segments_count{0};
    double duration_sec{0.0};
    bool encrypted{false};
    std::string encryption_method; // "NONE", "AES_128", "SAMPLE_AES_CTR", "CENC"
};

struct StreamSelectionOptions {
    std::string video_filter{"best"};     // -sv argument
    std::string audio_filter{"best"};     // -sa argument (default to best to avoid multi-language audio bloat)
    std::string subtitle_filter{"all"};   // -ss argument
    std::string output_format{"mp4"};     // "mp4" or "mkv"
    uint32_t thread_count{8};
    bool concurrent_download{true};       // -mt
    std::string custom_hls_key_hex;       // Optional user-supplied AES-128 key
    std::string cached_manifest_body;     // Optional cached manifest from Cat Catch Deep Search
};

enum class PauseReason {
    kNone,
    kUser,
    kSystemSuspend,
    kError,
    kQueueSchedule
};

inline std::string PauseReasonToString(PauseReason r) {
    switch (r) {
        case PauseReason::kUser: return "USER";
        case PauseReason::kSystemSuspend: return "SYSTEM_SUSPEND";
        case PauseReason::kError: return "ERROR";
        case PauseReason::kQueueSchedule: return "QUEUE_SCHEDULE";
        default: return "NONE";
    }
}

inline PauseReason PauseReasonFromString(const std::string& s) {
    if (s == "USER") return PauseReason::kUser;
    if (s == "SYSTEM_SUSPEND") return PauseReason::kSystemSuspend;
    if (s == "ERROR") return PauseReason::kError;
    if (s == "QUEUE_SCHEDULE") return PauseReason::kQueueSchedule;
    return PauseReason::kNone;
}

struct TaskModel {
    std::string task_id;
    TaskType task_type{TaskType::kHttpFile};
    TaskState state{TaskState::kQueued};
    PauseReason pause_reason{PauseReason::kNone};
    std::string queue_id{"main"};
    int32_t queue_order{0};
    std::string category{"General"};
    std::string url;
    std::string page_url;
    std::string file_name;
    std::string save_dir;
    std::string part_path;
    std::string correlation_id;
    uint64_t total_bytes{0};
    uint64_t downloaded_bytes{0};
    uint64_t speed_bps{0};
    uint64_t uploaded_bytes{0};                       // Real upload accounting for BitTorrent (0 for HTTP/Streams)
    uint64_t upload_speed_bps{0};                     // Outgoing payload transfer rate
    uint64_t speed_limit_bps{0};                      // Per-task bandwidth limit in bps (0 = no task-specific limit)
    double stream_percent{0.0};
    uint32_t completed_segments{0};
    uint32_t total_segments{0};
    bool range_supported{false};
    uint32_t max_connections{8};
    ConflictPolicy conflict_policy{ConflictPolicy::kAutoRename};
    uint32_t retry_count{0};
    bool scheduled{false};
    std::string schedule_time_local;
    std::string schedule_stop_time_local;             // Optional scheduled stop time ("HH:MM")
    std::string status_detail;                        // Granular stage (e.g., "Downloading video segments (5/10)")
    std::string etag;
    std::string last_modified;
    std::map<std::string, std::string> headers;       // Non-sensitive: Referer, Origin, User-Agent
    std::map<std::string, std::string> auth_headers;  // Sensitive: Cookie, Authorization (DPAPI/AES encrypted at rest)
    std::vector<std::string> alternate_urls;          // User-configured or discovered alternate mirror URLs
    size_t current_url_index{0};                      // Current mirror index attempted
    StreamSelectionOptions stream_options;
    std::string expected_sha256;
    std::string error_code;
    std::string error_message;
    int64_t created_at_ms{0};
    int64_t updated_at_ms{0};
    int64_t completed_at_ms{0};
    std::vector<Segment> segments;
    std::vector<std::string> torrent_file_paths;
    std::vector<uint8_t> torrent_file_selection;
};

struct QueueConfig {
    std::string queue_id{"main"};
    std::string name{"Main Download Queue"};
    uint32_t max_concurrent_tasks{3};
    uint64_t speed_limit_bps{0};
    bool schedule_enabled{false};
    std::string start_time_local; // "HH:MM"
    std::string stop_time_local;  // "HH:MM"
    std::string post_completion_action{"NONE"};
};

struct AppSettings {
    std::string theme{"coral_dark"}; // Flux Coral Charcoal identity ("coral_dark")
    uint32_t max_concurrent_tasks{3};
    uint64_t speed_limit_bps{0};
    uint32_t default_connections_per_task{8};
    std::string default_download_dir;
    bool category_subfolders_enabled{true};
    ConflictPolicy conflict_policy{ConflictPolicy::kAutoRename};
    bool auto_start_downloads{true};
    int stall_timeout_ms{3000};
    uint64_t min_split_bytes{131072};
    bool browser_capture_enabled{true};
    uint64_t min_capture_size_bytes{1048576};
    bool close_to_tray{true};
    bool start_minimized{false};
    bool reduced_motion{false};
    bool show_completion_notification{true};
    bool av_strict_mode{false};                       // Fail task on scanner error/timeout rather than marking completed

    // Proxy & Authentication (Phase 2 Feature B)
    std::string proxy_mode{"system"};                 // "direct", "system", "manual"
    std::string proxy_server;                         // e.g. "http://127.0.0.1:8080"
    std::string proxy_bypass;                         // e.g. "<local>;*.internal"
    std::string proxy_username;
    std::string proxy_password_encrypted;             // DPAPI-encrypted blob/base64
    std::string server_auth_username;
    std::string server_auth_password_encrypted;       // DPAPI-encrypted blob/base64

    // Configurable Categories & Destination Folders (Phase 2 Feature E)
    std::map<std::string, std::string> category_folders;           // Category -> Custom Directory Path
    std::map<std::string, std::string> custom_extension_categories;// Extension (lowercase) -> Category Name
    std::map<std::string, std::string> site_rules;                 // Domain -> Category or Directory Path

    // Queue & Retry Automation (Phase 2 Feature D)
    uint32_t max_auto_retries{5};
    uint32_t retry_interval_sec{15};
    std::string queue_completion_action{"NONE"};      // "NONE", "SLEEP", "SHUTDOWN"
    std::string table_column_widths;

    bool operator==(const AppSettings& other) const {
        return theme == other.theme &&
               max_concurrent_tasks == other.max_concurrent_tasks &&
               speed_limit_bps == other.speed_limit_bps &&
               default_connections_per_task == other.default_connections_per_task &&
               default_download_dir == other.default_download_dir &&
               category_subfolders_enabled == other.category_subfolders_enabled &&
               conflict_policy == other.conflict_policy &&
               auto_start_downloads == other.auto_start_downloads &&
               stall_timeout_ms == other.stall_timeout_ms &&
               min_split_bytes == other.min_split_bytes &&
               browser_capture_enabled == other.browser_capture_enabled &&
               min_capture_size_bytes == other.min_capture_size_bytes &&
               close_to_tray == other.close_to_tray &&
               start_minimized == other.start_minimized &&
               reduced_motion == other.reduced_motion &&
               show_completion_notification == other.show_completion_notification &&
               av_strict_mode == other.av_strict_mode &&
               proxy_mode == other.proxy_mode &&
               proxy_server == other.proxy_server &&
               proxy_bypass == other.proxy_bypass &&
               proxy_username == other.proxy_username &&
               proxy_password_encrypted == other.proxy_password_encrypted &&
               server_auth_username == other.server_auth_username &&
               server_auth_password_encrypted == other.server_auth_password_encrypted &&
               category_folders == other.category_folders &&
               custom_extension_categories == other.custom_extension_categories &&
               site_rules == other.site_rules &&
               max_auto_retries == other.max_auto_retries &&
               retry_interval_sec == other.retry_interval_sec &&
               queue_completion_action == other.queue_completion_action &&
               table_column_widths == other.table_column_widths;
    }
    bool operator!=(const AppSettings& other) const { return !(*this == other); }
};

std::string TaskTypeToString(TaskType t);
TaskType TaskTypeFromString(const std::string& s);
std::string TaskStateToString(TaskState s);
TaskState TaskStateFromString(const std::string& s);
std::string ConflictPolicyToString(ConflictPolicy p);
ConflictPolicy ConflictPolicyFromString(const std::string& s);
std::string InferCategoryFromFilename(const std::string& filename);
std::string InferCategoryFromMime(const std::string& mime_type);
int64_t NowEpochMs();
std::string GenerateUuidV4();

} // namespace openidm
