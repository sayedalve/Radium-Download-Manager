// SPDX-License-Identifier: Apache-2.0
#include "core/download_controller.hpp"
#include "brisk_engine/http_client.hpp"

#include "common/crypto_utils.hpp"
#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {
void TriggerSystemShutdown() {
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp;
        if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        }
        CloseHandle(hToken);
    }
    InitiateSystemShutdownExW(nullptr, nullptr, 0, TRUE, FALSE, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_OTHER);
}

void TriggerSystemSleep() {
    SetSystemPowerState(TRUE, FALSE);
}
} // namespace
#endif

namespace {
std::string BytesToHexStr(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<unsigned int>(b);
    }
    return oss.str();
}

std::vector<uint8_t> HexStrToBytes(std::string_view hex) {
    std::vector<uint8_t> bytes;
    if (hex.size() % 2 != 0) return bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned int byte_val = 0;
        std::stringstream ss;
        ss << std::hex << hex.substr(i, 2);
        if (ss >> byte_val) {
            bytes.push_back(static_cast<uint8_t>(byte_val));
        } else {
            return {};
        }
    }
    return bytes;
}

std::string ExtractHostFromUrl(const std::string& url) {
    if (url.empty()) return "";
    auto parsed = openidm::brisk::HttpClient::ParseUrl(url);
    if (parsed.ok()) return parsed.value->host;
    return "";
}

std::string ExtractExtensionLower(const std::string& filename) {
    if (filename.empty()) return "";
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= filename.size()) return "";
    std::string ext = filename.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return ext;
}

std::optional<std::filesystem::path> ResolveTaskOutputPath(
    const std::filesystem::path& save_dir,
    const std::filesystem::path& relative_path) {
    if (relative_path.empty() || relative_path.is_absolute() || relative_path.has_root_name() ||
        relative_path.has_root_directory()) {
        return std::nullopt;
    }
    for (const auto& component : relative_path) {
        if (component == "..") return std::nullopt;
    }

    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(save_dir, ec);
    if (ec) return std::nullopt;
    const auto target = std::filesystem::weakly_canonical(root / relative_path, ec);
    if (ec) return std::nullopt;
    const auto relative_target = target.lexically_relative(root);
    if (relative_target.empty() || relative_target.is_absolute()) return std::nullopt;
    const auto first_component = relative_target.begin();
    if (first_component == relative_target.end() || *first_component == "..") return std::nullopt;
    return target;
}

bool IsValidTimeFormat(const std::string& t) {
    if (t.empty()) return true;
    if (t.size() != 5 || t[2] != ':') return false;
    if (!std::isdigit(static_cast<unsigned char>(t[0])) || !std::isdigit(static_cast<unsigned char>(t[1])) ||
        !std::isdigit(static_cast<unsigned char>(t[3])) || !std::isdigit(static_cast<unsigned char>(t[4]))) {
        return false;
    }
    int hh = (t[0] - '0') * 10 + (t[1] - '0');
    int mm = (t[3] - '0') * 10 + (t[4] - '0');
    return hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59;
}

bool IsTimeInScheduledWindow(const std::string& cur, const std::string& start, const std::string& stop) {
    if (!IsValidTimeFormat(cur) || cur.empty()) return false;
    if (!IsValidTimeFormat(start) || !IsValidTimeFormat(stop)) return false;
    if (start.empty() && stop.empty()) return true;
    if (!start.empty() && stop.empty()) {
        return cur >= start;
    }
    if (start.empty() && !stop.empty()) {
        return cur < stop;
    }
    // Both start and stop are present
    if (start < stop) {
        // Normal intraday window, e.g. 14:00 to 18:00
        return cur >= start && cur < stop;
    } else if (start > stop) {
        // Overnight window crossing midnight, e.g. 23:00 to 06:00
        return cur >= start || cur < stop;
    } else {
        // start == stop
        return false;
    }
}

bool HasScheduleWindowElapsed(const std::string& cur, const std::string& start, const std::string& stop) {
    if (!IsValidTimeFormat(cur) || cur.empty()) return false;
    if (!IsValidTimeFormat(start) || !IsValidTimeFormat(stop)) return false;
    if (stop.empty()) return false;
    if (start < stop) {
        // Normal intraday window [start, stop)
        // If cur >= stop, the window has elapsed. If cur < start, it is in the future.
        return cur >= stop;
    } else if (start > stop) {
        // Overnight window [start, midnight) U [midnight, stop)
        // The non-active period is [stop, start).
        return cur >= stop && cur < start;
    }
    return false;
}
} // namespace

namespace openidm {

DownloadController::DownloadController(ControllerConfig cfg)
    : m_cfg(std::move(cfg)),
      m_max_concurrent(m_cfg.max_concurrent_tasks),
      m_global_speed_limit_bps(m_cfg.global_speed_limit_bps) {
    m_settings.max_concurrent_tasks = m_cfg.max_concurrent_tasks;
    m_settings.speed_limit_bps = m_cfg.global_speed_limit_bps;
    m_settings.default_download_dir = m_cfg.default_download_dir.string();
    m_settings.stall_timeout_ms = m_cfg.stall_timeout_ms;
    m_settings.min_split_bytes = m_cfg.min_split_bytes;
}

void DownloadController::SetTorrentMetadataSelectionCallback(
    torrent::TorrentMetadataSelectionCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cfg.torrent_metadata_selection_callback = std::move(callback);
}

DownloadController::~DownloadController() {
    Shutdown();
}

Result<void> DownloadController::Initialize() {
    std::error_code ec;
    std::filesystem::create_directories(m_cfg.default_download_dir, ec);
    std::filesystem::create_directories(m_cfg.temp_root_dir, ec);

    auto open_res = m_store.Open(m_cfg.state_db_path);
    if (!open_res.ok()) return open_res;

    auto rec_res = m_store.RecoverInterruptedTasks(65536);
    if (rec_res.ok() && *rec_res.value > 0) {
        OPENIDM_LOG_INFO("Controller", "Recovered " + std::to_string(*rec_res.value) + " interrupted tasks from SQLite WAL");
    }

    auto loaded = m_store.ListTasks();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (loaded.ok()) {
            std::string startup_hh_mm;
            if (m_cfg.local_time_override) {
                startup_hh_mm = m_cfg.local_time_override();
            } else {
                auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                std::tm local_tm{};
#ifdef _WIN32
                localtime_s(&local_tm, &now_t);
#else
                localtime_r(&now_t, &local_tm);
#endif
                char time_buf[16] = {0};
                std::strftime(time_buf, sizeof(time_buf), "%H:%M", &local_tm);
                startup_hh_mm = time_buf;
            }
            for (auto& t : *loaded.value) {
                if (t.scheduled && !t.schedule_stop_time_local.empty()) {
                    bool elapsed = HasScheduleWindowElapsed(startup_hh_mm, t.schedule_time_local, t.schedule_stop_time_local);
                    if (elapsed) {
                        t.scheduled = false;
                        t.status_detail = "Schedule Expired (Window elapsed while offline)";
                        t.schedule_time_local.clear();
                        t.schedule_stop_time_local.clear();
                        m_store.UpsertTask(t);
                    }
                }
                m_tasks[t.task_id] = std::move(t);
            }
        }
        LoadPersistedSettingsLocked();
    }

    m_stopping = false;
    m_scheduler_thread = std::thread(&DownloadController::ScheduleLoop, this);
    return Result<void>::Ok();
}

void DownloadController::LoadPersistedSettingsLocked() {
    auto sres = m_store.GetSetting("app_settings", "");
    if (!sres.ok() || sres.value->empty()) return;
    try {
        auto val = json::Parse(*sres.value);
        m_settings.theme = val["theme"].as_string("coral_dark");
        if (m_settings.theme == "dark") {
            m_settings.theme = "coral_dark";
        }
        m_settings.max_concurrent_tasks = std::clamp<uint32_t>(
            static_cast<uint32_t>(val["maxConcurrentTasks"].as_int64(m_cfg.max_concurrent_tasks)), 1u, 16u);
        m_settings.speed_limit_bps = static_cast<uint64_t>(val["speedLimitBps"].as_int64(m_cfg.global_speed_limit_bps));
        m_settings.default_connections_per_task = std::clamp<uint32_t>(
            static_cast<uint32_t>(val["defaultConnectionsPerTask"].as_int64(8)), 1u, 32u);
        std::string saved_dir = val["defaultDownloadDir"].as_string("");
        if (!saved_dir.empty()) {
            m_settings.default_download_dir = saved_dir;
            m_cfg.default_download_dir = saved_dir;
        }
        m_settings.category_subfolders_enabled = val["categorySubfoldersEnabled"].as_bool(true);
        m_settings.conflict_policy = ConflictPolicyFromString(val["conflictPolicy"].as_string("auto_rename"));
        m_settings.auto_start_downloads = val["autoStartDownloads"].as_bool(true);
        m_settings.stall_timeout_ms = std::clamp<int>(
            static_cast<int>(val["stallTimeoutMs"].as_int64(m_cfg.stall_timeout_ms)), 500, 60000);
        m_settings.min_split_bytes = std::clamp<uint64_t>(
            static_cast<uint64_t>(val["minSplitBytes"].as_int64(static_cast<int64_t>(m_cfg.min_split_bytes))),
            65536ULL, 10485760ULL);
        m_settings.browser_capture_enabled = val["browserCaptureEnabled"].as_bool(true);
        m_settings.min_capture_size_bytes = std::clamp<uint64_t>(
            static_cast<uint64_t>(val["minCaptureSizeBytes"].as_int64(1048576)),
            0ULL, 10737418240ULL);
        m_settings.close_to_tray = val["closeToTray"].as_bool(true);
        m_settings.start_minimized = val["startMinimized"].as_bool(false);
        m_settings.reduced_motion = val["reducedMotion"].as_bool(false);
        m_settings.show_completion_notification = val["showCompletionNotification"].as_bool(true);
        m_settings.av_strict_mode = val["avStrictMode"].as_bool(false);

        m_settings.proxy_mode = val["proxyMode"].as_string("direct");
        m_settings.proxy_server = val["proxyServer"].as_string("");
        m_settings.proxy_bypass = val["proxyBypass"].as_string("");
        m_settings.proxy_username = val["proxyUsername"].as_string("");
        m_settings.proxy_password_encrypted = val["proxyPasswordEncrypted"].as_string("");
        m_settings.server_auth_username = val["serverAuthUsername"].as_string("");
        m_settings.server_auth_password_encrypted = val["serverAuthPasswordEncrypted"].as_string("");

        if (val.contains("categoryFolders") && val["categoryFolders"].is_object()) {
            m_settings.category_folders.clear();
            for (const auto& [k, v] : val["categoryFolders"].as_object()) {
                m_settings.category_folders[k] = v.as_string();
            }
        }
        if (val.contains("customExtensionCategories") && val["customExtensionCategories"].is_object()) {
            m_settings.custom_extension_categories.clear();
            for (const auto& [k, v] : val["customExtensionCategories"].as_object()) {
                m_settings.custom_extension_categories[k] = v.as_string();
            }
        }
        if (val.contains("siteRules") && val["siteRules"].is_object()) {
            m_settings.site_rules.clear();
            for (const auto& [k, v] : val["siteRules"].as_object()) {
                m_settings.site_rules[k] = v.as_string();
            }
        }

        if (val.contains("tableColumnWidths")) {
            m_settings.table_column_widths = val["tableColumnWidths"].as_string();
        }
        m_settings.max_auto_retries = std::clamp<uint32_t>(
            static_cast<uint32_t>(val["maxAutoRetries"].as_int64(5)), 0u, 50u);
        m_settings.retry_interval_sec = std::clamp<uint32_t>(
            static_cast<uint32_t>(val["retryIntervalSec"].as_int64(15)), 1u, 3600u);
        m_settings.queue_completion_action = val["queueCompletionAction"].as_string("NONE");

        m_cfg.stall_timeout_ms = m_settings.stall_timeout_ms;
        m_cfg.min_split_bytes = m_settings.min_split_bytes;
        m_max_concurrent.store(m_settings.max_concurrent_tasks);
        m_global_speed_limit_bps.store(m_settings.speed_limit_bps);
    } catch (...) {}
}

