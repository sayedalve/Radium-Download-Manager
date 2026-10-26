#!/usr/bin/env python3
"""
Radium Download Manager — Unified Comparative Benchmark Runner
Provides a single, standardized, reproducible benchmark CLI for running
live and local transfers comparing Radium against competitor baselines or previous versions.
"""

import argparse
import hashlib
import json
import os
import psutil
import subprocess
import sys
import threading
import time
from pathlib import Path

PAYLOAD_PRESETS = {
    "500mb": {
        "url": "https://sg-lg.regxa.com/500MB.test",
        "expected_bytes": 500000000,
        "reference_sha256": "b16f001a0731c8636f163586c16276b3714b8977242b67ae05fa60b96f685ebc",
        "label": "500 MB (Regxa SG Mirror)"
    },
    "100mb": {
        "url": "https://fsn1-speed.hetzner.com/100MB.bin",
        "expected_bytes": 104857600,
        "reference_sha256": "fb80864319d5c92d774a37b924fdb2736465492923767cb232386b932304abac",
        "label": "100 MB (Hetzner FSN1)"
    }
}

def compute_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()

def monitor_process(pid_getter, stop_event, metrics):
    cpu_samples = []
    mem_samples = []
    tcp_samples = []
    while not stop_event["stop"]:
        try:
            pid = pid_getter()
            if pid:
                p = psutil.Process(pid)
                cpu = p.cpu_percent(interval=0.25)
                mem = p.memory_info().rss / (1024 * 1024)
                all_pids = [pid]
                for child in p.children(recursive=True):
                    try:
                        all_pids.append(child.pid)
                        cpu += child.cpu_percent(interval=None)
                        mem += child.memory_info().rss / (1024 * 1024)
                    except Exception:
                        pass
                cpu_samples.append(cpu)
                mem_samples.append(mem)

                conns = 0
                for c in psutil.net_connections(kind="tcp"):
                    if c.pid in all_pids and c.status == "ESTABLISHED":
                        conns += 1
                tcp_samples.append(conns)
            else:
                time.sleep(0.25)
        except Exception:
            time.sleep(0.25)
    metrics["cpu_samples"] = cpu_samples
    metrics["mem_samples"] = mem_samples
    metrics["tcp_samples"] = tcp_samples

def run_single_radium(radium_exe: str, url: str, expected_bytes: int, ref_sha256: str,
                       out_dir: Path, trial_idx: int, connections: int = 8, dry_run: bool = False):
    if dry_run:
        return {
            "app": "Radium Download Manager",
            "version": "dry-run",
            "status": "DRY_RUN",
            "elapsed_s": 0.0,
            "throughput_mb_s": 0.0,
            "peak_mem_mb": 0.0,
            "peak_cpu_percent": 0.0,
            "sha256_match": True,
            "telemetry": {}
        }

    out_file = out_dir / f"radium_trial_{trial_idx}.bin"
    db_file = out_dir / f"radium_trial_{trial_idx}.db"
    log_file = out_dir / f"radium_trial_{trial_idx}.log"

    for p in [out_file, db_file, log_file]:
        if p.exists():
            p.unlink()

    env = os.environ.copy()
    env["RADIUM_HTTP_RANGE_TRACE"] = "1"

    cmd = [
        radium_exe,
        "--db", str(db_file),
        "--out", str(out_dir),
        "--connections", str(connections),
        "--download", url
    ]

    t_start = time.perf_counter()
    proc = subprocess.Popen(cmd, env=env)

    stop_event = {"stop": False}
    metrics = {}
    t_mon = threading.Thread(target=monitor_process, args=(lambda: proc.pid, stop_event, metrics))
    t_mon.start()

    proc.wait(timeout=900)
    t_end = time.perf_counter()
    stop_event["stop"] = True
    t_mon.join(timeout=2)

    elapsed = t_end - t_start

    # Check for downloaded output
    default_name = url.split("/")[-1].split("?")[0]
    default_out = out_dir / default_name
    if default_out.exists():
        if out_file.exists():
            out_file.unlink()
        default_out.rename(out_file)

    size = out_file.stat().st_size if out_file.exists() else 0
    sha = compute_sha256(str(out_file)) if (size == expected_bytes and expected_bytes > 0) else None

    # Parse telemetry
    telemetry = {
        "range_requests_issued": 0,
        "range_splits": 0,
        "split_cancellations": 0,
        "segment_completions": 0,
        "client_bytes_received": 0,
        "file_bytes_written": 0,
        "slippage_bytes": 0,
        "requests": []
    }
    if log_file.exists():
        with open(log_file, "r", errors="ignore") as f:
            for line in f:
                if "HTTP_RANGE_TRACE" in line:
                    telemetry["range_requests_issued"] += 1
                    try:
                        j_str = line.split("HTTP_RANGE_TRACE ", 1)[1].strip()
                        rec = json.loads(j_str)
                        telemetry["requests"].append(rec)
                        telemetry["client_bytes_received"] += rec.get("client_bytes_received", 0)
                        telemetry["file_bytes_written"] += rec.get("file_bytes_written", 0)
                        if rec.get("outcome") == "cancelled":
                            telemetry["split_cancellations"] += 1
                        if rec.get("cancel_reason") == "range_split":
                            telemetry["range_splits"] += 1
                        if rec.get("body_complete") is True or rec.get("outcome") == "complete":
                            telemetry["segment_completions"] += 1
                    except Exception:
                        pass
        telemetry["slippage_bytes"] = max(0, telemetry["client_bytes_received"] - telemetry["file_bytes_written"])

    cpu_list = metrics.get("cpu_samples", [0.0])
    mem_list = metrics.get("mem_samples", [0.0])
    avg_cpu = sum(cpu_list) / max(1, len(cpu_list))
    peak_cpu = max(cpu_list) if cpu_list else 0.0
    peak_mem = max(mem_list) if mem_list else 0.0

    return {
        "app": "Radium Download Manager",
        "elapsed_s": round(elapsed, 2),
        "size_bytes": size,
        "throughput_mb_s": round((size / (1000 * 1000)) / elapsed, 2) if elapsed > 0 else 0,
        "throughput_mib_s": round((size / (1024 * 1024)) / elapsed, 2) if elapsed > 0 else 0,
        "avg_cpu_percent": round(avg_cpu, 1),
        "peak_cpu_percent": round(peak_cpu, 1),
        "peak_mem_mb": round(peak_mem, 1),
        "sha256": sha,
        "sha256_match": (sha == ref_sha256) if ref_sha256 else True,
        "telemetry": telemetry,
        "status": "COMPLETED" if (size == expected_bytes and (not ref_sha256 or sha == ref_sha256)) else "FAILED"
    }

