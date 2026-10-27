// SPDX-License-Identifier: GPL-3.0-or-later
#include "brisk_engine/segment_scheduler.hpp"
#include "common/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace openidm::brisk {

namespace {

uint64_t SchedulerElapsedUs(std::chrono::steady_clock::time_point start,
                            std::chrono::steady_clock::time_point end) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

class SchedulerTimedLock {
public:
    SchedulerTimedLock(std::mutex& mutex, const char* lock_name, const char* operation)
        : m_lock(mutex, std::defer_lock), m_lock_name(lock_name), m_operation(operation),
          m_enabled(std::getenv("RADIUM_HTTP_RANGE_TRACE") != nullptr) {
        const auto wait_start = m_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        m_lock.lock();
        if (m_enabled) {
            m_acquired = std::chrono::steady_clock::now();
            m_wait_us = SchedulerElapsedUs(wait_start, m_acquired);
        }
    }

    ~SchedulerTimedLock() {
        if (!m_enabled) return;
        const auto released = std::chrono::steady_clock::now();
        const uint64_t hold_us = SchedulerElapsedUs(m_acquired, released);
        m_lock.unlock();
        const auto steady_tick_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        OPENIDM_LOG_INFO("HttpRangeTrace", "HTTP_RUNTIME_TRACE {\"event\":\"lock_timing\",\"lock\":\"" +
            std::string(m_lock_name) + "\",\"operation\":\"" + m_operation +
            "\",\"wait_us\":" + std::to_string(m_wait_us) + ",\"hold_us\":" +
            std::to_string(hold_us) + ",\"steady_tick_us\":" + std::to_string(steady_tick_us) + "}");
    }

    SchedulerTimedLock(const SchedulerTimedLock&) = delete;
    SchedulerTimedLock& operator=(const SchedulerTimedLock&) = delete;

private:
    std::unique_lock<std::mutex> m_lock;
    const char* m_lock_name;
    const char* m_operation;
    bool m_enabled{false};
    uint64_t m_wait_us{0};
    std::chrono::steady_clock::time_point m_acquired{};
};

} // namespace

SegmentScheduler::SegmentScheduler(uint64_t total_bytes, uint32_t max_connections, uint64_t min_split_bytes)
    : m_total_bytes(total_bytes),
      m_max_connections(std::clamp(max_connections, 1u, 32u)),
      m_min_split_bytes(std::max<uint64_t>(4096, min_split_bytes)) {}

bool SegmentScheduler::ValidateSegmentPartition(const std::vector<Segment>& segments, uint64_t total_bytes) {
    if (segments.empty()) return false;
    if (total_bytes == 0) return false;

    // Check for duplicate segment IDs
    std::vector<uint32_t> ids;
    ids.reserve(segments.size());
    for (const auto& s : segments) ids.push_back(s.segment_id);
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        return false;
    }

    // Sort a copy by start_offset
    std::vector<Segment> sorted = segments;
    std::sort(sorted.begin(), sorted.end(), [](const Segment& a, const Segment& b) {
        return a.start_offset < b.start_offset;
    });

    if (sorted.front().start_offset != 0) {
        return false; // Does not start at byte 0
    }
    if (sorted.back().end_offset != total_bytes - 1) {
        return false; // Does not end at total_bytes - 1
    }

    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto& s = sorted[i];
        if (s.start_offset > s.end_offset) return false;
        if (s.end_offset >= total_bytes) return false;

        // Cursor bounds: start_offset <= current_offset <= end_offset + 1
        if (s.current_offset < s.start_offset || s.current_offset > (s.end_offset + 1)) {
            return false;
        }

        // Completion flag consistency
        bool is_fully_read = (s.current_offset == s.end_offset + 1);
        if (s.completed && !is_fully_read) {
            return false; // Marked completed prematurely without full bytes
        }

        if (i > 0) {
            // Guard arithmetic against integer overflow
            if (sorted[i - 1].end_offset == UINT64_MAX) return false;
            // No gaps and no overlaps: start must be exactly end_offset + 1
            if (s.start_offset != sorted[i - 1].end_offset + 1) {
                return false;
            }
        }
    }

    return true;
}

void SegmentScheduler::Initialize(const std::vector<Segment>& existing_segments) {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "Initialize");
    m_segments.clear();
    m_next_segment_id = 0;

    if (!existing_segments.empty() && ValidateSegmentPartition(existing_segments, m_total_bytes)) {
        for (const auto& s : existing_segments) {
            // XDM normalizeRestoredChunks: a crash or pause right after writing the last byte
            // of a chunk persists current_offset == end_offset + 1 but completed == false.
            // Normalize it to completed = true so resume never leaves it in an incomplete state.
            bool is_complete = s.completed || (s.current_offset > s.end_offset);
            auto live = std::make_shared<LiveSegment>(
                s.segment_id, s.start_offset, s.current_offset, s.end_offset, is_complete);
            m_segments.push_back(live);
            if (s.segment_id >= m_next_segment_id) {
                m_next_segment_id = s.segment_id + 1;
            }
        }
        return;
    }

    // Brisk Dynamic Connection Spawn: start with a single segment covering [0, total_bytes - 1]
    uint64_t end = (m_total_bytes > 0) ? (m_total_bytes - 1) : 0;
    m_segments.push_back(std::make_shared<LiveSegment>(m_next_segment_id++, 0, 0, end, false));
}