Result<void> DownloadController::SaveSettingsLocked() {
    json::Value obj = json::Object{};
    obj["theme"] = m_settings.theme;
    obj["maxConcurrentTasks"] = static_cast<int64_t>(m_settings.max_concurrent_tasks);
    obj["speedLimitBps"] = static_cast<int64_t>(m_settings.speed_limit_bps);
    obj["defaultConnectionsPerTask"] = static_cast<int64_t>(m_settings.default_connections_per_task);
    obj["defaultDownloadDir"] = m_settings.default_download_dir;
    obj["categorySubfoldersEnabled"] = m_settings.category_subfolders_enabled;
    obj["conflictPolicy"] = ConflictPolicyToString(m_settings.conflict_policy);
    obj["autoStartDownloads"] = m_settings.auto_start_downloads;
    obj["stallTimeoutMs"] = static_cast<int64_t>(m_settings.stall_timeout_ms);
    obj["minSplitBytes"] = static_cast<int64_t>(m_settings.min_split_bytes);
    obj["browserCaptureEnabled"] = m_settings.browser_capture_enabled;
    obj["minCaptureSizeBytes"] = static_cast<int64_t>(m_settings.min_capture_size_bytes);
    obj["closeToTray"] = m_settings.close_to_tray;
    obj["startMinimized"] = m_settings.start_minimized;
    obj["reducedMotion"] = m_settings.reduced_motion;
    obj["showCompletionNotification"] = m_settings.show_completion_notification;
    obj["avStrictMode"] = m_settings.av_strict_mode;

    obj["proxyMode"] = m_settings.proxy_mode;
    obj["proxyServer"] = m_settings.proxy_server;
    obj["proxyBypass"] = m_settings.proxy_bypass;
    obj["proxyUsername"] = m_settings.proxy_username;
    obj["proxyPasswordEncrypted"] = m_settings.proxy_password_encrypted;
    obj["serverAuthUsername"] = m_settings.server_auth_username;
    obj["serverAuthPasswordEncrypted"] = m_settings.server_auth_password_encrypted;

    json::Value cat_folders_obj = json::Object{};
    for (const auto& [k, v] : m_settings.category_folders) cat_folders_obj[k] = v;
    obj["categoryFolders"] = cat_folders_obj;

    json::Value ext_cat_obj = json::Object{};
    for (const auto& [k, v] : m_settings.custom_extension_categories) ext_cat_obj[k] = v;
    obj["customExtensionCategories"] = ext_cat_obj;

    json::Value site_rules_obj = json::Object{};
    for (const auto& [k, v] : m_settings.site_rules) site_rules_obj[k] = v;
    obj["siteRules"] = site_rules_obj;

    obj["tableColumnWidths"] = m_settings.table_column_widths;
    obj["tableColumnWidths"] = m_settings.table_column_widths;
    obj["maxAutoRetries"] = static_cast<int64_t>(m_settings.max_auto_retries);
    obj["retryIntervalSec"] = static_cast<int64_t>(m_settings.retry_interval_sec);
    obj["queueCompletionAction"] = m_settings.queue_completion_action;

    return m_store.SetSetting("app_settings", obj.dump());
}

void DownloadController::Shutdown() {
    std::vector<std::shared_ptr<ActiveTaskControl>> to_join;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) return;
        m_stopping = true;
        for (auto& [id, ctrl] : m_active) {
            ctrl->pause_requested.store(true);
            to_join.push_back(ctrl);
        }
    }
    m_cv.notify_all();
    if (m_scheduler_thread.joinable()) {
        m_scheduler_thread.join();
    }
    for (auto& ctrl : to_join) {
        if (ctrl->worker_thread.joinable()) {
            ctrl->worker_thread.join();
        }
    }
    m_store.Close();
}

std::string DownloadController::DetermineCategoryImpl(const AppSettings& settings, const std::string& filename, const std::string& url) {
    // 1. Site rules: Domain -> Category check
    if (!url.empty()) {
        std::string host = ExtractHostFromUrl(url);
        if (!host.empty()) {
            auto it = settings.site_rules.find(host);
            if (it != settings.site_rules.end()) {
                const std::string& val = it->second;
                if (val == "Video" || val == "Audio" || val == "Compressed" ||
                    val == "Documents" || val == "Programs" || val == "General") {
                    return val;
                }
            }
        }
    }

    // 2. Custom extension mapping
    std::string ext = ExtractExtensionLower(filename);
    if (!ext.empty()) {
        auto it = settings.custom_extension_categories.find(ext);
        if (it != settings.custom_extension_categories.end() && !it->second.empty()) {
            return it->second;
        }
    }

    // 3. Fallback to standard extension inference
    return InferCategoryFromFilename(filename);
}

std::string DownloadController::DetermineCategory(const std::string& filename, const std::string& url) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return DetermineCategoryImpl(m_settings, filename, url);
}

std::filesystem::path DownloadController::ResolveCategorySaveDirImpl(
    const AppSettings& settings, const std::filesystem::path& default_dir, const std::string& category, const std::string& url) {
    // Priority: Site rule (directory) > Category folder > Subfolder > Default dir

    // 1. Site rules (domain -> custom directory)
    if (!url.empty()) {
        std::string host = ExtractHostFromUrl(url);
        if (!host.empty()) {
            auto it = settings.site_rules.find(host);
            if (it != settings.site_rules.end()) {
                const std::string& val = it->second;
                if (val.find('/') != std::string::npos || val.find('\\') != std::string::npos) {
                    return std::filesystem::path(val);
                }
            }
        }
    }

    // 2. Configurable category folders
    if (!category.empty()) {
        auto it = settings.category_folders.find(category);
        if (it != settings.category_folders.end() && !it->second.empty()) {
            return std::filesystem::path(it->second);
        }
    }

    // 3. Subfolder
    if (settings.category_subfolders_enabled && !category.empty() && category != "General") {
        return default_dir / category;
    }

    // 4. Default download directory
    return default_dir;
}

std::filesystem::path DownloadController::ResolveCategorySaveDir(const std::string& category, const std::string& url) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return ResolveCategorySaveDirImpl(m_settings, m_cfg.default_download_dir, category, url);
}

