// SPDX-License-Identifier: Apache-2.0
#include "storage/sqlite_store.hpp"

#include "common/crypto_utils.hpp"
#include "common/json_mini.hpp"
#include "common/logger.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "third_party/sqlite3/sqlite3.h"

namespace openidm {

namespace {
std::string ColText(sqlite3_stmt* stmt, int col) {
    const unsigned char* p = sqlite3_column_text(stmt, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}
} // namespace

SqliteStore::~SqliteStore() {
    Close();
}

Result<void> SqliteStore::Open(const std::filesystem::path& db_path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
    std::error_code ec;
    if (db_path.has_parent_path()) {
        std::filesystem::create_directories(db_path.parent_path(), ec);
    }
    if (sqlite3_open(db_path.string().c_str(), &m_db) != SQLITE_OK) {
        std::string err = m_db ? sqlite3_errmsg(m_db) : "sqlite3_open failed";
        if (m_db) { sqlite3_close(m_db); m_db = nullptr; }
        return Result<void>::Err("ERR_SQLITE_OPEN", err);
    }

    const char* kSchemaSql = R"SQL(
        
        PRAGMA synchronous = NORMAL;
        PRAGMA foreign_keys = ON;
        PRAGMA busy_timeout = 5000;
        PRAGMA user_version = 1;

        CREATE TABLE IF NOT EXISTS queues (
            queue_id                TEXT PRIMARY KEY,
            name                    TEXT NOT NULL,
            max_concurrent_tasks    INTEGER NOT NULL DEFAULT 3,
            speed_limit_bps         INTEGER NOT NULL DEFAULT 0,
            schedule_enabled        INTEGER NOT NULL DEFAULT 0,
            start_time_local        TEXT,
            stop_time_local         TEXT,
            post_completion_action  TEXT NOT NULL DEFAULT 'NONE'
        );

        INSERT OR IGNORE INTO queues(queue_id, name, max_concurrent_tasks, speed_limit_bps)
        VALUES ('main', 'Main Download Queue', 3, 0);

        CREATE TABLE IF NOT EXISTS tasks (
            task_id                 TEXT PRIMARY KEY,
            task_type               TEXT NOT NULL,
            state                   TEXT NOT NULL,
            pause_reason            TEXT NOT NULL DEFAULT 'NONE',
            queue_id                TEXT NOT NULL DEFAULT 'main' REFERENCES queues(queue_id),
            queue_order             INTEGER NOT NULL DEFAULT 0,
            category                TEXT NOT NULL DEFAULT 'General',
            url                     TEXT NOT NULL,
            page_url                TEXT,
            file_name               TEXT NOT NULL,
            save_dir                TEXT NOT NULL,
            part_path               TEXT,
            correlation_id          TEXT,
            total_bytes             INTEGER NOT NULL DEFAULT 0,
            downloaded_bytes        INTEGER NOT NULL DEFAULT 0,
            uploaded_bytes          INTEGER NOT NULL DEFAULT 0,
            range_supported         INTEGER NOT NULL DEFAULT 0,
            max_connections         INTEGER NOT NULL DEFAULT 8,
            conflict_policy         TEXT NOT NULL DEFAULT 'auto_rename',
            scheduled               INTEGER NOT NULL DEFAULT 0,
            schedule_time_local     TEXT,
            speed_limit_bps         INTEGER NOT NULL DEFAULT 0,
            etag                    TEXT,
            last_modified           TEXT,
            headers_json            TEXT NOT NULL DEFAULT '{}',
            encrypted_auth_blob     BLOB,
            stream_options_json     TEXT,
            expected_sha256         TEXT,
            error_code              TEXT,
            error_message           TEXT,
            alternate_urls_json     TEXT NOT NULL DEFAULT '[]',
            schedule_stop_time_local TEXT,
            created_at_ms           INTEGER NOT NULL,
            updated_at_ms           INTEGER NOT NULL,
            completed_at_ms         INTEGER
        );

        CREATE INDEX IF NOT EXISTS idx_tasks_state_queue ON tasks(state, queue_id, queue_order);

        CREATE TABLE IF NOT EXISTS task_segments (
            task_id                 TEXT NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,
            segment_id              INTEGER NOT NULL,
            start_offset            INTEGER NOT NULL,
            current_offset          INTEGER NOT NULL,
            end_offset              INTEGER NOT NULL,
            completed               INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (task_id, segment_id)
        );

        CREATE TABLE IF NOT EXISTS settings (
            key                     TEXT PRIMARY KEY,
            value_json              TEXT NOT NULL,
            updated_at_ms           INTEGER NOT NULL
        );
    )SQL";

