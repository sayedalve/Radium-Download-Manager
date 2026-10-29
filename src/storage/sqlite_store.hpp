// SPDX-License-Identifier: Apache-2.0
// OpenIDM — Persistent SQLite 3 Storage Layer (WAL Mode + DPAPI/AES Secret Protection)
// Implements DATA_MODEL_AND_STORAGE.md, ADR-0007, NFR-005, NFR-009.
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "common/types.hpp"

struct sqlite3;

namespace openidm {

class SqliteStore {
public:
    SqliteStore() = default;
    ~SqliteStore();

    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;

    Result<void> Open(const std::filesystem::path& db_path);
    void Close();

    Result<void> UpsertTask(const TaskModel& task);
    Result<void> UpdateTaskProgressAndSegments(
        const std::string& task_id,
        TaskState state,
        uint64_t downloaded_bytes,
        uint64_t total_bytes,
        bool range_supported,
        const std::vector<Segment>& segments,
        const std::string& error_code = "",
        const std::string& error_message = "");

    Result<std::optional<TaskModel>> GetTask(const std::string& task_id);
    Result<std::vector<TaskModel>> ListTasks();
    Result<void> DeleteTask(const std::string& task_id);

    // Crash recovery protocol: transitions stale DOWNLOADING/PROBING/MUXING tasks to PAUSED
    // and rolls back incomplete segment current_offset by 64 KiB safety margin.
    Result<int> RecoverInterruptedTasks(uint64_t rollback_margin_bytes = 65536);

    Result<void> SetSetting(const std::string& key, const std::string& value_json);
    Result<std::string> GetSetting(const std::string& key, const std::string& default_json = "{}");

    Result<void> UpsertQueue(const QueueConfig& queue);
    Result<std::vector<QueueConfig>> ListQueues();

private:
    Result<void> ExecSql(const std::string& sql);
    Result<std::vector<Segment>> LoadSegmentsLocked(const std::string& task_id);
    Result<void> SaveSegmentsLocked(const std::string& task_id, const std::vector<Segment>& segments);

    mutable std::mutex m_mutex;
    sqlite3* m_db{nullptr};
};

} // namespace openidm
