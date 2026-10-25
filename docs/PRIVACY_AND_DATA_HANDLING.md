# Privacy and Data Handling Specification

**Document Status:** Proposed Privacy & Data Governance Specification  
**Last Reviewed:** 2026-10-16  
**Scope:** Inventory of all user data processed by OpenIDM, storage locations, DPAPI encryption, log redaction rules, and retention policies.  
**Related Documents:** [SECURITY_THREAT_MODEL.md](SECURITY_THREAT_MODEL.md), [DATA_MODEL_AND_STORAGE.md](DATA_MODEL_AND_STORAGE.md), [BROWSER_EXTENSION.md](BROWSER_EXTENSION.md), [adr/0007-configuration-and-persistence.md](adr/0007-configuration-and-persistence.md)

---

## 1. Core Privacy Principles

1. **100% Local-First Operation (`NFR-010`):** OpenIDM contains zero analytics SDKs, zero telemetry beacons, zero crash-reporting cloud uploads, and zero remote configuration fetches (other than opt-in GitHub/release update checks explicitly enabled by the user).
2. **Minimum Necessary Credential Lifespan (`NFR-009`):** Browser session cookies (`Cookie`) and `Authorization` headers forwarded for an authenticated download are used strictly to complete that specific download task, encrypted at rest using Windows DPAPI while the task is active/paused, and **permanently wiped the moment the task reaches `Completed` or `Canceled`**.
3. **Mandatory Log Redaction:** No plaintext `Cookie`, `Set-Cookie`, `Authorization`, `Proxy-Authorization`, or signed URL token parameter is ever written to disk logs.

---

## 2. Data Inventory, Storage Locations & Retention Policy

| Data Element | Why It Is Processed | Where It Is Stored | Protection at Rest | Retention & Deletion Policy |
| :--- | :--- | :--- | :--- | :--- |
| **Target Download / Manifest URL** | Required to fetch the file or stream and allow user retry/history lookup. | `tasks.url` and `tasks.page_url` in `%LOCALAPPDATA%\\OpenIDM\\state.db` | OS User Profile ACL (`%LOCALAPPDATA%`) | Retained in user's download history until the user deletes the task or clicks "Clear Completed History". |
| **Transient Session Headers (`Cookie`, `Authorization`)** | Required when a download or HLS/DASH stream requires the browser's logged-in session to avoid `401`/`403` errors. | 1. `tasks.encrypted_auth_blob` in `state.db` while task is `Queued`/`Downloading`/`Paused`.<br>2. Ephemeral `@job.rsp` in `%LOCALAPPDATA%\\OpenIDM\\tmp\\<task_uuid>\\` during active stream worker run. | **Encrypted via Windows DPAPI (`CryptProtectData`)** in `state.db`; directory ACL restricted to `<UserSID>` for `@job.rsp`. | **Wiped immediately upon task completion or cancellation:** `UPDATE tasks SET encrypted_auth_blob = NULL WHERE task_id = ?;` and `@job.rsp` overwritten with zeros before `DeleteFileW`. |
| **Non-Credential Request Headers (`Referer`, `Origin`, `User-Agent`)** | Required by CDNs for hotlink validation on resume. | `tasks.headers_json` in `state.db` | OS User Profile ACL | Retained with the task record until the task is deleted by the user. |
| **Browser Detected Media Candidates (Per-Tab)** | Allows the user to click the extension popup or video overlay button and see streams detected on the current tab. | `chrome.storage.session` (in-memory inside browser process). | In-memory only (never written to browser disk storage). | Automatically purged when the tab is closed (`tabs.onRemoved`), navigated to a new origin, or when the browser closes. |
| **Extension User Preferences & Domain Exclusions** | Remembers which file extensions to intercept and which domains to bypass (`FR-009`). | `chrome.storage.local` (and mirrored in `state.db` `settings` table). | Browser profile storage | Retained until reset by user or extension uninstallation. |
| **Temporary Stream Segments (`.ts`, `.m4s`, `meta.json`)** | Intermediate fragments downloaded by `N_m3u8DL-RE.exe` prior to FFmpeg muxing. | `%LOCALAPPDATA%\\OpenIDM\\tmp\\<task_uuid>\\` | Directory DACL restricted to `<UserSID>` and `SYSTEM`. | Deleted automatically upon successful muxing (`--del-after-done true`) or task cancellation. Orphaned temp dirs cleaned on `openidm.exe` startup. |
| **Application & Task Diagnostic Logs** | Allows users and developers to diagnose network or muxing failures. | `%LOCALAPPDATA%\\OpenIDM\\logs\\openidm.log` (rotating: max 5 files x `2 MiB`). | Mandatory Regex Redaction before formatting (`Section 3`). | Oldest log file rotated out automatically once total log size exceeds `10 MiB`; user can click "Clear Logs" in Settings. |
| **Local Crash Minidumps (Optional)** | Diagnosing unhandled Win32 exceptions (`SetUnhandledExceptionFilter`). | `%LOCALAPPDATA%\\OpenIDM\\crashes\\*.dmp` (`MiniDumpNormal` — stack/registers only, **never** `MiniDumpWithFullMemory`). | Excludes heap memory so decrypted DPAPI buffers or cookies in RAM are not dumped to disk. | Never uploaded automatically; user may inspect or delete locally; auto-purged after 14 days. |

---

## 3. Mandatory Log Redaction Rules (`src/common/Logger.cpp`)

Before any string is written to `openidm.log` or displayed in the UI Task Log tab, `Logger::RedactSensitiveData(std::string_view input)` applies two redaction passes:

1. **HTTP Header Redaction (Case-Insensitive):**
   - Any header matching `Cookie`, `Set-Cookie`, `Authorization`, `Proxy-Authorization`, `X-Auth-Token`, or `X-Api-Key` has its value replaced with `[REDACTED]`.
   - Example: `-H "Cookie: sid=12345"` -> `-H "Cookie: [REDACTED]"`.
2. **URL Query Parameter Redaction:**
   - Query parameters matching `(token|access_token|auth|sig|signature|hdnea|Policy|Signature|Key-Pair-Id|X-Amz-Signature|X-Amz-Credential|X-Amz-Security-Token)=([^&\\s"]+)` are replaced with `$1=[REDACTED]` in application log files (while the full unredacted URL remains available only inside the authenticated user's Task Properties dialog).

### Browser media cache scope

Detected media metadata is held per browser tab and is cleared when that tab changes its main-frame document URL, including History API and fragment-based navigation. A popup request also checks the active tab's current URL against cached candidate identities. Navigation in another tab does not clear this tab's data. Service-worker session persistence remains limited to extension session state and is reconciled against the current document before popup display.