    if (!ExecSql("PRAGMA journal_mode = WAL;").ok()) { ExecSql("PRAGMA journal_mode = TRUNCATE;"); }
    auto schema_res = ExecSql(kSchemaSql);
    if (!schema_res.ok()) return schema_res;

    // Idempotent migration for databases created in earlier phases/versions
    auto try_add_col = [&](const std::string& sql) {
        char* errmsg = nullptr;
        sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errmsg);
        if (errmsg) sqlite3_free(errmsg);
    };
    try_add_col("ALTER TABLE tasks ADD COLUMN speed_limit_bps INTEGER NOT NULL DEFAULT 0;");
    try_add_col("ALTER TABLE tasks ADD COLUMN alternate_urls_json TEXT NOT NULL DEFAULT '[]';");
    try_add_col("ALTER TABLE tasks ADD COLUMN schedule_stop_time_local TEXT;");
    try_add_col("ALTER TABLE tasks ADD COLUMN pause_reason TEXT NOT NULL DEFAULT 'NONE';");
    try_add_col("ALTER TABLE tasks ADD COLUMN correlation_id TEXT;");
    try_add_col("ALTER TABLE tasks ADD COLUMN uploaded_bytes INTEGER NOT NULL DEFAULT 0;");
    try_add_col("ALTER TABLE tasks ADD COLUMN conflict_policy TEXT NOT NULL DEFAULT 'auto_rename';");
    try_add_col("ALTER TABLE tasks ADD COLUMN scheduled INTEGER NOT NULL DEFAULT 0;");
    try_add_col("ALTER TABLE tasks ADD COLUMN schedule_time_local TEXT;");

    return Result<void>::Ok();
}

void SqliteStore::Close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

Result<void> SqliteStore::ExecSql(const std::string& sql) {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string err = errmsg ? errmsg : "SQLite exec error";
        if (errmsg) sqlite3_free(errmsg);
        return Result<void>::Err("ERR_SQLITE_EXEC", err);
    }
    return Result<void>::Ok();
}

