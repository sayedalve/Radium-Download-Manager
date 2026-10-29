// SPDX-License-Identifier: Apache-2.0
// OpenIDM — Brisk HTTP Segmented Download Engine
// Direct integration of Brisk's HttpDownloadEngine & HttpDownloadConnection mechanisms:
//   - Dynamic Connection Spawning (1 -> max_connections)
//   - Dynamic Connection Reuse (Work-Stealing Byte-Range Split)
//   - Automatic Stall Detection & Connection Reset
//   - Single-Stream Fallback for Non-Range Servers
//   - Token-Bucket Bandwidth Limiting
#pragma once

#include <atomic>
#include <filesystem>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "brisk_engine/segment_scheduler.hpp"
#include "brisk_engine/http_client.hpp"
#include "common/types.hpp"

namespace openidm::brisk {

struct EngineProgress {
    std::string task_id;
    TaskState state{TaskState::kDownloading};
    uint64_t downloaded_bytes{0};
    uint64_t total_bytes{0};
    uint64_t speed_bps{0};
    bool range_supported{false};
    std::string resolved_filename;
    std::string etag;
    std::string last_modified;
    std::vector<Segment> segments;
};

struct EngineConfig {
    int stall_timeout_ms{3000};       // 3s default for fast stall reset; configurable up to 15000ms
    int max_retries_per_worker{6};
    uint64_t min_split_bytes{131072}; // 128 KiB minimum split remainder
    bool allow_loopback_redirect{false};
    HttpClient::HttpNetworkOptions net_opts{};
};

class TokenBucketLimiter {
public:
    void Consume(size_t bytes, std::atomic<uint64_t>& limit_bps, std::atomic<bool>& stop_flag) {
        uint64_t rate = limit_bps.load(std::memory_order_relaxed);
        if (rate == 0 || bytes == 0) return;

        std::unique_lock<std::mutex> lock(m_mu);
        auto now = std::chrono::steady_clock::now();
        if (m_last_time.time_since_epoch().count() == 0) {
            m_last_time = now;
        }
        double elapsed_sec = std::chrono::duration<double>(now - m_last_time).count();
        m_last_time = now;
        m_tokens = std::min<double>(static_cast<double>(rate), m_tokens + elapsed_sec * static_cast<double>(rate));

        m_tokens -= static_cast<double>(bytes);
        if (m_tokens < 0.0) {
            double wait_sec = (-m_tokens) / static_cast<double>(rate);
            lock.unlock();
            auto sleep_ms = static_cast<int>(wait_sec * 1000.0);
            for (int slept = 0; slept < sleep_ms && !stop_flag.load(); slept += 10) {
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min(10, sleep_ms - slept)));
            }
        }
    }

private:
    std::mutex m_mu;
    std::chrono::steady_clock::time_point m_last_time{};
    double m_tokens{0.0};
};

class HttpDownloadEngine {
public:
    explicit HttpDownloadEngine(EngineConfig cfg = {});
    ~HttpDownloadEngine();

    using ProgressCallback = std::function<void(const EngineProgress&)>;

    // Executes a synchronous or background download for TaskModel, updating task in place
    // and firing on_progress periodically. Dual token-bucket rate limiting enforces both
    // per-task bandwidth limits and aggregate global bandwidth limits simultaneously.
    Result<std::filesystem::path> ExecuteDownload(
        TaskModel& task,
        std::atomic<bool>& pause_requested,
        std::atomic<bool>& cancel_requested,
        std::atomic<uint64_t>& task_speed_limit_bps,
        ProgressCallback on_progress,
        std::shared_ptr<TokenBucketLimiter> global_limiter = nullptr,
        std::atomic<uint64_t>* global_speed_limit_bps = nullptr);

private:
    EngineConfig m_cfg;
};

} // namespace openidm::brisk