Result<TaskModel> DownloadController::AddTask(TaskModel task, bool start_immediately, bool force_duplicate) {
    if (task.url.empty()) {
        return Result<TaskModel>::Err("ERR_INVALID_URL", "Task URL cannot be empty");
    }
    if (task.url.find("ftp://") == 0 || task.url.find("ftps://") == 0) {
        task.task_type = TaskType::kFtp;
        auto ftp_res = ftp::FtpUrl::Parse(task.url);
        if (!ftp_res.ok()) return Result<TaskModel>::Err(ftp_res.error->code, ftp_res.error->message);
        if (task.file_name.empty()) task.file_name = ftp_res.value->file_name;
    } else if (task.url.find("magnet:?") == 0) {
        task.task_type = TaskType::kTorrent;
        auto mag_res = torrent::TorrentParser::ParseMagnetUri(task.url);
        if (!mag_res.ok()) return Result<TaskModel>::Err(mag_res.error->code, mag_res.error->message);
        if (task.file_name.empty()) task.file_name = mag_res.value->name;
    } else if (task.task_type == TaskType::kTorrent || task.url.ends_with(".torrent") ||
               (std::filesystem::exists(task.url) && std::filesystem::path(task.url).extension() == ".torrent")) {
        task.task_type = TaskType::kTorrent;
        std::error_code ec;
        if (std::filesystem::exists(task.url, ec)) {
            auto tor_res = torrent::TorrentParser::ParseTorrentFile(task.url);
            if (!tor_res.ok()) return Result<TaskModel>::Err(tor_res.error->code, tor_res.error->message);
            if (task.file_name.empty()) task.file_name = tor_res.value->name;
            task.total_bytes = task.torrent_file_paths.empty() ? tor_res.value->total_length : 0;
            if (task.torrent_file_paths.empty()) {
                for (const auto& file : tor_res.value->files) {
                    task.torrent_file_paths.push_back(file.relative_path.generic_string());
                    task.torrent_file_selection.push_back(file.selected ? 1 : 0);
                }
                task.total_bytes = tor_res.value->selected_bytes();
            } else if (task.torrent_file_paths.size() == tor_res.value->files.size()) {
                uint64_t selected_bytes = 0;
                for (size_t i = 0; i < tor_res.value->files.size() && i < task.torrent_file_selection.size(); ++i) {
                    selected_bytes += task.torrent_file_selection[i] ? tor_res.value->files[i].length : 0;
                }
                task.total_bytes = selected_bytes;
            }
        } else {
            auto parsed_u = brisk::HttpClient::ParseUrl(task.url);
            if (!parsed_u.ok()) return Result<TaskModel>::Err(parsed_u.error->code, parsed_u.error->message);
        }
    } else if (stream::RtspMmsAdapter::IsStreamUrl(task.url)) {
        task.task_type = TaskType::kRtsp;
        if (task.file_name.empty()) task.file_name = "stream_" + GenerateUuidV4().substr(0, 8) + ".mp4";
    } else {
        if (task.task_type == TaskType::kHttpFile && PathSanitizer::IsWebpageWatchUrl(task.url)) {
            return Result<TaskModel>::Err(
                "ERR_URL_IS_WEBPAGE",
                "The specified URL is an HTML webpage, not a direct media file or stream. "
                "To download video from this site, play it in your web browser with the Radium extension enabled."
            );
        }
        auto parsed_u = brisk::HttpClient::ParseUrl(task.url);
        if (!parsed_u.ok()) {
            return Result<TaskModel>::Err(parsed_u.error->code, parsed_u.error->message);
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!force_duplicate) {
        auto dup = core::DuplicateDetector::FindDuplicate(m_tasks, task.url, task.file_name, task.total_bytes, task.etag);
        if (dup.kind != core::DuplicateKind::kNone) {
            const auto& existing = dup.matched_task;
            if (existing.state == TaskState::kQueued ||
                existing.state == TaskState::kProbing ||
                existing.state == TaskState::kDownloading ||
                existing.state == TaskState::kMuxing) {
                return Result<TaskModel>::Ok(existing);
            }
        }
        if (!task.correlation_id.empty()) {
            for (const auto& [id, existing] : m_tasks) {
                if (existing.correlation_id == task.correlation_id &&
                    (existing.state == TaskState::kQueued ||
                     existing.state == TaskState::kProbing ||
                     existing.state == TaskState::kDownloading ||
                     existing.state == TaskState::kMuxing)) {
                    return Result<TaskModel>::Ok(existing);
                }
            }
        }
    }

    if (task.task_id.empty()) {
        task.task_id = GenerateUuidV4();
    }
    m_deleted_tasks.erase(task.task_id);

    if (task.file_name.empty()) {
        std::string fallback_ext = (task.task_type == TaskType::kHttpFile) ? "" : task.stream_options.output_format;
        task.file_name = PathSanitizer::ExtractFromUrl(task.url, fallback_ext);
    } else {
        task.file_name = PathSanitizer::SanitizeFileName(task.file_name);
    }
    if (task.category.empty() || task.category == "General") {
        task.category = DetermineCategoryImpl(m_settings, task.file_name, task.url);
    }
    if (task.save_dir.empty()) {
        task.save_dir = ResolveCategorySaveDirImpl(m_settings, m_cfg.default_download_dir, task.category, task.url).string();
    }
    if (task.max_connections == 0) {
        task.max_connections = m_settings.default_connections_per_task;
    }
    task.conflict_policy = m_settings.conflict_policy;
    if (task.created_at_ms <= 0) {
        task.created_at_ms = NowEpochMs();
    }
    task.updated_at_ms = task.created_at_ms;
    if (task.state != TaskState::kCompleted && task.state != TaskState::kFailed && task.state != TaskState::kDownloading) {
        task.state = (start_immediately && !task.scheduled) ? TaskState::kQueued : TaskState::kPaused;
        if (task.state == TaskState::kPaused) {
            task.pause_reason = task.scheduled ? PauseReason::kNone : PauseReason::kUser;
        }
        task.status_detail = (task.state == TaskState::kQueued)
                                 ? "Queued"
                                 : (task.scheduled ? ("Scheduled (" + task.schedule_time_local + ")") : "Paused");
    }

    auto db_res = m_store.UpsertTask(task);
    if (!db_res.ok()) {
        return Result<TaskModel>::Err(db_res.error->code, db_res.error->message);
    }

    m_tasks[task.task_id] = task;
    m_cv.notify_all();
    return Result<TaskModel>::Ok(task);
}

core::DuplicateMatch DownloadController::CheckDuplicateTask(
    const std::string& url,
    const std::string& file_name,
    uint64_t file_size,
    const std::string& etag,
    const std::string& exclude_task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return core::DuplicateDetector::FindDuplicate(m_tasks, url, file_name, file_size, etag, exclude_task_id);
}

Result<TaskModel> DownloadController::IngestCompletedTask(TaskModel task, const std::filesystem::path& staged_file_path) {
    std::error_code ec;
    if (!std::filesystem::exists(staged_file_path, ec)) {
        return Result<TaskModel>::Err("ERR_STAGED_FILE_NOT_FOUND", "Staged file not found: " + staged_file_path.string());
    }
    uint64_t file_size = std::filesystem::file_size(staged_file_path, ec);

    AppSettings settings_snapshot;
    std::filesystem::path default_dir;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        settings_snapshot = m_settings;
        default_dir = m_cfg.default_download_dir;
    }

    if (task.file_name.empty()) {
        task.file_name = "browser_file_" + GenerateUuidV4().substr(0, 8);
    } else {
        task.file_name = PathSanitizer::SanitizeFileName(task.file_name);
    }
    if (task.task_id.empty()) {
        task.task_id = GenerateUuidV4();
    }
    task.task_type = TaskType::kHttpFile;
    if (task.category.empty() || task.category == "General") {
        task.category = DetermineCategoryImpl(settings_snapshot, task.file_name, task.url);
    }
    if (task.save_dir.empty()) {
        task.save_dir = ResolveCategorySaveDirImpl(settings_snapshot, default_dir, task.category, task.url).string();
    }
    std::filesystem::create_directories(task.save_dir, ec);

    std::filesystem::path dest_path = std::filesystem::path(task.save_dir) / task.file_name;

    // Conflict resolution
    if (std::filesystem::exists(dest_path, ec)) {
        if (settings_snapshot.conflict_policy == ConflictPolicy::kAutoRename) {
            std::string stem = dest_path.stem().string();
            std::string ext = dest_path.extension().string();
            int counter = 1;
            while (std::filesystem::exists(dest_path, ec) && counter < 1000) {
                dest_path = std::filesystem::path(task.save_dir) / (stem + " (" + std::to_string(counter) + ")" + ext);
                counter++;
            }
            task.file_name = dest_path.filename().string();
        } else if (settings_snapshot.conflict_policy == ConflictPolicy::kFailIfExists) {
            return Result<TaskModel>::Err("ERR_FILE_EXISTS", "Target file already exists: " + dest_path.string());
        }
    }

    std::filesystem::rename(staged_file_path, dest_path, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(staged_file_path, dest_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::filesystem::remove(staged_file_path, ec);
        } else {
            return Result<TaskModel>::Err("ERR_FILE_MOVE_FAILED", "Failed to move staged file: " + ec.message());
        }
    }

    if (task.created_at_ms <= 0) {
        task.created_at_ms = NowEpochMs();
    }
    task.updated_at_ms = NowEpochMs();
    task.completed_at_ms = task.updated_at_ms;
    task.state = TaskState::kCompleted;
    task.total_bytes = file_size;
    task.downloaded_bytes = file_size;
    task.speed_bps = 0;
    task.stream_percent = 100.0;
    task.completed_segments = 1;
    task.total_segments = 1;
    task.range_supported = false;
    task.status_detail = "Completed";

    // Antivirus scan
    bool av_available = m_cfg.av_available_override ? m_cfg.av_available_override() : security::AntivirusScanner::IsWindowsDefenderAvailable();
    if (av_available) {
        if (std::filesystem::exists(dest_path, ec)) {
            auto scan_res = m_cfg.av_scan_override ? m_cfg.av_scan_override(dest_path) : security::AntivirusScanner::ScanWithDefender(dest_path);
            if (scan_res.ok()) {
                if (scan_res.value->status == security::ScanStatus::kThreatDetected) {
                    task.state = TaskState::kFailed;
                    std::filesystem::remove(dest_path, ec);
                    task.status_detail = "Threat detected by Antivirus (" + scan_res.value->scanner_name + ")";
                    task.error_code = "ERR_THREAT_DETECTED";
                    task.error_message = "A security threat was detected in the downloaded file.";
                    OPENIDM_LOG_WARN("Controller", "Antivirus threat detected in ingested file: " + dest_path.string());
                } else if (scan_res.value->status == security::ScanStatus::kClean) {
                    task.status_detail = "Completed (Antivirus Clean)";
                } else if (scan_res.value->status == security::ScanStatus::kScannerError) {
                    if (m_settings.av_strict_mode) {
                        task.state = TaskState::kFailed;
                        task.error_code = "ERR_AV_SCAN_FAILED";
                        task.error_message = "Antivirus scanner failed in strict mode";
                        task.status_detail = "Failed: Antivirus scanner error (Strict mode)";
                    } else {
                        task.status_detail = "Completed (Antivirus Scan Failed)";
                    }
                } else {
                    task.status_detail = "Completed (Antivirus Skipped)";
                }
            } else {
                if (m_settings.av_strict_mode) {
                    task.state = TaskState::kFailed;
                    task.error_code = "ERR_AV_SCAN_FAILED";
                    task.error_message = "Antivirus scanner error: " + scan_res.error->message;
                    task.status_detail = "Failed: Antivirus scan error (Strict mode)";
                } else {
                    task.status_detail = "Completed (Antivirus Scan Error: " + scan_res.error->message + ")";
                }
            }
        }
    } else {
        if (m_settings.av_strict_mode) {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_AV_UNAVAILABLE";
            task.error_message = "Antivirus scanner is unavailable in strict mode";
            task.status_detail = "Failed: Antivirus scanner unavailable (Strict mode)";
        } else {
            task.status_detail = "Completed (Not Scanned - Scanner Unavailable)";
        }
    }

#ifdef _WIN32
    if (m_settings.show_completion_notification) {
        MessageBeep(MB_ICONASTERISK);
    }
#endif

    std::lock_guard<std::mutex> lock(m_mutex);
    m_deleted_tasks.erase(task.task_id);
    auto db_res = m_store.UpsertTask(task);
    if (!db_res.ok()) {
        return Result<TaskModel>::Err(db_res.error->code, db_res.error->message);
    }
    m_tasks[task.task_id] = task;
    m_cv.notify_all();
    return Result<TaskModel>::Ok(task);
}

Result<std::vector<TaskModel>> DownloadController::AddBatchTasks(
    const std::vector<std::string>& urls,
    const std::string& page_url,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& auth_headers) {

    std::vector<TaskModel> added;
    for (const auto& u : urls) {
        TaskModel t;
        t.url = u;
        t.page_url = page_url;
        t.headers = headers;
        t.auth_headers = auth_headers;
        if (u.find(".m3u8") != std::string::npos) {
            t.task_type = TaskType::kStreamHls;
        } else if (u.find(".mpd") != std::string::npos) {
            t.task_type = TaskType::kStreamDash;
        } else {
            t.task_type = TaskType::kHttpFile;
        }
        auto res = AddTask(std::move(t), true, false);
        if (res.ok()) {
            added.push_back(*res.value);
        }
    }
    return Result<std::vector<TaskModel>>::Ok(std::move(added));
}

Result<stream::StreamProbeResult> DownloadController::ProbeStream(
    const std::string& manifest_url,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& auth_headers,
    const std::string& cached_manifest_body) {

    stream::StreamAdapterConfig scfg{
        m_cfg.nm3u8dl_binary_path,
        m_cfg.ffmpeg_binary_path,
        m_cfg.temp_root_dir,
        m_cfg.allow_loopback
    };
    stream::Nm3u8dlReAdapter adapter(std::move(scfg));
    return adapter.ProbeManifest(manifest_url, headers, auth_headers, cached_manifest_body);
}