Result<void> SqliteStore::UpsertTask(const TaskModel& task) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return Result<void>::Err("ERR_SQLITE_CLOSED", "Database not open");

    json::Value headers_obj = json::Object{};
    for (const auto& [k, v] : task.headers) headers_obj[k] = v;

    std::vector<uint8_t> encrypted_auth;
    // Wipe credentials immediately if task is Completed or Canceled (NFR-009)
    if (task.state != TaskState::kCompleted && task.state != TaskState::kCanceled && !task.auth_headers.empty()) {
        json::Value auth_obj = json::Object{};
        for (const auto& [k, v] : task.auth_headers) auth_obj[k] = v;
        auto prot = CryptoUtils::ProtectSecret(auth_obj.dump());
        if (prot.ok()) encrypted_auth = *prot.value;
    }

    json::Value stream_obj = json::Object{};
    stream_obj["video_filter"] = task.stream_options.video_filter;
    stream_obj["audio_filter"] = task.stream_options.audio_filter;
    stream_obj["subtitle_filter"] = task.stream_options.subtitle_filter;
    stream_obj["output_format"] = task.stream_options.output_format;
    stream_obj["thread_count"] = task.stream_options.thread_count;
    stream_obj["concurrent_download"] = task.stream_options.concurrent_download;
    stream_obj["custom_hls_key_hex"] = task.stream_options.custom_hls_key_hex;
    stream_obj["cached_manifest_body"] = task.stream_options.cached_manifest_body;
    if (!task.torrent_file_paths.empty()) {
        json::Array selection;
        for (size_t i = 0; i < task.torrent_file_paths.size(); ++i) {
            json::Value entry(json::Object{});
            entry["path"] = task.torrent_file_paths[i];
            entry["selected"] = i < task.torrent_file_selection.size() && task.torrent_file_selection[i] != 0;
            selection.push_back(std::move(entry));
        }
        stream_obj["torrent_selection"] = std::move(selection);
    }

    const char* sql = R"SQL(
        INSERT INTO tasks (
            task_id, task_type, state, pause_reason, queue_id, queue_order, category,
            url, page_url, file_name, save_dir, part_path, correlation_id,
            total_bytes, downloaded_bytes, uploaded_bytes, range_supported, max_connections,
            conflict_policy, scheduled, schedule_time_local, speed_limit_bps,
            etag, last_modified, headers_json, encrypted_auth_blob, stream_options_json,
            expected_sha256, error_code, error_message, alternate_urls_json, schedule_stop_time_local,
            created_at_ms, updated_at_ms, completed_at_ms
        ) VALUES (
            ?, ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?,
            ?, ?, ?, ?,
            ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?,
            ?, ?, ?
        )
        ON CONFLICT(task_id) DO UPDATE SET
            task_type=excluded.task_type,
            state=excluded.state,
            pause_reason=excluded.pause_reason,
            queue_id=excluded.queue_id,
            queue_order=excluded.queue_order,
            category=excluded.category,
            url=excluded.url,
            page_url=excluded.page_url,
            file_name=excluded.file_name,
            save_dir=excluded.save_dir,
            part_path=excluded.part_path,
            correlation_id=excluded.correlation_id,
            total_bytes=excluded.total_bytes,
            downloaded_bytes=excluded.downloaded_bytes,
            uploaded_bytes=excluded.uploaded_bytes,
            range_supported=excluded.range_supported,
            max_connections=excluded.max_connections,
            conflict_policy=excluded.conflict_policy,
            scheduled=excluded.scheduled,
            schedule_time_local=excluded.schedule_time_local,
            speed_limit_bps=excluded.speed_limit_bps,
            etag=excluded.etag,
            last_modified=excluded.last_modified,
            headers_json=excluded.headers_json,
            encrypted_auth_blob=excluded.encrypted_auth_blob,
            stream_options_json=excluded.stream_options_json,
            expected_sha256=excluded.expected_sha256,
            error_code=excluded.error_code,
            error_message=excluded.error_message,
            alternate_urls_json=excluded.alternate_urls_json,
            schedule_stop_time_local=excluded.schedule_stop_time_local,
            updated_at_ms=excluded.updated_at_ms,
            completed_at_ms=excluded.completed_at_ms;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<void>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }

    std::string type_s = TaskTypeToString(task.task_type);
    std::string state_s = TaskStateToString(task.state);
    std::string pause_s = PauseReasonToString(task.pause_reason);
    std::string conflict_s = ConflictPolicyToString(task.conflict_policy);
    std::string headers_s = headers_obj.dump();
    std::string stream_s = stream_obj.dump();
    json::Array alt_arr;
    for (const auto& u : task.alternate_urls) {
        if (!u.empty()) alt_arr.push_back(u);
    }
    std::string alt_s = json::Value(alt_arr).dump();
    int64_t now_ms = NowEpochMs();
    int64_t created_ms = task.created_at_ms > 0 ? task.created_at_ms : now_ms;

    sqlite3_bind_text(stmt, 1, task.task_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, type_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, state_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, pause_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, task.queue_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, task.queue_order);
    sqlite3_bind_text(stmt, 7, task.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, task.url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, task.page_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, task.file_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, task.save_dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, task.part_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, task.correlation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 14, static_cast<int64_t>(task.total_bytes));
    sqlite3_bind_int64(stmt, 15, static_cast<int64_t>(task.downloaded_bytes));
    sqlite3_bind_int64(stmt, 16, static_cast<int64_t>(task.uploaded_bytes));
    sqlite3_bind_int(stmt, 17, task.range_supported ? 1 : 0);
    sqlite3_bind_int(stmt, 18, static_cast<int>(task.max_connections));
    sqlite3_bind_text(stmt, 19, conflict_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 20, task.scheduled ? 1 : 0);
    sqlite3_bind_text(stmt, 21, task.schedule_time_local.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 22, static_cast<int64_t>(task.speed_limit_bps));
    sqlite3_bind_text(stmt, 23, task.etag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 24, task.last_modified.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 25, headers_s.c_str(), -1, SQLITE_TRANSIENT);
    if (encrypted_auth.empty()) {
        sqlite3_bind_null(stmt, 26);
    } else {
        sqlite3_bind_blob(stmt, 26, encrypted_auth.data(), static_cast<int>(encrypted_auth.size()), SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 27, stream_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 28, task.expected_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 29, task.error_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 30, task.error_message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 31, alt_s.c_str(), -1, SQLITE_TRANSIENT);
    if (task.schedule_stop_time_local.empty()) {
        sqlite3_bind_null(stmt, 32);
    } else {
        sqlite3_bind_text(stmt, 32, task.schedule_stop_time_local.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(stmt, 33, created_ms);
    sqlite3_bind_int64(stmt, 34, now_ms);
    sqlite3_bind_int64(stmt, 35, task.completed_at_ms);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return Result<void>::Err("ERR_SQLITE_STEP", sqlite3_errmsg(m_db));
    }

    if (task.state == TaskState::kCompleted || task.state == TaskState::kCanceled) {
        sqlite3_stmt* del_stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, "DELETE FROM task_segments WHERE task_id = ?;", -1, &del_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(del_stmt, 1, task.task_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del_stmt);
            sqlite3_finalize(del_stmt);
        }
        return Result<void>::Ok();
    }
    return SaveSegmentsLocked(task.task_id, task.segments);
}

