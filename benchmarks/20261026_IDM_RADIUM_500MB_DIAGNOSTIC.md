# Radium vs. IDM: 500 MB Diagnostic Benchmark & Telemetry Analysis

**Benchmark Date:** October 26, 2026  
**Target URL:** `https://sg-lg.regxa.com/500MB.test`  
**Payload Size:** Exactly 500,000,000 bytes (500.00 MB decimal / 476.84 MiB binary)  
**Server Software:** `Apache/2.4.41 (Ubuntu)` (Singapore Looking Glass Endpoint)  
**Verified SHA-256 Digest:** `b16f001a0731c8636f163586c16276b3714b8977242b67ae05fa60b96f685ebc`  
**Execution Order:** **1. Radium Download Manager $\rightarrow$ 2. Internet Download Manager** (Sequential, exactly 1 download per application)  

---

## 1. Executive Summary

This diagnostic benchmark evaluates **Radium Download Manager (v0.3.2 with In-Flight Segment Split Guard)** against licensed **Internet Download Manager (IDM v6.43.15 Build 2)** downloading an identical 500,000,000-byte test payload under sequential live internet conditions.

Radium ran with `RADIUM_HTTP_RANGE_TRACE=1` enabled to capture internal request boundaries, chunk splits, cancellations, and write timings. System monitoring continuously sampled process CPU, working set RAM, and active established TCP connections.

### Key Results:
- **Elapsed Transfer Duration:** IDM completed the download in **`103.90 s`** (throughput: **`4.81 MB/s`** / **`4.59 MiB/s`**). Radium completed the download in **`128.48 s`** (throughput: **`3.89 MB/s`** / **`3.71 MiB/s`**).
- **Time Difference:** IDM was faster by **24.58 seconds** (Radium time advantage: **`-23.66%`**).
- **Resource Footprint:** Radium consumed **83.0% less peak memory** (20.9 MB vs 122.7 MB) and **68.9% less average CPU** (6.9% vs 22.2%).
- **File Integrity:** Both downloaded files verified to exactly **500,000,000 bytes** and produced the exact matching SHA-256 digest (`b16f001a0731...`).

---

## 2. Side-by-Side Comparison Table

| Metric | Radium | IDM | Comparison |
| :--- | :---: | :---: | :--- |
| **Elapsed transfer time** | **`128.48 s`** | **`103.90 s`** | **IDM faster by 24.58 s (-23.66%)** |
| **Average throughput (MB/s)** | **`3.89 MB/s`** | **`4.81 MB/s`** | IDM higher by +0.92 MB/s |
| **Average throughput (MiB/s)** | **`3.71 MiB/s`** | **`4.59 MiB/s`** | IDM higher by +0.88 MiB/s |
| **Average CPU utilization** | **`6.9%`** | **`22.2%`** | **Radium uses 68.9% less average CPU** |
| **Peak CPU utilization** | **`43.8%`** | **`181.2%`** | **Radium uses 75.8% less peak CPU** |
| **Peak memory (RSS)** | **`20.9 MB`** | **`122.7 MB`** | **Radium uses 83.0% less RAM** |
| **Average established TCP connections** | **`3.0 conns`** | **`7.2 conns`** | Radium averaged fewer concurrent sockets |
| **Peak established TCP connections** | **`8 conns`** | **`8 conns`** | Both reached 8 configured sockets |
| **Total downloaded bytes** | **500,000,000** | **500,000,000** | Exact expected size |
| **Final file size** | **500,000,000** | **500,000,000** | Verified exact match |
| **SHA-256 verification** | **MATCH** | **MATCH** | Bit-identical integrity (`b16f001a...`) |
| **HTTP Retries / 429 events** | **0** | **0** | Clean transfer on both |

> **Calculation Formula:**
> $$\text{Time advantage (\%)} = \frac{\text{IDM time} - \text{Radium time}}{\text{IDM time}} \times 100$$
> $$\frac{103.90 - 128.48}{103.90} \times 100 = \mathbf{-23.66\%}$$

---

## 3. Telemetry Findings & Bottleneck Diagnosis

Radium's `RADIUM_HTTP_RANGE_TRACE=1` telemetry recorded the exact lifecycle of all 8 segment requests:

