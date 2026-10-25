# Error Handling and Recovery Specification

**Document Status:** Proposed Subsystem Specification  
**Last Reviewed:** 2026-10-16  
**Scope:** Canonical error code taxonomy, retry policies, non-retryable user recovery workflows, and graceful shutdown/sleep/network-change handling.  
**Related Documents:** [DOWNLOAD_ENGINE_DESIGN.md](DOWNLOAD_ENGINE_DESIGN.md), [STREAM_DOWNLOAD_PIPELINE.md](STREAM_DOWNLOAD_PIPELINE.md), [DATA_MODEL_AND_STORAGE.md](DATA_MODEL_AND_STORAGE.md), [PRODUCT_REQUIREMENTS.md](PRODUCT_REQUIREMENTS.md)

---

## 1. Error Classification Philosophy

Every failure in OpenIDM is mapped to a deterministic `ErrorCode` (`src/common/error_codes.hpp`) categorized into one of three recovery classes:
1. **Transient & Automatically Retryable (`AutoRetry`):** Temporary network drops, DNS blips, socket timeouts, HTTP `429 Too Many Requests`, `500`, `502`, `503`, `504`. Handled automatically per segment worker using exponential backoff with jitter without failing the task.
2. **Resumable with User Action (`UserActionResumable`):** Expired signed URLs (`401`, `403`, `410`), missing session cookies, `ETag` mismatch on server update (`412`), or disk space exhaustion (`ERROR_DISK_FULL`). Preserves `.openidm.part` / `tmp/<uuid>` and guides the user to fix the condition (e.g., "Refresh Download Address" or free disk space) and click Resume.
3. **Terminal / Non-Retryable (`Fatal`):** `404 Not Found`, DRM-protected streams (`ERR_STREAM_DRM_PROTECTED`), invalid/corrupted manifests, or unsupported protocols. Immediately marks the task as `FAILED` with a clear explanation.

---

## 2. Canonical Error Code Matrix & Recovery Behavior

