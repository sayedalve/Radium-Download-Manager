// SPDX-License-Identifier: Apache-2.0
#include "brisk_engine/http_download_engine.hpp"

#include "brisk_engine/http_client.hpp"
#include "common/crypto_utils.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace openidm::brisk {

namespace {

// Cross-platform random-access .openidm.part file wrapper supporting concurrent thread writes at exact byte offsets
class PlatformPartFile {
public:
    PlatformPartFile() = default;
    ~PlatformPartFile() { Close(); }

    bool Open(const std::filesystem::path& path) {
#ifdef _WIN32
        m_handle = CreateFileW(
            path.wstring().c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        return m_handle != INVALID_HANDLE_VALUE;
#else
        m_fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
        return m_fd >= 0;
#endif
    }

    bool Truncate(uint64_t size) {
#ifdef _WIN32
        if (m_handle == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(size);
        if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN)) return false;
        return SetEndOfFile(m_handle) != 0;
#else
        if (m_fd < 0) return false;
        return ::ftruncate(m_fd, static_cast<off_t>(size)) == 0;
#endif
    }

    bool PWrite(const uint8_t* data, size_t len, uint64_t offset) {
#ifdef _WIN32
        if (m_handle == INVALID_HANDLE_VALUE) return false;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
        ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFULL);
        DWORD written = 0;
        return WriteFile(m_handle, data, static_cast<DWORD>(len), &written, &ov) && written == static_cast<DWORD>(len);
#else
        if (m_fd < 0) return false;
        ssize_t w = ::pwrite(m_fd, data, len, static_cast<off_t>(offset));
        return w == static_cast<ssize_t>(len);
#endif
    }

    void SyncAndClose() {
#ifdef _WIN32
        if (m_handle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_handle);
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            ::fsync(m_fd);
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

    void Close() {
#ifdef _WIN32
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE m_handle{INVALID_HANDLE_VALUE};
#else
    int m_fd{-1};
#endif
};

} // namespace

namespace {
bool HttpRangeTraceEnabled() {
    return std::getenv("RADIUM_HTTP_RANGE_TRACE") != nullptr;
}

uint64_t TraceEpochMilliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t TraceElapsedMicroseconds(const std::chrono::steady_clock::time_point& origin) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - origin).count());
}

void EmitRuntimeTrace(const std::string& event) {
    if (HttpRangeTraceEnabled()) {
        std::string timed_event = event;
        if (!timed_event.empty() && timed_event.back() == '}') {
            timed_event.pop_back();
            const auto steady_tick_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            timed_event += ",\"steady_tick_us\":" + std::to_string(steady_tick_us) + "}";
        }
        OPENIDM_LOG_INFO("HttpRangeTrace", "HTTP_RUNTIME_TRACE " + timed_event);
    }
}

void EmitHttpPhaseTrace(const char* phase, const std::chrono::steady_clock::time_point& origin) {
    if (HttpRangeTraceEnabled()) {
        OPENIDM_LOG_INFO("HttpRangeTrace", std::string("HTTP_PHASE_TRACE phase=") + phase +
            " elapsed_us=" + std::to_string(TraceElapsedMicroseconds(origin)));
    }
}

class RuntimeTimedLock {
public:
    RuntimeTimedLock(std::mutex& mutex, const char* lock_name, const char* operation,
                     uint64_t request_id = 0, uint32_t worker = 0, uint32_t segment = 0)
        : m_lock(mutex, std::defer_lock), m_lock_name(lock_name), m_operation(operation),
          m_request_id(request_id), m_worker(worker), m_segment(segment), m_enabled(HttpRangeTraceEnabled()) {
        const auto wait_start = m_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        m_lock.lock();
        if (m_enabled) {
            m_acquired = std::chrono::steady_clock::now();
            m_wait_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(m_acquired - wait_start).count());
        }
    }

    ~RuntimeTimedLock() {
        if (!m_enabled) return;
        const auto released = std::chrono::steady_clock::now();
        const uint64_t hold_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(released - m_acquired).count());
        m_lock.unlock();
        EmitRuntimeTrace("{\"event\":\"lock_timing\",\"lock\":\"" + std::string(m_lock_name) +
            "\",\"operation\":\"" + m_operation + "\",\"request_id\":" + std::to_string(m_request_id) +
            ",\"worker\":" + std::to_string(m_worker) + ",\"segment\":" + std::to_string(m_segment) +
            ",\"wait_us\":" + std::to_string(m_wait_us) + ",\"hold_us\":" + std::to_string(hold_us) + "}");
    }

    RuntimeTimedLock(const RuntimeTimedLock&) = delete;
    RuntimeTimedLock& operator=(const RuntimeTimedLock&) = delete;

private:
    std::unique_lock<std::mutex> m_lock;
    const char* m_lock_name;
    const char* m_operation;
    uint64_t m_request_id{0};
    uint32_t m_worker{0};
    uint32_t m_segment{0};
    bool m_enabled{false};
    uint64_t m_wait_us{0};
    std::chrono::steady_clock::time_point m_acquired{};
};

class RuntimeDurationAccumulator {
public:
    RuntimeDurationAccumulator(uint64_t& total_us, bool enabled)
        : m_total_us(total_us), m_enabled(enabled), m_start(enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~RuntimeDurationAccumulator() {
        if (m_enabled) {
            m_total_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - m_start).count());
        }
    }
    RuntimeDurationAccumulator(const RuntimeDurationAccumulator&) = delete;
    RuntimeDurationAccumulator& operator=(const RuntimeDurationAccumulator&) = delete;
private:
    uint64_t& m_total_us;
    bool m_enabled{false};
    std::chrono::steady_clock::time_point m_start{};
};
} // namespace

HttpDownloadEngine::HttpDownloadEngine(EngineConfig cfg) : m_cfg(cfg) {}
HttpDownloadEngine::~HttpDownloadEngine() = default;

