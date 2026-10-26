# Radium Download Manager: Consolidated Benchmark Results & Verification Log

**Document Status:** Authoritative Benchmark Registry  
**Last Updated:** October 26, 2026  
**Scope:** Controlled performance benchmarks of Radium Download Manager against industry standards (Internet Download Manager, aria2, and legacy Flux baselines).

---

## 1. Benchmark Hierarchy & Status

To maintain transparent and reproducible technical claims, benchmark results in this repository are categorized into active verified results and archived historical results:

| Category | Benchmark Payload | Endpoint / Network | Tested Build | Status | Authoritative Report |
|---|---|---|---|---|---|
| **Active Comparative Benchmark** | 500 MB Test File (`500MB.test`, 500,000,000 bytes) | `https://sg-lg.regxa.com/500MB.test` (Live Internet) | Radium v0.3.5 (Dev Build with Adaptive Range Splitting) | **Active / Current** | [`benchmarks/adaptive_500mb_final/20261026_500MB_ADAPTIVE_FINAL_REPORT.md`](adaptive_500mb_final/20261026_500MB_ADAPTIVE_FINAL_REPORT.md) & [`benchmarks/adaptive_500mb_confirmation/20261026_500MB_CONFIRMATION_REPORT.md`](adaptive_500mb_confirmation/20261026_500MB_CONFIRMATION_REPORT.md) |
| **Archived Large-File Benchmark** | 2.14 GB AlmaLinux ISO (`AlmaLinux-8.10-x86_64-minimal.iso`) | `https://mirror.xeonbd.com/almalinux/8/...` (Live Internet) | Radium v0.3.2 (Production Release) | **Archived / Baseline** | [`benchmarks/IDM_RADIUM_2GB_COMPARISON.md`](IDM_RADIUM_2GB_COMPARISON.md) |
| **Archived Rate-Limited Benchmark** | 100 MiB Binary (`100MB.bin`, 104,857,600 bytes) | `https://fsn1-speed.hetzner.com/100MB.bin` (Falkenstein Rate-Limited) | Radium v0.3.2 (Production Release) | **Archived / Historical** | See Section 3 Below |
| **Controlled Local Loopback Matrix** | 16 MiB & 128 MiB Synthetic HTTP Payloads | Local RFC 7233 Python Server (Paced / Latency / Loss) | Radium v0.3.5 vs. Baseline | **Active / Engine Tuning** | [`benchmarks/results/20261026_ADAPTIVE_SPLITTING_EVALUATION.md`](results/20261026_ADAPTIVE_SPLITTING_EVALUATION.md) |

---

## 2. Active 500 MB Head-to-Head Comparison (Radium v0.3.5 Dev Build vs. IDM 6.43.15)

The active comparison evaluated the **Adaptive Range Splitting Policy** implemented in `src/brisk_engine/segment_scheduler.cpp`. The benchmark tested two sequential live internet downloads of an identical 500,000,000 byte binary payload with reversed execution order to balance international transit fluctuations.

- **Payload URL:** `https://sg-lg.regxa.com/500MB.test`
- **Expected Byte Count:** 500,000,000 bytes
- **Verified SHA-256:** `b16f001a0731c8636f163586c16276b3714b8977242b67ae05fa60b96f685ebc`
- **Workstation:** Windows 11 Pro 64-bit | Intel Core i5-10400F @ 2.90GHz (6C/12T) | 16 GB DDR4 RAM | NVMe SSD
- **Concurrency Configured:** 8 connections

### Measured Results:

| Trial | Execution Order | Radium Duration | IDM Duration | Completion Time Reduction | Throughput (Radium / IDM) | Peak RAM (Radium / IDM) | Memory Reduction | SHA-256 Match |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Trial 1** | IDM $\rightarrow$ Radium | **184.75 s** | 221.21 s | **16.48%** (-36.46 s) | 2.71 MB/s / 2.26 MB/s (+19.91%) | **20.5 MB** / 53.8 MB | **61.90%** | Exact Match |
| **Trial 2** | Radium $\rightarrow$ IDM | **140.09 s** | 173.46 s | **19.24%** (-33.37 s) | 3.57 MB/s / 2.88 MB/s (+23.96%) | **20.4 MB** / 122.2 MB | **83.31%** | Exact Match |
| **Mean** | Combined | **162.42 s** | 197.34 s | **17.69%** (-34.92 s) | 3.14 MB/s / 2.57 MB/s (+22.18%) | **20.4 MB** / 88.0 MB | **76.76%** | Exact Match |

