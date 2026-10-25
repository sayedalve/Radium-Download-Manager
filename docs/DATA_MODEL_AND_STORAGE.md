# Data Model and Storage Specification

**Document Status:** Proposed Storage Specification  
**Last Reviewed:** 2026-10-16  
**Scope:** SQLite 3 database schema (`state.db`), task state transitions, DPAPI encryption wrapper, crash recovery protocol, temporary file layout, and atomic file replacement.  
**Related Documents:** [SYSTEM_ARCHITECTURE.md](SYSTEM_ARCHITECTURE.md), [DOWNLOAD_ENGINE_DESIGN.md](DOWNLOAD_ENGINE_DESIGN.md), [PRIVACY_AND_DATA_HANDLING.md](PRIVACY_AND_DATA_HANDLING.md), [adr/0007-configuration-and-persistence.md](adr/0007-configuration-and-persistence.md)

---

## 1. Storage Locations on Windows 11

### Task removal consistency

Task removal is serialized against worker completion. A worker is asked to stop and finish before owned partial data is cleaned. The controller deletes output only when the explicit permanent-delete action is selected; for multi-file torrents it deletes each selected metadata-relative payload path, not the display name used for the task row. Remove from List preserves completed user output. If partial cleanup, path validation, output deletion, or SQLite row deletion fails, the SQLite task row and in-memory task remain available for retry. After filesystem work succeeds, `SqliteStore::DeleteTask` must complete successfully before the task is removed from memory. SQLite step errors are returned to the caller rather than silently treated as success.

| Path | Purpose | Access Control |
| :--- | :--- | :--- |
| `%LOCALAPPDATA%\\OpenIDM\\state.db` | Primary SQLite 3 database (`WAL` mode) storing tasks, segments, queues, and settings. | Standard Windows User Profile ACL. |
| `%LOCALAPPDATA%\\OpenIDM\\state.db-wal` & `-shm` | SQLite Write-Ahead Log and shared-memory index files. | Standard Windows User Profile ACL. |
| `%LOCALAPPDATA%\\OpenIDM\\tmp\\<task_uuid>\\` | Per-task working directory for `N_m3u8DL-RE.exe` (`@job.rsp`, `meta.json`, `.ts`/`.m4s` segments). | Explicit DACL: `D:P(A;;GA;;;SY)(A;;GA;;;<UserSID>)`. |
| `%LOCALAPPDATA%\\OpenIDM\\tmp\\torrents\\<fetch_uuid>\\metainfo.torrent` | Browser-offered remote `.torrent` metainfo retained for the torrent task's resume/recovery; removed with its task-owned `part_path`. | Inherits the current user's `%LOCALAPPDATA%` ACL; metadata is capped at 16 MiB. |
| `<destination_dir>\\<filename>.openidm.part` | In-progress pre-allocated partial file for ordinary HTTP downloads. | Inherits `<destination_dir>` ACL. |
| `%LOCALAPPDATA%\\OpenIDM\\logs\\openidm.log` | Redacted rotating application log (`max 5 x 2 MiB`). | Standard Windows User Profile ACL. |

---

## 2. SQLite 3 Schema Definition (`SCHEMA_VERSION = 1`)