void DownloadController::ScheduleLoop() {
    std::map<std::string, std::chrono::steady_clock::time_point> retry_schedule;
    bool had_active = false;

    while (true) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(50), [&]() { return m_stopping; });
        if (m_stopping) break;

        std::vector<std::thread> finished_workers;
        for (auto it = m_active.begin(); it != m_active.end();) {
            if (it->second->finished.load()) {
                if (it->second->worker_thread.joinable()) {
                    finished_workers.emplace_back(std::move(it->second->worker_thread));
                }
                it = m_active.erase(it);
            } else {
                ++it;
            }
        }

        // Workers merge their final state under m_mutex. Joining while holding
        // this lock deadlocks the scheduler when a worker is finishing a pause.
        lock.unlock();
        for (auto& worker : finished_workers) {
            if (worker.joinable()) worker.join();
        }
        lock.lock();

        if (!m_active.empty()) {
            had_active = true;
        }

        // 1. Check for scheduled tasks that reached their local schedule time
        std::string cur_hh_mm;
        if (m_cfg.local_time_override) {
            cur_hh_mm = m_cfg.local_time_override();
        } else {
            auto now_time_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm local_tm{};
#ifdef _WIN32
            localtime_s(&local_tm, &now_time_t);
#else
            localtime_r(&now_time_t, &local_tm);
#endif
            char time_str[16] = {0};
            std::strftime(time_str, sizeof(time_str), "%H:%M", &local_tm);
            cur_hh_mm = time_str;
        }

        for (auto& [id, t] : m_tasks) {
            // Check scheduled start & stop time strictly on scheduled tasks
            if (t.scheduled) {
                bool in_window = IsTimeInScheduledWindow(cur_hh_mm, t.schedule_time_local, t.schedule_stop_time_local);
                if (t.state == TaskState::kPaused && t.pause_reason != PauseReason::kUser && in_window) {
                    t.state = TaskState::kQueued;
                    t.status_detail = "Queued (Schedule Started)";
                    if (t.schedule_stop_time_local.empty()) {
                        // One-shot start schedule has fired
                        t.scheduled = false;
                        t.schedule_time_local.clear();
                    }
                    m_store.UpsertTask(t);
                } else if (!in_window && !t.schedule_stop_time_local.empty()) {
                    if (t.state == TaskState::kDownloading || t.state == TaskState::kProbing || t.state == TaskState::kQueued) {
                        auto ait = m_active.find(id);
                        if (ait != m_active.end()) {
                            ait->second->pause_requested.store(true);
                        }
                        t.state = TaskState::kPaused;
                        t.pause_reason = PauseReason::kQueueSchedule;
                        t.status_detail = "Paused (Scheduled Stop " + t.schedule_stop_time_local + ")";
                        // Cleanly expire the schedule once stop time has arrived so it never blocks manual Resume or Retry!
                        t.scheduled = false;
                        t.schedule_time_local.clear();
                        t.schedule_stop_time_local.clear();
                        m_store.UpsertTask(t);
                    }
                }
            }
        }

        // 2. Check multi-queue schedules
        auto queues_res = m_store.ListQueues();
        if (queues_res.ok()) {
            for (const auto& q : *queues_res.value) {
                if (q.schedule_enabled) {
                    bool in_queue_window = IsTimeInScheduledWindow(cur_hh_mm, q.start_time_local, q.stop_time_local);
                    if (in_queue_window) {
                        for (auto& [id, t] : m_tasks) {
                            if (t.queue_id == q.queue_id && t.state == TaskState::kPaused &&
                                t.pause_reason != PauseReason::kUser && !t.scheduled) {
                                t.state = TaskState::kQueued;
                                t.status_detail = "Queued (Queue Schedule Started)";
                                m_store.UpsertTask(t);
                            }
                        }
                    } else if (!q.stop_time_local.empty() && cur_hh_mm == q.stop_time_local) {
                        for (auto& [id, t] : m_tasks) {
                            if (t.queue_id == q.queue_id && !t.scheduled &&
                                (t.state == TaskState::kDownloading || t.state == TaskState::kProbing || t.state == TaskState::kQueued)) {
                                auto ait = m_active.find(id);
                                if (ait != m_active.end()) {
                                    ait->second->pause_requested.store(true);
                                }
                                t.state = TaskState::kPaused;
                                t.status_detail = "Paused (Queue Scheduled Stop " + q.stop_time_local + ")";
                                m_store.UpsertTask(t);
                            }
                        }
                    }
                }
            }
        }

        // 3. Exponential backoff auto-retry for transient network errors
        auto now_steady = std::chrono::steady_clock::now();
        for (auto& [id, t] : m_tasks) {
            if (t.state == TaskState::kFailed && t.retry_count < m_settings.max_auto_retries) {
                bool is_transient = (t.error_code == "ERR_NETWORK_TIMEOUT" ||
                                     t.error_code == "ERR_NET_TIMEOUT" ||
                                     t.error_code == "ERR_TIMEOUT" ||
                                     t.error_code == "ERR_CONNECTION_RESET" ||
                                     t.error_code == "ERR_NET_CONNECTION_RESET" ||
                                     t.error_code == "ERR_NET_CONNECT" ||
                                     t.error_code == "ERR_NET_SEND" ||
                                     t.error_code == "ERR_NET_DNS" ||
                                     t.error_code == "ERR_HTTP_TIMEOUT" ||
                                     t.error_code == "ERR_HTTP_5XX_TRANSIENT" ||
                                     t.error_code == "ERR_HTTP_429_RATE_LIMITED" ||
                                     t.error_code == "ERR_HTTP_503" ||
                                     t.error_code == "ERR_HTTP_502" ||
                                     t.error_code == "ERR_HTTP_504" ||
                                     t.error_code == "ERR_HTTP_429" ||
                                     t.error_code == "ERR_FTP_TIMEOUT" ||
                                     t.error_code == "ERR_FTP_DISCONNECTED" ||
                                     t.error_code == "ERR_FTP_CONNECT" ||
                                     t.error_code == "ERR_STREAM_WORKER_CRASH");
                if (is_transient) {
                    auto rit = retry_schedule.find(id);
                    if (rit == retry_schedule.end()) {
                        uint32_t delay_sec = m_settings.retry_interval_sec * (1u << std::min<uint32_t>(t.retry_count, 6u));
                        retry_schedule[id] = now_steady + std::chrono::seconds(delay_sec);
                        t.status_detail = "Retrying in " + std::to_string(delay_sec) + "s (Attempt " +
                                          std::to_string(t.retry_count + 1) + "/" + std::to_string(m_settings.max_auto_retries) + ")";
                        m_store.UpsertTask(t);
                    } else if (now_steady >= rit->second && t.pause_reason != PauseReason::kUser) {
                        retry_schedule.erase(rit);
                        ++t.retry_count;
                        t.state = TaskState::kQueued;
                        t.status_detail = "Queued (Auto-Retry #" + std::to_string(t.retry_count) + ")";
                        t.error_code.clear();
                        t.error_message.clear();
                        m_store.UpsertTask(t);
                    }
                }
            } else if (t.state != TaskState::kFailed) {
                retry_schedule.erase(id);
            }
        }

        // 4. Queue / Global Post-Completion Action
        if (m_active.empty() && had_active) {
            bool has_pending = false;
            for (const auto& [id, t] : m_tasks) {
                if (t.state == TaskState::kQueued || t.state == TaskState::kProbing ||
                    t.state == TaskState::kDownloading || t.state == TaskState::kMuxing ||
                    t.state == TaskState::kVerifying) {
                    has_pending = true;
                    break;
                }
            }
            if (!has_pending) {
                had_active = false;
                std::string action = m_settings.queue_completion_action;
                if (action == "SLEEP" || action == "SHUTDOWN") {
                    m_settings.queue_completion_action = "NONE";
                    SaveSettingsLocked();
                    OPENIDM_LOG_INFO("Controller", "All downloads completed. Executing post-completion action: " + action);
#ifdef _WIN32
                    if (action == "SLEEP") {
                        TriggerSystemSleep();
                    } else if (action == "SHUTDOWN") {
                        TriggerSystemShutdown();
                    }
#endif
                }
            }
        }

        uint32_t max_conc = m_max_concurrent.load();
        while (m_active.size() < max_conc) {
            std::string next_id;
            int32_t min_order = INT32_MAX;
            int64_t oldest_ts = INT64_MAX;
            for (const auto& [id, t] : m_tasks) {
                if (t.state == TaskState::kQueued && m_active.find(id) == m_active.end()) {
                    if (t.queue_order < min_order || (t.queue_order == min_order && t.created_at_ms < oldest_ts)) {
                        min_order = t.queue_order;
                        oldest_ts = t.created_at_ms;
                        next_id = id;
                    }
                }
            }
            if (next_id.empty()) break;

            m_tasks[next_id].state = TaskState::kProbing;
            m_tasks[next_id].status_detail = "Starting transfer...";
            auto ctrl = std::make_shared<ActiveTaskControl>();
            ctrl->task_speed_limit_bps.store(m_tasks[next_id].speed_limit_bps);
            m_active[next_id] = ctrl;
            ctrl->worker_thread = std::thread(&DownloadController::RunTaskWorker, this, next_id, ctrl);
        }
    }
}

