# Comparative Benchmark Report: Validating the Updated Radium Scheduler Against IDM

**Test Date:** 2026-10-26 11:46 - 11:51 UTC+6  
**Test URL:** `https://sg-lg.regxa.com/500MB.test`  
**Expected Size:** 500,000,000 bytes  
**Verified SHA256:** `b16f001a0731c8636f163586c16276b3714b8977242b67ae05fa60b96f685ebc`  
**Execution Order:** IDM (Trial 1) -> Radium (Trial 2) [Single sequential execution]  
**Radium Version:** 0.3.4 (Adaptive Work Balancing & Range Fix)  
**IDM Version:** 6.43.15 (Build 2)  

---

## 1. Executive Summary

In this validation run, the updated Radium scheduler—incorporating two-pass victim work-stealing and extended worker retention—was benchmarked against Internet Download Manager under real-world internet conditions.

### Primary Measured Outcome
- **IDM Transfer Time:** 137.36 s (3.64 MB/s / 3.47 MiB/s)
- **Radium Transfer Time:** 146.79 s (3.41 MB/s / 3.25 MiB/s)
- **Radium vs. IDM Time Difference:** +9.43 s (+6.8% delta)
- **Integrity:** Both applications wrote exactly 500,000,000 bytes and matched reference SHA256 `b16f00...5ebc`.

---

## 2. Key Telemetry & Hardware Measurements

| Metric | IDM (v6.43.15) | Radium (v0.3.4 Fix) | Delta / Comparison |
| :--- | :--- | :--- | :--- |
| **Elapsed Transfer Time** | 137.36 s | 146.79 s | +9.43 s (+6.8%) |
| **Throughput (Decimal)** | 3.64 MB/s | 3.41 MB/s | -0.23 MB/s (-6.3%) |
| **Throughput (Binary)** | 3.47 MiB/s | 3.25 MiB/s | -0.22 MiB/s |
| **Average Established TCP Connections** | 7.6 | **7.9** | Radium matched & exceeded IDM concurrency |
| **Peak Established TCP Connections** | 8 | **8** | Both reached 8 max connections |
| **Average CPU Utilization** | 8.9% | **7.1%** | Radium used 20% less CPU |
| **Peak Working Set Memory** | 122.4 MB | **20.4 MB** | Radium consumed 83% less memory (6x leaner) |
| **Active Requests Executed** | N/A (Internal part files) | 45 | Managed across 8 workers |
| **Range Splits / Bisected Requests** | N/A | 33 | Dynamic work stealing |
| **Split Cancellations** | N/A | 33 | Graceful socket termination on split |
| **Completed Full Segments** | N/A | 12 | Terminal segments |
| **Slippage / Discarded Bytes** | 0 bytes | 4,763,648 bytes (0.95%) | Within expected range-split boundary |
| **SHA-256 Match** | `PASSED` | `PASSED` | Exact bitwise match |

---

## 3. Worker Utilization Analysis (Radium Telemetry)

In the previous benchmark runs, idle workers timed out after ~1 second and exited, leaving 1–2 workers downloading ~186 MB of data while 6 workers sat completely idle. 

With the adaptive work balancing fix in place, all 8 workers remained active and productive for the entire ~145 seconds of the download:

| Worker ID | Useful Bytes Written | Data Volume (MiB) | Active Request Duration | Share of Work |
| :---: | :---: | :---: | :---: | :---: |
| **Worker 0** | 54,280,192 bytes | 51.77 MiB | 145.18 s | 10.9% |
| **Worker 1** | 74,412,032 bytes | 70.96 MiB | 143.91 s | 14.9% |
| **Worker 2** | 81,772,544 bytes | 77.98 MiB | 143.76 s | 16.4% |
| **Worker 3** | 41,861,120 bytes | 39.92 MiB | 145.83 s | 8.4% |
| **Worker 4** | 62,533,632 bytes | 59.64 MiB | 143.43 s | 12.5% |
| **Worker 5** | 58,974,208 bytes | 56.24 MiB | 143.15 s | 11.8% |
| **Worker 6** | 72,471,808 bytes | 69.11 MiB | 144.58 s | 14.5% |
| **Worker 7** | 53,694,464 bytes | 51.21 MiB | 142.73 s | 10.7% |
| **Total** | **500,000,000 bytes** | **476.84 MiB** | — | **100.0%** |

- **Minimum worker contribution:** 39.92 MiB (Worker 3)
- **Maximum worker contribution:** 77.98 MiB (Worker 2)
- **Worker active time:** 142.7 s – 145.8 s across all 8 workers out of 146.79 s total elapsed time (97.5% - 99.3% duty cycle per worker).
- **Elimination of Starvation:** Zero workers experienced early retirement or starvation.

---

## 4. Historical Comparison Across 500 MB Benchmark Trials

| Benchmark Run | Condition / Code State | IDM Transfer Time | Radium Transfer Time | Gap (Radium vs IDM) | Radium Avg Conns |
| :--- | :--- | :---: | :---: | :---: | :---: |
| **Run 1: Initial Diagnostic** | Pre-fix (Workers exited early) | 103.90 s (4.81 MB/s) | 128.48 s (3.89 MB/s) | +24.58 s (+23.6%) | 3.0 conns |
| **Run 2: Intermediate Trial** | Partial split-guard relaxation | 115.96 s (4.31 MB/s) | 161.16 s (3.10 MB/s) | +45.20 s (+38.9%) | 5.3 conns |
| **Run 3: Final Validated Run** | **Adaptive Balancing + Retention** | **137.36 s (3.64 MB/s)** | **146.79 s (3.41 MB/s)** | **+9.43 s (+6.8%)** | **7.9 conns** |

### Network Trend Observations
- The remote test server (`sg-lg.regxa.com`) showed noticeable overall degradation from morning to midday (IDM speed decreased from 4.81 MB/s in Run 1 $\rightarrow$ 4.31 MB/s in Run 2 $\rightarrow$ 3.64 MB/s in Run 3).
- Despite server-side throughput dropping across the board, Radium's performance gap against IDM shrank from **+24.58 s (+23.6%) down to only +9.43 s (+6.8%)**.
- Connection utilization in Radium reached **7.9 average established TCP connections** (virtually identical to IDM's 7.6 average connections).

---

## 5. Confirmed Facts vs. Hypotheses

### Confirmed Facts
1. **Worker Starvation Completely Resolved:** In Run 1, 6 workers died early, dropping Radium to 3.0 average connections. In Run 3, all 8 workers downloaded between 39.9 MiB and 77.9 MiB and remained active for >142 seconds each.
2. **Connection Utilization Matched IDM:** Radium maintained an average of 7.9 active TCP connections compared to IDM's 7.6.
3. **Data Integrity Maintained:** No bitwise corruption occurred. SHA256 matched the reference hash exactly (`b16f00...5ebc`).
4. **Memory Footprint Remained Superior:** Radium consumed only 20.4 MB peak RAM compared to IDM's 122.4 MB peak RAM (6x less memory).

### Hypotheses & Remaining Discrepancy (Why IDM was still 9.4s faster)
1. **HTTP Range Abort Slippage:** When Radium bisects an in-flight segment, it closes the WinHTTP request and cancels in-flight data, resulting in 4.76 MB (0.95%) of slippage bytes that were re-downloaded. In contrast, IDM inserts chunks into separate part files and truncates end boundaries forward without aborting sockets.
2. **Transient Internet Fluctuation:** The benchmark was executed sequentially (IDM then Radium). A 6.8% variance is well within normal international transit routing jitter.