```sql
-- Executed on connection open:
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;
PRAGMA user_version = 1;

-- 1. Download Queues Table
CREATE TABLE IF NOT EXISTS queues (
    queue_id                TEXT PRIMARY KEY,              -- 'main', 'scheduled_night', etc.
    name                    TEXT NOT NULL,
    max_concurrent_tasks    INTEGER NOT NULL DEFAULT 3 CHECK (max_concurrent_tasks BETWEEN 1 AND 16),
    speed_limit_bps         INTEGER NOT NULL DEFAULT 0,    -- 0 = unlimited (bytes/sec)
    schedule_enabled        INTEGER NOT NULL DEFAULT 0,    -- 0 or 1
    start_time_local        TEXT,                          -- 'HH:MM' (24h) or NULL
    stop_time_local         TEXT,                          -- 'HH:MM' (24h) or NULL
    post_completion_action   TEXT NOT NULL DEFAULT 'NONE'   -- 'NONE', 'EXIT_APP', 'SLEEP_OS', 'SHUTDOWN_OS'
);

-- 2. Tasks Table (Ordinary HTTP & Stream Downloads)
CREATE TABLE IF NOT EXISTS tasks (
    task_id                 TEXT PRIMARY KEY,              -- UUID v4 string
    task_type               TEXT NOT NULL CHECK (task_type IN ('HTTP_FILE', 'STREAM_HLS', 'STREAM_DASH', 'STREAM_MSS')),
    state                   TEXT NOT NULL CHECK (state IN ('QUEUED', 'PROBING', 'DOWNLOADING', 'MUXING', 'VERIFYING', 'PAUSED', 'COMPLETED', 'FAILED', 'CANCELED')),
    queue_id                TEXT NOT NULL DEFAULT 'main' REFERENCES queues(queue_id),
    queue_order             INTEGER NOT NULL DEFAULT 0,
    category                TEXT NOT NULL DEFAULT 'General' CHECK (category IN ('Video', 'Audio', 'Compressed', 'Documents', 'Programs', 'General')),
    url                     TEXT NOT NULL,                 -- Final resolved download or manifest URL
    page_url                TEXT,                          -- Originating browser tab URL
    file_name               TEXT NOT NULL,                 -- Sanitized final basename (e.g. 'ubuntu-24.04.iso')
    save_dir                TEXT NOT NULL,                 -- Absolute UTF-8 directory path
    part_path               TEXT,                          -- Absolute path to .openidm.part or tmp/<uuid>
    total_bytes             INTEGER NOT NULL DEFAULT 0,    -- 0 if unknown/stream
    downloaded_bytes        INTEGER NOT NULL DEFAULT 0,
    range_supported         INTEGER NOT NULL DEFAULT 0,    -- 0 or 1
    max_connections         INTEGER NOT NULL DEFAULT 8 CHECK (max_connections BETWEEN 1 AND 32),
    etag                    TEXT,                          -- Strong ETag validator for safe resume
    last_modified           TEXT,                          -- HTTP Last-Modified validator
    headers_json            TEXT NOT NULL DEFAULT '{}',    -- Non-sensitive headers (Referer, Origin, User-Agent)
    encrypted_auth_blob     BLOB,                          -- DPAPI-encrypted JSON {'Cookie':'...','Authorization':'...'}
    stream_options_json     TEXT,                          -- Selected video/audio/subtitle track filters & container format
    expected_checksum_algo  TEXT,                          -- NULL, 'SHA256', 'SHA1', 'MD5'
    expected_checksum_hex   TEXT,
    error_code              TEXT,                          -- Canonical error code (e.g. 'ERR_HTTP_403_EXPIRED')
    error_message           TEXT,
    created_at_ms           INTEGER NOT NULL,
    updated_at_ms           INTEGER NOT NULL,
    completed_at_ms         INTEGER
);

CREATE INDEX IF NOT EXISTS idx_tasks_state_queue ON tasks(state, queue_id, queue_order);
CREATE INDEX IF NOT EXISTS idx_tasks_category ON tasks(category, created_at_ms DESC);

-- 3. HTTP Byte-Range Segments Table (1..32 rows per active/paused HTTP_FILE task)
CREATE TABLE IF NOT EXISTS task_segments (
    task_id                 TEXT NOT NULL REFERENCES tasks(task_id) ON DELETE CASCADE,
    segment_id              INTEGER NOT NULL,
    start_offset            INTEGER NOT NULL,
    current_offset          INTEGER NOT NULL,
    end_offset              INTEGER NOT NULL,
    completed               INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (task_id, segment_id)
);

-- 4. Application Settings Key-Value Table
CREATE TABLE IF NOT EXISTS settings (
    key                     TEXT PRIMARY KEY,
    value_json              TEXT NOT NULL,
    updated_at_ms           INTEGER NOT NULL
);
```

---

## 3. Canonical Task State Transitions

| Current State | Allowed Next States | Trigger & Storage Invariant |
| :--- | :--- | :--- |
| `QUEUED` | `PROBING`, `PAUSED`, `CANCELED` | Picked up by `QueueScheduler` when `active_tasks < max_concurrent_tasks`. |
| `PROBING` | `DOWNLOADING`, `PAUSED`, `FAILED`, `CANCELED` | Initial `WinHTTP` probe or `N_m3u8DL-RE --skip-download` completes. Inserts initial `task_segments` rows in the same SQLite transaction. |
| `DOWNLOADING` | `MUXING`, `VERIFYING`, `PAUSED`, `FAILED`, `CANCELED` | Batched `current_offset` updates committed every `1,000 ms`. |
| `MUXING` | `VERIFYING`, `FAILED`, `CANCELED` | Stream tasks only (`N_m3u8DL-RE` invoking `ffmpeg.exe`). |
| `VERIFYING` | `COMPLETED`, `FAILED` | Verifies byte count, optional checksum (`BCryptHash`), and executes atomic file rename. |
| `PAUSED` | `QUEUED`, `PROBING`, `CANCELED` | Retains `.openidm.part` (or `tmp/<uuid>`) and `encrypted_auth_blob`. |
| `FAILED` | `QUEUED`, `PROBING`, `CANCELED` | Retains partial data for resumable errors; records `error_code` and `error_message`. |
| `COMPLETED` | *(Terminal — or Delete)* | **Immediately sets `encrypted_auth_blob = NULL` and deletes `task_segments` rows.** |
| `CANCELED` | *(Terminal — or Delete)* | **Immediately sets `encrypted_auth_blob = NULL`, deletes `task_segments`, and deletes `.openidm.part` / `tmp/<uuid>`.** |