void SegmentScheduler::InitializeEquipartition(uint32_t initial_connections) {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "InitializeEquipartition");
    m_segments.clear();
    m_next_segment_id = 0;

    if (m_total_bytes == 0) {
        m_segments.push_back(std::make_shared<LiveSegment>(m_next_segment_id++, 0, 0, 0, false));
        return;
    }

    uint32_t conns = std::clamp(initial_connections, 1u, m_max_connections);
    // Ensure each partition is at least min_split_bytes (or total_bytes if smaller)
    if (m_min_split_bytes > 0 && conns > 1) {
        while (conns > 1 && (m_total_bytes / conns) < m_min_split_bytes) {
            --conns;
        }
    }

    uint64_t base_chunk = m_total_bytes / conns;
    uint64_t remainder = m_total_bytes % conns;
    uint64_t current_offset = 0;

    for (uint32_t i = 0; i < conns; ++i) {
        uint64_t chunk_len = base_chunk + (i < remainder ? 1ULL : 0ULL);
        uint64_t seg_start = current_offset;
        uint64_t seg_end = (i == conns - 1) ? (m_total_bytes - 1) : (seg_start + chunk_len - 1);
        current_offset = seg_end + 1;

        m_segments.push_back(std::make_shared<LiveSegment>(
            m_next_segment_id++, seg_start, seg_start, seg_end, false));
    }
}

