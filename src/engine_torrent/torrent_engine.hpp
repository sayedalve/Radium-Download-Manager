// SPDX-License-Identifier: GPL-3.0-or-later
// OpenIDM / Flux — BitTorrent Engine Coordinator
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "brisk_engine/http_download_engine.hpp"
#include "common/types.hpp"
#include "engine_torrent/torrent_parser.hpp"

namespace openidm::torrent {

struct TorrentProgress {
    std::string task_id;
    TaskState state{TaskState::kDownloading};
    uint64_t downloaded_bytes{0};
    // Includes proportionally weighted blocks in pieces that have not passed
    // their hash check yet. This is transient UI progress, not resume state.
    uint64_t received_bytes{0};
    uint64_t total_bytes{0};
    uint64_t speed_bps{0};
    uint64_t uploaded_bytes{0};
    uint64_t upload_speed_bps{0};
    uint32_t completed_pieces{0};
    uint32_t total_pieces{0};
    std::string status_detail;
};

using TorrentProgressCallback = std::function<void(const TorrentProgress&)>;
// Invoked after BEP 9 metadata has been decoded and before storage is allocated.
// The callback may update TorrentInfo::files[].selected and can reject the task.
using TorrentMetadataSelectionCallback = std::function<Result<void>(TorrentInfo&)>;

class TorrentEngine {
public:
    TorrentEngine() = default;
    ~TorrentEngine() = default;

    static Result<void> ExecuteTorrentDownload(
        TaskModel& task,
        TorrentInfo& torrent_info,
        std::atomic<bool>& pause_requested,
        std::atomic<bool>& cancel_requested,
        std::atomic<uint64_t>& speed_limit_bps,
        TorrentProgressCallback on_progress,
        std::shared_ptr<brisk::TokenBucketLimiter> global_limiter = nullptr,
        std::atomic<uint64_t>* global_speed_limit_bps = nullptr,
        uint32_t max_peers = 32,
        bool seed_after_completion = false,
        double max_seed_ratio = 1.0,
        uint32_t max_seed_time_sec = 0,
        TorrentMetadataSelectionCallback on_metadata = nullptr);
};

} // namespace openidm::torrent