---

## 4. Crash Recovery & Startup Reconciliation Protocol (`NFR-005`)

### 4.1 Torrent selection persistence

For multi-file torrents, the selected relative paths and boolean selection state are persisted in the existing task stream-options JSON as `torrent_selection`. This avoids a schema migration while keeping selection tied to the task record. JSON import/export serializes the same data as `torrentSelection`; CSV uses an escaped `TorrentSelection` JSON column. On restore, paths are matched against the parsed torrent metadata by normalized relative path; unknown paths are ignored and an empty selection is rejected before the engine starts.

When `openidm.exe` starts up (`DownloadController::InitializeFromStorage()`):
1. **Wal Checkpoint:** SQLite automatically replays any committed frames in `state.db-wal`.
2. **Stale Active State Transition:** Any task found in `PROBING`, `DOWNLOADING`, `MUXING`, or `VERIFYING` state (indicating `openidm.exe` was terminated ungracefully via power loss, BSOD, or Task Manager) is transitioned to **`PAUSED` (with flag `interrupted_by_crash = true`)**—or re-queued if `auto_resume_on_startup` is enabled.
3. **Segment Offset Rollback Safety Margin:** For interrupted `HTTP_FILE` tasks, `WinHttpDownloadEngine` inspects each incomplete `task_segments` row where `current_offset > start_offset`:
   - Because OS file buffers may not have been flushed to physical storage before a sudden power loss, the recovery routine rewinds `current_offset = std::max(start_offset, current_offset - 65536)` (`64 KiB` safety overlap) and verifies that `<part_path>` exists on disk with size `>= total_bytes`.
   - If `<part_path>` was deleted externally while `openidm.exe` was closed, all `task_segments` rows for that task are reset to `0` and the task state is set to `FAILED (ERR_STORAGE_PART_FILE_MISSING)`.
4. **Orphaned Stream Temp Cleanup:** Scans `%LOCALAPPDATA%\\OpenIDM\\tmp\\*`: any subdirectory `<uuid>` that does not correspond to an existing task in `state.db` is recursively deleted, and any residual `*.rsp` files from a crashed run are securely wiped.

---

## 5. Safe Atomic File Replacement (`FR-026`)

To guarantee that a destination file (e.g., `C:\\Users\\Alice\\Downloads\\Video\\movie.mp4`) is never left in a half-written state:
1. All bytes are written to `C:\\Users\\Alice\\Downloads\\Video\\movie.mp4.openidm.part` (same directory and NTFS/exFAT volume).
2. During `VERIFYING`, `FileCommitter::CommitPartFile` checks if `movie.mp4` already exists:
   - **`Auto-Rename (Default)`:** Iterates `movie (1).mp4`, `movie (2).mp4` using `CREATE_NEW` / `ReplaceIfExists = FALSE` in `FILE_RENAME_INFO` (`SetFileInformationByHandle(..., FileRenameInfoEx, ...)` with `FILE_RENAME_FLAG_POSIX_SEMANTICS`) to avoid TOCTOU race conditions.
   - **`Overwrite`:** Sets `FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS`, atomically replacing the target entry in the NTFS Master File Table.
3. Removes the Mark-of-the-Web (`Zone.Identifier` NTFS Alternate Data Stream) **only if disabled by enterprise policy**; by default, **writes `ZoneId=3` (`[ZoneTransfer]\r\nZoneId=3\r\n`) to `<final_file>:Zone.Identifier`** when downloading `.exe`, `.msi`, `.zip`, or `.docx` files from the internet so Windows Defender SmartScreen and Protected View function as expected by Windows 11 security standards!
