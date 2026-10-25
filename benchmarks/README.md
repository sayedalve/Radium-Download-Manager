# Radium Download Manager — Benchmark Suite & Reproducibility Guide

Welcome to the Radium Download Manager benchmarking directory. This suite provides tools and verified datasets to evaluate:
1. **Real-world Internet Throughput & Adaptive Rate Limiting (Hetzner Speedtest 100 MiB)**.
2. **Controlled RFC 7233 Loopback Multi-Connection Scaling (1, 4, 8, 16, 32 connections)**.
3. **HTTP 429 Adaptive Concurrency Reduction & Work-Stealing Segment Preservation**.
4. **Competitor & Prior Application Comparisons (Internet Download Manager, aria2, and legacy Flux versions)**.

---

## 1. Directory Structure

```
benchmarks/
├── README.md                           # This guide: methodology, metrics, commands
├── RESULTS.md                          # Full narrative analysis of Hetzner 100MB benchmark
├── LATEST_COMPARISON.md                # Controlled competitor benchmark (aria2 vs Radium)
├── scripts/
│   ├── run_local_benchmark.py          # Deterministic local loopback benchmark runner
│   ├── run_real_internet_benchmark.py  # Live internet benchmark harness (IDM vs Radium)
│   ├── run-competitor-benchmarks.py    # Multi-tool competitor comparison runner (aria2, etc.)
│   ├── run_radium_vs_idm_8conns.py     # Controlled 10-trial Radium vs IDM 8-connection runner
│   └── run_radium_vs_idm_2gb.py        # Dedicated 2 GB Radium vs IDM benchmark runner
├── results/
│   ├── 20261021_hetzner_100mb_radium.json # Verified live Hetzner 100 MiB run metadata
│   ├── 20261021_radium_vs_idm_8conns_trials.json     # 10-trial raw metrics (Radium vs IDM)
│   ├── 20261021_radium_vs_idm_8conns_comparison.csv # 10-trial comparison spreadsheet
│   ├── 20261025_radium_vs_idm_2gb_trials.json       # 2 GB raw metrics (Radium vs IDM)
│   ├── 20261025_radium_vs_idm_2gb_comparison.csv   # 2 GB comparison spreadsheet
│   ├── local_benchmark_matrix.json     # Deterministic local connection matrix results
│   └── summary_comparison.csv          # Spreadsheet-ready comparative CSV
├── fixtures/                           # Deterministic test fixtures (fixture_1mb.bin, fixture_4mb.bin)
└── IDM_RADIUM_2GB_COMPARISON.md        # Real 2 GB benchmark comparison report
```

---

## 2. Benchmark Methodologies

### 2.1 Live Internet Benchmark (Hetzner 100 MiB)
- **Target URL:** `https://fsn1-speed.hetzner.com/100MB.bin`
- **Payload:** Exactly 104,857,600 bytes (100.00 MiB)
- **Authoritative SHA-256:** `d91a64cda4e30fe6b42e971eb8c6681ae73ddb11b6a8b60f4769ec47a8e22c44`
- **Methodology:** 
  1. Sequential execution (never simultaneous) to avoid local interface saturation.
  2. Measures total elapsed wall-clock seconds from download start to completion.
  3. Validates file size and cryptographic SHA-256 hash against the authoritative digest.
  4. Records configured, peak, and effective connection concurrency truthfully.

### 2.2 Deterministic Local Loopback Benchmarks
- **Server:** Built-in Python RFC 7233 multithreaded HTTP range server on `127.0.0.1`.
- **Purpose:** Eliminates external network jitter, ISP peering variations, and upstream server throttling to evaluate raw CPU efficiency, memory working set, and dynamic connection reuse.
- **Fault Injection:** Supports simulating HTTP 429 Too Many Requests when active connections exceed a defined concurrency ceiling, verifying automatic backoff and connection halving without data loss.

---

## 3. How to Reproduce

### 3.1 Run Deterministic Local Benchmark Suite
To test 1, 4, 8, 16 connections and simulated 429 adaptive throttling locally without internet access:
```powershell
python benchmarks/scripts/run_local_benchmark.py --suite --out benchmarks/results/local_benchmark_matrix.json
```

To run a single custom local test:
```powershell
# 64 MiB payload with 16 connections
python benchmarks/scripts/run_local_benchmark.py --size-mb 64 --conns 16
```

### 3.2 Run Real Internet Benchmark (Hetzner 100 MiB)
> [!NOTE]
> Live internet tests require an active internet connection and are strictly separated from local regression tests. Never run live tests in offline CI.

```powershell
# Run using the automated script:
python benchmarks/scripts/run_real_internet_benchmark.py

# Or run the controlled 10-trial Radium vs IDM benchmark (8 connections each):
python benchmarks/scripts/run_radium_vs_idm_8conns.py --trials 5

# Or run the 2 GB Radium vs IDM benchmark:
python benchmarks/scripts/run_radium_vs_idm_2gb.py --trials 2

# Or invoke the compiled Radium binary directly:
dist\Radium.exe --db benchmarks/real_internet_work/bench.db --out benchmarks/real_internet_work --connections 32 --download https://fsn1-speed.hetzner.com/100MB.bin
```

---

## 4. Governed Performance Results Summary

| Scenario / Target | Tool / Version | Concurrency (Cfg / Peak / Eff) | Duration | Speed (MiB/s) | Speed (Mbps) | SHA-256 Match | Status |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Hetzner 100 MiB (Live)** | Radium v0.3.2 | 32 / 21 / 1 | **120.95 s** | **0.83 MiB/s** | **6.94 Mbps** | **EXACT PASS** | **COMPLETED** |
| **Hetzner 100 MiB (Live)** | IDM 6.43.15 | 8 / 4 / 2 | **88.37 s** | **1.13 MiB/s** | **9.49 Mbps** | **EXACT PASS** | **COMPLETED** |
| **Hetzner 100 MiB (Live)** | Flux v0.3.1 (Hist.) | 1 / 1 / 1 | **73.17 s** | **1.37 MiB/s** | **11.46 Mbps** | **EXACT PASS** | **COMPLETED** |
| **Hetzner 100 MiB (Live)** | Flux v0.3.1 (Hist.) | 32 / 32 / 0 | 240.25 s | N/A | N/A | Incomplete | FAILED (429) |
| **256 MiB Loopback** | Radium / Flux | 8 / 5 / 8 | **0.897 s** | **285.36 MiB/s** | **2393.8 Mbps** | **EXACT PASS** | **COMPLETED** |
| **256 MiB Loopback** | aria2 1.37.0 | 16 / 13 / 16 | **1.382 s** | **185.25 MiB/s** | **1554.0 Mbps** | **EXACT PASS** | **COMPLETED** |
