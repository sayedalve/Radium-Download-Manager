# Confirmation Benchmark Report: 500 MB Live Trial with Reverse Execution Order

**Test Date:** 2026-10-26 12:35 – 12:41 UTC+6  
**Test URL:** `https://sg-lg.regxa.com/500MB.test`  
**Expected Size:** 500,000,000 bytes  
**Verified SHA-256:** `b16f001a0731c8636f163586c16276b3714b8977242b67ae05fa60b96f685ebc`  
**Execution Order:** **Radium (Trial 1) -> IDM (Trial 2)** [Reversed from previous run]  
**Radium Version:** 0.3.5 (Adaptive Range Splitting Policy)  
**IDM Version:** 6.43.15 (Build 2)  

---

## 1. Executive Summary

This confirmation trial was conducted to test whether Radium's improved performance in Trial 4 was reproducible when the execution order was **reversed** (running Radium first, then IDM).

### Measured Outcome
- **Radium Transfer Time:** **140.09 s (3.57 MB/s / 3.40 MiB/s)**
- **IDM Transfer Time:** **173.46 s (2.88 MB/s / 2.75 MiB/s)**
- **Delta:** Radium completed **33.37 seconds faster (-19.2% transfer time)** than IDM.
- **Reproducibility:** Radium finished faster than IDM in **both** back-to-back live trials:
  - Trial 4 (IDM first): Radium was **36.46 s faster (-16.5%)**.
  - Trial 5 (Radium first): Radium was **33.37 s faster (-19.2%)**.
- **Integrity:** Both applications wrote exactly 500,000,000 bytes and matched the reference SHA-256 hash.

---

## 2. Head-to-Head Comparison: Trial 4 vs. Trial 5

| Metric | Trial 4: IDM First | Trial 5: Radium First (Confirmation) |
| :--- | :---: | :---: |
| **Execution Order** | IDM $\rightarrow$ Radium | **Radium $\rightarrow$ IDM** |
| **Radium Transfer Time** | 184.75 s | **140.09 s** |
| **IDM Transfer Time** | 221.21 s | **173.46 s** |
| **Time Delta (Radium vs. IDM)** | **-36.46 s (-16.5%)** | **-33.37 s (-19.2%)** |
| **Radium Throughput** | 2.71 MB/s | **3.57 MB/s** |
| **IDM Throughput** | 2.26 MB/s | **2.88 MB/s** |
| **Radium Average TCP Conns** | 7.9 | **7.7** |
| **IDM Average TCP Conns** | 7.7 | **7.7** |
| **Radium Range Splits** | 20 | **25** (vs. 33 in baseline) |
| **Radium Discarded Data (Slippage)** | 2.04 MB (0.41%) | **2.74 MB (0.55%)** (vs. 4.76 MB in baseline) |
| **Radium Average CPU Utilization** | 11.0% | **12.1%** |
| **IDM Average CPU Utilization** | 16.4% | **16.5%** |
| **Radium Peak Working Set RAM** | 20.5 MB | **20.4 MB** |
| **IDM Peak Working Set RAM** | 53.8 MB | **122.2 MB** |
| **SHA-256 Bitwise Match** | `PASSED` | `PASSED` |

---

## 3. Worker Workload Distribution (Radium Confirmation Trial)

All 8 workers remained active, productive, and balanced across the entire 140.09 seconds:

| Worker ID | Data Volume Written | Percentage of Total File | Active Request Duration |
| :---: | :---: | :---: | :---: |
| **Worker 0** | 55.59 MiB (58,294,272 B) | 11.7% | 139.22 s (99.4%) |
| **Worker 1** | 66.25 MiB (69,464,064 B) | 13.9% | 132.25 s (94.4%) |
| **Worker 2** | 49.34 MiB (51,736,576 B) | 10.3% | 137.39 s (98.1%) |
| **Worker 3** | 58.05 MiB (60,874,752 B) | 12.2% | 137.79 s (98.4%) |
| **Worker 4** | 53.20 MiB (55,779,328 B) | 11.2% | 131.13 s (93.6%) |
| **Worker 5** | 52.47 MiB (55,021,568 B) | 11.0% | 128.96 s (92.1%) |
| **Worker 6** | 75.72 MiB (79,394,048 B) | 15.9% | 138.03 s (98.5%) |
| **Worker 7** | 66.22 MiB (69,435,392 B) | 13.9% | 132.68 s (94.7%) |
| **Total** | **500,000,000 B** | **100.0%** | — |

- Every worker contributed between **49.3 MiB and 75.7 MiB**.
- Worker active duty cycles ranged from **92.1% to 99.4%**. Zero worker starvation occurred.

---

## 4. Chronological Progression Across All Five 500 MB Benchmarks

| Trial | Scenario / Scheduler State | Execution Order | IDM Time | Radium Time | Gap (Radium vs IDM) | Radium Slippage Bytes |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: |
| **Trial 1** | Pre-Fix: Workers died after ~1s | Radium $\rightarrow$ IDM | 103.90 s (4.81 MB/s) | 128.48 s (3.89 MB/s) | +24.58 s (+23.6%) | N/A |
| **Trial 2** | Intermediate: Split lockout | IDM $\rightarrow$ Radium | 115.96 s (4.31 MB/s) | 161.16 s (3.10 MB/s) | +45.20 s (+38.9%) | N/A |
| **Trial 3** | Validated: Work balancing (256K) | IDM $\rightarrow$ Radium | 137.36 s (3.64 MB/s) | 146.79 s (3.41 MB/s) | +9.43 s (+6.8%) | 4.76 MB (0.95%) |
| **Trial 4** | Adaptive: Ramp 256K / Steal 1M | IDM $\rightarrow$ Radium | 221.21 s (2.26 MB/s) | 184.75 s (2.71 MB/s) | **-36.46 s (-16.5%)** | 2.04 MB (0.41%) |
| **Trial 5** | **Adaptive Confirmation (Reversed)** | **Radium $\rightarrow$ IDM** | **173.46 s (2.88 MB/s)** | **140.09 s (3.57 MB/s)** | **-33.37 s (-19.2%)** | **2.74 MB (0.55%)** |

---

## 5. Confirmed Facts vs. Balanced Scientific Conclusions

### Confirmed Facts
1. **The Performance Advantage is Reproducible Across Execution Orders:**
   - In Trial 4 (IDM first), Radium finished 36.46 seconds faster (-16.5%).
   - In Trial 5 (Radium first), Radium finished 33.37 seconds faster (-19.2%).
   - The ~33–36 second advantage was not an artifact of running second or riding on cached routes.
2. **Reduced Splitting Overhead is Maintained:**
   - Pre-adaptive baseline: 33 splits, 4.76 MB slippage (0.95%).
   - Adaptive Confirmation: 25 splits, 2.74 MB slippage (0.55%), a **42.4% reduction in discarded data**.
3. **Hardware Efficiency:** Radium consumed 20.4 MB peak RAM (vs. 122.2 MB for IDM, a **6x reduction**) and used ~25% less CPU (12.1% vs. 16.5%).
4. **Data Integrity:** Exact bitwise match verified on both 500 MB downloads (`b16f00...5ebc`).

### Balanced Conclusions & Governance
- Two consecutive trials demonstrate that the adaptive range splitting policy produces a measurable, repeatable speed improvement over the previous baseline on this network link.
- However, two trials do not constitute general superiority across all network topologies, CDNs, packet loss rates, and server throttling rules. Network speeds to the Singapore test server showed substantial variation across the day (Trial 1 transfer speeds were ~4.8 MB/s while Trial 4 and 5 speeds were ~2.7–3.6 MB/s). Continued validation across diverse endpoints will remain part of ongoing release engineering.
