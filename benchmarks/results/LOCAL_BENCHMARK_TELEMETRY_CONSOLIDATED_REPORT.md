# Local Benchmark Telemetry & Summary Audit Consolidation

**Audit Date:** 2026-10-26  
**Status:** Consolidated Summary Report  
**Scope:** Aggregated performance and telemetry metrics from local benchmark suites (16 MiB, 128 MiB, lock/disk audits, alternative 1 MiB splitting thresholds, and adaptive scheduler evaluations). Full per-run raw event traces and JSON outputs are archived in the `raw-benchmark-evidence-archive` GitHub Actions artifact.

---

## Summary: `benchmarks/results/local_128mb_adaptive/summary.json`
- **Total Executed Runs in Trace:** 24

| Scenario | Conns | Runs | Median Transfer (ms) | Median Throughput (MiB/s) | Peak Client Active | Total Discarded (Bytes) | Integrity & Telemetry |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **fast** | 4 | 3 | 505.77 ms | 253.08 MiB/s | 4 | 1,409,024 B | PASS |
| **fast** | 8 | 3 | 646.19 ms | 198.08 MiB/s | 7 | 1,343,488 B | PASS |
| **latency_only** | 4 | 3 | 670.24 ms | 190.98 MiB/s | 4 | 2,154,496 B | PASS |
| **latency_only** | 8 | 3 | 853.25 ms | 150.02 MiB/s | 7 | 4,268,032 B | PASS |
| **pacing_only** | 4 | 3 | 1330.82 ms | 96.18 MiB/s | 4 | 884,736 B | PASS |
| **pacing_only** | 8 | 3 | 1070.97 ms | 119.52 MiB/s | 8 | 2,277,376 B | PASS |
| **latency_paced** | 4 | 3 | 1011.13 ms | 126.59 MiB/s | 4 | 1,359,872 B | PASS |
| **latency_paced** | 8 | 3 | 1242.47 ms | 103.02 MiB/s | 8 | 3,473,408 B | PASS |

## Summary: `benchmarks/results/local_128mb_baseline/summary.json`
- **Total Executed Runs in Trace:** 24

| Scenario | Conns | Runs | Median Transfer (ms) | Median Throughput (MiB/s) | Peak Client Active | Total Discarded (Bytes) | Integrity & Telemetry |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **fast** | 4 | 3 | 820.95 ms | 155.92 MiB/s | 4 | 655,360 B | PASS |
| **fast** | 8 | 3 | 538.18 ms | 237.84 MiB/s | 7 | 1,523,712 B | PASS |
| **latency_only** | 4 | 3 | 574.51 ms | 222.80 MiB/s | 4 | 2,875,392 B | PASS |
| **latency_only** | 8 | 3 | 910.22 ms | 140.63 MiB/s | 7 | 2,748,416 B | PASS |
| **pacing_only** | 4 | 3 | 1302.79 ms | 98.25 MiB/s | 4 | 786,432 B | PASS |
| **pacing_only** | 8 | 3 | 1005.70 ms | 127.27 MiB/s | 8 | 2,433,024 B | PASS |
| **latency_paced** | 4 | 3 | 1067.81 ms | 119.87 MiB/s | 4 | 2,801,664 B | PASS |
| **latency_paced** | 8 | 3 | 975.87 ms | 131.16 MiB/s | 8 | 2,686,976 B | PASS |

## Summary: `benchmarks/results/local_16mb_adaptive/summary.json`
- **Total Executed Runs in Trace:** 24