Result<void> SqliteStore::SaveSegmentsLocked(const std::string& task_id, const std::vector<Segment>& segments) {
    if (!m_db) return Result<void>::Err("ERR_SQLITE_CLOSED", "Database not open");
    if (!ExecSql("BEGIN IMMEDIATE TRANSACTION;").ok()) {
        return Result<void>::Err("ERR_SQLITE_TX", "Failed to begin transaction");
    }

    sqlite3_stmt* del_stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM task_segments WHERE task_id = ?;", -1, &del_stmt, nullptr) != SQLITE_OK) {
        ExecSql("ROLLBACK;");
        return Result<void>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(del_stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(del_stmt) != SQLITE_DONE) {
        sqlite3_finalize(del_stmt);
        ExecSql("ROLLBACK;");
        return Result<void>::Err("ERR_SQLITE_EXEC", sqlite3_errmsg(m_db));
    }
    sqlite3_finalize(del_stmt);

    const char* ins_sql = R"SQL(
        INSERT INTO task_segments(task_id, segment_id, start_offset, current_offset, end_offset, completed)
        VALUES (?, ?, ?, ?, ?, ?);
    )SQL";
    sqlite3_stmt* ins_stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, ins_sql, -1, &ins_stmt, nullptr) != SQLITE_OK) {
        ExecSql("ROLLBACK;");
        return Result<void>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }

    for (const auto& seg : segments) {
        sqlite3_reset(ins_stmt);
        sqlite3_clear_bindings(ins_stmt);
        sqlite3_bind_text(ins_stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins_stmt, 2, static_cast<int>(seg.segment_id));
        sqlite3_bind_int64(ins_stmt, 3, static_cast<int64_t>(seg.start_offset));
        sqlite3_bind_int64(ins_stmt, 4, static_cast<int64_t>(seg.current_offset));
        sqlite3_bind_int64(ins_stmt, 5, static_cast<int64_t>(seg.end_offset));
        sqlite3_bind_int(ins_stmt, 6, seg.completed ? 1 : 0);
        if (sqlite3_step(ins_stmt) != SQLITE_DONE) {
            sqlite3_finalize(ins_stmt);
            ExecSql("ROLLBACK;");
            return Result<void>::Err("ERR_SQLITE_STEP", sqlite3_errmsg(m_db));
        }
    }
    sqlite3_finalize(ins_stmt);
    return ExecSql("COMMIT;");
}

