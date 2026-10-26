# Local lock and disk timing audit — 2026-10-26

All runs used the local HTTP fixture and the same deterministic 128 MiB payload (SHA-256 `5cfe7273fff9c99ddc392b141261658761f581ef1d2b644330a6b8eb276ab264`). Each row is three sequential runs; 24/24 completed, reconciled client telemetry with server request logs, and matched the expected output hash. No internet requests were made. The fixture used 25 ms fixed response latency and/or 5 ms pacing per 256 KiB chunk depending on scenario.

| Scenario | Connections | Transfer median (range), ms | Process median, ms | Peak client requests (median) | Worker slot occupancy (median) | Scheduler lock wait/hold total (median), ms | Segment-write lock wait/hold (median), ms | File write total (median), ms | Worker queue wait (median), ms | Request non-callback time (median), ms | Fixture header/body medians, ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| fast | 4 | 777.3 (562.1–955.7) | 1164.4 | 4 | 75.4% | 1.9/11.8 | 5.7/257.8 | 252.4 | 751.3 | 640.2 | 0.6/285.4 |
| fast | 8 | 878.7 (602.6–1167.8) | 1206.5 | 7 | 43.6% | 25.8/105.5 | 62.8/504.6 | 477.3 | 548.3 | 284.6 | 0.6/194.2 |
| latency_only | 4 | 1071.9 (889.7–1077.3) | 1478.7 | 4 | 52.3% | 46.7/121.2 | 104.2/534.7 | 519.9 | 55.9 | 541.3 | 26.0/15.9 |
| latency_only | 8 | 1118.5 (1114.0–1176.3) | 1543.5 | 7 | 37.6% | 144.2/238.3 | 174.1/714.0 | 672.9 | 232.6 | 679.9 | 26.1/81.5 |
| pacing_only | 4 | 1414.0 (1065.7–1508.6) | 1770.9 | 4 | 66.3% | 41.6/109.7 | 99.7/668.3 | 661.3 | 80.3 | 2367.2 | 0.6/803.1 |
| pacing_only | 8 | 1387.4 (921.2–2267.2) | 1767.2 | 8 | 53.2% | 41.3/151.3 | 131.3/788.0 | 769.9 | 1816.7 | 2105.6 | 0.6/219.7 |
| latency_paced | 4 | 1351.6 (1025.3–1876.1) | 1708.4 | 4 | 71.5% | 41.0/104.2 | 97.3/502.4 | 491.3 | 110.4 | 2474.9 | 26.0/439.0 |
| latency_paced | 8 | 1191.8 (934.0–1393.0) | 1591.2 | 8 | 60.8% | 69.8/155.8 | 116.2/540.3 | 521.4 | 1817.3 | 2392.0 | 26.1/182.7 |

## Interpretation

- The matrix does not support a single connection-independent bottleneck. 8 connections had longer transfer medians in `fast` (879 vs 777 ms) and `latency_only` (1119 vs 1072 ms), were similar in `pacing_only` (1387 vs 1414 ms), and were faster in `latency_paced` (1192 vs 1352 ms). The three-run ranges overlap in several scenarios and are wide in `fast`/`pacing_only`.
- Scheduler lock wait totals rose at 8 connections in `fast` and `latency_only` (26 vs 2 ms; 144 vs 47 ms median), so contention is a plausible contributor in those fixtures. They are cumulative across threads, not exclusive wall-clock delay. This is not enough to establish contention as the primary cause.
- Segment-write lock waits and file-write durations also rose at 8 connections for `fast` and `latency_only`. The lock hold includes the guarded write callback; those totals overlap across workers. No isolated disk saturation measurement or hardware counter was collected, so disk limitation is not confirmed.
- The independent latency case recorded ~26 ms median request-to-headers time, matching the configured 25 ms delay. Pacing-only recorded sub-millisecond header delay and longer body-handler time. Thus the fixture now distinguishes server response latency from body pacing. `request_non_callback_wait_us` is an approximation (request wall time less callback processing); it also includes WinHTTP setup and other non-callback gaps, so it must not be read as pure network wait.
- Worker occupancy was lower at 8 connections in the fast and latency-only cases; queue/worker wait was variable. The local fixture is loopback and runs were sequential, so these figures cannot establish behavior on external servers or explain previous live IDM comparisons.
- Runtime tracing was enabled and adds logging/timing overhead, particularly because lock samples are emitted during the benchmark. These results locate where time accumulates but are not clean production-performance measurements.

## Test/build

- Incremental MSVC Release build: `radium` and `radium_unit_tests` succeeded.
- C++ regression suite: 72/72 passed.
- Focused Python fixture test: `python tests/test_local_benchmark_telemetry.py` passed. `pytest` is not installed in the active Python environment, so the test was run directly through its standalone entry point.
- No download engine scheduling or behavior was changed. No internet benchmark, commit, or push was performed.

## Raw evidence

Each `*_repeatN.json` contains the client runtime event trace, phase timings, server handler/request records, hashes, and aggregate metrics. `summary.json` contains run-level comparisons. This report is saved beside them.
