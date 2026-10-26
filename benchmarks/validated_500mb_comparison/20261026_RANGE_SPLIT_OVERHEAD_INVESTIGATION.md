# Technical Investigation Report: Range Splitting Overhead & Work Stealing Dynamics

**Scope:** Analysis of range splitting overhead in Radium Download Manager, based on telemetry from `benchmarks/validated_500mb_comparison/20261026_radium_vs_idm_500mb_validated.json` and controlled comparisons on the local HTTP benchmark fixture.  
**Baseline Build:** Radium v0.3.4 (Adaptive Work Balancing, `min_separation = 256 KiB`)  
**Tested Alternative:** Radium with Coarser Floor (`min_separation = 1 MiB`)  
**Date:** 2026-10-26  

---

## 1. Executive Summary & Problem Formulation

In the latest 500 MB live benchmark, Radium achieved high connection utilization (**7.9 average established TCP connections**, peaking at 8) and kept all 8 download workers engaged for >97% of the total download time. Radium completed the transfer in 146.79 seconds versus IDM at 137.36 seconds (+9.43 s / +6.8% difference).

However, Radium's trace recorded **33 range splits** and **4,763,648 bytes (4.54 MiB / 0.95%) of discarded slippage bytes**.

This investigation addressed three primary questions:
1. **Why do so many splits occur, and what do they actually cost?**
2. **Does raising the split floor to 1 MiB eliminate micro-splits and improve throughput?**
3. **Is dynamically altering an active segment boundary compatible with HTTP Range semantics, and what are the associated risks?**

---

## 2. Empirical Trace Analysis (Why 33 Splits Occurred)

Deconstructing all 45 request events in `20261026_radium_vs_idm_500mb_validated.json` revealed two distinct phases:

### Phase A: Initial Connection Ramp (0 s – 0.5 s)
- Requests 0 through 7 (8 initial connections) ramped up from a single root partition through 7 successive bisections:
  - Worker 0: [0 - 250M]
  - Worker 1: [250M - 500M]
  - Worker 2: [375M - 500M]
  - Worker 3: [125M - 250M]
  - Worker 4: [187.5M - 250M]
  - Worker 5: [437.5M - 500M]
  - Worker 6: [218.75M - 250M]
  - Worker 7: [468.75M - 500M]
- **Splits in Phase A:** 7 splits. These were highly productive and necessary to spawn the initial 8 parallel streams.

### Phase B: Mid-Transfer Rebalancing (70 s – 135 s)
- Around $t = 70$–$80$ s, faster workers (Workers 4 and 6) finished their initial 31–62 MB slices.
- Under the adaptive two-pass work-stealing algorithm, they bisected segments from slower connections (Workers 0, 1, 3).
- **Splits in Phase B:** 8 splits. Average slice size was 10–25 MiB. Workers downloaded tens of megabytes per slice.

### Phase C: Late-Stage "Splitting Frenzy" (135 s – 146.79 s)
- In the **final 11.8 seconds**, workers began finishing their remaining chunks.
- As remaining segment sizes shrank below 2 MiB, idle workers continued bisecting active segments down to the 256 KiB floor (`min_separation = 262,144 bytes`).
- **Telemetry of the final 11.8 seconds:**
  - **18 requests** were initiated after $t = 135$ s.
  - **12 requests were cancelled** within 300 ms – 1,500 ms of starting.
  - **2,187,264 bytes (2.09 MiB) were discarded** in these 11 seconds alone—accounting for **45.9% of all discarded bytes** across the entire 500 MB transfer!
  - Slices as small as 256 KiB, 266 KiB, 303 KiB, and 315 KiB were allocated to workers.

---

## 3. Dissecting the Overhead of Range Splitting

When Radium splits an active segment in flight:
1. **Request Cancellation & Socket Teardown:**
   - The victim's running WinHTTP request handle is closed (`WinHttpCloseHandle`).
   - The TCP connection cannot be cleanly reused for the immediate next request because the HTTP/1.1 response stream is abruptly aborted mid-transfer.