| Error Code | Trigger Condition | Recovery Class | Automatic Retry Policy | Preserves Partial Data? | User-Facing Recovery Action |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **`ERR_NET_TIMEOUT`** | `ERROR_WINHTTP_TIMEOUT` or 0 bytes read for `> stall_timeout_sec` (`15s`). | `AutoRetry` | Reset worker connection and reconnect at `current_offset`. Up to `10` retries with backoff (`1s, 2s, 4s, 8s, 15s...`); pause retry counter while OS reports no internet connectivity (`NLM_CONNECTIVITY_DISCONNECTED`). | **Yes** | Automatic; user may also manually Pause/Resume. |
| **`ERR_NET_CONNECTION_RESET`** | `ERROR_WINHTTP_CONNECTION_ERROR`, TLS reset, or DNS failure during active download. | `AutoRetry` | Reconnect worker at `current_offset` with exponential backoff (`1s..30s`, max 10 attempts). | **Yes** | Automatic. |
| **`ERR_HTTP_5XX_TRANSIENT`** | Server returns `500`, `502`, `503`, or `504`. | `AutoRetry` | Respect `Retry-After` header if present (capped at `120s`), else exponential backoff up to 8 attempts. | **Yes** | Automatic. |
| **`ERR_HTTP_429_RATE_LIMITED`** | Server/CDN returns `429 Too Many Requests` during initial probe or on a segment worker connection. | `AutoRetry` (Adaptive Scale-Down) | Parse `Retry-After` (seconds or RFC 7231 HTTP-date), immediately reduce `max_connections` by 50% via `SegmentScheduler::ReduceConnectionsByHalf()` (minimum `1` connection), disable aggressive segment splitting, wait `Retry-After` (or bounded exponential backoff), and continue download. | **Yes** | Automatic connection scale-down & backoff. |
| **`ERR_HTTP_403_EXPIRED_OR_AUTH`** | Server returns `401 Unauthorized`, `403 Forbidden`, or `410 Gone` when resuming or downloading. | `UserActionResumable` | Do **not** spam retries (max 1 retry after 2s). Transition task to `PAUSED (NeedsRefresh)`. | **Yes** | Display **"Refresh Download Address"** button (`FR-006`): user clicks link in browser again or pastes updated signed URL to resume from existing bytes. |
| **`ERR_HTTP_404_NOT_FOUND`** | Server returns `404 Not Found` on initial probe. | `Fatal` | None (0 retries). | No | Inform user that the remote file does not exist on the server. |
| **`ERR_HTTP_VALIDATOR_MISMATCH`** | On resume, server returns `412 Precondition Failed` or a different `ETag` / `Content-Length` than stored in `state.db` (`FR-005`). | `UserActionResumable` | None. Abort resume before writing to `.openidm.part`. | **Yes** (Held until user decides) | Prompt user: *"The file on the server has changed since this download was paused. Restart download from the beginning?"* |
| **`ERR_HTTP_RANGE_NOT_SATISFIABLE`** | Server returns `416 Range Not Satisfiable`. | `Fatal` / `Restart` | Re-probe `Content-Length`; if local part matches, verify; otherwise prompt to restart. | Yes | Prompt user to restart from byte 0. |
| **`ERR_STREAM_INVALID_MANIFEST`** | `N_m3u8DL-RE` probe exits non-zero or fails to parse `#EXTM3U` / `<MPD` (e.g., server returned an HTML login page instead of a manifest). | `UserActionResumable` | None. | No | Show the first 200 characters of the server response (redacted) and advise refreshing the video page in the browser. |
| **`ERR_STREAM_DRM_PROTECTED`** | Layer 2 detects `video.mediaKeys !== null` or `meta.json` contains `SAMPLE-AES-CTR` / Widevine / PlayReady `cenc` encryption without user key (`NFR-015`). | `Fatal` | None. | No | Display clear notice: *"This stream is protected by Digital Rights Management (DRM). OpenIDM does not bypass DRM protection."* |
| **`ERR_STREAM_WORKER_CRASH`** | `N_m3u8DL-RE.exe` or `ffmpeg.exe` exits with unhandled exception or non-zero exit code during download/mux. | `UserActionResumable` | Retry once from existing `--tmp-dir` segment cache; if second attempt fails, transition to `FAILED`. | **Yes** (`--tmp-dir` segments kept) | Allow user to click **Retry** (which reuses already-downloaded segments in `tmp/<uuid>`) or inspect the Task Log tab. |
| **`ERR_DEPENDENCY_MISSING`** | `N_m3u8DL-RE.exe` or `ffmpeg.exe` is missing or fails SHA-256 integrity check when starting a stream task (e.g., Core-only installer). | `UserActionResumable` | None. | **Yes** | Open the built-in **"Download Verified Stream Components"** dialog (`ADR-0008`) or let user browse to `ffmpeg.exe`. |
| **`ERR_DISK_FULL`** | `SetFileInformationByHandle` (`FileEndOfFileInfo`) or `WriteFile` returns `ERROR_DISK_FULL` (`112`) or `ERROR_HANDLE_DISK_FULL` (`39`). | `UserActionResumable` | Pause all active tasks writing to that volume immediately. | **Yes** | Notify user of required free space (`total_bytes - downloaded_bytes`) and allow resuming or changing destination drive. |
| **`ERR_TORRENT_EMPTY`** | A browser-offered `.torrent` URL returns an empty body. | `Fatal` | None. | No | Remove the queued offer and explain that the server returned no metainfo. |
| **`ERR_TORRENT_STORAGE`** | The app cannot create or persist the retrieved metainfo file under `%LOCALAPPDATA%\\OpenIDM\\tmp\\torrents`. | `UserActionResumable` | None. | No | Remove the queued offer and report the local storage failure. |
| **`ERR_STORAGE_PART_CORRUPTED`** | `.openidm.part` is missing, truncated below `max(current_offset)`, or final SHA-256 verification fails. | `UserActionResumable` | None. | Kept as `.corrupt` if checksum failed | Offer one-click **"Restart Download"** (for HTTP files, or re-verify segments). |
| **`ERR_FILE_DELETE_FAILED`** | Explicit permanent deletion fails because an output path is unsafe, cannot be inspected, or cannot be removed. | `UserActionResumable` | None. | Yes; task record remains available | Explain the filesystem error and allow retry after closing the file, correcting its attributes, or selecting the correct output task. |
| **`ERR_PART_DELETE_FAILED`** | Task-owned partial data cannot be cleaned during removal. | `UserActionResumable` | None. | Task record remains available | Explain the filesystem error; retry removal after resolving the lock or permission issue. |
| **`ERR_SQLITE_DELETE`** | SQLite reports an error while deleting the task row. | `UserActionResumable` | None. | Completed output may already have been removed by an explicit permanent-delete action; task remains in memory | Report storage failure and preserve the visible task for reconciliation/retry. |

