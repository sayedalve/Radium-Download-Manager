// SPDX-License-Identifier: GPL-3.0-or-later
#include "brisk_engine/segment_scheduler.hpp"

#include <algorithm>

namespace openidm::brisk {

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
    std::lock_guard<std::mutex> lock(m_mutex);
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

std::shared_ptr<LiveSegment> SegmentScheduler::AcquireNextOrStealSegment(bool allow_split) {
    std::lock_guard<std::mutex> lock(m_mutex);

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
    //    Find the active segment with the largest remaining byte range >= 2 * min_separation
    std::shared_ptr<LiveSegment> best_victim = nullptr;
    uint64_t max_remaining = 0;
    uint64_t min_separation = (std::max<uint64_t>)(m_min_split_bytes, 262144ULL);

    for (auto& seg : m_segments) {
        if (seg->completed.load()) continue;
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

    if (!best_victim) {
        return nullptr;
    }

    std::lock_guard<std::mutex> write_lock(best_victim->write_mu);
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

    auto stolen = std::make_shared<LiveSegment>(
        m_next_segment_id++, split_start, split_start, old_end, false);
    stolen->active_worker.store(true);
    m_segments.push_back(stolen);
    return stolen;
}

bool SegmentScheduler::RollbackUnstartedStolenSegment(uint32_t segment_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
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
            std::lock_guard<std::mutex> pred_lock(pred->write_mu);
            pred->end_offset.store(end);
            m_segments.erase(it);
            return true;
        }
    }
    victim->active_worker.store(false);
    return false;
}

std::vector<Segment> SegmentScheduler::GetSnapshots() const {
    std::lock_guard<std::mutex> lock(m_mutex);
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
    std::lock_guard<std::mutex> lock(m_mutex);
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
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_segments.empty()) return false;
    for (const auto& s : m_segments) {
        if (!s->completed.load(std::memory_order_relaxed)) return false;
    }
    return true;
}

size_t SegmentScheduler::ActiveWorkerCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& s : m_segments) {
        if (s->active_worker.load(std::memory_order_relaxed) && !s->completed.load(std::memory_order_relaxed)) {
            ++count;
        }
    }
    return count;
}

bool SegmentScheduler::HasIncompleteUnassignedSegment() const {
    std::lock_guard<std::mutex> lock(m_mutex);
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
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t min_separation = (std::max<uint64_t>)(m_min_split_bytes, 262144ULL);
    for (const auto& s : m_segments) {
        if (s->completed.load(std::memory_order_relaxed)) continue;
        uint64_t curr = s->current_offset.load(std::memory_order_relaxed);
        uint64_t end = s->end_offset.load(std::memory_order_relaxed);
        if (end > curr && (end - curr + 1) >= 2 * min_separation) {
            return true;
        }
    }
    return false;
}

uint32_t SegmentScheduler::GetMaxConnections() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_max_connections;
}

void SegmentScheduler::SetMaxConnections(uint32_t conns) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_max_connections = std::clamp(conns, 1u, 32u);
}

void SegmentScheduler::ReduceConnectionsByHalf() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_max_connections = std::max(1u, m_max_connections / 2u);
    m_consecutive_successes = 0;
}

void SegmentScheduler::ReduceConnectionsStepped() {
    std::lock_guard<std::mutex> lock(m_mutex);
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
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_max_connections < ceiling_conns) {
        ++m_max_connections;
    }
    m_consecutive_successes = 0;
}

void SegmentScheduler::ResetConsecutiveFailures() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consecutive_failures = 0;
}

void SegmentScheduler::RecordSuccess() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consecutive_failures = 0;
    ++m_consecutive_successes;
}

uint32_t SegmentScheduler::IncrementConsecutiveFailures() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return ++m_consecutive_failures;
}

uint32_t SegmentScheduler::GetConsecutiveFailures() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_consecutive_failures;
}

} // namespace openidm::brisk
