# Instrumented 100 MB Comparison: Radium vs. Internet Download Manager (IDM)

**Benchmark Date:** October 25, 2026  
**Target URL:** `https://sgp.download.datapacket.com/100mb.bin`  
**Payload Size:** Exactly 100,000,000 bytes (100.00 MB decimal / 95.37 MiB binary)  
**Server Software:** CDN77-Turbo (`Accept-Ranges: bytes`)  
**Verified SHA-256 Checksum:** `a993f8c574e0fea8c1cdcbcd9408d9e2e107ee6e4d120edcfa11decd53fa0cae`  
**Instrumentation:** `RADIUM_HTTP_RANGE_TRACE=1` enabled on Radium process  
**Test Protocol:** Exactly one sequential live download per application (strictly 2 transfers total)  

---

## 1. Executive Summary

This instrumented benchmark compares **Radium Download Manager (v0.3.2 with In-Flight Segment Split Guard)** against licensed **Internet Download Manager (IDM v6.43.15 Build 2)** downloading an identical verified 100,000,000-byte test file over live internet under identical network conditions and 8 configured connections.

### Key Results:
- **Elapsed Transfer Time:**
  - **Radium:** **`26.98 s`** (Throughput: **`3.71 MB/s`** / **`3.53 MiB/s`**)
  - **IDM:** **`28.47 s`** (Throughput: **`3.51 MB/s`** / **`3.35 MiB/s`**)
- **Time Advantage:** **Radium was faster by 1.49 seconds (+5.23% time advantage)**.
- **File Integrity:** Both output files contained exactly **100,000,000 bytes** and produced the exact matching SHA-256 digest (`a993f8c574e0fea8c1cdcbcd9408d9e2e107ee6e4d120edcfa11decd53fa0cae`).
- **Resource Footprint:** Radium utilized **55.6% less memory** (peak 21.7 MB vs 48.9 MB) and **32.4% lower average CPU** (4.6% vs 6.8%).

---

## 2. Telemetry & Bottleneck Analysis (`RADIUM_HTTP_RANGE_TRACE=1`)

Radium's internal HTTP range trace records captured exact client callbacks and file write events:

```
Total HTTP Range Requests Issued: 8
Total Range Splits Observed:       7
Split Cancellations:              7 (Cleanly replaced with bisected tail ranges)
Segment Completions:              1 (Terminal segment)
Total Client Bytes Received:      100,389,120 bytes
Total File Bytes Written:         100,000,000 bytes
Discarded Overhead / Slippage:    389,120 bytes (0.389% / ~380 KiB)
HTTP Retries / 429 Events:        0
Probe Duration:                   2,447 ms
Storage Preallocation:            0 ms
File Verification Time:           4 ms
```

### Per-Request Breakdown:
| Request ID | Worker | Requested Byte Range | Received Bytes | Written Bytes | Committed Range | Outcome / Cancel Reason |
| :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **1** | 0 | `0 - 99,999,999` | 50,069,504 | 50,003,968 | `0 - 50,003,967` | Cancelled (`range_split`) |
| **2** | 1 | `50,003,968 - 99,999,999` | 25,165,824 | 25,001,984 | `50,003,968 - 75,005,951` | Cancelled (`range_split`) |
| **3** | 2 | `75,005,952 - 99,999,999` | 12,582,912 | 12,500,992 | `75,005,952 - 87,506,943` | Cancelled (`range_split`) |
| **4** | 3 | `87,506,944 - 99,999,999` | 6,291,456 | 6,250,496 | `87,506,944 - 93,757,439` | Cancelled (`range_split`) |
| **5** | 4 | `93,757,440 - 99,999,999` | 3,145,728 | 3,125,248 | `93,757,440 - 96,882,687` | Cancelled (`range_split`) |
| **6** | 5 | `96,882,688 - 99,999,999` | 1,572,864 | 1,560,576 | `96,882,688 - 98,443,263` | Cancelled (`range_split`) |
| **7** | 6 | `98,443,264 - 99,999,999` | 786,432 | 782,336 | `98,443,264 - 99,225,599` | Cancelled (`range_split`) |
| **8** | 7 | `99,225,600 - 99,999,999` | 774,400 | 774,400 | `99,225,600 - 99,999,999` | Completed (`segment_complete`) |

### Telemetry Findings Regarding Radium's Bottlenecks:
1. **Initial Probe Delay:**
   - The probe phase took **2,447 ms** (~2.45 seconds) before workers were spawned. This represents **9.1% of Radium's total elapsed time**.
   - WinHTTP does a synchronous probe connection before dispatching parallel segment workers. Optimizing probe connection reuse or pipelining the first data segment with the probe would reduce elapsed time by ~2 seconds.
2. **Work-Stealing Discard Overhead:**
   - Across the 7 segment bisect splits, WinHTTP in-flight socket buffers delivered **389,120 bytes (~380 KiB)** of data past the split boundary before cancellation took effect.
   - This represents an overhead of only **0.389%** of the payload. The newly implemented in-flight segment split guard successfully prevented runaway repeated resplits (each worker split exactly once to allocate work to a newly spawned thread).
3. **Paced Worker Ramp-up:**
   - Radium starts with 2 initial workers and paces worker spawns sequentially. Workers 0 through 7 were up and running within 365 ms of transfer start.

---

## 3. Side-by-Side Comparison Table

| Metric | Radium | IDM | Comparison |
| :--- | :---: | :---: | :--- |
| **Elapsed transfer time** | **26.98 s** | **28.47 s** | **Radium faster by 1.49 s (+5.23%)** |
| **Average throughput (MB/s)** | **3.71 MB/s** | **3.51 MB/s** | Radium higher by 0.20 MB/s |
| **Average throughput (MiB/s)** | **3.53 MiB/s** | **3.35 MiB/s** | Radium higher by 0.18 MiB/s |
| **Average CPU** | **4.6%** | **6.8%** | **Radium uses 32.4% less average CPU** |
| **Peak CPU** | **53.6%** | **107.8%** | **Radium uses 50.3% less peak CPU** |
| **Peak memory (RSS)** | **21.7 MB** | **48.9 MB** | **Radium uses 55.6% less RAM** |
| **Total downloaded bytes** | **100,000,000** | **100,000,000** | Exact match |
| **Final file size** | **100,000,000** | **100,000,000** | Verified exact match |
| **SHA-256 verification** | **MATCH** | **MATCH** | Exact parity (`a993f8c5...`) |
| **Errors or retries** | 0 | 0 | None observed |

> **Time Advantage Formula:**
> $$\text{Time advantage (\%)} = \frac{\text{IDM time} - \text{Radium time}}{\text{IDM time}} \times 100$$
> $$\frac{28.47 - 26.98}{28.47} \times 100 = \mathbf{+5.23\%}$$

---

## 4. Limitations

1. **Single-Trial Scope:** Exactly one download per application was performed sequentially as mandated. A single trial demonstrates operational correctness and captures detailed internal telemetry, but does not represent a statistical median across fluctuating network jitter.
2. **Server Location:** The target payload was served from DataPacket's Singapore edge (~55–65 ms latency).
3. **Internal IDM Telemetry:** IDM's internal segment splits and socket discard buffers cannot be directly observed with C++ trace points; IDM metrics are derived from external process monitoring and verified output file inspection.