*Formulas applied:*
- *Completion Time Reduction:* `(IDM Time - Radium Time) / IDM Time * 100`
- *Throughput Improvement:* `(Radium Throughput - IDM Throughput) / IDM Throughput * 100`
- *Memory Reduction:* `(IDM Memory - Radium Memory) / IDM Memory * 100`

---

## 3. Historical 100 MiB Alternating Benchmark (Radium v0.3.2 Baseline vs. IDM 6.43.15)

Conducted on October 21, 2026 across 10 alternating sequential trials (5 Radium, 5 IDM) downloading a 104,857,600 byte test file from Hetzner Falkenstein under server-side rate-limiting (`429 Too Many Requests`):

| Metric | Radium v0.3.2 (Production Release) | Internet Download Manager (IDM 6.43.15) | Comparison / Notes |
| :--- | :---: | :---: | :--- |
| **Configured Concurrency** | 8 connections | 8 connections | Baseline |
| **Effective Concurrency** | 1 connection (Fallback) | 3 connections (Selective drop) | IDM held 3 streams |
| **Median Duration** | **79.44 s** | **76.64 s** | IDM faster by 2.80 s (3.53%) |
| **Median Throughput** | **1.26 MiB/s** (10.56 Mbps) | **1.30 MiB/s** (10.95 Mbps) | Parity (< 4% variance) |
| **Data Integrity (SHA-256)** | 5 / 5 (100% exact match) | 5 / 5 (100% exact match) | Digest: `d91a64cda4e30fe...` |
| **Median Peak RAM (RSS)** | **18.30 MB** | **48.80 MB** | **Radium consumed 62.50% less RAM** |
| **Median Peak CPU Usage** | **15.6%** | **56.2%** | **Radium used 72.24% less peak CPU** |

---

## 4. Historical 2 GB Large-File Benchmark (Radium v0.3.2 Baseline vs. IDM 6.43.15)

Conducted on October 25, 2026 downloading an authentic 2,140,143,616 byte ISO (`AlmaLinux-8.10-x86_64-minimal.iso`) with 8 configured connections:

| Metric | Radium v0.3.2 | Internet Download Manager (IDM 6.43.15) | Comparison |
| :--- | :---: | :---: | :--- |
| **Median Duration** | **596.20 s** | **580.50 s** | IDM faster by 15.70 s (2.70%) |
| **Median Throughput** | **3.60 MiB/s** (3.78 MB/s) | **3.68 MiB/s** (3.86 MB/s) | IDM +2.2% throughput |
| **Data Integrity (SHA-256)** | 2 / 2 (100% exact match) | 2 / 2 (100% exact match) | Digest: `e524329700abe47...` |
| **Peak RAM (RSS)** | **~42–45 MB** | **~58–61 MB** | **Radium consumed ~26% less RAM** |
| **Peak CPU Usage** | **~3.8–4.1%** | **~5.2–5.8%** | **Radium used ~27% less CPU** |

Full report: [`benchmarks/IDM_RADIUM_2GB_COMPARISON.md`](IDM_RADIUM_2GB_COMPARISON.md).

---

## 5. Raw Evidence Retention & GitHub Actions Artifact Archives

To prevent repository clone bloat while preserving scientific evidence and reproducibility:
- **Consolidated Summaries & Reports:** Reusable test fixtures, Markdown reports, and consolidated audit summaries (such as [`benchmarks/results/LOCAL_BENCHMARK_TELEMETRY_CONSOLIDATED_REPORT.md`](results/LOCAL_BENCHMARK_TELEMETRY_CONSOLIDATED_REPORT.md)) remain tracked directly in Git.
- **Raw Multi-Repeat Trial Dumps & Telemetry Traces:** Individual per-run trace dumps (`*repeat*.json`, `summary*.json`, and generated CSV files totaling ~69 MB across the benchmarking passes) are preserved in downloadable GitHub Actions workflow artifacts:
  - **Archive Artifact Name:** `raw-benchmark-evidence-archive`
  - **Source Workflow Run:** [Run #40286662496](https://github.com/sayedalve/Radium-Download-Manager/actions/runs/40286662496)
  - **Retention:** 90 days (and re-uploaded on scheduled benchmark runs via `benchmark-comparison.yml` with a 30-day rolling window).
  - **Download via GitHub CLI:**
    ```powershell
    gh run download 40286662496 -n raw-benchmark-evidence-archive -D ./benchmarks_raw_evidence
    ```

---

## 6. Verification Script

To verify all mathematical calculations across the benchmark reports, run the automated verification script:

```powershell
python scripts/verify_benchmark_metrics.py
```
