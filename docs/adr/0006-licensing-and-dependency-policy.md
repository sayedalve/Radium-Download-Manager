# ADR-0006: Project Licensing and Third-Party Dependency Policy

- **Date:** 2026-10-16
- **Status:** Proposed
- **Deciders:** Principal Software Architect, Application Security Engineer, Release Engineer
- **Related Documents:** [../LICENSE_REVIEW.md](../LICENSE_REVIEW.md), [../TECHNOLOGY_STACK.md](../TECHNOLOGY_STACK.md), [../UPSTREAM_PROJECT_ANALYSIS.md](../UPSTREAM_PROJECT_ANALYSIS.md)

---

## Status

**Proposed** (Baseline Governance Policy).

## Context

OpenIDM combines architectural designs, detection patterns, and binary tools from multiple open-source ecosystems:
- [Brisk](https://github.com/BrisklyDev/brisk) is licensed under **`GPL-3.0`**.
- [Cat Catch (`cat-catch`)](https://github.com/xifangczy/cat-catch) v2.0+ is licensed under **`GPL-3.0`**.
- [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE) is licensed under **`MIT`**.
- [FFmpeg](https://ffmpeg.org/) builds are licensed under **`LGPL-2.1-or-later`** or **`GPL-3.0-or-later`**.

We need a clear project license and a strict dependency gate so future contributors and AI agents never introduce license incompatibilities, supply-chain vulnerabilities, or binary bloat.

## Decision

1. **Project License:** License all original OpenIDM source code (`src/`, `extension/`, `installer/`, `tests/`, `docs/`) under **GNU General Public License v3.0 or later (`GPL-3.0-or-later`)**.
2. **Dependency Allowlist Gate:**
   - **C++ Desktop (`vcpkg.json`):** Only `sqlite3` (Public Domain), `nlohmann-json` or `yyjson` (`MIT`), `wil` (`MIT`), and `gtest` (`BSD-3-Clause`, test-only) are pre-approved.
   - **Browser Extension (`extension/package.json`):** Zero runtime npm dependencies (`"dependencies": {}`). Only build-time `devDependencies` (`typescript`, `esbuild` or `rollup`, `@types/chrome`, `@types/firefox-webext-browser`, `vitest`, `playwright`) are permitted.
   - **Bundled Worker Binaries:** Only unmodified `N_m3u8DL-RE.exe` (`MIT`) and `ffmpeg.exe` (`LGPL-2.1+` / `GPL-3.0+`) are permitted. Do **not** bundle `mp4decrypt.exe` (Bento4, `GPL-2.0-only` without "or later" clause unless verified, and unnecessary given our strict no-DRM policy).
3. **Provenance & Attribution Tracking:**
   - Every file that adapts rules or code from an upstream `GPL-3.0` or `MIT` project must carry an explicit SPDX header (`// SPDX-License-Identifier: GPL-3.0-or-later`) and upstream attribution comment.
   - Because `BrisklyDev/brisk-browser-extension` (`v1.4.1`, `4f60c7f`) lacks a verified root `LICENSE` file, **zero code may be copied verbatim from `brisk-browser-extension`** until its license is formally verified.

## Alternatives Considered

1. **Licensing OpenIDM under MIT or Apache-2.0 (`Rejected`):**
   - *Why Rejected:* An MIT-licensed repository cannot incorporate or adapt `GPL-3.0` code or detection rule sets from `cat-catch` v2.x or `brisk`. Choosing `GPL-3.0-or-later` from day one ensures full compatibility with `GPL-3.0`, `LGPL-2.1+`, `MIT`, `BSD-3-Clause`, and `Apache-2.0` components.
2. **Allowing Unrestricted npm Runtime Packages in the Browser Extension (`Rejected`):**
   - *Why Rejected:* Third-party runtime npm packages in browser extensions with `<all_urls>` and `webRequest` permissions represent a severe supply-chain risk (`THREAT-010`) and trigger slow manual reviews on Chrome Web Store and addons.mozilla.org.

## Rationale

`GPL-3.0-or-later` aligns with the project vision (*"build a free and genuinely open source Windows download manager"*) while providing maximum legal compatibility with Brisk, Cat Catch, N_m3u8DL-RE, and FFmpeg.

## Consequences

- **Positive:** Clear legal compatibility across all reference projects; zero supply-chain exposure from runtime npm packages.
- **Negative:** Commercial closed-source forks cannot embed OpenIDM without releasing their source code under GPL-3.0 (which is an intentional feature for a genuinely open-source IDM alternative).

## Risks & Mitigations

- **Risk:** Distributing `ffmpeg.exe` (`GPL-3.0`) in the installer without providing corresponding FFmpeg source code violates GPL-3.0 Section 6.
- **Mitigation:** Archive the exact matching source tarball (`ffmpeg-x.y.z.tar.xz` and build script) alongside every GitHub Release that ships an installer containing `ffmpeg.exe` ([../LICENSE_REVIEW.md](../LICENSE_REVIEW.md)).
