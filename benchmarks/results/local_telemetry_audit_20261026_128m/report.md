# Controlled Local HTTP Telemetry Audit

Date: 2026-10-26  
Executable: `build_msvc/Release/radium.exe`  
SHA-256: `4844A828B8B8A60129DD9E70704A5F834C72C0CCB7EDA107B4FF61C31BC63CCD`  
Fixture: existing `scripts/benchmark_server.py`, bound to `127.0.0.1`; 128 MiB deterministic payload  
Payload SHA-256: `5cfe7273fff9c99ddc392b141261658761f581ef1d2b644330a6b8eb276ab264`

No public internet endpoints were contacted. The engine/scheduler behavior was not changed. Each scenario/configuration was run three times sequentially with the same fixture settings.

## Scenarios

- **Fast:** no fixed response delay and no per-chunk delay.
- **Latency paced:** 25 ms fixed delay before each response, then 5 ms after each 256 KiB body chunk.
- Both cases used byte-range responses and were tested at 4 and 8 configured connections.

## Results

Medians are across three repetitions; parentheses show the observed range where useful.

| Scenario | Connections | Initialization ms | Transfer ms | Finalization ms | Process completion ms | Useful MiB/s | Peak client requests | Peak fixture payload handlers | Peak all fixture handlers | Workers started | Worker-slot occupancy | Requests incl. probe | Splits / canceled requests | Discarded callback bytes | Scheduler idle wait ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Fast | 4 | 43.1 | 965.6 (918.1–989.4) | 5.3 | 1330.3 | 132.6 | 4 | 4 | 4 | 4 | 45.8% | 5 (5–8) | 3 / 3 | 196,608 | 522.7 |
| Fast | 8 | 42.6 | 838.4 (330.6–984.0) | 3.9 | 1207.0 | 152.7 | 6 (4–6) | 6 (4–6) | 6 | 6 (4–6) | 26.5% | 9 (8–10) | 7 / 7 | 634,880 | 697.9 |
| Latency paced | 4 | 66.7 | 1828.0 (1650.4–2052.3) | 4.0 | 2208.8 | 70.0 | 4 | 4 | 4 (4–5) | 4 | 59.1% | 5 (5–6) | 3 / 3 | 196,608 | 774.7 |
| Latency paced | 8 | 65.7 | 1987.0 (1984.7–2021.5) | 4.7 | 2418.7 | 64.4 | 7 | 6 (6–7) | 8 (8–9) | 7 | 29.7% | 9 (9–10) | 7 / 7 | 684,032 (643,072–1,069,056) | 1463.4 |

All 12 runs completed. Every output hash matched the fixture hash. For every run, runtime start/end counts matched fixture request count; requested ranges and statuses matched; client response ranges and statuses matched; callback bytes did not exceed the fixture's socket-write offers; client writes summed to 134,217,728 bytes; and final file size/hash matched. The raw client events, engine phase events, and server request records are retained in the per-run JSON files. `summary.json` contains all raw runs plus the grouped metrics.

The client active-request metric comes from request start/end intervals emitted by the running engine. Fixture payload-handler peaks count server-side body handlers; all-handler peaks also include requests that are waiting before response headers. Worker counts come from actual worker start/stop events. Worker-slot occupancy is summed worker lifetime divided by transfer time × configured slots; it is thread occupancy, not CPU utilization. These are distinct measurements and are not inferred from configured connections. `range_split` cancellations are counted separately from the successful terminal `segment_complete` callback stop.

## Interpretation

In this local setup, eight configured connections did not improve the latency-paced result: its median transfer took 1.987 s versus 1.828 s at four connections (about 8.7% longer). The 8-connection case reached seven active client requests, started seven workers, issued about twice as many requests/splits, and discarded more bytes (about 0.51% of payload versus 0.15%). That extra callback data is not large enough to account for the full timing difference. The fast 8-connection group varied widely (0.331–0.984 s), so its median does not establish a stable advantage.

These results identify a local 4-versus-8 behavior difference, but they do not isolate its cause. Server pacing, response cancellation, scheduler timing, and host scheduling may interact. The current trace does not measure scheduler lock contention or disk write latency, and server socket writes do not prove peer receipt. No performance optimization is recommended from this evidence. It does not establish Radium's performance relative to IDM or general internet behavior.

## Files

- `fast_4conn_repeat1.json` through `fast_8conn_repeat3.json`
- `latency_paced_4conn_repeat1.json` through `latency_paced_8conn_repeat3.json`
- `summary.json`
- `report.md` (this report)