| Scenario | Conns | Runs | Median Transfer (ms) | Median Throughput (MiB/s) | Peak Client Active | Total Discarded (Bytes) | Integrity & Telemetry |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **fast** | 4 | 3 | 141.19 ms | 113.33 MiB/s | 4 | 491,520 B | PASS |
| **fast** | 8 | 3 | 135.53 ms | 118.06 MiB/s | 2 | 0 B | PASS |
| **latency_only** | 4 | 3 | 204.57 ms | 78.21 MiB/s | 3 | 1,441,792 B | PASS |
| **latency_only** | 8 | 3 | 148.84 ms | 107.50 MiB/s | 3 | 638,976 B | PASS |
| **pacing_only** | 4 | 3 | 203.23 ms | 78.73 MiB/s | 4 | 589,824 B | PASS |
| **pacing_only** | 8 | 3 | 200.83 ms | 79.67 MiB/s | 4 | 589,824 B | PASS |
| **latency_paced** | 4 | 3 | 268.31 ms | 59.63 MiB/s | 4 | 1,163,264 B | PASS |
| **latency_paced** | 8 | 3 | 269.44 ms | 59.38 MiB/s | 4 | 1,605,632 B | PASS |

## Summary: `benchmarks/results/local_lock_disk_audit_20261026/summary.json`
- **Total Executed Runs in Trace:** 24

| Scenario | Conns | Runs | Median Transfer (ms) | Median Throughput (MiB/s) | Peak Client Active | Total Discarded (Bytes) | Integrity & Telemetry |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **fast** | 4 | 3 | 777.28 ms | 164.68 MiB/s | 4 | 999,424 B | PASS |
| **fast** | 8 | 3 | 878.68 ms | 145.67 MiB/s | 7 | 1,818,624 B | PASS |
| **latency_only** | 4 | 3 | 1071.91 ms | 119.41 MiB/s | 4 | 2,826,240 B | PASS |
| **latency_only** | 8 | 3 | 1118.52 ms | 114.44 MiB/s | 8 | 3,989,504 B | PASS |
| **pacing_only** | 4 | 3 | 1414.04 ms | 90.52 MiB/s | 4 | 819,200 B | PASS |
| **pacing_only** | 8 | 3 | 1387.39 ms | 92.26 MiB/s | 8 | 2,859,008 B | PASS |
| **latency_paced** | 4 | 3 | 1351.57 ms | 94.70 MiB/s | 4 | 1,638,400 B | PASS |
| **latency_paced** | 8 | 3 | 1191.78 ms | 107.40 MiB/s | 8 | 3,260,416 B | PASS |

## Summary: `benchmarks/results/local_telemetry_audit/summary.json`
- **Total Executed Runs in Trace:** 24

| Scenario | Conns | Runs | Median Transfer (ms) | Median Throughput (MiB/s) | Peak Client Active | Total Discarded (Bytes) | Integrity & Telemetry |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **fast** | 4 | 3 | 69.44 ms | 230.42 MiB/s | 2 | 0 B | PASS |
| **fast** | 8 | 3 | 112.20 ms | 142.61 MiB/s | 2 | 131,072 B | PASS |
| **latency_only** | 4 | 3 | 186.92 ms | 85.60 MiB/s | 3 | 819,200 B | PASS |
| **latency_only** | 8 | 3 | 180.81 ms | 88.49 MiB/s | 3 | 1,081,344 B | PASS |
| **pacing_only** | 4 | 3 | 244.33 ms | 65.49 MiB/s | 4 | 753,664 B | PASS |
| **pacing_only** | 8 | 3 | 244.86 ms | 65.34 MiB/s | 4 | 393,216 B | PASS |
| **latency_paced** | 4 | 3 | 317.78 ms | 50.35 MiB/s | 4 | 753,664 B | PASS |
| **latency_paced** | 8 | 3 | 312.30 ms | 51.23 MiB/s | 4 | 589,824 B | PASS |

## Summary: `benchmarks/results/local_telemetry_audit/summary_alternative_1mb.json`
- **Total Executed Runs in Trace:** 24

