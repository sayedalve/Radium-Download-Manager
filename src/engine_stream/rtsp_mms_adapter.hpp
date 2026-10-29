// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — RTSP, MMS & Live Media Stream Adapter (FFmpeg Out-of-Process Worker)
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "common/types.hpp"

namespace openidm::stream {

struct RtspProgress {
    std::string task_id;
    TaskState state{TaskState::kDownloading};
    uint64_t downloaded_bytes{0};
    double duration_sec{0.0};
    uint64_t speed_bps{0};
    std::string status_detail;
};

using RtspProgressCallback = std::function<void(const RtspProgress&)>;

class RtspMmsAdapter {
public:
    explicit RtspMmsAdapter(std::filesystem::path ffmpeg_path, bool allow_loopback = false);
    ~RtspMmsAdapter() = default;

    static bool IsStreamUrl(std::string_view url);
    static bool ValidateStreamUrl(std::string_view url);

    Result<std::filesystem::path> ExecuteStreamCapture(
        TaskModel& task,
        std::atomic<bool>& pause_requested,
        std::atomic<bool>& cancel_requested,
        RtspProgressCallback on_progress);

private:
    std::filesystem::path m_ffmpeg_path;
    bool m_allow_loopback{false};
};

} // namespace openidm::stream
