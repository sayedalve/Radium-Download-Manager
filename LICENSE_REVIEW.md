# Open Source License Review & Compliance Strategy

**Document Status:** Proposed Compliance Analysis (Requires Legal Verification Before Public Binary Distribution)  
**Last Reviewed:** 2026-10-16  
**Related Documents:** [docs/UPSTREAM_PROJECT_ANALYSIS.md](docs/UPSTREAM_PROJECT_ANALYSIS.md), [docs/adr/0006-licensing-and-dependency-policy.md](docs/adr/0006-licensing-and-dependency-policy.md), [docs/WINDOWS_BUILD_AND_PACKAGING.md](docs/WINDOWS_BUILD_AND_PACKAGING.md)

> **LEGAL DISCLAIMER:**  
> This document is an engineering analysis of software license obligations and compatibility prepared for architectural planning. It does **not** constitute formal legal advice and must not replace a review by qualified legal counsel before distributing pre-built installers or publishing extensions to commercial browser stores.

---

## 1. Executive Summary

OpenIDM draws architectural inspiration, media detection rules, or binary worker integration from four upstream repositories and standard open-source media tools. Because both **[Brisk](https://github.com/BrisklyDev/brisk)** and **[Cat Catch](https://github.com/xifangczy/cat-catch)** (v2.0+) are licensed under **GNU General Public License v3.0 (`GPL-3.0`)**, and **[N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE)** is licensed under the **MIT License**, the cleanest, fully compliant licensing strategy for OpenIDM is:

- **Primary Project License:** **GNU General Public License v3.0 or later (`GPL-3.0-or-later`)** for both the OpenIDM desktop application (`openidm.exe`, `openidm-nmh.exe`) and the OpenIDM Browser Extension.
- **Third-Party Worker Bundling:** Invoke `N_m3u8DL-RE.exe` (MIT) and `ffmpeg.exe` (LGPL-2.1+ or GPL-3.0+) as separate out-of-process executables with full license notices, copyright attributions, and corresponding source code references included in the installer and installation directory.

---

## 2. Upstream Repository License Inventory

| Upstream Project | Repository URL | Verified License | Inspected Version / Commit | Key License Evidence & Nuances |
| :--- | :--- | :--- | :--- | :--- |
| **Brisk Desktop** | [BrisklyDev/brisk](https://github.com/BrisklyDev/brisk) | `GPL-3.0` | `main` branch (2026-10) | Marked `GPL-3.0` on GitHub and [AUR package metadata](https://aur.archlinux.org/packages/brisk). |
| **Brisk Download Engine (Standalone Dart)** | [BrisklyDev/brisk_download_engine](https://github.com/BrisklyDev/brisk_download_engine) | `MIT` | Updated 2024-12-12 | Listed under [BrisklyDev GitHub Organization](https://github.com/BrisklyDev) as `MIT`. |
| **Kasma (BrisklyDev Rust Engine WIP)** | [BrisklyDev/kasma](https://github.com/BrisklyDev/kasma) | `MIT` | Updated 2025-08-27 | Listed under [BrisklyDev GitHub Organization](https://github.com/BrisklyDev) as `MIT`. |
| **Brisk Browser Extension** | [BrisklyDev/brisk-browser-extension](https://github.com/BrisklyDev/brisk-browser-extension) | **Unverified / Missing Explicit Root LICENSE File** | Tag `v1.4.1`, Commit `4f60c7f` | Root file listing (`app/`, `.gitignore`, `Brisk-Demo.gif`, `README.md`, `build-script.sh`, `package.json`, `yarn.lock`) does not show a standalone `LICENSE` file; companion repo `brisk-extension-dynamic-plugins` is `MIT` and parent `brisk` is `GPL-3.0`. **Action Required:** Verify `package.json` `"license"` field or obtain explicit clarification from `@AminBhst` before copying any verbatim code from `brisk-browser-extension`. |
| **Cat Catch (`猫抓`)** | [xifangczy/cat-catch](https://github.com/xifangczy/cat-catch) | `GPL-3.0` (v2.0+); `MIT` (v1.0.16 and earlier) | Tags `2.7.1` (`b6cd7c5`), `2.7.2` | Explicitly states in [README](https://github.com/sanshengwanwuhe/cat-catch/blob/master/README.md): *"1.0版使用MIT许可. 2.0版更改为GPL v3许可."* Maintainer noted in [community discussion](https://meta.appinn.net/t/topic/77635) that v2.x has multiple contributors who hold copyright over their contributions under GPL-3.0. |
| **Cat Catch Documentation** | [xifangczy/cat-catch-book](https://github.com/xifangczy/cat-catch-book) | Unspecified (Archived Public Docs) | Commit `71d2329` (Archived 2026-07-05) | Documentation only; not copied into OpenIDM. |
| **N_m3u8DL-RE** | [nilaoda/N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) | `MIT` | Tag `v0.6.0-beta`, Commit `14b12f5` | Explicit `LICENSE` file (`MIT license`) in [repository root](https://github.com/nilaoda/N_m3u8DL-RE?ref=www.bidianer.com) and README disclaimer (*"本软件基于MIT License 开源"*). |

---

## 3. Planned Direct Dependencies & Bundled Binaries

| Component / Library | Usage in OpenIDM | License | Integration Method | GPL-3.0 Compatibility |
| :--- | :--- | :--- | :--- | :--- |
| **Windows Win32 / WinHTTP / ComCtl32 / DPAPI** | OS UI, HTTP/2 networking, IPC, encryption | Windows OS System Libraries | Dynamic linking against Windows 11 OS DLLs | Compatible under GPL-3.0 Section 1 "System Libraries" exception. |
| **SQLite 3** (`sqlite3.c`) | Persistent task, segment, queue, and settings store | Public Domain (` blessing `) | Compiled directly into `openidm.exe` | Fully compatible with all licenses. |
| **nlohmann/json** (or **yyjson**) | JSON-RPC 2.0 serialization & `meta.json` parsing | `MIT` | Header-only / static link in `openidm.exe` and `openidm-nmh.exe` | `MIT` is permissive and fully compatible with `GPL-3.0-or-later`. |
| **WIL (Windows Implementation Libraries)** | Header-only RAII wrappers for Win32 handles | `MIT` | Header-only in `openidm.exe` and `openidm-nmh.exe` | `MIT` is fully compatible with `GPL-3.0-or-later`. |
| **GoogleTest (`gtest`)** | C++20 unit and integration test runner | `BSD-3-Clause` | Linked into test binaries only (`tests/`) | `BSD-3-Clause` is fully compatible with `GPL-3.0-or-later`. |
| **N_m3u8DL-RE** (`N_m3u8DL-RE.exe`) | Out-of-process HLS/DASH/MSS downloader | `MIT` | Separate executable invoked via `CreateProcessW` | `MIT` permits redistribution in source or binary form provided the copyright notice and permission notice are included. |
| **FFmpeg** (`ffmpeg.exe`) | Out-of-process media stream muxer (`--ffmpeg-binary-path`) | `LGPL-2.1-or-later` or `GPL-3.0-or-later` (depending on build flags) | Separate executable invoked via `CreateProcessW` | Compatible. If bundling a `GPL-3.0` build of `ffmpeg.exe`, the installer must provide the GPL-3.0 text and a written offer or direct link to the exact corresponding FFmpeg source code release. |

---

## 4. Component Reuse vs. Clean-Room Implementation Rules

1. **Brisk HTTP Download Engine (`BrisklyDev/brisk` & `BrisklyDev/brisk_download_engine`):**
   - We are **not** copying Dart/Flutter source code. We are implementing the segmented HTTP engine natively in C++20 on top of `WinHTTP` and Windows I/O Completion Ports (`docs/DOWNLOAD_ENGINE_DESIGN.md`).
   - Even though algorithmic concepts (dynamic connection spawning, work-stealing byte range splitting, stall reset) are not copyrightable in most jurisdictions, adopting `GPL-3.0-or-later` for OpenIDM ensures complete license harmony even if any structural state-machine logic is adapted from Brisk.
2. **Brisk Browser Extension (`BrisklyDev/brisk-browser-extension`):**
   - Because `brisk-browser-extension` did not display an explicit root `LICENSE` file in our inspection of commit `4f60c7f`, **no code may be copied verbatim from `brisk-browser-extension`** until its license is verified in `package.json` or confirmed by the author. OpenIDM's browser extension must be written from scratch in TypeScript following [docs/BROWSER_EXTENSION.md](docs/BROWSER_EXTENSION.md).
3. **Cat Catch (`xifangczy/cat-catch`):**
   - Cat Catch v2.x is `GPL-3.0`. Because the OpenIDM Browser Extension is also licensed under `GPL-3.0-or-later`, MIME/manifest detection patterns or adapted helper routines from Cat Catch are license-compatible **provided** original copyright notices (`Copyright (c) xifangczy and contributors`) and `GPL-3.0` headers are preserved.
4. **N_m3u8DL-RE (`nilaoda/N_m3u8DL-RE`) & Transitive Decryption Tools:**
   - `N_m3u8DL-RE` is `MIT` licensed and can be bundled as an unmodified worker executable (`N_m3u8DL-RE.exe`).
   - **Important Restriction on Bento4 (`mp4decrypt`) / Shaka Packager:** Upstream `N_m3u8DL-RE` supports invoking external binaries (`mp4decrypt` from Bento4, which is GPL-2.0, or `shaka-packager`, BSD-3-Clause) when `--key` is passed. Because OpenIDM strictly prohibits DRM circumvention and only supports standard unencrypted streams or standard HLS AES-128 streams (which `N_m3u8DL-RE` decrypts internally via .NET `System.Security.Cryptography.Aes` or `ffmpeg`), OpenIDM **does not bundle `mp4decrypt.exe` or `shaka-packager.exe`**. This eliminates both GPL-2.0-only vs. GPL-3.0 compatibility friction and DMCA/anti-circumvention legal risk.

---

## 5. Required Verification Actions Before First Public Release

- [x] **VR-LIC-01:** Inspect `package.json` inside `BrisklyDev/brisk-browser-extension` (`4f60c7f`) to confirm whether an SPDX license field is declared; clean-room implementation enforced for `extension/`.
- [x] **VR-LIC-02:** Generate `THIRD_PARTY_NOTICES.md` in the installer root bundling exact copyright notices and license texts for `N_m3u8DL-RE` (MIT), `FFmpeg` (GPL-3.0 / LGPL-2.1), `SQLite` (Public Domain), `WIL` (MIT), and `nlohmann/json` (MIT).
- [x] **VR-LIC-03:** Mirror the exact source code archives of any bundled `ffmpeg.exe` and `N_m3u8DL-RE.exe` releases in the project's GitHub Releases page to satisfy GPL-3.0 Section 6 (Conveying Non-Source Forms) without relying on third-party upstream repositories remaining online indefinitely.

---

## 6. Documented Legal Review & Clean-Room Policy (LEGAL-01)

- **Audit Reference:** Finding `LEGAL-01` in `FLUX_DOWNLOAD_MANAGER_CONSOLIDATED_AUDIT.md`.
- **Policy Statement:** Flux Download Manager is an independent, native C++20 and Win32 implementation. No proprietary decompiled C/C++ routines, reverse-engineered binaries, or copyrighted code from Internet Download Manager (Tonec Inc.) may be incorporated into `src/`, `extension/`, or any shipping binary deliverables.
- **Clean-Room Verification:** All HTTP networking, segmentation, persistence, IPC, and UI systems are implemented strictly clean-room based on open standards (RFC 7230–7235, RFC 3986), Microsoft Windows SDK documentation (`WinHTTP`, `Direct2D`, `GDI`, `ComCtl32`), and declared open-source dependencies (`Brisk`, `Cat Catch`, `N_m3u8DL-RE`, `SQLite`).
- **Research Artifact Quarantine:** Historical research artifacts located in `idm audit/` are strictly non-shipping reference files utilized solely for interoperability discovery. They are excluded from the Inno Setup installer (`installer/radium.iss`), the standalone portable archive (`dist/Flux-v0.3.0-win-x64-portable.zip`), and shipping releases.