def main():
    parser = argparse.ArgumentParser(
        description="Unified Comparative Benchmark Runner for Radium Download Manager",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument("--size", choices=["500mb", "100mb"], default="500mb",
                        help="Payload size preset to evaluate")
    parser.add_argument("--mode", choices=["baseline", "adaptive"], default="adaptive",
                        help="Scheduler policy mode")
    parser.add_argument("--iterations", type=int, default=1,
                        help="Number of repeat trials to run")
    parser.add_argument("--connections", type=int, default=8,
                        help="Segment connection concurrency limit")
    parser.add_argument("--out-dir", type=str, default="benchmarks/run_comparison_output",
                        help="Directory to save trial results and summary")
    parser.add_argument("--radium-bin", type=str, default="build/win-x64/Release/radium.exe",
                        help="Path to Radium binary")
    parser.add_argument("--dry-run", action="store_true",
                        help="Validate arguments, environment, and paths without performing live network downloads")

    args = parser.parse_args()

    preset = PAYLOAD_PRESETS[args.size]
    out_path = Path(args.out_dir)
    out_path.mkdir(parents=True, exist_ok=True)

    radium_path = Path(args.radium_bin).resolve()
    if not args.dry_run and not radium_path.exists():
        sys.exit(f"Error: Radium binary not found at '{radium_path}'. Build first or specify --radium-bin.")

    print(f"============================================================")
    print(f" Radium Benchmark Runner: size={args.size} ({preset['label']})")
    print(f" Mode: {args.mode} | Iterations: {args.iterations} | Conns: {args.connections}")
    print(f" Output Directory: {out_path}")
    print(f" Dry Run Mode: {args.dry_run}")
    print(f"============================================================\n")

    trials = []
    for i in range(1, args.iterations + 1):
        print(f"--- Running Trial {i}/{args.iterations} ---")
        t_res = run_single_radium(
            radium_exe=str(radium_path),
            url=preset["url"],
            expected_bytes=preset["expected_bytes"],
            ref_sha256=preset["reference_sha256"],
            out_dir=out_path,
            trial_idx=i,
            connections=args.connections,
            dry_run=args.dry_run
        )
        print(f"Trial {i} result: status={t_res['status']}, elapsed={t_res['elapsed_s']}s, peak_mem={t_res['peak_mem_mb']}MB")
        trials.append(t_res)
        if i < args.iterations and not args.dry_run:
            time.sleep(2)

    # Compute summary
    completed = [t for t in trials if t["status"] in ("COMPLETED", "DRY_RUN")]
    summary = {
        "benchmark_timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "configuration": {
            "size": args.size,
            "mode": args.mode,
            "iterations": args.iterations,
            "connections": args.connections,
            "dry_run": args.dry_run,
            "url": preset["url"],
            "expected_bytes": preset["expected_bytes"]
        },
        "aggregate": {
            "total_trials": len(trials),
            "completed_trials": len(completed),
            "mean_elapsed_s": round(sum(t["elapsed_s"] for t in completed) / max(1, len(completed)), 2),
            "mean_throughput_mb_s": round(sum(t["throughput_mb_s"] for t in completed) / max(1, len(completed)), 2),
            "mean_peak_mem_mb": round(sum(t["peak_mem_mb"] for t in completed) / max(1, len(completed)), 2)
        },
        "trials": trials
    }

    summary_file = out_path / "run_summary.json"
    with open(summary_file, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    print(f"\n[+] Benchmark finished. Summary recorded to: {summary_file}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
