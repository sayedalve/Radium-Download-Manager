# Release and Update Process Specification

**Document Status:** Active Release Engineering Specification (`v0.3.0`)
**Last Reviewed:** 2026-10-18
**Scope:** Semantic versioning, release branching, CI artifact pipeline, cryptographic signing (Authenticode + Ed25519), third-party worker pinning, security advisories, and rollback procedures.
**Related Documents:** [WINDOWS_BUILD_AND_PACKAGING.md](WINDOWS_BUILD_AND_PACKAGING.md), [SECURITY_THREAT_MODEL.md](SECURITY_THREAT_MODEL.md), [../LICENSE_REVIEW.md](../LICENSE_REVIEW.md), [adr/0008-packaging-and-updates.md](adr/0008-packaging-and-updates.md)

---

## 1. Versioning Scheme

Radium Download Manager follows **Semantic Versioning 2.0.0 (`MAJOR.MINOR.PATCH`)**:
- **`0.y.z` (Current Milestones):** Production-ready release baseline (`v0.3.1`).
- **`MAJOR`:** Incremented on breaking changes to the Native Messaging IPC protocol (`protocolVersion` major bump) or incompatible SQLite schema changes that cannot be migrated automatically.
- **`MINOR`:** Backwards-compatible new features (e.g., adding dynamic rate limiting, queue scheduling, or pinned upgrades of `N_m3u8DL-RE` / `FFmpeg`).
- **`PATCH`:** Bug fixes, security patches, and media detection rule updates.

Both the desktop application (`CMakeLists.txt` `VERSION 0.3.0`) and browser extension (`extension/manifest.json` `"version"`) share synchronized `MAJOR.MINOR.PATCH` numbers on every release tag (`vX.Y.Z`).

---

## 2. Release Branching & CI Build Pipeline

1. **`main` Branch:** Protected trunk; all merges require passing unit tests (`UT-*`), integration tests (`IT-*`), security tests (`SEC-*`), and binary size budget gates.
2. **Release Tags (`vX.Y.Z`):** Pushing an annotated Git tag `vX.Y.Z` triggers the reproducible release workflow (`.github/workflows/windows-release.yml`):
   - Compiles `radium.exe` (WIN32 GUI subsystem) and `radium-nmh.exe` (CUI console subsystem) in `Release` mode (`/O2 /GL /LTCG /MT /guard:cf`).
   - Fetches and verifies the exact SHA-256-pinned upstream `N_m3u8DL-RE.exe` and `ffmpeg.exe` binaries via `scripts/fetch-workers.ps1`.
   - Runs automated C++20 unit tests (`radium_unit_tests.exe`), browser extension tests (`test_extension_detection.js`), and integration tests (`run_all_integration_tests.py`).
   - Builds `Radium-Setup-x64-vX.Y.Z.exe` via Inno Setup 6 (`installer/radium.iss` / `installer/radium.iss`), configured with lowest privilege execution (`{autopf}`) and component selection.
   - Packages standalone portable archives (`Radium-vX.Y.Z-Windows-x64-Full.zip` and `Radium-vX.Y.Z-Windows-x64-Core.zip`) via `scripts/package-release-zip.ps1`.
   - Computes SHA-256 digests for all generated packages in `dist/SHA256SUMS.txt`.
   - Drafts a GitHub Release on `https://github.com/sayedalve/Radium-Download-Manager/releases` with attached distribution packages and licenses.

---

## 3. Published Release Artifacts & Distribution Bundle

Every GitHub Release (`vX.Y.Z`) publishes the following complete artifact set:

| Artifact Filename | Description | Verification / Compliance Purpose |
| :--- | :--- | :--- |
| `Radium-Setup-x64-vX.Y.Z.exe` | Dual-component Windows installer (`radium.exe`, `radium-nmh.exe`, optional stream workers). | Primary end-user installer with automatic browser manifest registration. |
| `Radium-vX.Y.Z-Windows-x64-Full.zip` | Full offline portable archive with pre-bundled `N_m3u8DL-RE.exe` and `ffmpeg.exe`. | Self-contained, zero-download portable deployment for USB / testing. |
| `Radium-vX.Y.Z-Windows-x64-Core.zip` | Lightweight core portable archive (`radium.exe`, `radium-nmh.exe`, extension, manifests; `< 2 MB`). | Minimal footprint option for users with existing FFmpeg in PATH. |
| `flux-extension-vX.Y.Z.zip` | Chromium MV3 extension bundle (Chrome, Edge, Brave, and Firefox). | Unpacked offline browser integration and store submission. |
| `SHA256SUMS.txt` | Cryptographic SHA-256 digests of all release binaries and archives. | Tamper verification by users and package managers. |
| `LICENSE` & `THIRD_PARTY_NOTICES.md` | GNU General Public License v3.0 and complete third-party notices. | Full compliance with GPL-3.0, MIT, and LGPL licensing requirements. |

---

## 4. Cryptographic Update Verification & Rollback Procedure (`FR-028`, `THREAT-010`)

### 4.1 Update Verification Sequence
When the user triggers **Check for Updates** (or enables periodic background checks):
1. `radium.exe` fetches `update-manifest.json` over HTTPS (`WinHTTP` with TLS 1.2/1.3).
2. Verifies the cryptographic signature over the raw bytes of `update-manifest.json` using the hardcoded public key compiled into `radium.exe`.
3. Compares `manifest.version` against current `APP_VERSION` (preventing downgrade attacks unless the user explicitly executes a manual installer).
4. Downloads the installer `.exe` to `%LOCALAPPDATA%\Flux\updates\`, verifies its `SHA-256` against `manifest.sha256`, and executes the installer with `/SILENT` or interactive wizard prompt.

### 4.2 Emergency Rollback & Bad-Release Revocation
If a critical regression is discovered in `vX.Y.Z`:
1. Immediately publish `vX.Y.(Z+1)` reverting the offending commit, or update `update-manifest.json` to point `latest_stable` to the last known good version while adding `"revoked_versions": ["X.Y.Z"]`.
2. Because SQLite schema migrations follow an **expand-only additive rule** across minor/patch releases (never dropping columns required by the prior minor version), users who reinstall the previous `vX.Y.(Z-1)` installer retain their `state.db` download history without database corruption.