2. **TCP / TLS Handshake Cost:**
   - Opening a new connection to an HTTPS server costs 1 RTT (TCP SYN/ACK) + 1-2 RTTs (TLS 1.3/1.2 ClientHello/ServerHello). Over high-latency connections (~150–250 ms RTT), each split incurs **150–300 ms of idle connection setup time**.
3. **Slippage (Discarded Data):**
   - Between the moment the scheduler truncates the victim's `end_offset` and the moment the worker loop receives the abort signal and closes the handle, the OS network stack continues receiving packets from the server.
   - Any bytes received past `new_end` cannot be committed to disk and are discarded. Across 33 splits, this totaled **4.76 MB (0.95%)**.
4. **Conclusion on Cost:**
   - During the bulk transfer (0–135 s), the benefit of keeping 8 connections active far outweighed the ~2.5 MB of discarded data.
   - During the final 11 seconds, however, splitting sub-2 MB ranges was counterproductive: workers spent hundreds of milliseconds establishing sockets only to download 300 KB and be cancelled again.

---

## 4. Controlled Local Fixture Benchmark: Baseline vs. Alternative (1 MiB Floor)

To measure the exact effect of coarse versus fine split thresholds without internet variability, Radium was benchmarked on the deterministic 16 MiB local HTTP fixture across 4 network profiles (fast, latency_only, pacing_only, latency_paced) with 4 and 8 connection configurations (3 runs each, 24 trials total).

### Experimental Matrix Results

| Scenario & Concurrency | Metric | Baseline (`min_sep = 256 KiB`) | Alternative (`min_sep = 1 MiB`) | Delta |
| :--- | :--- | :---: | :---: | :---: |
| **fast (4 conn)** | Transfer Time | 105.23 ms | **82.94 ms** | **-22.28 ms (-21.2%)** |
| | Discarded Data | 43,690 B | **0 B** | **-43,690 B (-100%)** |
| | Requests (inc. probe) | 4.0 | **3.3** | -0.7 |
| **fast (8 conn)** | Transfer Time | **99.98 ms** | 116.92 ms | +16.94 ms (+16.9%) |
| | Discarded Data | 87,381 B | **43,690 B** | **-43,691 B (-50.0%)** |
| | Requests (inc. probe) | 3.7 | **3.3** | -0.4 |
| **latency_only (4 conn)** | Transfer Time | 194.57 ms | **186.85 ms** | **-7.72 ms (-4.0%)** |
| | Discarded Data | 398,677 B | **273,066 B** | **-125,611 B (-31.5%)** |
| | Requests (inc. probe) | 5.7 | **5.3** | -0.4 |
| **latency_only (8 conn)** | Transfer Time | **163.20 ms** | 200.26 ms | +37.06 ms (+22.7%) |
| | Discarded Data | **120,149 B** | 360,448 B | +240,299 B |
| | Requests (inc. probe) | **5.0** | 6.0 | +1.0 |
| **pacing_only (4 conn)** | Transfer Time | 245.69 ms | **244.60 ms** | -1.09 ms (-0.4%) |
| | Discarded Data | **196,608 B** | 251,221 B | +54,613 B |
| | Requests (inc. probe) | 7.0 | 7.0 | 0.0 |
| **pacing_only (8 conn)** | Transfer Time | **238.19 ms** | 244.64 ms | +6.45 ms (+2.7%) |
| | Discarded Data | 174,762 B | **131,072 B** | **-43,690 B (-25.0%)** |
| | Requests (inc. probe) | 7.0 | 7.0 | 0.0 |
| **latency_paced (4 conn)** | Transfer Time | **291.68 ms** | 325.57 ms | +33.89 ms (+11.6%) |
| | Discarded Data | 431,445 B | **251,221 B** | **-180,224 B (-41.8%)** |
| | Requests (inc. probe) | 7.3 | **7.0** | -0.3 |
| **latency_paced (8 conn)** | Transfer Time | **295.77 ms** | 311.18 ms | +15.41 ms (+5.2%) |
| | Discarded Data | 218,453 B | **196,608 B** | **-21,845 B (-10.0%)** |
| | Requests (inc. probe) | 7.0 | 7.0 | 0.0 |