void DownloadController::RunTaskWorker(std::string task_id, std::shared_ptr<ActiveTaskControl> ctrl) {
    TaskModel local_task;
    int stall_timeout_ms = 3000;
    uint64_t min_split_bytes = 131072;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end() || m_deleted_tasks.count(task_id)) {
            ctrl->finished.store(true);
            return;
        }
        local_task = it->second;
        stall_timeout_ms = m_cfg.stall_timeout_ms;
        min_split_bytes = m_cfg.min_split_bytes;
    }

    std::string task_type_str = TaskTypeToString(local_task.task_type);
    bool is_youtube_task = (local_task.url.find("youtube.com") != std::string::npos ||
                            local_task.url.find("youtu.be") != std::string::npos ||
                            local_task.page_url.find("youtube.com") != std::string::npos ||
                            local_task.page_url.find("youtu.be") != std::string::npos ||
                            local_task.stream_options.cached_manifest_body.find("\"fluxType\":\"YOUTUBE_STREAM\"") != std::string::npos ||
                            local_task.stream_options.cached_manifest_body.find("\"fluxType\": \"YOUTUBE_STREAM\"") != std::string::npos);
    if (is_youtube_task) {
        task_type_str = "YOUTUBE_EXTRACTION";
    }

    OPENIDM_LOG_INFO("Controller", "Dispatching task=" + task_id + " type=" + task_type_str +
                     " correlation_id=" + local_task.correlation_id + " url=" + local_task.url);

    auto last_db_sync = std::chrono::steady_clock::now();

    if (local_task.task_type == TaskType::kFtp) {
        auto ftp_url_res = ftp::FtpUrl::Parse(local_task.url);
        if (ftp_url_res.ok()) {
            std::filesystem::path part_p = std::filesystem::path(local_task.save_dir) / (local_task.file_name + "." + local_task.task_id + ".part");
            local_task.part_path = part_p.string();
            local_task.state = TaskState::kDownloading;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_tasks.find(task_id);
                if (it != m_tasks.end()) {
                    it->second.part_path = part_p.string();
                }
            }

            auto dl_res = ftp::FtpClient::DownloadFile(
                ftp_url_res.value.value(),
                part_p,
                local_task.downloaded_bytes,
                ctrl->pause_requested,
                ctrl->cancel_requested,
                ctrl->task_speed_limit_bps,
                [&](const ftp::FtpProgress& fp) {
                    if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) return;
                    local_task.downloaded_bytes = fp.downloaded_bytes;
                    local_task.total_bytes = fp.total_bytes;
                    local_task.speed_bps = fp.speed_bps;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        auto it = m_tasks.find(task_id);
                        if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                            if (it->second.state == TaskState::kCanceled || it->second.state == TaskState::kPaused) return;
                            it->second.state = fp.state;
                            it->second.downloaded_bytes = fp.downloaded_bytes;
                            it->second.total_bytes = fp.total_bytes;
                            it->second.speed_bps = fp.speed_bps;
                            it->second.status_detail = fp.status_detail;
                        }
                    }
                },
                m_global_limiter,
                &m_global_speed_limit_bps,
                10000,
                m_cfg.allow_loopback);

            if (dl_res.ok()) {
                uint64_t final_size = dl_res.value.value();
                bool checksum_ok = true;
                if (local_task.total_bytes > 0 && final_size != local_task.total_bytes) {
                    checksum_ok = false;
                    local_task.state = TaskState::kFailed;
                    local_task.error_code = "ERR_FTP_SIZE_MISMATCH";
                    local_task.error_message = "Downloaded FTP size (" + std::to_string(final_size) +
                        ") did not match expected size (" + std::to_string(local_task.total_bytes) + ")";
                    local_task.status_detail = "Failed: Size mismatch";
                } else if (!local_task.expected_sha256.empty()) {
                    auto calc_sha = CryptoUtils::ComputeFileSha256Hex(part_p);
                    auto to_low = [](std::string s) {
                        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        return s;
                    };
                    if (!calc_sha.ok() || to_low(calc_sha.value.value()) != to_low(local_task.expected_sha256)) {
                        checksum_ok = false;
                        local_task.state = TaskState::kFailed;
                        local_task.error_code = "ERR_CHECKSUM_MISMATCH";
                        local_task.error_message = "Downloaded FTP file SHA-256 did not match expected checksum";
                        local_task.status_detail = "Failed: SHA-256 mismatch";
                    }
                }
                if (checksum_ok) {
                    auto commit_res = PathSanitizer::CommitPartFile(
                        part_p,
                        local_task.save_dir,
                        local_task.file_name,
                        local_task.conflict_policy);
                    if (commit_res.ok()) {
                        local_task.file_name = commit_res.value->filename().string();
                        local_task.part_path.clear();
                        local_task.state = TaskState::kCompleted;
                        local_task.downloaded_bytes = final_size;
                        local_task.total_bytes = final_size;
                        local_task.completed_at_ms = NowEpochMs();
                        local_task.status_detail = "Completed (FTP)";
                    } else {
                        local_task.state = TaskState::kFailed;
                        local_task.error_code = commit_res.error->code;
                        local_task.error_message = commit_res.error->message;
                        local_task.status_detail = "Failed to commit: " + commit_res.error->message;
                    }
                }
            } else {
                if (ctrl->cancel_requested.load()) {
                    local_task.state = TaskState::kCanceled;
                    local_task.status_detail = "Canceled";
                } else if (ctrl->pause_requested.load()) {
                    local_task.state = TaskState::kPaused;
                    local_task.status_detail = "Paused";
                } else {
                    local_task.state = TaskState::kFailed;
                    local_task.error_code = dl_res.error->code;
                    local_task.error_message = dl_res.error->message;
                    local_task.status_detail = "Failed: " + dl_res.error->message;
                }
            }
        } else {
            local_task.state = TaskState::kFailed;
            local_task.error_code = ftp_url_res.error->code;
            local_task.error_message = ftp_url_res.error->message;
            local_task.status_detail = "Failed: " + ftp_url_res.error->message;
        }
    } else if (local_task.task_type == TaskType::kTorrent) {
        torrent::TorrentInfo tinfo;
        Result<torrent::TorrentInfo> parse_res = Result<torrent::TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Torrent metainfo was not parsed");
        if (local_task.url.find("magnet:?") == 0) {
            parse_res = torrent::TorrentParser::ParseMagnetUri(local_task.url);
        } else {
            parse_res = torrent::TorrentParser::ParseTorrentFile(local_task.url);
        }
        if (!parse_res.ok()) {
            local_task.state = TaskState::kFailed;
            local_task.error_code = parse_res.error->code;
            local_task.error_message = parse_res.error->message;
            local_task.status_detail = "Failed: " + parse_res.error->message;
            OPENIDM_LOG_ERROR("Controller", "Torrent task " + task_id + " parse failed [" + local_task.error_code + "]: " + local_task.error_message);
        } else {
            tinfo = parse_res.value.value();
            if (!local_task.torrent_file_paths.empty() && tinfo.files.size() == local_task.torrent_file_paths.size()) {
                for (size_t i = 0; i < tinfo.files.size() && i < local_task.torrent_file_selection.size(); ++i) {
                    if (tinfo.files[i].relative_path.generic_string() == local_task.torrent_file_paths[i]) {
                        tinfo.files[i].selected = local_task.torrent_file_selection[i] != 0;
                    }
                }
            }
        }
        torrent::TorrentMetadataSelectionCallback metadata_selection;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            metadata_selection = m_cfg.torrent_metadata_selection_callback;
        }
        auto tor_dl = parse_res.ok() ? torrent::TorrentEngine::ExecuteTorrentDownload(
            local_task,
            tinfo,
            ctrl->pause_requested,
            ctrl->cancel_requested,
            ctrl->task_speed_limit_bps,
            [&](const torrent::TorrentProgress& tp) {
                if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) return;
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_tasks.find(task_id);
                if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                    if (it->second.state == TaskState::kCanceled || it->second.state == TaskState::kPaused) return;
                    if ((it->second.state == TaskState::kCompleted || it->second.state == TaskState::kFailed) &&
                        tp.state != TaskState::kCompleted && tp.state != TaskState::kFailed) {
                        OPENIDM_LOG_WARN("Controller", "Ignoring stale torrent progress task=" + task_id +
                                         " phase=" + TaskStateToString(tp.state) + " after terminal state=" + TaskStateToString(it->second.state));
                        return;
                    }
                    it->second.state = tp.state;
                    // received_bytes includes partial, not-yet-hash-verified
                    // torrent blocks for live UI progress. ExecuteTorrentDownload
                    // keeps local_task.downloaded_bytes verified-only, so these
                    // transient bytes are never committed as resumable progress.
                    it->second.downloaded_bytes = std::max(tp.downloaded_bytes, tp.received_bytes);
                    it->second.total_bytes = tp.total_bytes;
                    it->second.speed_bps = tp.speed_bps;
                    it->second.uploaded_bytes = tp.uploaded_bytes;
                    it->second.upload_speed_bps = tp.upload_speed_bps;
                    it->second.completed_segments = tp.completed_pieces;
                    it->second.total_segments = tp.total_pieces;
                    it->second.status_detail = tp.status_detail;
                    local_task.uploaded_bytes = tp.uploaded_bytes;
                }
            },
            m_global_limiter,
            &m_global_speed_limit_bps,
            32,
            false,
            1.0,
            0,
            std::move(metadata_selection)) : Result<void>::Err(local_task.error_code, local_task.error_message);
        if (!tor_dl.ok() && parse_res.ok()) {
            if (ctrl->pause_requested.load() || tor_dl.error->code == "ERR_TORRENT_SELECTION_CANCELED") {
                local_task.state = TaskState::kPaused;
                local_task.pause_reason = PauseReason::kUser;
                local_task.status_detail = "Paused";
            } else {
                local_task.state = TaskState::kFailed;
                local_task.error_code = tor_dl.error->code;
                local_task.error_message = tor_dl.error->message;
                local_task.status_detail = "Failed: " + tor_dl.error->message;
            }
        }
    } else if (local_task.task_type == TaskType::kRtsp) {
        stream::RtspMmsAdapter rtsp_ad(m_cfg.ffmpeg_binary_path, m_cfg.allow_loopback);
        auto rtsp_res = rtsp_ad.ExecuteStreamCapture(
            local_task,
            ctrl->pause_requested,
            ctrl->cancel_requested,
            [&](const stream::RtspProgress& rp) {
                if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) return;
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_tasks.find(task_id);
                if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                    if (it->second.state == TaskState::kCanceled || it->second.state == TaskState::kPaused) return;
                    it->second.state = rp.state;
                    it->second.downloaded_bytes = rp.downloaded_bytes;
                    it->second.speed_bps = rp.speed_bps;
                    it->second.status_detail = rp.status_detail;
                }
            });
        if (!rtsp_res.ok()) {
            if (ctrl->cancel_requested.load()) {
                local_task.state = TaskState::kCanceled;
                local_task.status_detail = "Canceled";
            } else if (ctrl->pause_requested.load()) {
                local_task.state = TaskState::kPaused;
                local_task.status_detail = "Paused";
            } else {
                local_task.state = TaskState::kFailed;
                local_task.error_code = rtsp_res.error->code;
                local_task.error_message = rtsp_res.error->message;
                local_task.status_detail = "Failed: " + rtsp_res.error->message;
            }
        }
    } else if (local_task.task_type == TaskType::kHttpFile) {
        brisk::EngineConfig ecfg;
        ecfg.stall_timeout_ms = stall_timeout_ms;
        ecfg.min_split_bytes = min_split_bytes;
        ecfg.allow_loopback_redirect = m_cfg.allow_loopback;

        // Snapshot network & proxy settings under lock
        std::string p_mode, p_serv, p_by, p_user, p_pass_enc, s_user, s_pass_enc;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            p_mode = m_settings.proxy_mode;
            p_serv = m_settings.proxy_server;
            p_by = m_settings.proxy_bypass;
            p_user = m_settings.proxy_username;
            p_pass_enc = m_settings.proxy_password_encrypted;
            s_user = m_settings.server_auth_username;
            s_pass_enc = m_settings.server_auth_password_encrypted;
        }

        ecfg.net_opts.proxy_mode = p_mode;
        ecfg.net_opts.proxy_server = p_serv;
        ecfg.net_opts.proxy_bypass = p_by;
        ecfg.net_opts.proxy_username = p_user;
        if (!p_pass_enc.empty()) {
            auto raw = HexStrToBytes(p_pass_enc);
            auto unp = CryptoUtils::UnprotectSecret(raw);
            if (unp.ok()) ecfg.net_opts.proxy_password = *unp.value;
        }
        ecfg.net_opts.server_auth_username = s_user;
        if (!s_pass_enc.empty()) {
            auto raw = HexStrToBytes(s_pass_enc);
            auto unp = CryptoUtils::UnprotectSecret(raw);
            if (unp.ok()) ecfg.net_opts.server_auth_password = *unp.value;
        }
        ecfg.net_opts.enable_integrated_auth = true;

        std::vector<std::string> urls_to_try;
        urls_to_try.push_back(local_task.url);
        for (const auto& alt : local_task.alternate_urls) {
            if (!alt.empty() && alt != local_task.url) {
                urls_to_try.push_back(alt);
            }
        }

        Result<std::filesystem::path> res = Result<std::filesystem::path>::Err("ERR_NOT_ATTEMPTED", "Transfer not attempted");
        size_t start_idx = (local_task.current_url_index < urls_to_try.size()) ? local_task.current_url_index : 0;

        for (size_t uidx = start_idx; uidx < urls_to_try.size(); ++uidx) {
            if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) break;

            const std::string& candidate_url = urls_to_try[uidx];
            if (uidx != start_idx) {
                OPENIDM_LOG_INFO("Controller", "Attempting alternate mirror (" + std::to_string(uidx) + "): " + candidate_url);
                auto probe_res = brisk::HttpClient::ProbeResource(
                    candidate_url, local_task.headers, local_task.auth_headers, ecfg.net_opts, m_cfg.allow_loopback);
                if (probe_res.ok()) {
                    if (local_task.total_bytes > 0 && probe_res.value->has_content_length && probe_res.value->content_length > 0) {
                        if (probe_res.value->content_length != local_task.total_bytes) {
                            OPENIDM_LOG_WARN("Controller", "Mirror Content-Length mismatch (" +
                                             std::to_string(probe_res.value->content_length) + " vs " +
                                             std::to_string(local_task.total_bytes) + "). Skipping mirror: " + candidate_url);
                            continue;
                        }
                    }
                } else {
                    OPENIDM_LOG_WARN("Controller", "Mirror probe failed: " + candidate_url + " (" + probe_res.error->message + ")");
                    continue;
                }
                local_task.url = candidate_url;
                local_task.current_url_index = static_cast<uint32_t>(uidx);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_tasks.find(task_id);
                    if (it != m_tasks.end()) {
                        it->second.url = candidate_url;
                        it->second.current_url_index = static_cast<uint32_t>(uidx);
                        m_store.UpsertTask(it->second);
                    }
                }
            }

            brisk::HttpDownloadEngine engine(ecfg);
            res = engine.ExecuteDownload(
                local_task,
                ctrl->pause_requested,
                ctrl->cancel_requested,
                ctrl->task_speed_limit_bps,
                [&](const brisk::EngineProgress& ep) {
                    if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) return;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        auto it = m_tasks.find(task_id);
                        if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                            auto& t = it->second;
                            if (t.state == TaskState::kCanceled || t.state == TaskState::kPaused) return;
                            t.state = ep.state;
                            t.downloaded_bytes = ep.downloaded_bytes;
                            t.total_bytes = ep.total_bytes;
                            t.speed_bps = ep.speed_bps;
                            t.range_supported = ep.range_supported;
                            t.status_detail = local_task.status_detail;
                            if (!ep.resolved_filename.empty()) t.file_name = ep.resolved_filename;
                            if (!ep.etag.empty()) t.etag = ep.etag;
                            if (!ep.last_modified.empty()) t.last_modified = ep.last_modified;
                            t.segments = ep.segments;
                        }
                    }
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_db_sync).count() >= 500) {
                        last_db_sync = now;
                        if (!ctrl->cancel_requested.load() && !ctrl->pause_requested.load()) {
                            m_store.UpdateTaskProgressAndSegments(
                                task_id, ep.state, ep.downloaded_bytes, ep.total_bytes, ep.range_supported, ep.segments);
                        }
                    }
                },
                m_global_limiter,
                &m_global_speed_limit_bps);

            if (res.ok() || ctrl->cancel_requested.load() || ctrl->pause_requested.load()) {
                break;
            }
        }
        if (!res.ok()) {
            if (ctrl->cancel_requested.load()) {
                local_task.state = TaskState::kCanceled;
                local_task.status_detail = "Canceled";
            } else if (ctrl->pause_requested.load()) {
                local_task.state = TaskState::kPaused;
                local_task.status_detail = "Paused";
            } else {
                local_task.state = TaskState::kFailed;
                local_task.error_code = res.error->code;
                local_task.error_message = res.error->message;
                local_task.status_detail = "Failed: " + res.error->message;
                OPENIDM_LOG_ERROR("Controller", "HTTP task " + task_id + " FAILED [" + local_task.error_code + "]: " + local_task.error_message + " (URL: " + local_task.url + ")");
            }
        }
    } else {
        stream::StreamAdapterConfig scfg{
            m_cfg.nm3u8dl_binary_path,
            m_cfg.ffmpeg_binary_path,
            m_cfg.temp_root_dir,
            m_cfg.allow_loopback
        };
        stream::Nm3u8dlReAdapter adapter(std::move(scfg));
        Result<std::filesystem::path> res;
        bool is_yt = (task_type_str == "YOUTUBE_EXTRACTION" ||
                      local_task.url.find("googlevideo.com") != std::string::npos ||
                      local_task.url.find("youtube.com/watch") != std::string::npos ||
                      local_task.url.find("youtu.be/") != std::string::npos ||
                      local_task.url.find("youtube.com/shorts/") != std::string::npos ||
                      local_task.stream_options.cached_manifest_body.find("\"fluxType\":\"YOUTUBE_STREAM\"") != std::string::npos ||
                      local_task.stream_options.cached_manifest_body.find("\"fluxType\": \"YOUTUBE_STREAM\"") != std::string::npos);
        auto progress_cb = [&](const stream::StreamProgress& sp) {
            if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) return;
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_tasks.find(task_id);
            if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                if (it->second.state == TaskState::kCanceled || it->second.state == TaskState::kPaused) return;
                if ((it->second.state == TaskState::kCompleted || it->second.state == TaskState::kFailed) &&
                    sp.phase != TaskState::kCompleted && sp.phase != TaskState::kFailed) {
                    OPENIDM_LOG_WARN("Controller", "Ignoring stale stream progress task=" + task_id +
                                     " phase=" + TaskStateToString(sp.phase) + " after terminal state=" + TaskStateToString(it->second.state));
                    return;
                }
                auto& t = it->second;
                t.state = sp.phase;
                t.downloaded_bytes = sp.downloaded_bytes;
                t.total_bytes = sp.total_bytes;
                t.speed_bps = sp.speed_bps;
                t.stream_percent = sp.percent;
                t.completed_segments = sp.completed_segments;
                t.total_segments = sp.total_segments;
                t.status_detail = sp.status_line;
            }
        };

        if (is_yt) {
            res = adapter.ExecuteYouTubeExtraction(
                local_task,
                ctrl->pause_requested,
                ctrl->cancel_requested,
                ctrl->task_speed_limit_bps.load(),
                progress_cb);
        } else {
            res = adapter.ExecuteStreamDownload(
                local_task,
                ctrl->pause_requested,
                ctrl->cancel_requested,
                ctrl->task_speed_limit_bps.load(),
                progress_cb);
        }
        if (!res.ok()) {
            if (ctrl->cancel_requested.load()) {
                local_task.state = TaskState::kCanceled;
                local_task.status_detail = "Canceled";
            } else if (ctrl->pause_requested.load()) {
                local_task.state = TaskState::kPaused;
                local_task.status_detail = "Paused";
            } else {
                local_task.state = TaskState::kFailed;
                local_task.error_code = res.error->code;
                local_task.error_message = res.error->message;
                local_task.status_detail = "Failed: " + res.error->message;
                OPENIDM_LOG_ERROR("Controller", "Stream task " + task_id + " FAILED [" + local_task.error_code + "]: " + local_task.error_message + " (URL: " + local_task.url + ")");
            }
        }
    }

    local_task.speed_bps = 0;
    local_task.updated_at_ms = NowEpochMs();

    if (ctrl->cancel_requested.load()) {
        local_task.state = TaskState::kCanceled;
        local_task.status_detail = "Canceled";
    } else if (ctrl->pause_requested.load() && local_task.state != TaskState::kCompleted && local_task.state != TaskState::kFailed) {
        local_task.state = TaskState::kPaused;
        local_task.status_detail = "Paused";
    }

    if (local_task.state == TaskState::kCompleted) {
        // Post-Download Antivirus Scan (outside m_mutex to avoid locking controller for up to 30s)
        bool av_available = m_cfg.av_available_override ? m_cfg.av_available_override() : security::AntivirusScanner::IsWindowsDefenderAvailable();
        if (av_available) {
            std::filesystem::path final_p = std::filesystem::path(local_task.save_dir) / local_task.file_name;
            std::error_code ec;
            if (std::filesystem::exists(final_p, ec)) {
                // Update state to kFinalizing during scan so UI and IPC reflect that post-processing is running
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_tasks.find(task_id);
                    if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                        it->second.state = TaskState::kFinalizing;
                        it->second.status_detail = "Finalizing output (Antivirus scan in progress)...";
                    }
                }
                auto scan_res = m_cfg.av_scan_override ? m_cfg.av_scan_override(final_p) : security::AntivirusScanner::ScanWithDefender(final_p);
                if (scan_res.ok()) {
                    if (scan_res.value->status == security::ScanStatus::kThreatDetected) {
                        local_task.state = TaskState::kFailed;
                        local_task.status_detail = "Threat detected by Antivirus (" + scan_res.value->scanner_name + ")";
                        local_task.error_code = "ERR_THREAT_DETECTED";
                        local_task.error_message = "A security threat was detected in the downloaded file.";
                        OPENIDM_LOG_WARN("Controller", "Antivirus threat detected: " + final_p.string());
                        std::error_code rm_ec;
                        std::filesystem::remove(final_p, rm_ec);
                    } else if (scan_res.value->status == security::ScanStatus::kClean) {
                        local_task.state = TaskState::kCompleted;
                        local_task.status_detail = "Completed (Antivirus Clean)";
                    } else if (scan_res.value->status == security::ScanStatus::kScannerError) {
                        if (m_settings.av_strict_mode) {
                            local_task.state = TaskState::kFailed;
                            local_task.error_code = "ERR_AV_SCAN_FAILED";
                            local_task.error_message = "Antivirus scanner failed in strict mode";
                            local_task.status_detail = "Failed: Antivirus scanner error (Strict mode)";
                        } else {
                            local_task.status_detail = "Completed (Antivirus Scan Failed)";
                        }
                    } else {
                        local_task.state = TaskState::kCompleted;
                        local_task.status_detail = "Completed (Antivirus Skipped)";
                    }
                } else {
                    if (m_settings.av_strict_mode) {
                        local_task.state = TaskState::kFailed;
                        local_task.error_code = "ERR_AV_SCAN_FAILED";
                        local_task.error_message = "Antivirus scanner error: " + scan_res.error->message;
                        local_task.status_detail = "Failed: Antivirus scan error (Strict mode)";
                    } else {
                        local_task.status_detail = "Completed (Antivirus Scan Error: " + scan_res.error->message + ")";
                    }
                }
            }
        } else {
            if (m_settings.av_strict_mode) {
                local_task.state = TaskState::kFailed;
                local_task.error_code = "ERR_AV_UNAVAILABLE";
                local_task.error_message = "Antivirus scanner is unavailable in strict mode";
                local_task.status_detail = "Failed: Antivirus scanner unavailable (Strict mode)";
            } else {
                local_task.status_detail = "Completed (Not Scanned - Scanner Unavailable)";
            }
        }
