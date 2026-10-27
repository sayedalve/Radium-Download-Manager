// SPDX-License-Identifier: GPL-3.0-or-later
// OpenIDM — Brisk Dynamic Connection Spawning & Work-Stealing Range Splitter
// Adapted from BrisklyDev/brisk:
//   - lib/download_engine/engine/http_download_engine.dart
//   - lib/download_engine/segment/segment.dart
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "common/types.hpp"

namespace openidm::brisk {

struct LiveSegment {
    uint32_t segment_id{0};
    uint64_t start_offset{0};
    std::atomic<uint64_t> current_offset{0};
    std::atomic<uint64_t> end_offset{0};
    std::atomic<bool> completed{false};
    std::atomic<bool> active_worker{false};
    // True while the current HTTP request still uses the pre-split end offset.
    // Prevents repeatedly shrinking one in-flight response before it observes
    // the first split and exits.
    std::atomic<bool> split_pending{false};
    mutable std::mutex write_mu;

    LiveSegment(uint32_t id, uint64_t start, uint64_t curr, uint64_t end, bool comp = false)
        : segment_id(id), start_offset(start), current_offset(curr), end_offset(end), completed(comp) {}

    Segment Snapshot() const {
        return Segment{
            segment_id,
            start_offset,
            current_offset.load(std::memory_order_relaxed),
            end_offset.load(std::memory_order_relaxed),
            completed.load(std::memory_order_relaxed)
        };
    }
};

class SegmentScheduler {
public:
    SegmentScheduler(uint64_t total_bytes, uint32_t max_connections, uint64_t min_split_bytes = 262144);

    // Initialize either from fresh start (1 initial segment [0, total_bytes-1]) or saved resume segments
    void Initialize(const std::vector<Segment>& existing_segments = {});

    // Pre-calculates balanced contiguous, non-overlapping partitions across initial_connections before workers launch
    void InitializeEquipartition(uint32_t initial_connections);

    // Validates that existing_segments forms a complete, non-overlapping partition of [0, total_bytes - 1]
    // with valid cursors, no gaps, no overlaps, and consistent completion flags (FDM-002, TG-01).
    static bool ValidateSegmentPartition(const std::vector<Segment>& segments, uint64_t total_bytes);

    // Claims an existing unassigned incomplete segment, OR dynamically splits the largest active segment
    // (Brisk's Dynamic Connection Spawn & Dynamic Connection Reuse / Work-Stealing).
    std::shared_ptr<LiveSegment> AcquireNextOrStealSegment(bool allow_split);

    // If a secondary worker's Range request is rejected by the server (e.g. 200 OK instead of 206)
    // before any bytes are written, merges the stolen range back into the preceding contiguous segment
    // so the primary worker can complete the transfer without leaving an orphaned segment (BUG-008).
    bool RollbackUnstartedStolenSegment(uint32_t segment_id);

    std::vector<Segment> GetSnapshots() const;
    uint64_t TotalDownloadedBytes() const;
    bool AllCompleted() const;
    size_t ActiveWorkerCount() const;
    bool CanSplitAnySegment() const;
    bool HasIncompleteUnassignedSegment() const;

    // Concurrency and rate limiting management
    uint32_t GetMaxConnections() const;
    void SetMaxConnections(uint32_t conns);
    void ReduceConnectionsByHalf();
    void ReduceConnectionsStepped();
    void TryRampUpConnections(uint32_t ceiling_conns);
    void ResetConsecutiveFailures();
    void RecordSuccess();
    uint32_t IncrementConsecutiveFailures();
    uint32_t GetConsecutiveFailures() const;

private:
    mutable std::mutex m_mutex;
    uint64_t m_total_bytes{0};
    uint32_t m_max_connections{8};
    uint64_t m_min_split_bytes{262144}; // Default 256 KiB min split remainder
    uint32_t m_next_segment_id{0};
    uint32_t m_consecutive_failures{0};
    uint32_t m_consecutive_successes{0};
    std::vector<std::shared_ptr<LiveSegment>> m_segments;
};

} // namespace openidm::brisk