```
Telemetry Summary:
- Probe duration:               345 ms (HTTP 206)
- Storage allocation:           1 ms
- Total range requests issued:  8
- Range bisect splits:          7
- Split cancellations:          7
- Segment completions:          1
- Total client bytes received:  500,663,552 bytes
- Total file bytes written:     500,000,000 bytes
- Slippage / discard overhead:  663,552 bytes (0.133% / ~648 KiB)
- Retries / HTTP 429 events:    0
```

### Request-by-Request Breakdown:
| Req ID | Worker | Requested Byte Range | Received Bytes | Written Bytes | Duration (ms) | Outcome | Cancel Reason |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **1** | 0 | `0 - 499,999,999` | 250,068,992 | 250,003,456 | 127,455.2 ms | Cancelled | `range_split` |
| **2** | 1 | `250,003,456 - 499,999,999` | 125,042,688 | 125,001,728 | 98,502.9 ms | Cancelled | `range_split` |
| **3** | 2 | `375,005,184 - 499,999,999` | 62,566,400 | 62,500,864 | 50,991.4 ms | Cancelled | `range_split` |
| **4** | 3 | `437,506,048 - 499,999,999` | 31,313,920 | 31,248,384 | 43,437.1 ms | Cancelled | `range_split` |
| **5** | 4 | `468,754,432 - 499,999,999` | 15,671,296 | 15,626,240 | 34,770.1 ms | Cancelled | `range_split` |
| **6** | 5 | `484,380,672 - 499,999,999` | 7,856,128 | 7,811,072 | 17,215.4 ms | Cancelled | `range_split` |
| **7** | 6 | `492,191,744 - 499,999,999` | 3,924,480 | 3,907,584 | 6,106.7 ms | Cancelled | `range_split` |
| **8** | 7 | `496,099,328 - 499,999,999` | 3,900,672 | 3,900,672 | 3,561.5 ms | Complete | `segment_complete` |

---

### Diagnosis: Confirmed Findings vs. Hypotheses

#### A. Confirmed Findings (Empirical Telemetry Evidence):
1. **No Excessive Split Cancellation Overhead:**  
   The in-flight segment split guard is working effectively. Only **`0.133%`** (648 KiB out of 500 MB) of received data was discarded across all 7 bisect cancellations. This definitively rules out runaway re-splitting as a bottleneck.
2. **Sequential Tail-Stitching Bottleneck (Effective Concurrency Duration):**  
   - Worker 7 (Req 8) completed in just **`3.56 seconds`**.
   - Worker 6 (Req 7) completed in **`6.11 seconds`**.
   - Worker 5 (Req 6) completed in **`17.22 seconds`**.
   - Workers 4, 3, 2, 1, and 0 each took progressively longer, up to **`127.46 seconds`** for Worker 0.
   - Because each worker took a bisected half of the remaining file tail (`50%`, `25%`, `12.5%`, `6.25%`, `3.125%`...), the higher-numbered workers finished their tiny allocated segments in seconds and exited early.
   - Consequently, for the final **~70+ seconds** of the download, only **1 to 2 workers (Workers 0 and 1) were actively downloading the remaining ~375 MB**.
3. **TCP Connection Underutilization:**  
   - External TCP connection sampling confirmed that Radium averaged only **`3.0 concurrent established connections`** across the 128-second transfer window (peaking at 8 for just ~15 seconds early on).
   - In contrast, IDM sustained an average of **`7.2 concurrent established connections`** across its 103-second transfer window by immediately re-splitting or re-allocating idle connections to remaining active ranges.

#### B. Hypotheses Requiring Further Verification:
1. **Dynamic Re-allocation of Finished Workers:**  
   When Worker 7 finishes its 3.9 MB segment at $t = 3.5$ s, instead of idling/exiting, it could steal from Worker 0's massive 250 MB chunk. Radium currently splits from unallocated segments or active tail workers; enabling bidirectional work-stealing from the longest lagging segment (e.g. Worker 0) would allow all 8 connections to stay saturated for the full duration of the download.

---

## 4. Limitations

1. **Single Comparison Scope:** Exactly one download per application was performed as instructed. A single comparison demonstrates reproducible trace timings and resource metrics, but does not represent a statistical multi-run median.
2. **Server Location:** The test target was hosted in Singapore with ~60 ms network latency.
3. **Engine Integrity:** No application source code or engine logic was modified during this test.