#ifdef _WIN32
        if (m_settings.show_completion_notification && local_task.state == TaskState::kCompleted) {
            MessageBeep(MB_ICONASTERISK);
        }
#endif
    }

    // BUG-007 Fix: Granular merge under m_mutex without resurrecting deleted tasks
    bool should_persist = false;
    TaskModel to_save;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_deleted_tasks.count(task_id)) {
            auto it = m_tasks.find(task_id);
            if (it != m_tasks.end()) {
                auto& t = it->second;
                // Terminal state protection: Never overwrite a user-canceled task with completed
                if (t.state == TaskState::kCanceled && local_task.state == TaskState::kCompleted) {
                    local_task.state = TaskState::kCanceled;
                    local_task.status_detail = "Canceled";
                }
                if (t.state == TaskState::kPaused && ctrl->pause_requested.load() &&
                    local_task.state != TaskState::kCanceled) {
                    local_task.state = TaskState::kPaused;
                    local_task.status_detail = "Paused";
                }
                t.state = local_task.state;
                t.file_name = local_task.file_name;
                t.category = local_task.category;
                t.part_path = local_task.part_path;
                t.total_bytes = local_task.total_bytes;
                t.downloaded_bytes = local_task.downloaded_bytes;
                t.speed_bps = 0;
                t.upload_speed_bps = 0;
                t.uploaded_bytes = local_task.uploaded_bytes;
                t.stream_percent = local_task.stream_percent;
                t.completed_segments = local_task.completed_segments;
                t.total_segments = local_task.total_segments;
                t.range_supported = local_task.range_supported;
                t.status_detail = local_task.status_detail;
                t.etag = local_task.etag;
                t.last_modified = local_task.last_modified;
                t.segments = local_task.segments;
                t.error_code = local_task.error_code;
                t.error_message = local_task.error_message;
                t.updated_at_ms = local_task.updated_at_ms;
                t.completed_at_ms = local_task.completed_at_ms;
                if (t.state == TaskState::kCompleted || t.state == TaskState::kCanceled) {
                    t.auth_headers.clear();
                }
                to_save = t;
                should_persist = true;
            }
        }
    }
    if (should_persist) {
        m_store.UpsertTask(to_save);
    }

    ctrl->finished.store(true);
    m_cv.notify_all();
}

Result<void> DownloadController::StartTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    if (it->second.state == TaskState::kPaused || it->second.state == TaskState::kFailed ||
        it->second.state == TaskState::kQueued) {
        it->second.scheduled = false;
        it->second.schedule_time_local.clear();
        it->second.schedule_stop_time_local.clear();
        it->second.pause_reason = PauseReason::kNone;
        it->second.state = TaskState::kQueued;
        it->second.status_detail = "Queued";
        it->second.error_code.clear();
        it->second.error_message.clear();
        m_store.UpsertTask(it->second);
        m_cv.notify_all();
    }
    return Result<void>::Ok();
}

Result<void> DownloadController::PauseTask(const std::string& task_id) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
        it->second.pause_reason = PauseReason::kUser;
        if (it->second.state == TaskState::kQueued) {
            it->second.state = TaskState::kPaused;
            it->second.status_detail = "Paused";
            m_store.UpsertTask(it->second);
            return Result<void>::Ok();
        }
        auto ait = m_active.find(task_id);
        if (ait != m_active.end()) {
            // Do not synchronously wait for network workers on the UI/IPC caller.
            // The worker observes this flag, exits its current I/O operation, and
            // performs the final persistence merge below without resurrecting the task.
            ait->second->pause_requested.store(true);
            it->second.state = TaskState::kPaused;
            it->second.status_detail = "Pausing...";
            it->second.speed_bps = 0;
            m_store.UpsertTask(it->second);
        }
    }
    return Result<void>::Ok();
}

Result<void> DownloadController::ResumeTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    if (it->second.state == TaskState::kPaused || it->second.state == TaskState::kFailed) {
        it->second.scheduled = false;
        it->second.schedule_time_local.clear();
        it->second.schedule_stop_time_local.clear();
        it->second.pause_reason = PauseReason::kNone;
        it->second.state = TaskState::kQueued;
        it->second.status_detail = "Queued (Resuming)";
        it->second.error_code.clear();
        it->second.error_message.clear();
        m_store.UpsertTask(it->second);
        m_cv.notify_all();
    }
    return Result<void>::Ok();
}

Result<void> DownloadController::CancelTask(const std::string& task_id) {
    std::shared_ptr<ActiveTaskControl> ctrl;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
        if (it->second.state == TaskState::kCompleted) {
            return Result<void>::Err("ERR_ALREADY_COMPLETED", "Completed task cannot be canceled");
        }
        auto ait = m_active.find(task_id);
        if (ait != m_active.end() && !ait->second->finished.load()) {
            ctrl = ait->second;
            ctrl->cancel_requested.store(true);
        } else {
            std::error_code ec;
            if (!it->second.part_path.empty()) {
                std::filesystem::remove_all(it->second.part_path, ec);
            }
            it->second.part_path.clear();
            it->second.segments.clear();
            it->second.downloaded_bytes = 0;
            it->second.speed_bps = 0;
            it->second.state = TaskState::kCanceled;
            it->second.status_detail = "Canceled";
            it->second.auth_headers.clear();
            m_store.UpsertTask(it->second);
            return Result<void>::Ok();
        }
    }
    if (ctrl) {
        while (!ctrl->finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it != m_tasks.end()) {
            std::error_code ec;
            if (!it->second.part_path.empty()) {
                std::filesystem::remove_all(it->second.part_path, ec);
            }
            it->second.part_path.clear();
            it->second.segments.clear();
            it->second.downloaded_bytes = 0;
            it->second.speed_bps = 0;
            it->second.state = TaskState::kCanceled;
            it->second.status_detail = "Canceled";
            it->second.auth_headers.clear();
            m_store.UpsertTask(it->second);
        }
    }
    return Result<void>::Ok();
}

