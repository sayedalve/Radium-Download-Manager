# Live 100 MB Validation: Radium Download Manager vs. Internet Download Manager (IDM)

**Benchmark Date:** October 25, 2026  
**Target URL:** `https://sgp.download.datapacket.com/100mb.bin`  
**Payload Size:** Exactly 100,000,000 bytes (100.00 MB decimal / 95.37 MiB binary)  
**Server Software:** CDN77-Turbo (`Accept-Ranges: bytes`)  
**SHA-256 Checksum:** `a993f8c574e0fea8c1cdcbcd9408d9e2e107ee6e4d120edcfa11decd53fa0cae`  
**Test Protocol:** Exactly one sequential live download per application (strictly 2 transfers total)  

---

## 1. Executive Summary

This validation benchmark evaluates the latest Radium Download Manager engine containing the **in-flight segment split guard** (`segment_scheduler.cpp`) against licensed **Internet Download Manager (IDM v6.43.15 Build 2)** over a live internet broadband connection.

Both applications downloaded the identical 100,000,000-byte test file under identical network conditions with 8 configured connections.

### Key Results:
- **Elapsed Download Time:** IDM completed the 100 MB transfer in **28.86 s** (average throughput **3.46 MB/s** / **3.30 MiB/s**). Radium completed the transfer in **63.17 s** (average throughput **1.58 MB/s** / **1.51 MiB/s**).
- **Time Advantage:** IDM was faster by **34.31 seconds** (**-118.88%** time advantage).
- **File Integrity:** Both output files contained exactly **100,000,000 bytes** and produced the identical cryptographic SHA-256 digest (`a993f8c574e0fea8c1cdcbcd9408d9e2e107ee6e4d120edcfa11decd53fa0cae`).
- **Resource Efficiency:** Radium operated with **59.9% lower peak memory** (20.9 MB vs 52.2 MB) and **87.6% lower average CPU utilization** (2.7% vs 21.7%).

---

## 2. Test Environment & Executable Verification

| Parameter | Specification |
| :--- | :--- |
| **Operating System** | Windows 11 Pro 64-bit |
| **Processor** | Intel Core i5-10400F @ 2.90 GHz (6 Cores / 12 Threads) |
| **System Memory** | 16 GB DDR4 RAM |
| **Destination Volume** | NVMe SSD NTFS Partition |
| **Radium Executable** | `build/win-x64/Release/radium.exe` (Incremental Release build with scheduler guard) |
| **Radium Executable SHA-256** | `B6D435218FFDFC7772D028B89510CFF6E6EE03253779DEC8C7DC4519E218789A` |
| **Radium Build Timestamp** | 2026-10-25 23:08:59 |
| **IDM Executable** | `C:\Program Files (x86)\Internet Download Manager\IDMan.exe` (v6.43.15 Build 2) |
| **Configured Concurrency** | 8 parallel segment connections per task |
| **Timing Method** | External Python high-resolution monotonic timer (`time.perf_counter`) |
| **Process Monitoring** | `psutil` sampling interval 200 ms recording process tree CPU% and RSS memory |

---

## 3. Measured Results Comparison

| Metric | Radium | IDM | Difference / Observation |
| :--- | :---: | :---: | :--- |
| **Elapsed transfer time** | **63.17 s** | **28.86 s** | IDM faster by 34.31 s (-118.88%) |
| **Average throughput (MB/s)** | **1.58 MB/s** | **3.46 MB/s** | IDM higher by 1.88 MB/s |
| **Average throughput (MiB/s)** | **1.51 MiB/s** | **3.30 MiB/s** | IDM higher by 1.79 MiB/s |
| **Average CPU** | **2.7%** | **21.7%** | **Radium uses 87.6% less CPU** |
| **Peak CPU** | **61.6%** | **133.0%** | **Radium uses 53.7% less peak CPU** |
| **Peak memory (RSS)** | **20.9 MB** | **52.2 MB** | **Radium uses 59.9% less RAM** |
| **Total downloaded bytes** | **100,000,000** | **100,000,000** | Exact expected size |
| **Final file size** | **100,000,000** | **100,000,000** | Verified exact match |
| **SHA 256 verification** | **MATCH** (`a993f8c5...`) | **MATCH** (`a993f8c5...`) | Bit-identical integrity |
| **Errors or retries** | None (0) | None (0) | Clean transfer |

> **Calculation Formula:**
> $$\text{Time advantage (\%)} = \frac{\text{IDM time} - \text{Radium time}}{\text{IDM time}} \times 100$$
> $$\frac{28.86 - 63.17}{28.86} \times 100 = -118.88\%$$

---

## 4. Limitations & Scope

- **Sample Size:** Exactly one trial was conducted per application as instructed. A single trial does not establish a universal performance guarantee across all network topologies, geographical CDNs, or server rate-limiting policies.
- **Network Routing:** The DataPacket Singapore node demonstrated aggressive multi-stream ramp-up under IDM's chunking strategy, while Radium's segment scheduler prioritized stable zero-split ranges with significantly reduced CPU and memory footprint.
