#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager — Benchmark Runner & Metrics Collector
# Executes integration benchmarks, validates resource budgets,
# and persists results to benchmarks/latest_metrics.json.

import argparse
import datetime
import json
import os
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IS_WINDOWS = (os.name == "nt" or platform.system().lower().startswith("win"))


def run_benchmarks(output_path: Path = None, verbose: bool = True) -> dict:
    if output_path is None:
        output_path = ROOT / "benchmarks" / "latest_metrics.json"

    output_path.parent.mkdir(parents=True, exist_ok=True)

    test_script = ROOT / "tests" / "run_all_integration_tests.py"
    if not test_script.exists():
        raise FileNotFoundError(f"Integration benchmark runner missing: {test_script}")

    print(f"[Radium Benchmark] Running test benchmarks on {platform.system()} {platform.release()} ({platform.machine()})...")
    t0 = datetime.datetime.now(datetime.timezone.utc)

    cmd = [sys.executable, str(test_script)]
    res = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)

    if verbose and res.stdout:
        print(res.stdout)
    if res.returncode != 0:
        if res.stderr:
            print(res.stderr, file=sys.stderr)
        raise RuntimeError(f"Benchmark suite failed with exit code {res.returncode}")

    results_file = ROOT / "build" / "benchmark_results.json"
    if not results_file.exists():
        raise FileNotFoundError(f"Expected benchmark output file not found: {results_file}")

    metrics = json.loads(results_file.read_text(encoding="utf-8"))
    metrics["timestamp_utc"] = t0.isoformat()
    metrics["python_version"] = sys.version.split()[0]
    metrics["os_environment"] = {
        "system": platform.system(),
        "release": platform.release(),
        "version": platform.version(),
        "machine": platform.machine(),
        "processor": platform.processor(),
    }

    output_path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    print(f"[Flux Benchmark] Saved benchmark metrics to: {output_path}")
    return metrics


def main():
    parser = argparse.ArgumentParser(description="Flux Download Manager Benchmark Execution and Metrics Persistence")
    parser.add_argument("--out", type=str, default=None, help="Target JSON output path")
    parser.add_argument("--quiet", action="store_true", help="Quiet output")
    args = parser.parse_args()

    out_p = Path(args.out).resolve() if args.out else None
    metrics = run_benchmarks(out_p, verbose=not args.quiet)

    print("\n========================================================")
    print("             FLUX DOWNLOAD MANAGER BENCHMARK SUMMARY    ")
    print("========================================================")
    print(f"Platform:               {metrics['host_platform']}")
    print(f"Product:                {metrics.get('product_name', 'Flux Download Manager')} v{metrics.get('version', '0.3.0')}")
    print(f"Combined Core Binaries: {metrics['combined_core_binaries_kb']} KB (Budget: <= 3072 KB)")
    print(f"Cold Startup Latency:   {metrics['cold_startup_list_median_ms']} ms (Budget: <= 150 ms)")
    print(f"Idle Daemon RSS:        {metrics['idle_daemon_rss_mb']} MB (Budget: <= 25 MB)")
    print(f"Peak HTTP 8-Conn RSS:   {metrics['peak_http_8conn_rss_mb']} MB (Budget: <= 65 MB)")
    print(f"Peak Stream Worker RSS: {metrics['peak_stream_worker_rss_mb']} MB (Budget: <= 65 MB)")
    print(f"1,000-Task UI Snapshot: {metrics['large_1000_tasks_snapshot_ms']} ms (Budget: <= 300 ms)")
    print(f"Loopback Throughput:    {metrics['throughput_16mb_8conn_median_mbps']} Mbps (Floor: >= 50 Mbps)")
    print("========================================================\n")


if __name__ == "__main__":
    main()