Result<std::filesystem::path> HttpDownloadEngine::ExecuteDownload(
    TaskModel& task,
    std::atomic<bool>& pause_requested,
    std::atomic<bool>& cancel_requested,
    std::atomic<uint64_t>& task_speed_limit_bps,
    ProgressCallback on_progress,
    std::shared_ptr<TokenBucketLimiter> global_limiter,
    std::atomic<uint64_t>* global_speed_limit_bps) {

    const bool http_trace_enabled = HttpRangeTraceEnabled();
    const auto engine_start_tp = std::chrono::steady_clock::now();
    std::atomic<uint64_t> next_trace_request_id{0};
    std::atomic<uint32_t> active_http_requests{0};
    EmitHttpPhaseTrace("initialization_start", engine_start_tp);

    auto task_limiter = std::make_shared<TokenBucketLimiter>();

    auto consume_both_limiters = [&](size_t bytes, std::atomic<bool>& cancel_flag) {
        if (bytes == 0) return;
        if (task_speed_limit_bps.load(std::memory_order_relaxed) > 0) {
            task_limiter->Consume(bytes, task_speed_limit_bps, cancel_flag);
        }
        if (global_limiter && global_speed_limit_bps && global_speed_limit_bps->load(std::memory_order_relaxed) > 0) {
            global_limiter->Consume(bytes, *global_speed_limit_bps, cancel_flag);
        }
    };

    std::map<std::string, std::string> req_headers = task.headers;
    for (const auto& [k, v] : task.auth_headers) {
        req_headers[k] = v;
    }

    // -------------------------------------------------------------------------
    // Phase 1: Initial Probe (FR-001)
    // -------------------------------------------------------------------------
    task.state = TaskState::kProbing;
    auto probe_t0 = std::chrono::steady_clock::now();
    task.status_detail = "Probing server & headers...";
    HttpResponseMeta probe_meta;
    std::atomic<bool> probe_cancel{false};
    const uint64_t probe_request_id = http_trace_enabled ? next_trace_request_id.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    const auto probe_request_start = std::chrono::steady_clock::now();
    const uint32_t active_at_probe_start = active_http_requests.fetch_add(1, std::memory_order_relaxed) + 1;
    if (http_trace_enabled) {
        EmitRuntimeTrace("{\"event\":\"request_start\",\"request_id\":" + std::to_string(probe_request_id) +
            ",\"request_kind\":\"probe\",\"worker\":-1,\"segment\":-1,\"range_start\":0,\"range_end\":0" +
            ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) +
            ",\"active_requests\":" + std::to_string(active_at_probe_start) + "}");
    }

    auto probe_res = HttpClient::PerformGet(
        task.url,
        req_headers,
        0,
        0,
        m_cfg.stall_timeout_ms,
        m_cfg.allow_loopback_redirect,
        probe_cancel,
        [&](const HttpResponseMeta& meta) -> HeaderAction {
            probe_meta = meta;
            return HeaderAction::kStopSuccess;
        },
        nullptr,
        m_cfg.net_opts);
    const uint32_t active_after_probe = active_http_requests.fetch_sub(1, std::memory_order_relaxed) - 1;
    if (http_trace_enabled) {
        const auto probe_request_end = std::chrono::steady_clock::now();
        EmitRuntimeTrace("{\"event\":\"request_end\",\"request_id\":" + std::to_string(probe_request_id) +
            ",\"request_kind\":\"probe\",\"status\":" + std::to_string(probe_meta.status_code) +
            ",\"response_start\":" + std::to_string(probe_meta.range_start) +
            ",\"response_end\":" + std::to_string(probe_meta.range_end) +
            ",\"client_bytes_received\":0,\"file_bytes_written\":0,\"elapsed_us\":" +
            std::to_string(TraceElapsedMicroseconds(engine_start_tp)) + ",\"duration_us\":" +
            std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(probe_request_end - probe_request_start).count()) +
            ",\"active_requests\":" + std::to_string(active_after_probe) +
            ",\"outcome\":\"" + (probe_res.ok() ? std::string("complete") : std::string("failed")) + "\"}");
    }

    // FDM-020: If probe is rate-limited (HTTP 429) or overloaded (HTTP 503), back off using Retry-After
    // and retry a bounded number of times. Servers may apply per-IP rate
    // limits or transient backpressure to all requests, including the initial range-capability probe.
    constexpr int kMaxProbeAttempts = 3;
    for (int probe_attempt = 1;
         probe_attempt < kMaxProbeAttempts &&
         (probe_meta.status_code == 429 || probe_meta.status_code == 503 ||
          (!probe_res.ok() && probe_res.error &&
           (probe_res.error->code == "ERR_HTTP_429_RATE_LIMITED" || probe_res.error->code == "ERR_HTTP_5XX_TRANSIENT")));
         ++probe_attempt) {
        int retry_sec = probe_meta.retry_after_sec;
        if (retry_sec > 30) {
            // Server requested a longer wait than is suitable for active probe blocking.
            // Transition task to user_action_resumable paused state so it can be resumed safely without hammering.
            OPENIDM_LOG_WARN("HttpEngine", "Task " + task.task_id + ": Probe received HTTP " +
                std::to_string(probe_meta.status_code) + " with Retry-After: " + std::to_string(retry_sec) +
                "s > 30s probe cap; pausing task for deferred resumption");
            task.state = TaskState::kPaused;
            task.pause_reason = PauseReason::kError;
            task.error_code = (probe_meta.status_code == 429) ? "ERR_HTTP_429_RATE_LIMITED" : "ERR_HTTP_503";
            task.error_message = "Server requested wait of " + std::to_string(retry_sec) + "s (HTTP " + std::to_string(probe_meta.status_code) + ")";
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(task.error_code, task.error_message, true, true);
        }

        int wait_sec = (retry_sec > 0) ? retry_sec : (probe_attempt * 2);
        OPENIDM_LOG_WARN("HttpEngine", "Task " + task.task_id + ": Probe received HTTP " +
                             std::to_string(probe_meta.status_code) + "; waiting full "
                             + std::to_string(wait_sec) + "s then retrying probe (attempt "
                             + std::to_string(probe_attempt + 1) + "/" + std::to_string(kMaxProbeAttempts) + ")...");
        for (int slept = 0; slept < wait_sec * 1000; slept += 50) {
            if (pause_requested.load() || cancel_requested.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, wait_sec * 1000 - slept)));
        }
        if (!pause_requested.load() && !cancel_requested.load()) {
            probe_cancel.store(false);
            probe_meta = {};
            probe_res = HttpClient::PerformGet(
                task.url,
                req_headers,
                0,
                0,
                m_cfg.stall_timeout_ms,
                m_cfg.allow_loopback_redirect,
                probe_cancel,
                [&](const HttpResponseMeta& meta) -> HeaderAction {
                    probe_meta = meta;
                    return HeaderAction::kStopSuccess;
                },
                nullptr,
                m_cfg.net_opts);
        }
    }

    // If server returned 416 Range Not Satisfiable during range probe:
    // RFC 9110 Section 14.4: Content-Range: bytes */0 indicates a verified empty resource.
    if (probe_meta.status_code == 416) {
        if (probe_meta.is_unsatisfied_range && probe_meta.has_range_total && probe_meta.range_total == 0) {
            OPENIDM_LOG_INFO("HttpEngine", "Task " + task.task_id + ": Probe received 416 with bytes */0 (verified empty resource)");
            probe_meta.status_code = 200;
            probe_meta.content_length = 0;
            probe_meta.has_content_length = true;
        } else {
            // Unexpected 416 or unknown length: attempt un-ranged GET to verify actual resource state
            probe_cancel.store(false);
            auto unrange_res = HttpClient::PerformGet(
                task.url,
                req_headers,
                std::nullopt,
                std::nullopt,
                m_cfg.stall_timeout_ms,
                m_cfg.allow_loopback_redirect,
                probe_cancel,
                [&](const HttpResponseMeta& meta) -> HeaderAction {
                    probe_meta = meta;
                    return HeaderAction::kStopSuccess;
                },
                nullptr,
                m_cfg.net_opts);
            if (!unrange_res.ok()) {
                probe_res = unrange_res;
            }
        }
    }

    auto probe_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - probe_t0).count();
    OPENIDM_LOG_INFO("HttpEngine", "Task " + task.task_id + ": Probe completed in " + std::to_string(probe_ms) + " ms (HTTP " + std::to_string(probe_meta.status_code) + ")");

    if (!probe_res.ok() || probe_meta.status_code >= 400) {
        Error probe_err = !probe_res.ok() ? *probe_res.error : *HttpClient::MapHttpStatusToError(probe_meta).error;
        task.state = probe_err.user_action_resumable ? TaskState::kPaused : TaskState::kFailed;
        task.error_code = probe_err.code;
        task.error_message = probe_err.message;
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(
            probe_err.code, probe_err.message, probe_err.retryable, probe_err.user_action_resumable);
    }

    // FDM-007: Cross-origin redirect credential scrubbing
    if (!probe_meta.final_url.empty() && probe_meta.final_url != task.url) {
        auto orig_u = HttpClient::ParseUrl(task.url);
        auto final_u = HttpClient::ParseUrl(probe_meta.final_url);
        if (orig_u.ok() && final_u.ok()) {
            bool cross_origin = (orig_u.value->scheme != final_u.value->scheme ||
                                 orig_u.value->host != final_u.value->host ||
                                 orig_u.value->port != final_u.value->port);
            if (cross_origin) {
                OPENIDM_LOG_INFO("HttpEngine", "Cross-origin redirect detected (" + task.url + " -> " + probe_meta.final_url + "); scrubbing credentials and authorization headers");
                static const char* kSensitiveHeaders[] = {
                    "cookie", "cookie2", "authorization", "proxy-authorization",
                    "Cookie", "Cookie2", "Authorization", "Proxy-Authorization"
                };
                for (const char* h : kSensitiveHeaders) {
                    req_headers.erase(h);
                    task.headers.erase(h);
                    task.auth_headers.erase(h);
                }
            }
        }
        task.url = probe_meta.final_url;
    }

    bool range_supported = (probe_meta.status_code == 206 && probe_meta.is_partial_content);
    uint64_t total_bytes = 0;
    if (range_supported && probe_meta.range_total > 0) {
        total_bytes = probe_meta.range_total;
    } else if (probe_meta.has_content_length) {
        total_bytes = probe_meta.content_length;
    }

    // Secondary Range capability probe: if server answered 200 OK (and didn't explicitly forbid ranges via Accept-Ranges: none)
    // for files > min_split_bytes, safely verify byte range support with a nonzero probe.
    if (!range_supported && probe_meta.status_code == 200 && !probe_meta.accept_ranges_none && total_bytes > m_cfg.min_split_bytes) {
        OPENIDM_LOG_INFO("HttpEngine", "Probe received 200 OK; performing secondary range verification probe to test Range capability...");
        HttpResponseMeta sec_meta;
        std::atomic<bool> sec_cancel{false};
        uint64_t test_start = total_bytes / 2;
        auto sec_res = HttpClient::PerformGet(
            task.url,
            req_headers,
            test_start,
            test_start,
            m_cfg.stall_timeout_ms,
            m_cfg.allow_loopback_redirect,
            sec_cancel,
            [&](const HttpResponseMeta& meta) -> HeaderAction {
                sec_meta = meta;
                return HeaderAction::kStopSuccess;
            },
            nullptr,
            m_cfg.net_opts);
        if (sec_res.ok() && sec_meta.status_code == 206 && sec_meta.is_partial_content &&
            !sec_meta.content_range_malformed && sec_meta.range_start == test_start) {
            range_supported = true;
            OPENIDM_LOG_INFO("HttpEngine", "Secondary range probe confirmed HTTP 206 Range capability!");
        } else {
            OPENIDM_LOG_INFO("HttpEngine", "Secondary range probe rejected HTTP 206 (status " + std::to_string(sec_meta.status_code) + "); falling back to single stream");
        }
    }

    // -------------------------------------------------------------------------
    // Resource-Aware Resume Validation (COR-03)
    // -------------------------------------------------------------------------
    bool is_resume = !task.segments.empty() && task.downloaded_bytes > 0;
    if (is_resume) {
        bool validator_mismatch = false;
        std::string mismatch_reason;

        if (!task.etag.empty() && !probe_meta.etag.empty() &&
            probe_meta.etag.rfind("W/", 0) != 0 && task.etag != probe_meta.etag) {
            validator_mismatch = true;
            mismatch_reason = "Remote ETag changed from " + task.etag + " to " + probe_meta.etag;
        } else if (!task.last_modified.empty() && !probe_meta.last_modified.empty() &&
                   task.last_modified != probe_meta.last_modified) {
            validator_mismatch = true;
            mismatch_reason = "Remote Last-Modified changed from " + task.last_modified + " to " + probe_meta.last_modified;
        } else if (task.total_bytes > 0 && total_bytes > 0 && task.total_bytes != total_bytes) {
            validator_mismatch = true;
            mismatch_reason = "Remote file size changed on resume (expected " +
                              std::to_string(task.total_bytes) + ", got " + std::to_string(total_bytes) + ")";
        }

        if (validator_mismatch) {
            OPENIDM_LOG_WARN("HttpEngine", "Validator mismatch detected (" + mismatch_reason + "); discarding stale partial segments and resetting file");
            task.segments.clear();
            task.downloaded_bytes = 0;
            if (!task.part_path.empty()) {
                std::error_code ec_remove;
                std::filesystem::remove(task.part_path, ec_remove);
            }
            task.state = TaskState::kFailed;
            task.error_code = "ERR_HTTP_VALIDATOR_MISMATCH";
            task.error_message = mismatch_reason;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(
                "ERR_HTTP_VALIDATOR_MISMATCH", task.error_message, false, false);
        }

        // Apply If-Range validator if available
        if (!task.etag.empty() && probe_meta.etag.rfind("W/", 0) != 0) {
            req_headers["If-Range"] = task.etag;
        } else if (!task.last_modified.empty()) {
            req_headers["If-Range"] = task.last_modified;
        }
    }

    task.range_supported = range_supported;
    task.total_bytes = total_bytes;
    if (!probe_meta.etag.empty()) task.etag = probe_meta.etag;
    if (!probe_meta.last_modified.empty()) task.last_modified = probe_meta.last_modified;

    if (!probe_meta.content_disposition.empty()) {
        std::string cd_name = PathSanitizer::ExtractFromContentDisposition(probe_meta.content_disposition);
        if (!cd_name.empty()) {
            task.file_name = cd_name;
        }
    }
    if (task.file_name.empty() || task.file_name == "download" || task.file_name == "index.html" || task.file_name == "file") {
        std::string url_name = PathSanitizer::ExtractFromUrl(task.url);
        if (!url_name.empty() && url_name != "download") {
            task.file_name = url_name;
        }
    }
    if (task.file_name.empty()) {
        task.file_name = "download.bin";
    }
    task.file_name = PathSanitizer::SanitizeFileName(task.file_name);
    // Preserve explicitly assigned category (e.g. from user choice, site rules, or custom extensions).
    // Only infer from filename or probe response MIME metadata if unassigned or fallback "General".
    if (task.category.empty() || task.category == "General") {
        std::string fn_cat = InferCategoryFromFilename(task.file_name);
        if (fn_cat != "General") {
            task.category = fn_cat;
        } else if (!probe_meta.content_type.empty()) {
            std::string mime_cat = InferCategoryFromMime(probe_meta.content_type);
            if (mime_cat != "General") {
                task.category = mime_cat;
            }
        }
    }

    // Pre-flight disk space check (BUG-009 fix)
    uint64_t remaining_to_download = (total_bytes > task.downloaded_bytes) ? (total_bytes - task.downloaded_bytes) : total_bytes;
    auto space_res = PathSanitizer::CheckAvailableDiskSpace(task.save_dir, remaining_to_download);
    if (!space_res.ok()) {
        task.state = TaskState::kFailed;
        task.error_code = space_res.error->code;
        task.error_message = space_res.error->message;
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(
            space_res.error->code, space_res.error->message, false, true);
    }

    std::error_code ec;
    std::filesystem::create_directories(task.save_dir, ec);

    // -------------------------------------------------------------------------
    // Collision-Resistant Part File Path (COR-01)
    // -------------------------------------------------------------------------
    std::filesystem::path part_path;
    if (!task.part_path.empty() && std::filesystem::exists(task.part_path, ec) &&
        task.part_path.find(task.task_id) != std::string::npos) {
        part_path = task.part_path;
    } else {
        part_path = std::filesystem::path(task.save_dir) / (task.file_name + "." + task.task_id + ".part");
        task.part_path = part_path.string();
    }

    // -------------------------------------------------------------------------
    // Validate Partial File on Resume (COR-02)
    // -------------------------------------------------------------------------
    if (is_resume) {
        if (!std::filesystem::exists(part_path, ec)) {
            OPENIDM_LOG_WARN("HttpEngine", "Part file missing on resume; resetting segments to restart from byte 0");
            is_resume = false;
            task.segments.clear();
            task.downloaded_bytes = 0;
        } else {
            uint64_t actual_part_sz = std::filesystem::file_size(part_path, ec);
            bool segments_consistent = SegmentScheduler::ValidateSegmentPartition(task.segments, total_bytes);
            uint64_t verified_dl_bytes = 0;
            if (segments_consistent) {
                for (const auto& s : task.segments) {
                    if (s.current_offset > s.start_offset && s.current_offset > actual_part_sz) {
                        segments_consistent = false;
                        break;
                    }
                    verified_dl_bytes += (s.current_offset - s.start_offset);
                }
            }
            if (!segments_consistent) {
                OPENIDM_LOG_WARN("HttpEngine", "Segment metadata invalid or inconsistent with partial file on disk; resetting segments");
                is_resume = false;
                task.segments.clear();
                task.downloaded_bytes = 0;
            } else {
                task.downloaded_bytes = verified_dl_bytes;
            }
        }
    }

    PlatformPartFile part_file;
    if (!part_file.Open(part_path)) {
        task.state = TaskState::kFailed;
        task.error_code = "ERR_STORAGE_OPEN";
        task.error_message = "Failed to open part file: " + part_path.string();
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err("ERR_STORAGE_OPEN", task.error_message);
    }

    if (total_bytes > 0) {
        auto format_sz = [](uint64_t b) -> std::string {
            if (b >= 1073741824ULL) return std::to_string(b / 1073741824ULL) + " GB";
            if (b >= 1048576ULL) return std::to_string(b / 1048576ULL) + " MB";
            if (b >= 1024ULL) return std::to_string(b / 1024ULL) + " KB";
            return std::to_string(b) + " B";
        };
        task.status_detail = "Allocating storage (" + format_sz(total_bytes) + ")...";
        if (on_progress) {
            EngineProgress ep;
            ep.task_id = task.task_id;
            ep.state = TaskState::kProbing;
            ep.downloaded_bytes = task.downloaded_bytes;
            ep.total_bytes = total_bytes;
            ep.speed_bps = 0;
            ep.range_supported = range_supported;
            ep.resolved_filename = task.file_name;
            on_progress(ep);
        }
        auto alloc_t0 = std::chrono::steady_clock::now();
        if (!is_resume) {
            if (!part_file.Truncate(total_bytes)) {
                part_file.Close();
                task.state = TaskState::kFailed;
                task.error_code = "ERR_DISK_FULL";
                task.error_message = "Failed to pre-allocate disk space";
                task.status_detail = task.error_message;
                return Result<std::filesystem::path>::Err("ERR_DISK_FULL", task.error_message, false, true);
            }
        } else {
            uint64_t actual_part_sz = std::filesystem::file_size(part_path, ec);
            if (actual_part_sz != total_bytes) {
                // XDM tempFileIntact rule: partial file on resume must match expected total_bytes exactly.
                // If it was enlarged by disk corruption or shorter than expected, normalize file size.
                part_file.Truncate(total_bytes);
            }
        }
        auto alloc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - alloc_t0).count();
        OPENIDM_LOG_INFO("HttpEngine", "Task " + task.task_id + ": Storage allocation completed in " + std::to_string(alloc_ms) + " ms");
    }

    task.state = TaskState::kDownloading;

    // -------------------------------------------------------------------------
    // Case A: Non-Range Server Single-Stream Fallback (FR-002 & BUG-008 Fix)
    // -------------------------------------------------------------------------
    if (!range_supported || total_bytes == 0) {
        // Clear any stale segments if server switched from Range to Non-Range on resume
        task.segments.clear();
        task.downloaded_bytes = 0;
        task.status_detail = "Downloading (Single Stream)...";
        part_file.Truncate(0);
        uint64_t written_offset = 0;
        auto start_tp = std::chrono::steady_clock::now();
        auto last_progress_tp = start_tp;
        uint64_t last_progress_bytes = 0;

        std::atomic<bool> stop_flag{false};
        uint64_t single_stream_speed = 0;
        auto single_res = HttpClient::PerformGet(
            task.url,
            req_headers,
            std::nullopt,
            std::nullopt,
            m_cfg.stall_timeout_ms,
            m_cfg.allow_loopback_redirect,
            stop_flag,
            [&](const HttpResponseMeta&) -> HeaderAction { return HeaderAction::kContinue; },
            [&](const uint8_t* data, size_t len) -> bool {
                if (pause_requested.load() || cancel_requested.load()) {
                    stop_flag.store(true);
                    return false;
                }
                consume_both_limiters(len, stop_flag);
                if (!part_file.PWrite(data, len, written_offset)) {
                    stop_flag.store(true);
                    return false;
                }
                written_offset += len;
                task.downloaded_bytes = written_offset;

                auto now = std::chrono::steady_clock::now();
                auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_tp).count();
                if (diff_ms >= 100 && on_progress) {
                    uint64_t interval_bytes = written_offset - last_progress_bytes;
                    uint64_t inst_speed = (interval_bytes * 1000ULL) / std::max<int64_t>(1, diff_ms);
                    if (single_stream_speed == 0) {
                        single_stream_speed = inst_speed;
                    } else {
                        single_stream_speed = static_cast<uint64_t>(0.25 * static_cast<double>(inst_speed) + 0.75 * static_cast<double>(single_stream_speed));
                    }
                    last_progress_bytes = written_offset;
                    last_progress_tp = now;

                    EngineProgress ep;
                    ep.task_id = task.task_id;
                    ep.state = TaskState::kDownloading;
                    ep.downloaded_bytes = written_offset;
                    ep.total_bytes = total_bytes;
                    ep.speed_bps = single_stream_speed;
                    ep.range_supported = false;
                    ep.resolved_filename = task.file_name;
                    on_progress(ep);
                }
                return true;
            },
            m_cfg.net_opts);

        part_file.SyncAndClose();

        if (cancel_requested.load()) {
            std::filesystem::remove(part_path, ec);
            task.state = TaskState::kCanceled;
            task.status_detail = "Canceled";
            return Result<std::filesystem::path>::Err("ERR_CANCELED", "Task canceled by user");
        }
        if (pause_requested.load()) {
            task.state = TaskState::kPaused;
            task.status_detail = "Paused";
            return Result<std::filesystem::path>::Err("ERR_PAUSED", "Task paused by user");
        }
        if (!single_res.ok()) {
            task.state = TaskState::kFailed;
            task.error_code = single_res.error->code;
            task.error_message = single_res.error->message;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(single_res.error->code, single_res.error->message);
        }
        task.total_bytes = written_offset;
    } else {
        // ---------------------------------------------------------------------
        // Case B: Brisk Multi-Segment Dynamic Spawn & Work-Stealing Engine (FR-003, FR-004)
        // ---------------------------------------------------------------------
        uint32_t max_conns = std::clamp(task.max_connections, 1u, 32u);
        SegmentScheduler scheduler(total_bytes, max_conns, m_cfg.min_split_bytes);
        if (is_resume && !task.segments.empty()) {
            scheduler.Initialize(task.segments);
        } else {
            // Pre-calculate balanced contiguous, non-overlapping partitions before data workers launch
            scheduler.InitializeEquipartition(max_conns);
        }

        std::atomic<bool> fatal_abort{false};
        std::atomic<bool> disable_splitting{false};
        std::atomic<uint32_t> running_workers{0};
        auto transfer_start_tp = std::chrono::steady_clock::now();
        std::mutex err_mu;
        std::optional<Error> fatal_error;

        auto worker_fn = [&](uint32_t worker_idx) {
            running_workers.fetch_add(1, std::memory_order_relaxed);
            if (http_trace_enabled) {
                EmitRuntimeTrace("{\"event\":\"worker_start\",\"worker\":" + std::to_string(worker_idx) +
                    ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) +
                    ",\"active_workers\":" + std::to_string(running_workers.load()) + "}");
            }
            uint64_t idle_wait_us = 0;
            uint64_t queue_wait_us = 0;
            uint64_t queue_wait_count = 0;
            auto stop_worker = [&]() {
                const uint32_t active_workers = running_workers.fetch_sub(1, std::memory_order_relaxed) - 1;
                if (http_trace_enabled) {
                    EmitRuntimeTrace("{\"event\":\"worker_stop\",\"worker\":" + std::to_string(worker_idx) +
                        ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) +
                        ",\"active_workers\":" + std::to_string(active_workers) +
                        ",\"idle_wait_us\":" + std::to_string(idle_wait_us) +
                        ",\"queue_wait_us\":" + std::to_string(queue_wait_us) +
                        ",\"queue_wait_count\":" + std::to_string(queue_wait_count) + "}");
                }
            };
            while (!fatal_abort.load() && !pause_requested.load() && !cancel_requested.load()) {
                const auto wait_started = std::chrono::steady_clock::now();
                auto seg = scheduler.AcquireNextOrStealSegment(!disable_splitting.load());
                const bool initially_waiting_for_segment = !seg;
                if (!seg) {
                    if (scheduler.AllCompleted()) {
                        break;
                    }
                    // Wait for work while uncompleted data remains in the scheduler
                    bool acquired = false;
                    for (int idle_retry = 0; idle_retry < 300; ++idle_retry) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        if (fatal_abort.load() || pause_requested.load() || cancel_requested.load() || scheduler.AllCompleted()) {
                            break;
                        }
                        if (scheduler.CanSplitAnySegment() || scheduler.HasIncompleteUnassignedSegment()) {
                            seg = scheduler.AcquireNextOrStealSegment(!disable_splitting.load());
                            if (seg) {
                                acquired = true;
                                break;
                            }
                        }
                    }
                    if (!acquired && !seg) {
                        const uint64_t wait_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - wait_started).count());
                        idle_wait_us += wait_us;
                        queue_wait_us += wait_us;
                        ++queue_wait_count;
                        break;
                    }
                }
                const uint64_t acquisition_wait_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - wait_started).count());
                idle_wait_us += acquisition_wait_us;
                if (initially_waiting_for_segment) {
                    queue_wait_us += acquisition_wait_us;
                    ++queue_wait_count;
                }

                int retries = 0;
                uint64_t last_progress_offset = seg->current_offset.load();
                while (!fatal_abort.load() && !pause_requested.load() && !cancel_requested.load()) {
                    uint64_t curr = seg->current_offset.load();
                    uint64_t end = seg->end_offset.load();
                    if (curr > end) {
                        seg->completed.store(true);
                        seg->active_worker.store(false);
                        break;
                    }

                    std::atomic<bool> req_cancel{false};
                    bool server_ignored_range = false;
                    int captured_retry_after_sec = 0; // captured from on_headers for 429 backoff
                    const uint64_t trace_request_id = http_trace_enabled
                        ? next_trace_request_id.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
                    const auto request_start_tp = std::chrono::steady_clock::now();
                    const uint64_t request_start_epoch_ms = http_trace_enabled ? TraceEpochMilliseconds() : 0;
                    const uint32_t active_at_start = active_http_requests.fetch_add(1, std::memory_order_relaxed) + 1;
                    uint64_t client_bytes_received = 0;
                    uint64_t file_bytes_written = 0;
                    uint64_t file_write_duration_us = 0;
                    uint64_t file_write_max_duration_us = 0;
                    uint64_t file_write_count = 0;
                    uint64_t callback_processing_us = 0;
                    uint64_t limiter_wait_us = 0;
                    uint64_t response_content_length = 0;
                    uint64_t response_range_start = 0;
                    uint64_t response_range_end = 0;
                    int response_status = 0;
                    std::string cancel_reason;

                    if (http_trace_enabled) {
                        EmitRuntimeTrace("{\"event\":\"request_start\",\"request_id\":" + std::to_string(trace_request_id) +
                            ",\"worker\":" + std::to_string(worker_idx) + ",\"segment\":" + std::to_string(seg->segment_id) +
                            ",\"range_start\":" + std::to_string(curr) + ",\"range_end\":" + std::to_string(end) +
                            ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) +
                            ",\"active_requests\":" + std::to_string(active_at_start) + "}");
                    }

                    auto res = HttpClient::PerformGet(
                        task.url,
                        req_headers,
                        curr,
                        end,
                        m_cfg.stall_timeout_ms,
                        m_cfg.allow_loopback_redirect,
                        req_cancel,
                        [&](const HttpResponseMeta& meta) -> HeaderAction {
                            response_status = meta.status_code;
                            response_content_length = meta.content_length;
                            response_range_start = meta.range_start;
                            response_range_end = meta.range_end;
                            captured_retry_after_sec = meta.retry_after_sec;
                            if (meta.status_code == 429 || meta.status_code >= 500) {
                                return HeaderAction::kAbort;
                            }
                            if (meta.status_code == 416) {
                                // Satisfied vs unsatisfied range: Only a confirmed empty file (total_bytes == 0 AND meta.is_unsatisfied_range && meta.range_total == 0)
                                // at curr == 0 can be marked complete.
                                if (curr == 0 && total_bytes == 0 && meta.is_unsatisfied_range && meta.has_range_total && meta.range_total == 0) {
                                    seg->completed.store(true);
                                    return HeaderAction::kStopSuccess;
                                }
                                // Unexpected 416 on nonempty file or missing/malformed range total must NEVER complete segment
                                std::lock_guard<std::mutex> lk(err_mu);
                                fatal_error = Error{"ERR_HTTP_RANGE_NOT_SATISFIABLE", "HTTP 416 Range Not Satisfiable on active segment", false, true};
                                fatal_abort.store(true);
                                return HeaderAction::kAbort;
                            }
                            // If server ignored the Range header and returned 200 OK
                            if (meta.status_code == 200) {
                                server_ignored_range = true;
                                disable_splitting.store(true);
                                OPENIDM_LOG_WARN("BriskEngine", "Server returned 200 OK to range request on worker (expected " +
                                    std::to_string(curr) + "-" + std::to_string(end) + "); aborting secondary stream");
                                return HeaderAction::kAbort;
                            }
                            // Validate HTTP 206 Partial Content requirements strictly before writing bytes
                            if (meta.status_code != 206 || !meta.is_partial_content || meta.content_range_malformed) {
                                std::lock_guard<std::mutex> lk(err_mu);
                                fatal_error = Error{"ERR_HTTP_RANGE_PROTOCOL", "Server returned invalid or malformed Content-Range header", false, false};
                                fatal_abort.store(true);
                                return HeaderAction::kAbort;
                            }
                            // Check starting offset
                            if (meta.range_start != curr) {
                                server_ignored_range = true;
                                disable_splitting.store(true);
                                OPENIDM_LOG_WARN("BriskEngine", "Server range_start mismatch (got " +
                                    std::to_string(meta.range_start) + ", expected " + std::to_string(curr) + "); aborting secondary stream");
                                return HeaderAction::kAbort;
                            }
                            // Check ending offset and bounds
                            if (meta.range_end < meta.range_start || (total_bytes > 0 && meta.range_end >= total_bytes)) {
                                std::lock_guard<std::mutex> lk(err_mu);
                                fatal_error = Error{"ERR_HTTP_RANGE_PROTOCOL", "Server returned impossible range bounds (end < start or end >= total)", false, false};
                                fatal_abort.store(true);
                                return HeaderAction::kAbort;
                            }
                            // Check total length consistency
                            if (total_bytes > 0 && meta.range_total > 0 && meta.range_total != total_bytes) {
                                std::lock_guard<std::mutex> lk(err_mu);
                                fatal_error = Error{"ERR_HTTP_VALIDATOR_MISMATCH",
                                    "Remote entity length changed mid-download (expected " +
                                    std::to_string(total_bytes) + ", got " + std::to_string(meta.range_total) + ")", false, false};
                                fatal_abort.store(true);
                                return HeaderAction::kAbort;
                            }
                            // Validate Content-Length matches byte range span
                            uint64_t expected_span = meta.range_end - meta.range_start + 1;
                            if (meta.has_content_length && meta.content_length != expected_span) {
                                std::lock_guard<std::mutex> lk(err_mu);
                                fatal_error = Error{"ERR_HTTP_RANGE_PROTOCOL",
                                    "Content-Length (" + std::to_string(meta.content_length) +
                                    ") does not match Content-Range span (" + std::to_string(expected_span) + ")", false, false};
                                fatal_abort.store(true);
                                return HeaderAction::kAbort;
                            }
                            // The new request may clear a pending split only if it
                            // was created with the scheduler's current end bound.
                            {
                                RuntimeTimedLock split_lock(seg->write_mu, "segment_write", "header_split_check",
                                    trace_request_id, worker_idx, seg->segment_id);
                                if (seg->end_offset.load() == end) {
                                    seg->split_pending.store(false);
                                }
                            }
                            return HeaderAction::kContinue;
                        },
                        [&](const uint8_t* data, size_t len) -> bool {
                            RuntimeDurationAccumulator callback_timer(callback_processing_us, http_trace_enabled);
                            client_bytes_received += static_cast<uint64_t>(len);
                            if (fatal_abort.load() || pause_requested.load() || cancel_requested.load()) {
                                cancel_reason = pause_requested.load() ? "user_pause" :
                                    (cancel_requested.load() ? "user_cancel" : "fatal_abort");
                                req_cancel.store(true);
                                return false;
                            }

                            const auto limiter_started = std::chrono::steady_clock::now();
                            consume_both_limiters(len, req_cancel);
                            limiter_wait_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - limiter_started).count());

                            RuntimeTimedLock write_lk(seg->write_mu, "segment_write", "body_write_callback",
                                trace_request_id, worker_idx, seg->segment_id);
                            uint64_t write_pos = seg->current_offset.load();
                            uint64_t current_end = seg->end_offset.load();
                            if (write_pos > current_end) {
                                seg->completed.store(true);
                                cancel_reason = current_end < end ? "range_split" : "segment_complete";
                                req_cancel.store(true);
                                return false;
                            }

                            uint64_t max_writable = (current_end >= write_pos) ? (current_end - write_pos + 1) : 0;
                            if (max_writable == 0) {
                                seg->completed.store(true);
                                cancel_reason = current_end < end ? "range_split" : "segment_complete";
                                req_cancel.store(true);
                                return false;
                            }
                            size_t to_write = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(len), max_writable));

                            const auto file_write_start = http_trace_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                            const bool file_write_ok = part_file.PWrite(data, to_write, write_pos);
                            if (http_trace_enabled) {
                                const auto file_write_end = std::chrono::steady_clock::now();
                                const uint64_t write_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    file_write_end - file_write_start).count());
                                file_write_duration_us += write_us;
                                file_write_max_duration_us = std::max(file_write_max_duration_us, write_us);
                                ++file_write_count;
                            }
                            if (!file_write_ok) {
                                std::lock_guard<std::mutex> lk(err_mu);
                                fatal_error = Error{"ERR_DISK_FULL", "Disk write failed on segment", false, true};
                                fatal_abort.store(true);
                                req_cancel.store(true);
                                cancel_reason = "disk_write_error";
                                return false;
                            }

                            seg->current_offset.fetch_add(to_write);
                            file_bytes_written += static_cast<uint64_t>(to_write);
                            if (to_write < len || seg->current_offset.load() > seg->end_offset.load()) {
                                cancel_reason = seg->end_offset.load() < end ? "range_split" : "segment_complete";
                                if (seg->current_offset.load() > seg->end_offset.load()) {
                                    seg->completed.store(true);
                                }
                                req_cancel.store(true);
                                return false;
                            }
                            return true;
                        },
                        m_cfg.net_opts);

                    const uint32_t active_after_request = active_http_requests.fetch_sub(1, std::memory_order_relaxed) - 1;

                    if (http_trace_enabled) {
                        const auto request_end_tp = std::chrono::steady_clock::now();
                        const uint64_t request_end_epoch_ms = TraceEpochMilliseconds();
                        const double start_ms = std::chrono::duration<double, std::milli>(request_start_tp - transfer_start_tp).count();
                        const double end_ms = std::chrono::duration<double, std::milli>(request_end_tp - transfer_start_tp).count();
                        const bool body_complete = response_status > 0 && response_content_length > 0 &&
                            client_bytes_received >= response_content_length;
                        const std::string outcome = body_complete ? "complete" :
                            (!cancel_reason.empty() ? "cancelled" : "failed");
                        EmitRuntimeTrace("{\"event\":\"request_end\",\"request_id\":" + std::to_string(trace_request_id) +
                            ",\"worker\":" + std::to_string(worker_idx) + ",\"status\":" + std::to_string(response_status) +
                            ",\"response_start\":" + std::to_string(response_range_start) +
                            ",\"response_end\":" + std::to_string(response_range_end) +
                            ",\"response_content_length\":" + std::to_string(response_content_length) +
                            ",\"client_bytes_received\":" + std::to_string(client_bytes_received) +
                            ",\"file_bytes_written\":" + std::to_string(file_bytes_written) +
                            ",\"file_write_duration_us\":" + std::to_string(file_write_duration_us) +
                            ",\"file_write_max_duration_us\":" + std::to_string(file_write_max_duration_us) +
                            ",\"file_write_count\":" + std::to_string(file_write_count) +
                            ",\"callback_processing_us\":" + std::to_string(callback_processing_us) +
                            ",\"limiter_wait_us\":" + std::to_string(limiter_wait_us) +
                            ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) +
                            ",\"duration_us\":" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(request_end_tp - request_start_tp).count()) +
                            ",\"active_requests\":" + std::to_string(active_after_request) +
                            ",\"outcome\":\"" + outcome + "\",\"cancel_reason\":\"" + cancel_reason + "\"}");
                        std::ostringstream trace;
                        trace << "HTTP_RANGE_TRACE {\"id\":" << trace_request_id
                              << ",\"worker\":" << worker_idx
                              << ",\"requested_start\":" << curr
                              << ",\"requested_end\":" << end
                              << ",\"status\":" << response_status
                              << ",\"response_start\":" << response_range_start
                              << ",\"response_end\":" << response_range_end
                              << ",\"response_content_length\":" << response_content_length
                              << ",\"client_bytes_received\":" << client_bytes_received
                              << ",\"file_bytes_written\":" << file_bytes_written
                              << ",\"committed_start\":" << curr
                              << ",\"committed_end\":";
                        if (file_bytes_written == 0) trace << "null";
                        else trace << curr + file_bytes_written - 1;
                        trace << ",\"start_epoch_ms\":" << request_start_epoch_ms
                              << ",\"end_epoch_ms\":" << request_end_epoch_ms
                              << ",\"start_ms\":" << start_ms
                              << ",\"end_ms\":" << end_ms
                              << ",\"body_complete\":" << (body_complete ? "true" : "false")
                              << ",\"outcome\":\"" << outcome << "\""
                              << ",\"cancel_reason\":\"" << cancel_reason << "\""
                              << ",\"error_code\":\"" << (res.ok() ? "" : res.error->code) << "\"}";
                        OPENIDM_LOG_INFO("HttpRangeTrace", trace.str());
                    }

                    if (seg->current_offset.load() > seg->end_offset.load()) {
                        seg->completed.store(true);
                        seg->active_worker.store(false);
                        break;
                    }

                    // If this request was cancelled due to a range split, clear split_pending
                    // because this request has now stopped reading and observed the split boundary.
                    if (cancel_reason == "range_split") {
                        seg->split_pending.store(false);
                    }

                    if (pause_requested.load() || cancel_requested.load() || fatal_abort.load()) {
                        seg->active_worker.store(false);
                        break;
                    }

                    if (server_ignored_range) {
                        // BUG-008 Fix: Merge unstarted stolen segment back into predecessor so Worker 0 finishes cleanly
                        if (!scheduler.RollbackUnstartedStolenSegment(seg->segment_id)) {
                            std::lock_guard<std::mutex> lk(err_mu);
                            fatal_error = Error{"ERR_HTTP_RANGE_PROTOCOL", "Server failed range check on active segment", false, false};
                            fatal_abort.store(true);
                        }
                        break;
                    }

                    // Forward progress check (NET-01): reset retries if bytes moved forward
                    uint64_t cur_pos = seg->current_offset.load();
                    if (cur_pos > last_progress_offset) {
                        last_progress_offset = cur_pos;
                        retries = 0;
                        scheduler.RecordSuccess();
                    }

                    if (!res.ok()) {
                        uint32_t fail_streak = scheduler.IncrementConsecutiveFailures();
                        int retry_after_ms = 0;

                        bool is_server_overloaded = (res.error->code == "ERR_HTTP_429_RATE_LIMITED" ||
                                                     (response_status == 503 && res.error->code == "ERR_HTTP_5XX_TRANSIENT"));

                        if (is_server_overloaded) {
                            disable_splitting.store(true);
                            scheduler.ReduceConnectionsStepped();
                            // If server requested a wait exceeding 15 seconds, don't keep active worker spinning.
                            // Defer task cleanly to resumable state so all surviving workers exit and pause.
                            if (captured_retry_after_sec > 15) {
                                seg->active_worker.store(false);
                                std::lock_guard<std::mutex> lk(err_mu);
                                fatal_error = Error{
                                    (response_status == 429) ? "ERR_HTTP_429_RATE_LIMITED" : "ERR_HTTP_503",
                                    "Server requested Retry-After delay of " + std::to_string(captured_retry_after_sec) + "s",
                                    true, true
                                };
                                fatal_abort.store(true);
                                stop_worker();
                                return;
                            }

                            retry_after_ms = (captured_retry_after_sec > 0)
                                                ? (captured_retry_after_sec * 1000)
                                                : 2000;
                            OPENIDM_LOG_WARN("BriskEngine", "Server overload/rate limit (HTTP " +
                                std::to_string(response_status) + ") on segment #" +
                                std::to_string(seg->segment_id) + "; reduced target concurrency to " +
                                std::to_string(scheduler.GetMaxConnections()) + ", waiting " +
                                std::to_string(retry_after_ms) + " ms");

                            // Adaptive connection fallback: If active concurrency exceeds reduced target,
                            // release unfinished segment safely back to pool and terminate worker.
                            uint32_t current_max = scheduler.GetMaxConnections();
                            if (running_workers.load() > current_max && running_workers.load() > 1) {
                                seg->active_worker.store(false);
                                OPENIDM_LOG_INFO("BriskEngine", "Worker retiring to respect adaptive rate limit concurrency (" +
                                    std::to_string(running_workers.load()) + " > " + std::to_string(current_max) +
                                    "); segment #" + std::to_string(seg->segment_id) + " returned to pool");
                                stop_worker();
                                return; // Fully exit worker thread
                            }
                        } else if (res.error->code == "ERR_NET_CONNECTION_RESET" ||
                                   res.error->code == "ERR_NET_CONNECT" ||
                                   res.error->code == "ERR_NET_TIMEOUT" ||
                                   res.error->code == "ERR_NET_SEND") {
                            if (fail_streak >= 3) {
                                // Server connection limit or repeated network drop detected
                                scheduler.ReduceConnectionsByHalf();
                                OPENIDM_LOG_WARN("BriskEngine", "Server connection limit/reset streak (" +
                                    std::to_string(fail_streak) + "); scaled down concurrency to " +
                                    std::to_string(scheduler.GetMaxConnections()));
                                uint32_t current_max = scheduler.GetMaxConnections();
                                if (running_workers.load() > current_max && running_workers.load() > 1) {
                                    seg->active_worker.store(false);
                                    OPENIDM_LOG_INFO("BriskEngine", "Worker retiring to respect network backoff concurrency (" +
                                        std::to_string(running_workers.load()) + " > " + std::to_string(current_max) +
                                        "); segment #" + std::to_string(seg->segment_id) + " returned to pool");
                                    stop_worker();
                                    return; // Fully exit worker thread
                                }
                            }
                        }

                        // For rate limiting / transient overload, allow up to 8 retries per worker with backoff and reset streak
                        int max_retries = is_server_overloaded ? 8 : m_cfg.max_retries_per_worker;
                        uint32_t max_streak = is_server_overloaded ? 16 : 12;

                        if (!res.error->retryable || retries >= max_retries || fail_streak > max_streak) {
                            seg->active_worker.store(false);
                            std::lock_guard<std::mutex> lk(err_mu);
                            if (!fatal_error.has_value()) {
                                fatal_error = res.error;
                            }
                            fatal_abort.store(true);
                            break;
                        }
                        ++retries;
                        OPENIDM_LOG_INFO("BriskEngine", "Resetting stalled/failed segment connection #" +
                            std::to_string(seg->segment_id) + " at offset " + std::to_string(seg->current_offset.load()) +
                            " (retry " + std::to_string(retries) + ", streak " + std::to_string(fail_streak) + ")");

                        // If server provided a valid Retry-After, we MUST respect it fully (up to 15s) and not truncate it!
                        int exp_backoff = (100 * (1 << std::min(retries, 6))) + (std::rand() % 150);
                        int backoff_ms = (retry_after_ms > 0) ? retry_after_ms : std::min(exp_backoff, 15000);
                        const auto backoff_started = std::chrono::steady_clock::now();

                        for (int slept = 0; slept < backoff_ms; slept += 50) {
                            if (fatal_abort.load() || pause_requested.load() || cancel_requested.load()) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, backoff_ms - slept)));
                        }
                        if (http_trace_enabled) {
                            const auto actual_wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - backoff_started).count();
                            EmitRuntimeTrace("{\"event\":\"retry_wait\",\"worker\":" + std::to_string(worker_idx) +
                                ",\"segment\":" + std::to_string(seg->segment_id) + ",\"retry\":" + std::to_string(retries) +
                                ",\"requested_wait_ms\":" + std::to_string(backoff_ms) +
                                ",\"actual_wait_us\":" + std::to_string(actual_wait_us) +
                                ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) + "}");
                        }
                    } else {
                        scheduler.ResetConsecutiveFailures();
                    }
                }
            }
            stop_worker();
        };

        auto start_tp = transfer_start_tp;
        EmitHttpPhaseTrace("transfer_start", engine_start_tp);
        uint64_t initial_bytes = scheduler.TotalDownloadedBytes();
        auto last_sp_tp = start_tp;
        uint64_t last_sp_bytes = initial_bytes;
        uint64_t current_speed = 0;

        std::vector<std::thread> workers;
        // Paced ramp-up: Spawn initial worker, then ramp concurrency in the coordinator loop
        // rather than burst-spawning 32 connections simultaneously at t=0, avoiding firewall/CDN flood triggers.
        // When initial equipartition segments exist, launch all initial workers immediately
        // so all partitions start transferring in parallel without artificial serialization delay.
        uint32_t target_initial = max_conns;
        for (uint32_t i = 0; i < target_initial; ++i) {
            workers.emplace_back(worker_fn, i);
        }

        uint32_t spawned = target_initial;
        uint64_t coordinator_poll_wait_us = 0;
        uint64_t coordinator_poll_count = 0;
        while (!scheduler.AllCompleted() && !fatal_abort.load() && !pause_requested.load() && !cancel_requested.load()) {
            const auto poll_wait_started = http_trace_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto now = std::chrono::steady_clock::now();
            if (http_trace_enabled) {
                coordinator_poll_wait_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    now - poll_wait_started).count());
                ++coordinator_poll_count;
            }

            uint32_t dynamic_max = std::min(max_conns, scheduler.GetMaxConnections());
            if (running_workers.load() < dynamic_max && spawned < 256 &&
                (scheduler.HasIncompleteUnassignedSegment() || (!disable_splitting.load() && scheduler.CanSplitAnySegment()))) {
                workers.emplace_back(worker_fn, spawned++);
            }

            auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sp_tp).count();
            if (diff_ms >= 100) {
                uint64_t dl = scheduler.TotalDownloadedBytes();
                task.downloaded_bytes = dl;
                task.segments = scheduler.GetSnapshots();
                task.status_detail = "Downloading (" + std::to_string(scheduler.ActiveWorkerCount()) + " active connections)";
                uint64_t delta_bytes = (dl >= last_sp_bytes) ? (dl - last_sp_bytes) : 0;
                uint64_t inst_speed = (delta_bytes * 1000ULL) / std::max<int64_t>(1, diff_ms);
                if (current_speed == 0) {
                    current_speed = inst_speed;
                } else {
                    current_speed = static_cast<uint64_t>(0.25 * static_cast<double>(inst_speed) + 0.75 * static_cast<double>(current_speed));
                }
                last_sp_bytes = dl;
                last_sp_tp = now;

                if (on_progress) {
                    EngineProgress ep;
                    ep.task_id = task.task_id;
                    ep.state = TaskState::kDownloading;
                    ep.downloaded_bytes = dl;
                    ep.total_bytes = total_bytes;
                    ep.speed_bps = current_speed;
                    ep.range_supported = true;
                    ep.resolved_filename = task.file_name;
                    ep.etag = task.etag;
                    ep.last_modified = task.last_modified;
                    ep.segments = task.segments;
                    on_progress(ep);
                }
            }
        }
        if (http_trace_enabled) {
            EmitRuntimeTrace("{\"event\":\"coordinator_wait_summary\",\"poll_wait_us\":" +
                std::to_string(coordinator_poll_wait_us) + ",\"poll_count\":" +
                std::to_string(coordinator_poll_count) + "}");
        }

        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }

        const auto file_sync_start = http_trace_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        part_file.SyncAndClose();
        if (http_trace_enabled) {
            const uint64_t file_sync_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - file_sync_start).count());
            EmitRuntimeTrace("{\"event\":\"file_sync\",\"duration_us\":" + std::to_string(file_sync_us) + "}");
        }
        EmitHttpPhaseTrace("transfer_end", engine_start_tp);

        task.downloaded_bytes = scheduler.TotalDownloadedBytes();
        task.segments = scheduler.GetSnapshots();

        if (cancel_requested.load()) {
            std::filesystem::remove(part_path, ec);
            task.state = TaskState::kCanceled;
            task.status_detail = "Canceled";
            return Result<std::filesystem::path>::Err("ERR_CANCELED", "Task canceled by user");
        }
        if (pause_requested.load()) {
            task.state = TaskState::kPaused;
            task.status_detail = "Paused";
            return Result<std::filesystem::path>::Err("ERR_PAUSED", "Task paused by user");
        }
        if (fatal_abort.load() || !scheduler.AllCompleted()) {
            std::lock_guard<std::mutex> lk(err_mu);
            Error err = fatal_error.value_or(Error{"ERR_NET_INCOMPLETE", "Segments did not complete", true, true});
            task.state = err.user_action_resumable ? TaskState::kPaused : TaskState::kFailed;
            task.error_code = err.code;
            task.error_message = err.message;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(err.code, err.message, err.retryable, err.user_action_resumable);
        }
    }

    // -------------------------------------------------------------------------
    // Phase 3: File Verification & Atomic Rename (FR-007, FR-026)
    // -------------------------------------------------------------------------
    EmitHttpPhaseTrace("finalization_start", engine_start_tp);
    task.state = TaskState::kVerifying;
    task.status_detail = "Verifying file integrity...";
    if (!task.expected_sha256.empty()) {
        auto sha_res = CryptoUtils::ComputeFileSha256Hex(part_path);
        if (!sha_res.ok()) {
            task.state = TaskState::kFailed;
            task.error_code = sha_res.error->code;
            task.error_message = sha_res.error->message;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(sha_res.error->code, sha_res.error->message);
        }
        std::string actual_sha = *sha_res.value;
        std::string expected_lower = task.expected_sha256;
        for (char& c : expected_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (actual_sha != expected_lower) {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_STORAGE_PART_CORRUPTED";
            task.error_message = "SHA-256 mismatch: expected " + expected_lower + ", got " + actual_sha;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err("ERR_STORAGE_PART_CORRUPTED", task.error_message, false, true);
        }
    }

    auto commit_res = PathSanitizer::CommitPartFile(part_path, task.save_dir, task.file_name, task.conflict_policy, task.total_bytes);
    if (!commit_res.ok()) {
        task.state = TaskState::kFailed;
        task.error_code = commit_res.error->code;
        task.error_message = commit_res.error->message;
        task.status_detail = task.error_message;
        return commit_res;
    }

    std::filesystem::path final_path = *commit_res.value;
    task.file_name = final_path.filename().string();
    task.part_path.clear();
    task.state = TaskState::kCompleted;
    task.status_detail = "Completed";
    task.completed_at_ms = NowEpochMs();
    task.auth_headers.clear();
    EmitHttpPhaseTrace("finalization_end", engine_start_tp);
    return Result<std::filesystem::path>::Ok(final_path);
}

} // namespace openidm::brisk