Result<void> SqliteStore::UpdateTaskProgressAndSegments(
    const std::string& task_id,
    TaskState state,
    uint64_t downloaded_bytes,
    uint64_t total_bytes,
    bool range_supported,
    const std::vector<Segment>& segments,
    const std::string& error_code,
    const std::string& error_message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return Result<void>::Err("ERR_SQLITE_CLOSED", "Database not open");

    std::string state_s = TaskStateToString(state);
    int64_t now_ms = NowEpochMs();
    int64_t comp_ms = (state == TaskState::kCompleted) ? now_ms : 0;

    const char* sql = R"SQL(
        UPDATE tasks SET
            state = ?,
            downloaded_bytes = ?,
            total_bytes = CASE WHEN ? > 0 THEN ? ELSE total_bytes END,
            range_supported = ?,
            error_code = ?,
            error_message = ?,
            updated_at_ms = ?,
            completed_at_ms = CASE WHEN ? > 0 THEN ? ELSE completed_at_ms END,
            encrypted_auth_blob = CASE WHEN ? IN ('COMPLETED', 'CANCELED') THEN NULL ELSE encrypted_auth_blob END
        WHERE task_id = ? AND (state NOT IN ('CANCELED', 'COMPLETED') OR ? IN ('CANCELED', 'COMPLETED'));
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<void>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(stmt, 1, state_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(downloaded_bytes));
    sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(total_bytes));
    sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(total_bytes));
    sqlite3_bind_int(stmt, 5, range_supported ? 1 : 0);
    sqlite3_bind_text(stmt, 6, error_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, error_message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, now_ms);
    sqlite3_bind_int64(stmt, 9, comp_ms);
    sqlite3_bind_int64(stmt, 10, comp_ms);
    sqlite3_bind_text(stmt, 11, state_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, task_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, state_s.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (state == TaskState::kCompleted || state == TaskState::kCanceled) {
        sqlite3_stmt* del_stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, "DELETE FROM task_segments WHERE task_id = ?;", -1, &del_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(del_stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del_stmt);
            sqlite3_finalize(del_stmt);
        }
        return Result<void>::Ok();
    }
    return SaveSegmentsLocked(task_id, segments);
}

Result<std::vector<Segment>> SqliteStore::LoadSegmentsLocked(const std::string& task_id) {
    std::vector<Segment> out;
    const char* sql = "SELECT segment_id, start_offset, current_offset, end_offset, completed FROM task_segments WHERE task_id = ? ORDER BY segment_id ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<std::vector<Segment>>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Segment s;
        s.segment_id = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
        s.start_offset = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
        s.current_offset = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
        s.end_offset = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        s.completed = sqlite3_column_int(stmt, 4) != 0;
        out.push_back(s);
    }
    sqlite3_finalize(stmt);
    return Result<std::vector<Segment>>::Ok(std::move(out));
}