| Scenario | Conns | Runs | Median Transfer (ms) | Median Throughput (MiB/s) | Peak Client Active | Total Discarded (Bytes) | Integrity & Telemetry |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **fast** | 4 | 3 | 69.44 ms | 230.42 MiB/s | 2 | 0 B | PASS |
| **fast** | 8 | 3 | 112.20 ms | 142.61 MiB/s | 2 | 131,072 B | PASS |
| **latency_only** | 4 | 3 | 186.92 ms | 85.60 MiB/s | 3 | 819,200 B | PASS |
| **latency_only** | 8 | 3 | 180.81 ms | 88.49 MiB/s | 3 | 1,081,344 B | PASS |
| **pacing_only** | 4 | 3 | 244.33 ms | 65.49 MiB/s | 4 | 753,664 B | PASS |
| **pacing_only** | 8 | 3 | 244.86 ms | 65.34 MiB/s | 4 | 393,216 B | PASS |
| **latency_paced** | 4 | 3 | 317.78 ms | 50.35 MiB/s | 4 | 753,664 B | PASS |
| **latency_paced** | 8 | 3 | 312.30 ms | 51.23 MiB/s | 4 | 589,824 B | PASS |

## Summary: `benchmarks/results/local_telemetry_audit/summary_baseline.json`
- **Total Executed Runs in Trace:** 24

| Scenario | Conns | Runs | Median Transfer (ms) | Median Throughput (MiB/s) | Peak Client Active | Total Discarded (Bytes) | Integrity & Telemetry |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **fast** | 4 | 3 | 106.99 ms | 149.55 MiB/s | 2 | 131,072 B | PASS |
| **fast** | 8 | 3 | 100.89 ms | 158.59 MiB/s | 2 | 262,144 B | PASS |
| **latency_only** | 4 | 3 | 181.39 ms | 88.21 MiB/s | 3 | 1,196,032 B | PASS |
| **latency_only** | 8 | 3 | 178.36 ms | 89.71 MiB/s | 3 | 360,448 B | PASS |
| **pacing_only** | 4 | 3 | 243.13 ms | 65.81 MiB/s | 4 | 589,824 B | PASS |
| **pacing_only** | 8 | 3 | 238.46 ms | 67.10 MiB/s | 4 | 524,288 B | PASS |
| **latency_paced** | 4 | 3 | 290.43 ms | 55.09 MiB/s | 4 | 1,294,336 B | PASS |
| **latency_paced** | 8 | 3 | 296.35 ms | 53.99 MiB/s | 4 | 655,360 B | PASS |

## Summary: `benchmarks/results/local_telemetry_audit_20261026/summary.json`
- **Total Executed Runs in Trace:** 12

| Scenario | Conns | Runs | Median Transfer (ms) | Median Throughput (MiB/s) | Peak Client Active | Total Discarded (Bytes) | Integrity & Telemetry |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **fast** | 4 | 3 | 75.33 ms | 212.39 MiB/s | 2 | 0 B | PASS |
| **fast** | 8 | 3 | 126.09 ms | 126.89 MiB/s | 3 | 0 B | PASS |
| **latency_paced** | 4 | 3 | 260.62 ms | 61.39 MiB/s | 4 | 0 B | PASS |
| **latency_paced** | 8 | 3 | 260.72 ms | 61.37 MiB/s | 4 | 0 B | PASS |

## Summary: `benchmarks/results/local_telemetry_audit_20261026_128m/summary.json`
- **Total Executed Runs in Trace:** 12

| Scenario | Conns | Runs | Median Transfer (ms) | Median Throughput (MiB/s) | Peak Client Active | Total Discarded (Bytes) | Integrity & Telemetry |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **fast** | 4 | 3 | 965.59 ms | 132.56 MiB/s | 4 | 843,776 B | PASS |
| **fast** | 8 | 3 | 838.37 ms | 152.68 MiB/s | 6 | 1,908,736 B | PASS |
| **latency_paced** | 4 | 3 | 1827.97 ms | 70.02 MiB/s | 4 | 622,592 B | PASS |
| **latency_paced** | 8 | 3 | 1986.95 ms | 64.42 MiB/s | 7 | 2,396,160 B | PASS |