---

## 3. System Lifecycle, Power Events, and Network Change Handling (`Section 8 Requirement`)

### 3.1 Preventing Unexpected OS Sleep During Active Downloads
While one or more tasks are in `DOWNLOADING` or `MUXING` state, `DownloadController` calls:
```cpp
SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
```
When all active tasks finish or pause, it restores normal power management via `SetThreadExecutionState(ES_CONTINUOUS)`.

### 3.2 Handling Manual System Sleep / Hibernate (`WM_POWERBROADCAST`)
- **`PBT_APMSUSPEND`:** When the user manually closes the laptop lid or forces Windows into Modern Standby / S3 Sleep, `MainWindow` receives `WM_POWERBROADCAST (PBT_APMSUSPEND)` and notifies `DownloadController::OnSystemSuspending()`, which synchronously flushes all in-flight SQLite segment offsets (`WAL` checkpoint) within `< 200 ms`.
- **`PBT_APMRESUMEAUTOMATIC`:** Upon wake, stalled `WinHTTP` connections are automatically reset at their persisted `current_offset` without incrementing the fatal error counter.

### 3.3 Handling Network Changes (`INetworkListManagerEvents`)
- `openidm.exe` subscribes to Windows Network List Manager COM events (`INetworkListManagerEvents::ConnectivityChanged`) or monitors `ERROR_WINHTTP_NAME_NOT_RESOLVED` / `ERROR_WINHTTP_CANNOT_CONNECT`.
- When Windows transitions to `NLM_CONNECTIVITY_DISCONNECTED` (e.g., Wi-Fi drop or VPN reconnect), active tasks enter a `WaitingForNetwork` sub-state (`retry_count` is frozen so a 2-minute Wi-Fi outage does not exhaust retry budgets) and automatically resume segment workers within `1 second` of connectivity restoration.

### 3.4 Graceful Application Shutdown (`WM_CLOSE` vs `WM_QUERYENDSESSION`)
- Clicking the main window `[X]` button minimizes `openidm.exe` to the Windows 11 system tray (`ShowWindow(hwnd, SW_HIDE)`) by default so background downloads continue uninterrupted.
- Choosing **Exit** from the tray/File menu or receiving `WM_QUERYENDSESSION` / `WM_ENDSESSION` (Windows Logout / Shutdown / Restart):
  1. Signals all `WinHTTP` workers to stop reading new chunks.
  2. Terminates active `N_m3u8DL-RE.exe` Job Objects cleanly (retaining `tmp/<uuid>` segment caches so streams can resume on next login).
  3. Commits final segment `current_offset` values to `state.db` and executes `sqlite3_wal_checkpoint_v2(..., SQLITE_CHECKPOINT_TRUNCATE)`.
  4. Completes shutdown in **`< 500 ms`** so Windows never displays `"OpenIDM is preventing Windows from shutting down"`.