static TaskModel ParseTaskRow(sqlite3_stmt* stmt) {
    TaskModel t;
    t.task_id = ColText(stmt, 0);
    t.task_type = TaskTypeFromString(ColText(stmt, 1));
    t.state = TaskStateFromString(ColText(stmt, 2));
    t.pause_reason = PauseReasonFromString(ColText(stmt, 3));
    t.queue_id = ColText(stmt, 4);
    t.queue_order = sqlite3_column_int(stmt, 5);
    t.category = ColText(stmt, 6);
    t.url = ColText(stmt, 7);
    t.page_url = ColText(stmt, 8);
    t.file_name = ColText(stmt, 9);
    t.save_dir = ColText(stmt, 10);
    t.part_path = ColText(stmt, 11);
    t.correlation_id = ColText(stmt, 12);
    t.total_bytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 13));
    t.downloaded_bytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 14));
    t.uploaded_bytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 15));
    t.range_supported = sqlite3_column_int(stmt, 16) != 0;
    t.max_connections = static_cast<uint32_t>(sqlite3_column_int(stmt, 17));
    t.conflict_policy = ConflictPolicyFromString(ColText(stmt, 18));
    t.scheduled = sqlite3_column_int(stmt, 19) != 0;
    t.schedule_time_local = ColText(stmt, 20);
    t.speed_limit_bps = static_cast<uint64_t>(sqlite3_column_int64(stmt, 21));
    t.etag = ColText(stmt, 22);
    t.last_modified = ColText(stmt, 23);

    try {
        auto h_val = json::Parse(ColText(stmt, 24));
        for (const auto& [k, v] : h_val.as_object()) {
            t.headers[k] = v.as_string();
        }
    } catch (...) {}

    const void* blob_ptr = sqlite3_column_blob(stmt, 25);
    int blob_len = sqlite3_column_bytes(stmt, 25);
    if (blob_ptr && blob_len > 0) {
        std::vector<uint8_t> cipher(
            static_cast<const uint8_t*>(blob_ptr),
            static_cast<const uint8_t*>(blob_ptr) + blob_len);
        auto plain = CryptoUtils::UnprotectSecret(cipher);
        if (plain.ok() && !plain.value->empty()) {
            try {
                auto a_val = json::Parse(*plain.value);
                for (const auto& [k, v] : a_val.as_object()) {
                    t.auth_headers[k] = v.as_string();
                }
            } catch (...) {}
        }
    }

    try {
        std::string s_json = ColText(stmt, 26);
        if (!s_json.empty()) {
            auto s_val = json::Parse(s_json);
            t.stream_options.video_filter = s_val["video_filter"].as_string("best");
            t.stream_options.audio_filter = s_val["audio_filter"].as_string("all");
            t.stream_options.subtitle_filter = s_val["subtitle_filter"].as_string("all");
            t.stream_options.output_format = s_val["output_format"].as_string("mp4");
            t.stream_options.thread_count = static_cast<uint32_t>(s_val["thread_count"].as_int64(8));
            t.stream_options.concurrent_download = s_val["concurrent_download"].as_bool(true);
            t.stream_options.custom_hls_key_hex = s_val["custom_hls_key_hex"].as_string("");
            t.stream_options.cached_manifest_body = s_val["cached_manifest_body"].as_string("");
            if (s_val.contains("torrent_selection") && s_val["torrent_selection"].is_array()) {
                for (const auto& entry : s_val["torrent_selection"].as_array()) {
                    if (!entry.is_object()) continue;
                    t.torrent_file_paths.push_back(entry["path"].as_string(""));
                    t.torrent_file_selection.push_back(entry["selected"].as_bool(true) ? 1 : 0);
                }
            }
        }
    } catch (...) {}

    t.expected_sha256 = ColText(stmt, 27);
    t.error_code = ColText(stmt, 28);
    t.error_message = ColText(stmt, 29);

    try {
        std::string alt_json = ColText(stmt, 30);
        if (!alt_json.empty()) {
            auto alt_val = json::Parse(alt_json);
            if (alt_val.is_array()) {
                for (const auto& elem : alt_val.as_array()) {
                    std::string u = elem.as_string();
                    if (!u.empty()) t.alternate_urls.push_back(u);
                }
            }
        }
    } catch (...) {}

    t.schedule_stop_time_local = ColText(stmt, 31);
    t.created_at_ms = sqlite3_column_int64(stmt, 32);
    t.updated_at_ms = sqlite3_column_int64(stmt, 33);
    t.completed_at_ms = sqlite3_column_int64(stmt, 34);
    return t;
}

Result<std::optional<TaskModel>> SqliteStore::GetTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return Result<std::optional<TaskModel>>::Err("ERR_SQLITE_CLOSED", "Database not open");

    const char* sql = R"SQL(
        SELECT task_id, task_type, state, pause_reason, queue_id, queue_order, category,
               url, page_url, file_name, save_dir, part_path, correlation_id,
               total_bytes, downloaded_bytes, uploaded_bytes, range_supported, max_connections,
               conflict_policy, scheduled, schedule_time_local, speed_limit_bps,
               etag, last_modified, headers_json, encrypted_auth_blob, stream_options_json,
               expected_sha256, error_code, error_message, alternate_urls_json, schedule_stop_time_local,
               created_at_ms, updated_at_ms, completed_at_ms
        FROM tasks
        WHERE task_id = ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<std::optional<TaskModel>>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<TaskModel> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = ParseTaskRow(stmt);
    }
    sqlite3_finalize(stmt);

    if (out) {
        auto seg_res = LoadSegmentsLocked(task_id);
        if (seg_res.ok()) {
            out->segments = std::move(*seg_res.value);
        }
    }
    return Result<std::optional<TaskModel>>::Ok(std::move(out));
}

