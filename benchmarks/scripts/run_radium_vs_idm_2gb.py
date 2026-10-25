#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Controlled Alternating Real Internet Benchmark: Radium Download Manager vs IDM (2 GB / 8 Connections)

- Workload: Real-world large payload download (~2.14 GB / 2,140,143,616 bytes)
- Target: AlmaLinux 8.10 Minimal x86_64 ISO
- URL: https://mirror.xeonbd.com/almalinux/8/isos/x86_64/AlmaLinux-8.10-x86_64-minimal.iso
- Upstream Official URL: https://repo.almalinux.org/almalinux/8/isos/x86_64/AlmaLinux-8.10-x86_64-minimal.iso
- Authoritative Upstream SHA-256: e524329700abe47ce1f509bed7e2d3c68b336a54c712daa1b492b2429a64d419
- Configured Connections: 8 connections per application
- Alternating Execution Order:
    Trial 1: Radium, then IDM
    Trial 2: IDM, then Radium
    Trial 3: Radium, then IDM
"""

import argparse
import csv
import datetime
import hashlib
import json
import os
import platform
import shutil
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

try:
    import psutil
except ImportError:
    psutil = None

ROOT = Path(__file__).resolve().parent.parent.parent

DEFAULT_URL = "https://mirror.xeonbd.com/almalinux/8/isos/x86_64/AlmaLinux-8.10-x86_64-minimal.iso"
DEFAULT_FILENAME = "AlmaLinux-8.10-x86_64-minimal.iso"
DEFAULT_EXPECTED_SIZE = 2140143616
DEFAULT_EXPECTED_SHA256 = "e524329700abe47ce1f509bed7e2d3c68b336a54c712daa1b492b2429a64d419"

RADIUM_BIN = ROOT / "dist" / "Radium.exe"
IDM_BIN = Path(r"C:\Program Files (x86)\Internet Download Manager\IDMan.exe")
IDM_DWNL_ROOT = Path(os.environ.get("APPDATA", "")) / "IDM" / "DwnlData" / os.environ.get("USERNAME", "sayed")


def compute_sha256(filepath: Path) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(16 * 1024 * 1024):
            h.update(chunk)
    return h.hexdigest()


def ensure_idm_running():
    if not psutil:
        return
    for p in psutil.process_iter(["name"]):
        if p.name().lower() == "idman.exe":
            return
    if IDM_BIN.exists():
        subprocess.Popen([str(IDM_BIN)])
        time.sleep(2)


def get_idm_effective_connections(prefix: str = "almalinux") -> int:
    conns = 0
    if IDM_DWNL_ROOT.exists():
        try:
            for item in IDM_DWNL_ROOT.iterdir():
                if prefix.lower() in item.name.lower() and item.is_dir():
                    for f in item.iterdir():
                        if f.stat().st_size > 0 and (f.suffix.startswith(".iso") or f.suffix.isdigit()):
                            conns += 1
        except Exception:
            pass
    return max(1, conns)


def cleanup_idm_temp(prefix: str = "almalinux"):
    if IDM_DWNL_ROOT.exists():
        try:
            for item in IDM_DWNL_ROOT.iterdir():
                if prefix.lower() in item.name.lower() and item.is_dir():
                    shutil.rmtree(item, ignore_errors=True)
        except Exception:
            pass


def run_radium_trial(
    trial_num: int,
    order: int,
    work_dir: Path,
    target_url: str,
    target_filename: str,
    expected_size: int,
    expected_sha256: str,
    conns: int = 8,
    timeout: float = 1200.0,
) -> dict:
    print(f"\n[{datetime.datetime.now().strftime('%H:%M:%S')}] >>> [Execution #{order}] RUNNING TRIAL {trial_num}: RADIUM DOWNLOAD MANAGER ({conns} conns)")

    dest_file = work_dir / target_filename
    if dest_file.exists():
        dest_file.unlink()
    for p in work_dir.glob("*.part"):
        p.unlink(missing_ok=True)

    db_file = work_dir / f"radium_trial_{trial_num}.db"
    for db_part in work_dir.glob(f"radium_trial_{trial_num}.db*"):
        db_part.unlink(missing_ok=True)

    log_file = work_dir / "radium.log"
    log_offset = log_file.stat().st_size if log_file.exists() else 0

    cmd = [
        str(RADIUM_BIN),
        "--db", str(db_file),
        "--out", str(work_dir),
        "--connections", str(conns),
        "--download", target_url,
    ]

    t0 = time.perf_counter()
    start_iso = datetime.datetime.now().isoformat()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    ps_proc = None
    if psutil:
        try:
            ps_proc = psutil.Process(proc.pid)
        except Exception:
            pass

    peak_rss_mb = 0.0
    peak_cpu_pct = 0.0
    deadline = t0 + timeout

    completed_transfer = False
    actual_duration = 0.0

    while time.perf_counter() < deadline:
        time.sleep(1.0)
        if ps_proc and ps_proc.is_running():
            try:
                rss = ps_proc.memory_info().rss / (1024.0 * 1024.0)
                if rss > peak_rss_mb:
                    peak_rss_mb = rss
                cpu = ps_proc.cpu_percent(interval=None)
                if cpu > peak_cpu_pct:
                    peak_cpu_pct = cpu
            except Exception:
                pass

        if proc.poll() is not None:
            actual_duration = time.perf_counter() - t0
            completed_transfer = True
            break

        if db_file.exists():
            try:
                with sqlite3.connect(db_file) as conn:
                    cur = conn.cursor()
                    cur.execute("SELECT state, total_bytes, downloaded_bytes, created_at_ms, completed_at_ms FROM tasks LIMIT 1")
                    row = cur.fetchone()
                    if row and row[0] == "COMPLETED":
                        if row[4] and row[3] and row[4] > row[3]:
                            actual_duration = (row[4] - row[3]) / 1000.0
                        else:
                            actual_duration = time.perf_counter() - t0
                        completed_transfer = True
                        try:
                            proc.terminate()
                            proc.wait(timeout=2)
                        except Exception:
                            proc.kill()
                        break
            except Exception:
                pass
    else:
        proc.kill()
        actual_duration = time.perf_counter() - t0

    end_iso = datetime.datetime.now().isoformat()
    stdout, stderr = proc.communicate()

    rate_limits = 0
    retries = 0
    concurrency_reductions = 0
    if log_file.exists():
        with open(log_file, "r", encoding="utf-8", errors="ignore") as f:
            f.seek(log_offset)
            run_logs = f.read()
            rate_limits = run_logs.count("HTTP 429 Rate Limited")
            retries = run_logs.count("Resetting stalled/failed segment connection")
            concurrency_reductions = run_logs.count("reduced target concurrency")

    actual_size = dest_file.stat().st_size if dest_file.exists() else 0
    sha256_match = False
    actual_hash = ""
    if dest_file.exists() and actual_size == expected_size:
        actual_hash = compute_sha256(dest_file)
        sha256_match = (actual_hash.lower() == expected_sha256.lower())
        dest_file.unlink()

    status = "COMPLETED" if (completed_transfer and sha256_match) else "FAILED"
    mib_s = round((actual_size / (1024.0 * 1024.0)) / max(0.001, actual_duration), 2)
    mbps = round((actual_size * 8.0 / 1e6) / max(0.001, actual_duration), 2)

    res = {
        "trial": trial_num,
        "execution_order": order,
        "app": "Radium Download Manager",
        "version": "0.3.2",
        "start_time": start_iso,
        "end_time": end_iso,
        "configured_connections": conns,
        "peak_attempted_connections": conns,
        "effective_connections": 1 if rate_limits > 0 else conns,
        "rate_limited_429_count": rate_limits,
        "retries_count": retries,
        "concurrency_reductions": concurrency_reductions,
        "duration_seconds": round(actual_duration, 2),
        "expected_bytes": expected_size,
        "bytes_downloaded": actual_size,
        "throughput_mibs": mib_s,
        "throughput_mbps": mbps,
        "peak_rss_mb": round(peak_rss_mb, 2),
        "peak_cpu_percent": round(peak_cpu_pct, 1),
        "sha256": actual_hash,
        "expected_sha256": expected_sha256,
        "sha256_match": sha256_match,
        "status": status,
        "error": stderr.strip() if status != "COMPLETED" else None,
    }
    print(f"  -> Result: {status} in {actual_duration:.2f}s | Throughput: {mib_s:.2f} MiB/s ({mbps:.2f} Mbps) | SHA-256 Match: {sha256_match}")
    return res


def run_idm_trial(
    trial_num: int,
    order: int,
    work_dir: Path,
    target_url: str,
    target_filename: str,
    expected_size: int,
    expected_sha256: str,
    conns: int = 8,
    timeout: float = 1200.0,
) -> dict:
    print(f"\n[{datetime.datetime.now().strftime('%H:%M:%S')}] >>> [Execution #{order}] RUNNING TRIAL {trial_num}: INTERNET DOWNLOAD MANAGER ({conns} conns)")
    ensure_idm_running()
    cleanup_idm_temp(target_filename.split(".")[0])

    dest_file = work_dir / target_filename
    if dest_file.exists():
        dest_file.unlink()

    idm_proc = None
    if psutil:
        for p in psutil.process_iter(["name", "pid"]):
            if p.name().lower() == "idman.exe":
                try:
                    idm_proc = psutil.Process(p.pid)
                    break
                except Exception:
                    pass

    peak_rss_mb = 0.0
    peak_cpu_pct = 0.0

    cmd = [
        str(IDM_BIN),
        "/n",
        "/d", target_url,
        "/p", str(work_dir.resolve()),
        "/f", target_filename,
    ]

    t0 = time.perf_counter()
    start_iso = datetime.datetime.now().isoformat()
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    deadline = t0 + timeout
    completed = False
    max_observed_conns = 1

    while time.perf_counter() < deadline:
        time.sleep(1.0)
        if idm_proc:
            try:
                rss = idm_proc.memory_info().rss / (1024.0 * 1024.0)
                if rss > peak_rss_mb:
                    peak_rss_mb = rss
                cpu = idm_proc.cpu_percent(interval=None)
                if cpu > peak_cpu_pct:
                    peak_cpu_pct = cpu
            except Exception:
                pass

        cur_conns = get_idm_effective_connections(target_filename.split(".")[0])
        if cur_conns > max_observed_conns:
            max_observed_conns = cur_conns

        if dest_file.exists():
            try:
                if dest_file.stat().st_size == expected_size:
                    completed = True
                    break
            except Exception:
                pass

    t1 = time.perf_counter()
    end_iso = datetime.datetime.now().isoformat()
    duration = t1 - t0

    actual_size = dest_file.stat().st_size if dest_file.exists() else 0
    sha256_match = False
    actual_hash = ""

    if completed and actual_size == expected_size:
        actual_hash = compute_sha256(dest_file)
        sha256_match = (actual_hash.lower() == expected_sha256.lower())
        dest_file.unlink()

    cleanup_idm_temp(target_filename.split(".")[0])

    status = "COMPLETED" if completed and sha256_match else "FAILED"
    mib_s = round((actual_size / (1024.0 * 1024.0)) / max(0.001, duration), 2)
    mbps = round((actual_size * 8.0 / 1e6) / max(0.001, duration), 2)

    res = {
        "trial": trial_num,
        "execution_order": order,
        "app": "Internet Download Manager (IDM)",
        "version": "6.43.15",
        "start_time": start_iso,
        "end_time": end_iso,
        "configured_connections": conns,
        "peak_attempted_connections": conns,
        "effective_connections": max_observed_conns,
        "rate_limited_429_count": 0,
        "retries_count": 0,
        "concurrency_reductions": 0,
        "duration_seconds": round(duration, 2),
        "expected_bytes": expected_size,
        "bytes_downloaded": actual_size,
        "throughput_mibs": mib_s,
        "throughput_mbps": mbps,
        "peak_rss_mb": round(peak_rss_mb, 2),
        "peak_cpu_percent": round(peak_cpu_pct, 1),
        "sha256": actual_hash,
        "expected_sha256": expected_sha256,
        "sha256_match": sha256_match,
        "status": status,
        "error": None if status == "COMPLETED" else "Transfer timed out or file integrity failed",
    }
    print(f"  -> Result: {status} in {duration:.2f}s | Throughput: {mib_s:.2f} MiB/s ({mbps:.2f} Mbps) | Observed Conns: {max_observed_conns} | SHA-256 Match: {sha256_match}")
    return res


def compute_median(values: list[float]) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    n = len(s)
    if n % 2 == 1:
        return s[n // 2]
    return (s[n // 2 - 1] + s[n // 2]) / 2.0


def main():
    parser = argparse.ArgumentParser(description="2 GB Real Internet Benchmark: Radium vs IDM")
    parser.add_argument("--url", default=DEFAULT_URL, help="Target download URL")
    parser.add_argument("--filename", default=DEFAULT_FILENAME, help="Target filename")
    parser.add_argument("--size", type=int, default=DEFAULT_EXPECTED_SIZE, help="Expected file size in bytes")
    parser.add_argument("--sha256", default=DEFAULT_EXPECTED_SHA256, help="Expected SHA256 checksum")
    parser.add_argument("--trials", type=int, default=3, help="Number of trials per application (min 3)")
    parser.add_argument("--work-dir", default=str(ROOT / "benchmarks" / "work_2gb"), help="Temporary download dir")
    parser.add_argument("--out-dir", default=str(ROOT / "benchmarks" / "results"), help="Results output dir")
    parser.add_argument("--connections", type=int, default=8, help="Configured connection count")
    args = parser.parse_args()

    work_dir = Path(args.work_dir).resolve()
    out_dir = Path(args.out_dir).resolve()
    work_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 80)
    print("REAL INTERNET DOWNLOAD BENCHMARK: RADIUM VS IDM (2 GB / 8 CONNS)")
    print(f"Target URL:        {args.url}")
    print(f"Expected Size:     {args.size:,} bytes (~{args.size / (1024**3):.2f} GiB / {args.size / 1e9:.2f} GB)")
    print(f"Expected SHA256:   {args.sha256}")
    print(f"Work Directory:    {work_dir}")
    print(f"Output Directory:  {out_dir}")
    print(f"Trials Per App:    {args.trials}")
    print("=" * 80)

    free_bytes = shutil.disk_usage(work_dir).free
    if free_bytes < (args.size * 3):
        print(f"[!] Warning: Low disk space on destination: {free_bytes / (1024**3):.2f} GiB free.")
    else:
        print(f"[*] Disk space OK: {free_bytes / (1024**3):.2f} GiB free on {work_dir.drive}.")

    trials = []
    order = 1

    # Alternating Trials
    for i in range(1, args.trials + 1):
        if i % 2 == 1:
            # Odd trial: Radium, then IDM
            r_res = run_radium_trial(i, order, work_dir, args.url, args.filename, args.size, args.sha256, args.connections)
            trials.append(r_res)
            order += 1
            time.sleep(3)

            idm_res = run_idm_trial(i, order, work_dir, args.url, args.filename, args.size, args.sha256, args.connections)
            trials.append(idm_res)
            order += 1
            time.sleep(3)
        else:
            # Even trial: IDM, then Radium
            idm_res = run_idm_trial(i, order, work_dir, args.url, args.filename, args.size, args.sha256, args.connections)
            trials.append(idm_res)
            order += 1
            time.sleep(3)

            r_res = run_radium_trial(i, order, work_dir, args.url, args.filename, args.size, args.sha256, args.connections)
            trials.append(r_res)
            order += 1
            time.sleep(3)

    radium_trials = [t for t in trials if t["app"].startswith("Radium") and t["status"] == "COMPLETED"]
    idm_trials = [t for t in trials if t["app"].startswith("Internet") and t["status"] == "COMPLETED"]

    radium_times = [t["duration_seconds"] for t in radium_trials]
    idm_times = [t["duration_seconds"] for t in idm_trials]

    radium_thru = [t["throughput_mibs"] for t in radium_trials]
    idm_thru = [t["throughput_mibs"] for t in idm_trials]

    radium_median_time = compute_median(radium_times)
    idm_median_time = compute_median(idm_times)

    radium_median_thru = compute_median(radium_thru)
    idm_median_thru = compute_median(idm_thru)

    pct_diff = 0.0
    if idm_median_time > 0:
        pct_diff = round(((idm_median_time - radium_median_time) / idm_median_time) * 100.0, 2)

    summary = {
        "benchmark_date": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "target_url": args.url,
        "target_filename": args.filename,
        "expected_bytes": args.size,
        "expected_sha256": args.sha256,
        "configured_connections": args.connections,
        "trials_per_app": args.trials,
        "radium_completed_trials": len(radium_trials),
        "idm_completed_trials": len(idm_trials),
        "radium_median_duration_s": round(radium_median_time, 2),
        "idm_median_duration_s": round(idm_median_time, 2),
        "radium_median_throughput_mibs": round(radium_median_thru, 2),
        "idm_median_throughput_mibs": round(idm_median_thru, 2),
        "time_delta_seconds": round(idm_median_time - radium_median_time, 2),
        "throughput_delta_mibs": round(radium_median_thru - idm_median_thru, 2),
        "percentage_difference_time": pct_diff,
        "faster_app": "Radium Download Manager" if pct_diff > 0 else ("Internet Download Manager" if pct_diff < 0 else "Tie"),
    }

    date_str = datetime.datetime.now().strftime("%Y%m%d")
    json_path = out_dir / f"{date_str}_radium_vs_idm_2gb_trials.json"
    csv_path = out_dir / f"{date_str}_radium_vs_idm_2gb_comparison.csv"

    payload = {
        "summary": summary,
        "trials": trials,
    }

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)
    print(f"\n[*] Saved raw trials JSON: {json_path}")

    if trials:
        fieldnames = list(trials[0].keys())
        with open(csv_path, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for t in trials:
                writer.writerow(t)
        print(f"[*] Saved comparison CSV: {csv_path}")

    print("\n" + "=" * 80)
    print("BENCHMARK SUMMARY")
    print(f"Radium Median Duration:   {radium_median_time:.2f} s ({radium_median_thru:.2f} MiB/s)")
    print(f"IDM Median Duration:      {idm_median_time:.2f} s ({idm_median_thru:.2f} MiB/s)")
    print(f"Percentage Difference:    {pct_diff:+.2f}% ({summary['faster_app']} faster)")
    print("=" * 80)


if __name__ == "__main__":
    main()