Result<void> DownloadController::RetryTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    auto& t = it->second;
    if (t.state != TaskState::kFailed && t.state != TaskState::kCanceled && t.state != TaskState::kPaused) {
        return Result<void>::Err("ERR_INVALID_STATE", "Only failed, canceled, or paused tasks can be retried");
    }

    // Deactivate any old schedules on Retry so task runs immediately
    t.scheduled = false;
    t.schedule_time_local.clear();
    t.schedule_stop_time_local.clear();
    t.pause_reason = PauseReason::kNone;

    // Clean up temporary files on Retry only for canceled tasks or unrecoverable validator/corruption errors
    if (t.state == TaskState::kCanceled ||
        t.error_code == "ERR_HTTP_VALIDATOR_MISMATCH" ||
        t.error_code == "ERR_STORAGE_PART_CORRUPTED" ||
        t.error_code == "ERR_STREAM_DRM_PROTECTED") {
        std::error_code ec;
        if (!t.part_path.empty()) {
            std::filesystem::remove_all(t.part_path, ec);
        }
        if (t.error_code == "ERR_HTTP_VALIDATOR_MISMATCH" || t.error_code == "ERR_STORAGE_PART_CORRUPTED") {
            t.part_path.clear();
            t.segments.clear();
            t.downloaded_bytes = 0;
            t.etag.clear();
            t.last_modified.clear();
        }
    }

    ++t.retry_count;
    t.state = TaskState::kQueued;
    t.status_detail = "Queued (Retry #" + std::to_string(t.retry_count) + ")";
    t.error_code.clear();
    t.error_message.clear();
    m_store.UpsertTask(t);
    m_cv.notify_all();
    return Result<void>::Ok();
}

Result<void> DownloadController::RemoveTask(const std::string& task_id, bool delete_file) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
        if (m_deleted_tasks.count(task_id)) {
            return Result<void>::Err("ERR_TASK_REMOVING", "Task removal is already in progress");
        }
        m_deleted_tasks.insert(task_id);
    }

    // Stop active worker if running and cleanly join thread
    std::shared_ptr<ActiveTaskControl> ctrl;
    std::thread worker_to_join;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto ait = m_active.find(task_id);
        if (ait != m_active.end()) {
            ctrl = ait->second;
            ctrl->cancel_requested.store(true);
        }
    }
    if (ctrl) {
        auto start_wait = std::chrono::steady_clock::now();
        while (!ctrl->finished.load() && 
               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_wait).count() < 3000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto ait = m_active.find(task_id);
        if (ait != m_active.end()) {
            if (ait->second->worker_thread.joinable()) {
                worker_to_join = std::move(ait->second->worker_thread);
            }
            m_active.erase(ait);
        }
    }
    if (worker_to_join.joinable()) {
        worker_to_join.join();
    }

    TaskModel task_snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) {
            m_deleted_tasks.erase(task_id);
            return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
        }
        task_snapshot = it->second;
    }

    auto fail_removal = [this, &task_id](const std::string& code, const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deleted_tasks.erase(task_id);
        m_cv.notify_all();
        return Result<void>::Err(code, message);
    };

    // Clean only the task-owned partial/temp path. Keep the completed user output
    // untouched unless the explicit permanent-delete action was requested.
    if (!task_snapshot.part_path.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(task_snapshot.part_path, ec);
        if (ec) {
            OPENIDM_LOG_ERROR("Controller", "Failed to remove partial output " + task_snapshot.part_path + ": " + ec.message());
            return fail_removal("ERR_PART_DELETE_FAILED", "Failed to remove partial download data: " + ec.message());
        }
    }

    // Delete physical file(s) permanently when requested (regardless of task state: paused, completed, failed)
    if (delete_file) {
        auto remove_output_path = [&](const std::string& relative_path, bool recursive) -> Result<void> {
            const auto output_path = ResolveTaskOutputPath(
                std::filesystem::u8path(task_snapshot.save_dir), std::filesystem::u8path(relative_path));
            if (!output_path.has_value()) {
                return Result<void>::Err("ERR_FILE_DELETE_FAILED", "Refusing to delete a path outside the download folder.");
            }

            std::error_code ec;
            const auto file_status = std::filesystem::symlink_status(*output_path, ec);
            if (ec == std::errc::no_such_file_or_directory ||
                (!ec && file_status.type() == std::filesystem::file_type::not_found)) {
                return Result<void>::Ok();
            }
            if (ec) {
                return Result<void>::Err("ERR_FILE_DELETE_FAILED", "Failed to inspect file on disk: " + ec.message());
            }
#ifdef _WIN32
            const DWORD attributes = GetFileAttributesW(output_path->c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
                return Result<void>::Err("ERR_FILE_DELETE_FAILED", "The downloaded file is read-only. Clear its read-only attribute and retry.");
            }
#endif
            if (recursive && file_status.type() == std::filesystem::file_type::directory) {
                std::filesystem::remove_all(*output_path, ec);
            } else {
                std::filesystem::remove(*output_path, ec);
            }
            if (ec) {
                return Result<void>::Err("ERR_FILE_DELETE_FAILED", "Failed to delete file from disk: " + ec.message());
            }
            return Result<void>::Ok();
        };

        if (task_snapshot.task_type == TaskType::kTorrent && !task_snapshot.torrent_file_paths.empty()) {
            // Multi-file torrents are written at their metadata-relative paths;
            // task.file_name is only the torrent's display/root name and may not
            // exist on disk. Delete each selected payload and propagate failures.
            for (size_t i = 0; i < task_snapshot.torrent_file_paths.size(); ++i) {
                if (i < task_snapshot.torrent_file_selection.size() && !task_snapshot.torrent_file_selection[i]) continue;
                auto result = remove_output_path(task_snapshot.torrent_file_paths[i], false);
                if (!result.ok()) {
                    OPENIDM_LOG_ERROR("Controller", "Failed to permanently delete torrent payload for task " + task_id + ": " + result.error->message);
                    return fail_removal(result.error->code, result.error->message);
                }
            }
        } else {
            auto result = remove_output_path(task_snapshot.file_name, true);
            if (!result.ok()) {
                OPENIDM_LOG_ERROR("Controller", "Failed to permanently delete output for task " + task_id + ": " + result.error->message);
                return fail_removal(result.error->code, result.error->message);
            }
        }

        // Clean up partial extension file if present
        if (task_snapshot.task_type != TaskType::kTorrent) {
            auto part_ext_path = ResolveTaskOutputPath(
                std::filesystem::u8path(task_snapshot.save_dir), std::filesystem::u8path(task_snapshot.file_name + ".part"));
            if (!part_ext_path.has_value()) {
                return fail_removal("ERR_FILE_DELETE_FAILED", "Refusing to delete a partial file outside the download folder.");
            }
            std::error_code part_ec;
            std::filesystem::remove(*part_ext_path, part_ec);
            if (part_ec && part_ec != std::errc::no_such_file_or_directory) {
                return fail_removal("ERR_FILE_DELETE_FAILED", "Failed to delete partial output file: " + part_ec.message());
            }
        }
    }

    // Persist removal before erasing the in-memory model so a storage failure
    // cannot make the task disappear only until the next refresh/restart.
    auto db_result = m_store.DeleteTask(task_id);
    if (!db_result.ok()) {
        return fail_removal(db_result.error->code, db_result.error->message);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.erase(task_id);
        // Worker completion is synchronized above; no callback can resurrect it.
        m_deleted_tasks.erase(task_id);
    }
    m_cv.notify_all();
    return Result<void>::Ok();
}

Result<void> DownloadController::UpdateTask(const TaskModel& task) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task.task_id);
    if (it == m_tasks.end() || m_deleted_tasks.count(task.task_id)) {
        return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    }
    it->second = task;
    it->second.updated_at_ms = NowEpochMs();
    auto db_res = m_store.UpsertTask(it->second);
    if (!db_res.ok()) {
        return Result<void>::Err(db_res.error->code, db_res.error->message);
    }
    m_cv.notify_all();
    return Result<void>::Ok();
}

Result<int> DownloadController::ClearCompletedHistory() {
    std::vector<std::string> to_remove;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, t] : m_tasks) {
            if (t.state == TaskState::kCompleted || t.state == TaskState::kCanceled) {
                to_remove.push_back(id);
            }
        }
        for (const auto& id : to_remove) {
            m_deleted_tasks.insert(id);
            m_tasks.erase(id);
        }
    }
    for (const auto& id : to_remove) {
        m_store.DeleteTask(id);
    }
    return Result<int>::Ok(static_cast<int>(to_remove.size()));
}

void DownloadController::PauseAllTasks() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, t] : m_tasks) {
            if (t.state == TaskState::kQueued || t.state == TaskState::kProbing ||
                t.state == TaskState::kDownloading || t.state == TaskState::kMuxing) {
                ids.push_back(id);
                auto ait = m_active.find(id);
                if (ait != m_active.end()) {
                    ait->second->pause_requested.store(true);
                }
            }
        }
    }
    for (const auto& id : ids) {
        PauseTask(id);
    }
}

void DownloadController::ResumeAllTasks() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, t] : m_tasks) {
        if (t.state == TaskState::kPaused && (!t.scheduled || t.pause_reason == PauseReason::kUser)) {
            // Resume All is an explicit user action and has the same meaning as
            // Resume on an individual task, including clearing its old schedule.
            t.scheduled = false;
            t.schedule_time_local.clear();
            t.schedule_stop_time_local.clear();
            t.state = TaskState::kQueued;
            t.pause_reason = PauseReason::kNone;
            t.status_detail = "Queued";
            m_store.UpsertTask(t);
        }
    }
    m_cv.notify_all();
}

void DownloadController::StartAllTasks() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, t] : m_tasks) {
        if ((t.state == TaskState::kPaused || t.state == TaskState::kFailed || t.state == TaskState::kQueued) && !t.scheduled) {
            t.scheduled = false;
            t.schedule_time_local.clear();
            t.schedule_stop_time_local.clear();
            t.pause_reason = PauseReason::kNone;
            t.state = TaskState::kQueued;
            t.status_detail = "Queued";
            t.error_code.clear();
            t.error_message.clear();
            m_store.UpsertTask(t);
        }
    }
    m_cv.notify_all();
}

Result<void> DownloadController::RefreshTaskUrl(
    const std::string& task_id,
    const std::string& new_url,
    const std::map<std::string, std::string>& new_headers,
    const std::map<std::string, std::string>& new_auth_headers) {

    PauseTask(task_id);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");

    it->second.url = new_url;

    // Clean replacement of auth headers
    if (!new_auth_headers.empty()) {
        it->second.auth_headers = new_auth_headers;
    }

    // Clean replacement of cookies (XDM rule: refreshed cookie replaces stale one instead of merging)
    bool has_new_cookie = false;
    for (const auto& [k, v] : new_headers) {
        std::string kl = k;
        for (char& c : kl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (kl == "cookie") {
            has_new_cookie = true;
            break;
        }
    }
    if (has_new_cookie) {
        for (auto hit = it->second.headers.begin(); hit != it->second.headers.end();) {
            std::string kl = hit->first;
            for (char& c : kl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (kl == "cookie") {
                hit = it->second.headers.erase(hit);
            } else {
                ++hit;
            }
        }
    }
    for (const auto& [k, v] : new_headers) it->second.headers[k] = v;

    it->second.retry_count = 0;
    it->second.state = TaskState::kQueued;
    it->second.status_detail = "Queued (Refreshed URL)";
    it->second.error_code.clear();
    it->second.error_message.clear();
    m_store.UpsertTask(it->second);
    m_cv.notify_all();
    return Result<void>::Ok();
}

void DownloadController::SetMaxConcurrentTasks(uint32_t max_tasks) {
    uint32_t clamped = std::clamp(max_tasks, 1u, 16u);
    m_max_concurrent.store(clamped);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings.max_concurrent_tasks = clamped;
        SaveSettingsLocked();
    }
    m_cv.notify_all();
}

uint32_t DownloadController::GetMaxConcurrentTasks() const {
    return m_max_concurrent.load();
}

void DownloadController::SetGlobalSpeedLimit(uint64_t bps) {
    m_global_speed_limit_bps.store(bps);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings.speed_limit_bps = bps;
    SaveSettingsLocked();
}

uint64_t DownloadController::GetGlobalSpeedLimit() const {
    return m_global_speed_limit_bps.load();
}

Result<void> DownloadController::SetTaskSpeedLimit(const std::string& task_id, uint64_t bps) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end() || m_deleted_tasks.count(task_id)) {
        return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    }
    it->second.speed_limit_bps = bps;
    auto ait = m_active.find(task_id);
    if (ait != m_active.end() && ait->second) {
        ait->second->task_speed_limit_bps.store(bps);
    }
    m_store.UpsertTask(it->second);
    return Result<void>::Ok();
}