Result<std::vector<TaskModel>> SqliteStore::ListTasks() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return Result<std::vector<TaskModel>>::Err("ERR_SQLITE_CLOSED", "Database not open");

    std::vector<TaskModel> tasks;
    const char* sql = R"SQL(
        SELECT task_id, task_type, state, pause_reason, queue_id, queue_order, category,
               url, page_url, file_name, save_dir, part_path, correlation_id,
               total_bytes, downloaded_bytes, uploaded_bytes, range_supported, max_connections,
               conflict_policy, scheduled, schedule_time_local, speed_limit_bps,
               etag, last_modified, headers_json, encrypted_auth_blob, stream_options_json,
               expected_sha256, error_code, error_message, alternate_urls_json, schedule_stop_time_local,
               created_at_ms, updated_at_ms, completed_at_ms
        FROM tasks
        ORDER BY created_at_ms DESC;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<std::vector<TaskModel>>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        tasks.push_back(ParseTaskRow(stmt));
    }
    sqlite3_finalize(stmt);

    std::map<std::string, std::vector<Segment>> seg_map;
    sqlite3_stmt* seg_stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT task_id, segment_id, start_offset, current_offset, end_offset, completed FROM task_segments ORDER BY segment_id ASC;", -1, &seg_stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(seg_stmt) == SQLITE_ROW) {
            std::string tid = ColText(seg_stmt, 0);
            Segment s;
            s.segment_id = static_cast<uint32_t>(sqlite3_column_int(seg_stmt, 1));
            s.start_offset = static_cast<uint64_t>(sqlite3_column_int64(seg_stmt, 2));
            s.current_offset = static_cast<uint64_t>(sqlite3_column_int64(seg_stmt, 3));
            s.end_offset = static_cast<uint64_t>(sqlite3_column_int64(seg_stmt, 4));
            s.completed = sqlite3_column_int(seg_stmt, 5) != 0;
            seg_map[tid].push_back(s);
        }
        sqlite3_finalize(seg_stmt);
    }
    for (auto& t : tasks) {
        auto it = seg_map.find(t.task_id);
        if (it != seg_map.end()) t.segments = std::move(it->second);
    }
    return Result<std::vector<TaskModel>>::Ok(std::move(tasks));
}

Result<void> SqliteStore::DeleteTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return Result<void>::Err("ERR_SQLITE_CLOSED", "Database not open");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM tasks WHERE task_id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<void>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    const int step_result = sqlite3_step(stmt);
    if (step_result != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(m_db);
        sqlite3_finalize(stmt);
        return Result<void>::Err("ERR_SQLITE_DELETE", message);
    }
    sqlite3_finalize(stmt);
    return Result<void>::Ok();
}

Result<int> SqliteStore::RecoverInterruptedTasks(uint64_t rollback_margin_bytes) {
    auto list = ListTasks();
    if (!list.ok()) return Result<int>::Err(list.error->code, list.error->message);

    int recovered_count = 0;
    for (auto& t : *list.value) {
        if (t.state == TaskState::kProbing || t.state == TaskState::kDownloading ||
            t.state == TaskState::kMuxing || t.state == TaskState::kVerifying ||
            t.state == TaskState::kFinalizing) {
            t.state = TaskState::kPaused;
            t.speed_bps = 0;
            t.status_detail = "Paused (Recovered after restart)";

            std::error_code ec;
            std::filesystem::path part_path = t.part_path.empty() ?
                (std::filesystem::path(t.save_dir) / (t.file_name + ".part")) :
                std::filesystem::path(t.part_path);
            bool part_exists = std::filesystem::exists(part_path, ec);
            uint64_t actual_part_sz = part_exists ? std::filesystem::file_size(part_path, ec) : 0;
            bool has_explicit_part = !t.part_path.empty();
            bool part_missing_on_disk = has_explicit_part && !part_exists;

            if (!t.segments.empty()) {
                uint64_t recomputed_dl = 0;
                for (auto& seg : t.segments) {
                    if (part_missing_on_disk) {
                        seg.current_offset = seg.start_offset;
                        seg.completed = false;
                    } else {
                        if (!seg.completed && seg.current_offset > seg.start_offset) {
                            uint64_t delta = seg.current_offset - seg.start_offset;
                            if (delta > rollback_margin_bytes) {
                                seg.current_offset -= rollback_margin_bytes;
                            } else {
                                seg.current_offset = seg.start_offset;
                            }
                        }
                        if (part_exists && seg.current_offset > actual_part_sz && actual_part_sz >= seg.start_offset) {
                            seg.current_offset = actual_part_sz;
                        }
                    }
                    recomputed_dl += seg.downloaded_bytes();
                }
                t.downloaded_bytes = recomputed_dl;
            } else {
                if (part_missing_on_disk) {
                    t.downloaded_bytes = 0;
                } else {
                    if (part_exists && actual_part_sz < t.downloaded_bytes) {
                        t.downloaded_bytes = actual_part_sz;
                    }
                    if (rollback_margin_bytes > 0 && t.downloaded_bytes > rollback_margin_bytes) {
                        t.downloaded_bytes -= rollback_margin_bytes;
                    }
                }
            }
            UpsertTask(t);
            ++recovered_count;
        }
    }
    return Result<int>::Ok(recovered_count);
}