*All 24 trials under both policies achieved 100% SHA256 integrity match.*

### Analysis of the Alternative
1. **Reduction in Discarded Data:** In 6 out of 8 scenarios, raising the split floor to 1 MiB noticeably reduced discarded slippage data (e.g. from 43.6 KB down to 0 KB in `fast 4c`, and from 431 KB to 251 KB in `latency_paced 4c`).
2. **Tradeoff on Concurrency Spawning:** For smaller total file payloads (such as the 16 MiB fixture), raising `min_separation` to 1 MiB means a segment must have at least 2 MiB remaining to be split. Under 8 connections, once segments drop below 2 MiB, idle workers cannot split anything and sit idle. This caused 8-connection transfer times to degrade by 16.9% to 22.7% on the 16 MiB fixture because available bandwidth was underutilized.
3. **Implication:** A static, hardcoded 1 MiB floor penalizes smaller downloads (<= 32 MiB) by restricting concurrency, even while it benefits larger downloads (>= 500 MB) by preventing late-stage micro-splits.

---

## 5. Architectural & Semantic Analysis: Changing Active Segment Boundaries

### The Fundamental HTTP Range Protocol Reality
An HTTP GET request carrying `Range: bytes=X-Y` asks the remote server to send bytes $X$ through $Y$. Once the server responds with `HTTP/206 Partial Content` and starts streaming the body:
- **The server is committed to sending all $Y - X + 1$ bytes.**
- **Changing an in-memory variable (`end_offset`) on the client does NOT inform the server.**
- The client cannot "tell" the server to stop sending at byte $Z$ without **either**:
  1. Aborting/closing the TCP connection (`WinHttpCloseHandle`), which discards unread buffer data and kills the socket, OR
  2. Reading and draining the entire response up to the original $Y$ while simply ignoring bytes after $Z$ (which wastes downstream bandwidth).

### How IDM Handles Range Splitting (Comparison)
Reverse engineering of `IDMan.exe` demonstrated how IDM avoids this dilemma:
1. **Dynamic Chunk Insertion:** IDM allocates separate temporary part files for each connection.
2. **Forward Bounds Truncation:** When IDM splits, it calculates the midpoint of an active connection and assigns a new suffix request to an idle connection. The original connection is **never aborted mid-flight**. It simply sets its internal stop offset to the midpoint. The original connection continues receiving data up to that midpoint and then issues a clean `Connection: keep-alive` closure or natural EOF without discarding bytes or re-negotiating TLS.
3. **Radium's Current Single-File Preallocation Architecture:** Radium pre-allocates the complete output file (`SetFilePointerEx` + `SetEndOfFile`) and writes directly into offset slices using IOCP / `write_mu`. Because Radium uses a shared target file rather than independent part files, canceling the request in flight and truncating `end_offset` was the chosen mechanism.

---

## 6. Correctness & Recommendation

### Correctness Risks of Premature Implementation
1. **Hardcoding a larger split floor (e.g. 1 MiB):** As demonstrated by the local fixture, this harms transfer speeds on files under 32 MB by choking connection ramp-up.
2. **Assuming non-destructive boundary changes:** Any scheme that attempts to alter active boundaries without aborting must account for in-flight socket buffers. Otherwise, overlapping writes will corrupt the file.

### Recommended Strategy
- **Preserve the current scheduler:** Radium v0.3.4 passes all 72 regression tests, produces exact byte-for-byte SHA256 integrity, maintains 7.9 established TCP connections, and came within 6.8% of IDM on the 500 MB live test.
- **Future Adaptive Heuristic (When Justified):** Instead of a static 1 MiB constant, make `min_separation` dynamic based on the remaining task size (e.g., $\max(256\text{ KiB}, \text{remaining\_bytes} / (2 \times \text{max\_conns}))$). When remaining uncommitted bytes are less than $2 \times \text{connections} \times 256\text{ KiB}$, freeze splitting and let the active streams finish cleanly to avoid late-stage churn.