uint64_t DownloadController::GetTaskSpeedLimit(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) return it->second.speed_limit_bps;
    return 0;
}

Result<void> DownloadController::AddAlternateUrl(const std::string& task_id, const std::string& alt_url) {
    if (alt_url.empty()) {
        return Result<void>::Err("ERR_INVALID_URL", "Alternate URL cannot be empty");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end() || m_deleted_tasks.count(task_id)) {
        return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    }
    for (const auto& existing : it->second.alternate_urls) {
        if (existing == alt_url) return Result<void>::Ok();
    }
    it->second.alternate_urls.push_back(alt_url);
    m_store.UpsertTask(it->second);
    return Result<void>::Ok();
}

std::vector<std::string> DownloadController::GetAlternateUrls(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) return it->second.alternate_urls;
    return {};
}

Result<void> DownloadController::SetTaskSchedule(
    const std::string& task_id,
    const std::string& start_time_local,
    const std::string& stop_time_local) {
    if (!IsValidTimeFormat(start_time_local)) {
        return Result<void>::Err("ERR_INVALID_SCHEDULE", "Invalid start time format (expected HH:MM 00:00-23:59)");
    }
    if (!IsValidTimeFormat(stop_time_local)) {
        return Result<void>::Err("ERR_INVALID_SCHEDULE", "Invalid stop time format (expected HH:MM 00:00-23:59)");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end() || m_deleted_tasks.count(task_id)) {
        return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    }
    const bool user_paused = it->second.pause_reason == PauseReason::kUser &&
                             it->second.state != TaskState::kCompleted &&
                             it->second.state != TaskState::kCanceled;
    const bool was_schedule_paused = it->second.state == TaskState::kPaused &&
                                     it->second.pause_reason == PauseReason::kQueueSchedule;
    it->second.scheduled = !start_time_local.empty();
    it->second.schedule_time_local = start_time_local;
    it->second.schedule_stop_time_local = stop_time_local;
    if (user_paused) {
        // User intent outranks task and queue schedules. Editing a schedule must
        // never clear an explicit pause or make the task eligible for auto-retry.
        it->second.pause_reason = PauseReason::kUser;
        if (it->second.state == TaskState::kPaused) {
            it->second.status_detail = "Paused (User)";
        }
    } else if (it->second.scheduled) {
        it->second.pause_reason = PauseReason::kQueueSchedule;
        if (it->second.state == TaskState::kQueued) {
            it->second.state = TaskState::kPaused;
            it->second.status_detail = "Scheduled (" + start_time_local + ")";
        }
    } else if (was_schedule_paused) {
        // Removing a schedule does not itself start the task. Resume remains an
        // explicit user action, matching the behavior for other paused tasks.
        it->second.pause_reason = PauseReason::kNone;
        it->second.status_detail = "Paused (Schedule Removed)";
    }
    m_store.UpsertTask(it->second);
    m_cv.notify_all();
    return Result<void>::Ok();
}

AppSettings DownloadController::GetSettings() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_settings;
}

Result<void> DownloadController::UpdateSettings(const AppSettings& settings) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings = settings;
        m_settings.max_concurrent_tasks = std::clamp(m_settings.max_concurrent_tasks, 1u, 16u);
        m_settings.default_connections_per_task = std::clamp(m_settings.default_connections_per_task, 1u, 32u);
        m_settings.stall_timeout_ms = std::clamp(m_settings.stall_timeout_ms, 500, 60000);
        m_settings.min_split_bytes = std::clamp<uint64_t>(m_settings.min_split_bytes, 65536ULL, 10485760ULL);
        m_settings.min_capture_size_bytes = std::clamp<uint64_t>(m_settings.min_capture_size_bytes, 0ULL, 10737418240ULL);
        m_settings.max_auto_retries = std::clamp<uint32_t>(m_settings.max_auto_retries, 0u, 50u);
        m_settings.retry_interval_sec = std::clamp<uint32_t>(m_settings.retry_interval_sec, 1u, 3600u);
        if (m_settings.theme != "coral_dark" && m_settings.theme != "dark" && m_settings.theme != "light" && m_settings.theme != "system" && m_settings.theme != "classic_blue") {
            m_settings.theme = "coral_dark";
        }
        if (!m_settings.default_download_dir.empty()) {
            m_cfg.default_download_dir = m_settings.default_download_dir;
            std::error_code ec;
            std::filesystem::create_directories(m_cfg.default_download_dir, ec);
        }
        m_cfg.stall_timeout_ms = m_settings.stall_timeout_ms;
        m_cfg.min_split_bytes = m_settings.min_split_bytes;
        m_max_concurrent.store(m_settings.max_concurrent_tasks);
        m_global_speed_limit_bps.store(m_settings.speed_limit_bps);
        auto sres = SaveSettingsLocked();
        if (!sres.ok()) return sres;
    }
    m_cv.notify_all();
    return Result<void>::Ok();
}

Result<AppSettings> DownloadController::RestoreDefaultSettings() {
    AppSettings defaults;
    defaults.theme = "coral_dark";
    defaults.max_concurrent_tasks = 3;
    defaults.speed_limit_bps = 0;
    defaults.default_connections_per_task = 8;
    defaults.default_download_dir = PathSanitizer::GetDefaultDownloadsDir(PathSanitizer::GetExecutableDir()).string();
    defaults.category_subfolders_enabled = true;
    defaults.conflict_policy = ConflictPolicy::kAutoRename;
    defaults.auto_start_downloads = true;
    defaults.stall_timeout_ms = 3000;
    defaults.min_split_bytes = 131072;
    defaults.browser_capture_enabled = true;
    defaults.min_capture_size_bytes = 1048576;
    defaults.close_to_tray = true;
    defaults.start_minimized = false;
    defaults.reduced_motion = false;
    defaults.show_completion_notification = true;

    defaults.proxy_mode = "system";
    defaults.proxy_server = "";
    defaults.proxy_bypass = "";
    defaults.proxy_username = "";
    defaults.proxy_password_encrypted = "";
    defaults.server_auth_username = "";
    defaults.server_auth_password_encrypted = "";
    defaults.category_folders = {};
    defaults.custom_extension_categories = {};
    defaults.site_rules = {};
    defaults.max_auto_retries = 5;
    defaults.retry_interval_sec = 15;
    defaults.queue_completion_action = "NONE";

    auto ures = UpdateSettings(defaults);
    if (!ures.ok()) return Result<AppSettings>::Err(ures.error->code, ures.error->message);
    return Result<AppSettings>::Ok(defaults);
}

bool DownloadController::AreWorkersAvailable() const {
    std::error_code ec;
    return std::filesystem::exists(m_cfg.nm3u8dl_binary_path, ec) &&
           std::filesystem::exists(m_cfg.ffmpeg_binary_path, ec);
}

void DownloadController::OnSystemSuspend() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, t] : m_tasks) {
            if (t.state == TaskState::kQueued || t.state == TaskState::kProbing ||
                t.state == TaskState::kDownloading || t.state == TaskState::kMuxing) {
                ids.push_back(id);
            }
        }
    }
    for (const auto& id : ids) {
        PauseTask(id);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& id : ids) {
            auto it = m_tasks.find(id);
            if (it != m_tasks.end() && it->second.state == TaskState::kPaused) {
                it->second.pause_reason = PauseReason::kSystemSuspend;
                it->second.status_detail = "Paused (System Suspend)";
                m_store.UpsertTask(it->second);
            }
        }
    }
}

void DownloadController::OnSystemResume() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, t] : m_tasks) {
        // Only resume tasks that were automatically paused due to system suspend (CTRL-02)
        // User-paused tasks (pause_reason == kUser) remain strictly paused!
        if (t.state == TaskState::kPaused && t.pause_reason == PauseReason::kSystemSuspend && !t.scheduled) {
            t.state = TaskState::kQueued;
            t.pause_reason = PauseReason::kNone;
            t.status_detail = "Queued (Resumed after sleep)";
            m_store.UpsertTask(t);
        }
    }
    m_cv.notify_all();
}

std::vector<TaskModel> DownloadController::ListTasks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<TaskModel> out;
    out.reserve(m_tasks.size());
    for (const auto& [id, t] : m_tasks) {
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const TaskModel& a, const TaskModel& b) {
        return a.created_at_ms > b.created_at_ms;
    });
    return out;
}

std::optional<TaskModel> DownloadController::GetTask(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) return it->second;
    return std::nullopt;
}

Result<TaskModel> DownloadController::WaitForTaskCompletion(const std::string& task_id, int timeout_ms) {
    auto last_progress = std::chrono::steady_clock::now();
    uint64_t last_bytes = 0;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_tasks.find(task_id);
            if (it == m_tasks.end()) {
                return Result<TaskModel>::Err("ERR_NOT_FOUND", "Task not found");
            }
            auto st = it->second.state;
            auto ait = m_active.find(task_id);
            bool worker_done = (ait == m_active.end() || ait->second->finished.load());
            if (worker_done && (st == TaskState::kCompleted || st == TaskState::kFailed ||
                                st == TaskState::kPaused || st == TaskState::kCanceled)) {
                return Result<TaskModel>::Ok(it->second);
            }
            if (it->second.downloaded_bytes > last_bytes) {
                last_bytes = it->second.downloaded_bytes;
                last_progress = std::chrono::steady_clock::now();
            }
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_progress).count();
        if (timeout_ms > 0 && elapsed > timeout_ms) {
            return Result<TaskModel>::Err("ERR_TIMEOUT", "Timed out waiting for task completion");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

Result<void> DownloadController::UpsertQueue(const QueueConfig& queue) {
    return m_store.UpsertQueue(queue);
}

std::vector<QueueConfig> DownloadController::ListQueues() {
    auto res = m_store.ListQueues();
    return res.ok() ? res.value.value() : std::vector<QueueConfig>{};
}

Result<void> DownloadController::MoveTaskToQueue(const std::string& task_id, const std::string& new_queue_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    it->second.queue_id = new_queue_id;
    m_store.UpsertTask(it->second);
    return Result<void>::Ok();
}

Result<std::vector<crawler::GrabbedItem>> DownloadController::CrawlSite(
    const crawler::SiteGrabberConfig& cfg,
    std::atomic<bool>& cancel_requested,
    crawler::GrabbedItemCallback on_item_found) {
    return crawler::SiteGrabber::Crawl(cfg, cancel_requested, on_item_found);
}

Result<security::ScanReport> DownloadController::ScanTaskFile(const std::string& task_id) {
    std::filesystem::path file_p;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) return Result<security::ScanReport>::Err("ERR_NOT_FOUND", "Task not found");
        file_p = std::filesystem::path(it->second.save_dir) / it->second.file_name;
    }
    return security::AntivirusScanner::ScanWithDefender(file_p);
}

Result<archive::ZipArchiveSummary> DownloadController::PreviewZipArchive(const std::filesystem::path& zip_path) {
    return archive::ZipPreviewer::InspectArchive(zip_path);
}

Result<std::string> DownloadController::ExportTasksData(storage::ExportFormat format, bool include_credentials) {
    std::vector<TaskModel> task_list = ListTasks();
    return storage::TaskImportExport::ExportTasksToString(task_list, format, include_credentials);
}

Result<int> DownloadController::ImportTasksData(const std::string& data, storage::ExportFormat format) {
    Result<std::vector<TaskModel>> imported;
    if (format == storage::ExportFormat::kJson) {
        imported = storage::TaskImportExport::ImportFromJson(data);
    } else {
        imported = storage::TaskImportExport::ImportFromCsv(data);
    }
    if (!imported.ok()) return Result<int>::Err(imported.error->code, imported.error->message);

    int count = 0;
    for (auto& t : imported.value.value()) {
        auto add_res = AddTask(std::move(t), true, false);
        if (add_res.ok()) count++;
    }
    return Result<int>::Ok(count);
}

} // namespace openidm