Result<void> SqliteStore::SetSetting(const std::string& key, const std::string& value_json) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return Result<void>::Err("ERR_SQLITE_CLOSED", "Database not open");
    const char* sql = "INSERT INTO settings(key, value_json, updated_at_ms) VALUES (?, ?, ?) ON CONFLICT(key) DO UPDATE SET value_json=excluded.value_json, updated_at_ms=excluded.updated_at_ms;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<void>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, NowEpochMs());
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return Result<void>::Ok();
}

Result<std::string> SqliteStore::GetSetting(const std::string& key, const std::string& default_json) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return Result<std::string>::Err("ERR_SQLITE_CLOSED", "Database not open");
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT value_json FROM settings WHERE key = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<std::string>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string out = default_json;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = ColText(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return Result<std::string>::Ok(out);
}

Result<void> SqliteStore::UpsertQueue(const QueueConfig& q) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return Result<void>::Err("ERR_SQLITE_CLOSED", "Database not open");
    const char* sql = R"SQL(
        INSERT INTO queues(queue_id, name, max_concurrent_tasks, speed_limit_bps, schedule_enabled, start_time_local, stop_time_local, post_completion_action)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(queue_id) DO UPDATE SET
            name=excluded.name,
            max_concurrent_tasks=excluded.max_concurrent_tasks,
            speed_limit_bps=excluded.speed_limit_bps,
            schedule_enabled=excluded.schedule_enabled,
            start_time_local=excluded.start_time_local,
            stop_time_local=excluded.stop_time_local,
            post_completion_action=excluded.post_completion_action;
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<void>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }
    sqlite3_bind_text(stmt, 1, q.queue_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, q.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, static_cast<int>(q.max_concurrent_tasks));
    sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(q.speed_limit_bps));
    sqlite3_bind_int(stmt, 5, q.schedule_enabled ? 1 : 0);
    sqlite3_bind_text(stmt, 6, q.start_time_local.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, q.stop_time_local.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, q.post_completion_action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return Result<void>::Ok();
}

Result<std::vector<QueueConfig>> SqliteStore::ListQueues() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_db) return Result<std::vector<QueueConfig>>::Err("ERR_SQLITE_CLOSED", "Database not open");
    std::vector<QueueConfig> out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT queue_id, name, max_concurrent_tasks, speed_limit_bps, schedule_enabled, start_time_local, stop_time_local, post_completion_action FROM queues;", -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<std::vector<QueueConfig>>::Err("ERR_SQLITE_PREPARE", sqlite3_errmsg(m_db));
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QueueConfig q;
        q.queue_id = ColText(stmt, 0);
        q.name = ColText(stmt, 1);
        q.max_concurrent_tasks = static_cast<uint32_t>(sqlite3_column_int(stmt, 2));
        q.speed_limit_bps = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        q.schedule_enabled = sqlite3_column_int(stmt, 4) != 0;
        q.start_time_local = ColText(stmt, 5);
        q.stop_time_local = ColText(stmt, 6);
        q.post_completion_action = ColText(stmt, 7);
        out.push_back(std::move(q));
    }
    sqlite3_finalize(stmt);
    return Result<std::vector<QueueConfig>>::Ok(std::move(out));
}

} // namespace openidm