std::shared_ptr<LiveSegment> SegmentScheduler::AcquireNextOrStealSegment(bool allow_split) {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "AcquireNextOrStealSegment");

    // 1. First check if there is any existing incomplete segment with no active worker (e.g. on start or resume)
    for (auto& seg : m_segments) {
        if (!seg->completed.load() && !seg->active_worker.load()) {
            uint64_t curr = seg->current_offset.load();
            uint64_t end = seg->end_offset.load();
            if (curr <= end) {
                seg->active_worker.store(true);
                return seg;
            } else {
                seg->completed.store(true);
            }
        }
    }

    if (!allow_split || m_total_bytes == 0) {
        return nullptr;
    }

    // Count active workers
    uint32_t active_count = 0;
    for (const auto& seg : m_segments) {
        if (seg->active_worker.load() && !seg->completed.load()) {
            ++active_count;
        }
    }
    if (active_count >= m_max_connections) {
        return nullptr;
    }

    // 2. Brisk Work-Stealing / Dynamic Connection Reuse:
    //    Find the active segment with the largest remaining byte range >= 2 * min_separation.
    //    Prefer segments that do NOT have a pending split in flight.
    //    Only if no non-pending segment can be bisected, fall back to a pending segment
    //    that holds a massive remaining backlog (>= 8 * min_separation).
    std::shared_ptr<LiveSegment> best_victim = nullptr;
    uint64_t max_remaining = 0;

    // Adaptive Range Splitting:
    // When initial concurrency is ramping up (active_count < m_max_connections), allow fine-grained
    // bisection (>= 256 KiB) so small or fast downloads scale out to all configured connections instantly.
    // Once all connections have ramped up (active_count == m_max_connections), subsequent work-stealing
    // requires a coarser split threshold (>= 1 MiB) to prevent late-stage micro-splits, connection churn,
    // and cancellation slippage near completion.
    uint64_t floor_bytes = (active_count + 1 < m_max_connections) ? 262144ULL : 1048576ULL;
    uint64_t min_separation = (std::max<uint64_t>)(m_min_split_bytes, floor_bytes);

    // Pass 1: candidate segments without split_pending
    for (auto& seg : m_segments) {
        if (seg->completed.load() || seg->split_pending.load()) continue;
        uint64_t curr = seg->current_offset.load();
        uint64_t end = seg->end_offset.load();
        if (end > curr) {
            uint64_t rem = end - curr + 1;
            if (rem >= 2 * min_separation && rem > max_remaining) {
                max_remaining = rem;
                best_victim = seg;
            }
        }
    }

    // Pass 2: fall back to segments with split_pending.
    // During initial concurrency ramp-up (active_count + 1 < m_max_connections), allow bisecting
    // an in-flight pending segment if its remaining backlog is larger than any un-pending candidate
    // (rem >= 2 * min_separation and rem > max_remaining) so concurrency scales across a balanced
    // bisection tree rather than a severely skewed tail-splitting cascade.
    // During steady-state (all connections already spawned), require a massive backlog (>= 8 * min_separation)
    // to strictly prevent repeated resplits and connection churn near completion.
    const uint64_t pending_threshold = (active_count + 1 < m_max_connections)
        ? (2 * min_separation)
        : (8 * min_separation);

    for (auto& seg : m_segments) {
        if (seg->completed.load()) continue;
        if (!seg->split_pending.load()) continue; // already evaluated in Pass 1
        uint64_t curr = seg->current_offset.load();
        uint64_t end = seg->end_offset.load();
        if (end > curr) {
            uint64_t rem = end - curr + 1;
            if (rem >= pending_threshold && rem > max_remaining) {
                max_remaining = rem;
                best_victim = seg;
            }
        }
    }

    if (!best_victim) {
        return nullptr;
    }

    SchedulerTimedLock write_lock(best_victim->write_mu, "segment_write", "split");
    uint64_t curr = best_victim->current_offset.load();
    uint64_t old_end = best_victim->end_offset.load();
    if (old_end <= curr) {
        return nullptr;
    }
    uint64_t rem = old_end - curr + 1;
    if (rem < 2 * min_separation) {
        return nullptr;
    }

    uint64_t raw_mid = curr + (rem / 2);
    // Align split point to 4096-byte page boundary if possible
    uint64_t split_start = (raw_mid + 4095ULL) & ~4095ULL;
    if (split_start <= curr + min_separation || split_start + min_separation > old_end + 1) {
        split_start = raw_mid;
    }
    if (split_start <= curr + min_separation || split_start >= old_end) {
        return nullptr;
    }

    // Atomically shrink victim's end_offset to split_start - 1 while holding write_mu
    best_victim->end_offset.store(split_start - 1);

    // Double check that victim has not concurrently advanced past split_start
    if (best_victim->current_offset.load() >= split_start) {
        best_victim->end_offset.store(old_end);
        return nullptr;
    }

    // Mark split_pending on the victim so it does not suffer repeated split collisions
    best_victim->split_pending.store(true);

    auto stolen = std::make_shared<LiveSegment>(
        m_next_segment_id++, split_start, split_start, old_end, false);
    stolen->active_worker.store(true);
    m_segments.push_back(stolen);
    if (std::getenv("RADIUM_HTTP_RANGE_TRACE") != nullptr) {
        const auto event_tick_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        OPENIDM_LOG_INFO("HttpRangeTrace", "HTTP_RUNTIME_TRACE {\"event\":\"range_split\",\"parent_segment\":" +
            std::to_string(best_victim->segment_id) + ",\"child_segment\":" + std::to_string(stolen->segment_id) +
            ",\"parent_start\":" + std::to_string(curr) + ",\"parent_old_end\":" + std::to_string(old_end) +
            ",\"parent_new_end\":" + std::to_string(split_start - 1) + ",\"child_start\":" +
            std::to_string(split_start) + ",\"child_end\":" + std::to_string(old_end) +
            ",\"steady_tick_us\":" + std::to_string(event_tick_us) + "}");
    }
    return stolen;
}

bool SegmentScheduler::RollbackUnstartedStolenSegment(uint32_t segment_id) {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "RollbackUnstartedStolenSegment");
    auto it = std::find_if(m_segments.begin(), m_segments.end(), [&](const std::shared_ptr<LiveSegment>& s) {
        return s->segment_id == segment_id;
    });
    if (it == m_segments.end()) return false;

    auto victim = *it;
    uint64_t start = victim->start_offset;
    uint64_t curr = victim->current_offset.load();
    uint64_t end = victim->end_offset.load();

    // Can only merge back into predecessor if no bytes were written in this stolen segment
    if (curr != start || start == 0) {
        victim->active_worker.store(false);
        return false;
    }

    for (auto& pred : m_segments) {
        if (pred->segment_id != segment_id && pred->end_offset.load() + 1 == start && !pred->completed.load()) {
            SchedulerTimedLock pred_lock(pred->write_mu, "segment_write", "rollback");
            pred->end_offset.store(end);
            pred->split_pending.store(false);
            m_segments.erase(it);
            return true;
        }
    }
    victim->active_worker.store(false);
    return false;
}

std::vector<Segment> SegmentScheduler::GetSnapshots() const {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "GetSnapshots");
    std::vector<Segment> out;
    out.reserve(m_segments.size());
    for (const auto& s : m_segments) {
        out.push_back(s->Snapshot());
    }
    std::sort(out.begin(), out.end(), [](const Segment& a, const Segment& b) {
        return a.start_offset < b.start_offset;
    });
    return out;
}

