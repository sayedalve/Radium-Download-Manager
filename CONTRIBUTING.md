# Contributing to OpenIDM

**Document Status:** Proposed Contribution Policy  
**Last Reviewed:** 2026-10-16  
**Related Documents:** [AGENTS.md](AGENTS.md), [docs/LICENSE_REVIEW.md](docs/LICENSE_REVIEW.md), [docs/DEVELOPMENT_SETUP.md](docs/DEVELOPMENT_SETUP.md), [docs/TEST_STRATEGY.md](docs/TEST_STRATEGY.md)

---

## 1. Before You Begin

1. Read [README.md](README.md) and [docs/INDEX.md](docs/INDEX.md) to understand the current project phase and architecture.
2. Check [docs/REPOSITORY_AUDIT.md](docs/REPOSITORY_AUDIT.md) and [docs/ROADMAP.md](docs/ROADMAP.md) to see which milestone is currently active.
3. If you are using an AI coding assistant or autonomous agent, ensure it reads [AGENTS.md](AGENTS.md) before modifying any file.
4. For unresolved design trade-offs, review [docs/OPEN_QUESTIONS.md](docs/OPEN_QUESTIONS.md) before opening a pull request that locks in an unreviewed design choice.

---

## 2. Development Workflow

### 2.1 Branch Naming
Use descriptive branch prefixes tied to roadmap milestones or issue numbers:
- `feat/m1-winhttp-engine` — New feature implementation tied to a requirement ID (`FR-xxx`).
- `fix/ipc-pipe-reconnect` — Bug fix with regression test.
- `sec/path-sanitization` — Security hardening tied to [docs/SECURITY_THREAT_MODEL.md](docs/SECURITY_THREAT_MODEL.md).
- `docs/adr-update` — Documentation or ADR updates.
- `test/hls-discontinuity` — Test harness or fixture additions.

### 2.2 Commit Message Conventions
Follow Conventional Commits with explicit requirement or risk references where applicable:
```text
feat(engine-http): implement dynamic work-stealing byte range split (FR-003)

- Split largest remaining active segment when a worker finishes its range
- Enforce 1 MiB minimum split remainder to avoid tail thrashing
- Checkpoint segment split atomically in SQLite task_segments table

Refs: FR-003, ADR-0005
```

---

## 3. Code Review & Pull Request Requirements

Every pull request must satisfy the following gates before merge:

1. **Requirement Traceability:** Reference the relevant `FR-xxx` or `NFR-xxx` identifier from [docs/PRODUCT_REQUIREMENTS.md](docs/PRODUCT_REQUIREMENTS.md).
2. **Documentation Synchronization:** Update all affected design documents listed in Section 9 of [AGENTS.md](AGENTS.md). Update [docs/FEATURE_MATRIX.md](docs/FEATURE_MATRIX.md) and [docs/TEST_MATRIX.md](docs/TEST_MATRIX.md) when implementation or test status changes.
3. **Security Checklist:**
   - If the PR touches IPC (`docs/NATIVE_MESSAGING_AND_IPC.md`), subprocess spawning (`CreateProcessW`), file path resolution, or browser extension message handling, explicitly verify compliance with [docs/SECURITY_THREAT_MODEL.md](docs/SECURITY_THREAT_MODEL.md).
   - Confirm no sensitive headers (`Cookie`, `Authorization`) are written to logs or passed on process command lines.
4. **Honest Test Reporting:** State clearly in the PR description:
   - What tests were executed and on which OS/browser versions.
   - What scenarios remain unverified and why.

---

## 4. Dependency & Third-Party Code Policy

1. Do not introduce new third-party libraries without updating [docs/TECHNOLOGY_STACK.md](docs/TECHNOLOGY_STACK.md) and [docs/LICENSE_REVIEW.md](docs/LICENSE_REVIEW.md).
2. Any adapted algorithm or code from upstream projects ([Brisk](https://github.com/BrisklyDev/brisk), [Cat Catch](https://github.com/xifangczy/cat-catch), [N_m3u8DL-RE](https://github.com/nilaoda/N_m3u8DL-RE)) must document its exact upstream repository URL, commit SHA/tag, and license in the file header and in [docs/UPSTREAM_PROJECT_ANALYSIS.md](docs/UPSTREAM_PROJECT_ANALYSIS.md).

---

## 5. Reporting Issues

- **Bug Reports:** Include Windows 11 build number (`winver`), browser name and version, extension version, sanitized task log (redacting cookies/tokens per [docs/PRIVACY_AND_DATA_HANDLING.md](docs/PRIVACY_AND_DATA_HANDLING.md)), and whether the failure occurred on an ordinary HTTP download or an HLS/DASH stream.
- **Security Vulnerabilities:** Do not post exploitable RCE, path traversal, or local privilege escalation payloads in public issues. Follow the threat reporting guidelines in [docs/SECURITY_THREAT_MODEL.md](docs/SECURITY_THREAT_MODEL.md).

---

## 6. Contribution Licensing

By submitting a contribution to this repository, you agree that your contribution is licensed under the project's primary license (**GNU General Public License v3.0 or later**, `GPL-3.0-or-later`), as detailed in [docs/LICENSE_REVIEW.md](docs/LICENSE_REVIEW.md).