uint64_t SegmentScheduler::TotalDownloadedBytes() const {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "TotalDownloadedBytes");
    uint64_t sum = 0;
    for (const auto& s : m_segments) {
        uint64_t start = s->start_offset;
        uint64_t curr = s->current_offset.load(std::memory_order_relaxed);
        uint64_t end = s->end_offset.load(std::memory_order_relaxed);
        if (s->completed.load(std::memory_order_relaxed) || curr > end) {
            if (end >= start) sum += (end - start + 1);
        } else if (curr > start) {
            sum += (curr - start);
        }
    }
    return sum;
}

bool SegmentScheduler::AllCompleted() const {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "AllCompleted");
    if (m_segments.empty()) return false;
    for (const auto& s : m_segments) {
        if (!s->completed.load(std::memory_order_relaxed)) return false;
    }
    return true;
}

size_t SegmentScheduler::ActiveWorkerCount() const {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "ActiveWorkerCount");
    size_t count = 0;
    for (const auto& s : m_segments) {
        if (s->active_worker.load(std::memory_order_relaxed) && !s->completed.load(std::memory_order_relaxed)) {
            ++count;
        }
    }
    return count;
}

bool SegmentScheduler::HasIncompleteUnassignedSegment() const {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "HasIncompleteUnassignedSegment");
    for (const auto& seg : m_segments) {
        if (!seg->completed.load() && !seg->active_worker.load()) {
            if (seg->current_offset.load() <= seg->end_offset.load()) {
                return true;
            }
        }
    }
    return false;
}

bool SegmentScheduler::CanSplitAnySegment() const {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "CanSplitAnySegment");
    uint64_t min_separation = (std::max<uint64_t>)(m_min_split_bytes, 262144ULL);
    // Pass 1: check for non-pending candidates
    for (const auto& s : m_segments) {
        if (s->completed.load(std::memory_order_relaxed) || s->split_pending.load(std::memory_order_relaxed)) continue;
        uint64_t curr = s->current_offset.load(std::memory_order_relaxed);
        uint64_t end = s->end_offset.load(std::memory_order_relaxed);
        if (end > curr) {
            uint64_t rem = end - curr + 1;
            if (rem >= 2 * min_separation) {
                return true;
            }
        }
    }
    // Pass 2: check for large pending candidates
    for (const auto& s : m_segments) {
        if (s->completed.load(std::memory_order_relaxed)) continue;
        uint64_t curr = s->current_offset.load(std::memory_order_relaxed);
        uint64_t end = s->end_offset.load(std::memory_order_relaxed);
        if (end > curr) {
            uint64_t rem = end - curr + 1;
            if (rem >= 8 * min_separation) {
                return true;
            }
        }
    }
    return false;
}

uint32_t SegmentScheduler::GetMaxConnections() const {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "GetMaxConnections");
    return m_max_connections;
}

void SegmentScheduler::SetMaxConnections(uint32_t conns) {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "SetMaxConnections");
    m_max_connections = std::clamp(conns, 1u, 32u);
}

void SegmentScheduler::ReduceConnectionsByHalf() {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "ReduceConnectionsByHalf");
    m_max_connections = std::max(1u, m_max_connections / 2u);
    m_consecutive_successes = 0;
}

void SegmentScheduler::ReduceConnectionsStepped() {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "ReduceConnectionsStepped");
    // Conservative multi-tier backdown: 32 -> 16 -> 8 -> 4 -> 3 -> 2 -> 1
    // Rather than immediately halving from 3 to 1 or 4 to 2 to 1, gracefully descend
    if (m_max_connections > 8) {
        m_max_connections /= 2u;
    } else if (m_max_connections > 3) {
        m_max_connections -= 1u;
    } else if (m_max_connections > 1) {
        m_max_connections -= 1u;
    }
    m_consecutive_successes = 0;
}

void SegmentScheduler::TryRampUpConnections(uint32_t ceiling_conns) {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "TryRampUpConnections");
    if (m_max_connections < ceiling_conns) {
        ++m_max_connections;
    }
    m_consecutive_successes = 0;
}

void SegmentScheduler::ResetConsecutiveFailures() {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "ResetConsecutiveFailures");
    m_consecutive_failures = 0;
}

void SegmentScheduler::RecordSuccess() {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "RecordSuccess");
    m_consecutive_failures = 0;
    ++m_consecutive_successes;
}

uint32_t SegmentScheduler::IncrementConsecutiveFailures() {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "IncrementConsecutiveFailures");
    return ++m_consecutive_failures;
}

uint32_t SegmentScheduler::GetConsecutiveFailures() const {
    SchedulerTimedLock lock(m_mutex, "scheduler_state", "GetConsecutiveFailures");
    return m_consecutive_failures;
}

} // namespace openidm::brisk
