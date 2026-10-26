import os
import sys
import time
import hashlib
import psutil
import subprocess
import json
import threading

URL = "https://sg-lg.regxa.com/500MB.test"
EXPECTED_BYTES = 500000000
REFERENCE_SHA256 = "b16f001a0731c8636f163586c16276b3714b8977242b67ae05fa60b96f685ebc"
BENCHMARK_DIR = os.path.abspath("benchmarks/validated_500mb_comparison")
os.makedirs(BENCHMARK_DIR, exist_ok=True)

RADIUM_EXE = os.path.abspath("build/win-x64/Release/radium.exe")
IDM_EXE = "C:\\Program Files (x86)\\Internet Download Manager\\IDMan.exe"

def compute_sha256(path):
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

                # TCP connection count
                conns = 0
                for c in psutil.net_connections(kind="tcp"):
                    if c.pid in all_pids and c.status == "ESTABLISHED":
                        conns += 1
                tcp_samples.append(conns)
            else:
                time.sleep(0.25)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            time.sleep(0.25)
        except Exception:
            time.sleep(0.25)
    metrics["cpu_samples"] = cpu_samples
    metrics["mem_samples"] = mem_samples
    metrics["tcp_samples"] = tcp_samples

def run_idm():
    print("\n========================================================")
    print("--- [ORDER 1] IDM DOWNLOAD STARTING ---")
    print("========================================================")
    filename = "500MB_idm_final.test"
    out_file = os.path.join(BENCHMARK_DIR, filename)
    if os.path.exists(out_file):
        os.remove(out_file)

    cmd = [
        IDM_EXE,
        "/d", URL,
        "/p", BENCHMARK_DIR,
        "/f", filename,
        "/n"
    ]

    t_start = time.perf_counter()
    subprocess.Popen(cmd)

    # Allow IDMan to start
    time.sleep(0.5)
    stop_event = {"stop": False}
    metrics = {}

    def get_idm_pid():
        for p in psutil.process_iter(["pid", "name"]):
            try:
                if p.info["name"] and "IDMan" in p.info["name"]:
                    return p.info["pid"]
            except Exception:
                pass
        return None

    t_mon = threading.Thread(target=monitor_process, args=(get_idm_pid, stop_event, metrics))
    t_mon.start()

    # Poll for target file creation and complete size
    max_wait = 600 # up to 10 minutes
    elapsed = 0
    poll_start = time.perf_counter()
    while time.perf_counter() - poll_start < max_wait:
        if os.path.exists(out_file):
            sz = os.path.getsize(out_file)
            if sz == EXPECTED_BYTES:
                time.sleep(0.5)
                elapsed = time.perf_counter() - t_start
                break
        time.sleep(0.25)

    stop_event["stop"] = True
    t_mon.join(timeout=2)

    size = os.path.getsize(out_file) if os.path.exists(out_file) else 0
    sha = compute_sha256(out_file) if size == EXPECTED_BYTES else None

    cpu_list = metrics.get("cpu_samples", [0.0])
    mem_list = metrics.get("mem_samples", [0.0])
    tcp_list = metrics.get("tcp_samples", [0])
    avg_cpu = sum(cpu_list) / max(1, len(cpu_list))
    peak_cpu = max(cpu_list) if cpu_list else 0.0
    peak_mem = max(mem_list) if mem_list else 0.0
    avg_tcp = sum(tcp_list) / max(1, len(tcp_list))
    peak_tcp = max(tcp_list) if tcp_list else 0

    print(f"IDM Download Finished: elapsed={elapsed:.2f}s, size={size}, sha={sha}")
    res = {
        "app": "Internet Download Manager",
        "version": "6.43.15 (Build 2)",
        "executable": IDM_EXE,
        "elapsed_s": round(elapsed, 2),
        "size_bytes": size,
        "throughput_mb_s": round((size / (1000 * 1000)) / elapsed, 2) if elapsed > 0 else 0,
        "throughput_mib_s": round((size / (1024 * 1024)) / elapsed, 2) if elapsed > 0 else 0,
        "avg_cpu_percent": round(avg_cpu, 1),
        "peak_cpu_percent": round(peak_cpu, 1),
        "peak_mem_mb": round(peak_mem, 1),
        "avg_tcp_conns": round(avg_tcp, 1),
        "peak_tcp_conns": peak_tcp,
        "sha256": sha,
        "sha256_match": (sha == REFERENCE_SHA256),
        "status": "COMPLETED" if (size == EXPECTED_BYTES and sha == REFERENCE_SHA256) else "FAILED"
    }
    return res

def run_radium():
    print("\n========================================================")
    print("--- [ORDER 2] RADIUM DOWNLOAD STARTING ---")
    print("========================================================")
    out_file = os.path.join(BENCHMARK_DIR, "500MB_radium_final.test")
    db_file = os.path.join(BENCHMARK_DIR, "radium_final.db")
    log_file = os.path.join(BENCHMARK_DIR, "radium.log")

    if os.path.exists(out_file):
        os.remove(out_file)
    if os.path.exists(db_file):
        os.remove(db_file)
    if os.path.exists(log_file):
        os.remove(log_file)

    env = os.environ.copy()
    env["RADIUM_HTTP_RANGE_TRACE"] = "1"

    cmd = [
        RADIUM_EXE,
        "--db", db_file,
        "--out", BENCHMARK_DIR,
        "--connections", "8",
        "--download", URL
    ]

    t_start = time.perf_counter()
    proc = subprocess.Popen(cmd, env=env)

    stop_event = {"stop": False}
    metrics = {}
    t_mon = threading.Thread(target=monitor_process, args=(lambda: proc.pid, stop_event, metrics))
    t_mon.start()

    proc.wait(timeout=600)
    t_end = time.perf_counter()
    stop_event["stop"] = True
    t_mon.join(timeout=2)

    elapsed = t_end - t_start

    default_out = os.path.join(BENCHMARK_DIR, "500MB.test")
    if os.path.exists(default_out):
        if os.path.exists(out_file):
            os.remove(out_file)
        os.rename(default_out, out_file)

    size = os.path.getsize(out_file) if os.path.exists(out_file) else 0
    sha = compute_sha256(out_file) if size == EXPECTED_BYTES else None

    # Parse telemetry log
    saved_log = os.path.join(BENCHMARK_DIR, "radium_range_trace.log")
    if os.path.exists(log_file):
        if os.path.exists(saved_log):
            os.remove(saved_log)
        os.rename(log_file, saved_log)

    cpu_list = metrics.get("cpu_samples", [0.0])
    mem_list = metrics.get("mem_samples", [0.0])
    tcp_list = metrics.get("tcp_samples", [0])
    avg_cpu = sum(cpu_list) / max(1, len(cpu_list))
    peak_cpu = max(cpu_list) if cpu_list else 0.0
    peak_mem = max(mem_list) if mem_list else 0.0
    avg_tcp = sum(tcp_list) / max(1, len(tcp_list))
    peak_tcp = max(tcp_list) if tcp_list else 0

    telemetry = {
        "probe_duration_ms": 0,
        "range_requests_issued": 0,
        "range_splits": 0,
        "split_cancellations": 0,
        "segment_completions": 0,
        "client_bytes_received": 0,
        "file_bytes_written": 0,
        "slippage_bytes": 0,
        "retries": 0,
        "http_429_events": 0,
        "requests": []
    }

    if os.path.exists(saved_log):
        with open(saved_log, "r", errors="ignore") as f:
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
                if "Probe completed in" in line:
                    try:
                        ms = int(line.split("Probe completed in ")[1].split(" ms")[0])
                        telemetry["probe_duration_ms"] = ms
                    except Exception:
                        pass

    telemetry["slippage_bytes"] = max(0, telemetry["client_bytes_received"] - telemetry["file_bytes_written"])
    telemetry["slippage_percent"] = round((telemetry["slippage_bytes"] / max(1, telemetry["file_bytes_written"])) * 100, 3)

    print(f"Radium Download Finished: elapsed={elapsed:.2f}s, size={size}, sha={sha}")
    res = {
        "app": "Radium Download Manager",
        "version": "0.3.4 (Scheduler Work Balancing Fix)",
        "executable": RADIUM_EXE,
        "elapsed_s": round(elapsed, 2),
        "size_bytes": size,
        "throughput_mb_s": round((size / (1000 * 1000)) / elapsed, 2) if elapsed > 0 else 0,
        "throughput_mib_s": round((size / (1024 * 1024)) / elapsed, 2) if elapsed > 0 else 0,
        "avg_cpu_percent": round(avg_cpu, 1),
        "peak_cpu_percent": round(peak_cpu, 1),
        "peak_mem_mb": round(peak_mem, 1),
        "avg_tcp_conns": round(avg_tcp, 1),
        "peak_tcp_conns": peak_tcp,
        "sha256": sha,
        "sha256_match": (sha == REFERENCE_SHA256),
        "telemetry": telemetry,
        "status": "COMPLETED" if (size == EXPECTED_BYTES and sha == REFERENCE_SHA256) else "FAILED"
    }
    return res

if __name__ == "__main__":
    print(f"Starting 500 MB Live Final Benchmark: IDM -> Radium")
    idm_res = run_idm()
    print("Resting 3 seconds before Radium download...")
    time.sleep(3)
    radium_res = run_radium()

    final_data = {
        "benchmark_date": "2026-10-26",
        "payload": {
            "url": URL,
            "expected_bytes": EXPECTED_BYTES,
            "size_mb": 500.0,
            "size_mib": round(EXPECTED_BYTES / (1024 * 1024), 2),
            "verified_sha256": REFERENCE_SHA256
        },
        "environment": {
            "os": "Windows 11 Pro 64-bit",
            "cpu": "Intel Core i5-10400F @ 2.90GHz (6C/12T)",
            "ram": "16 GB DDR4",
            "instrumentation": "RADIUM_HTTP_RANGE_TRACE=1",
            "execution_order": ["IDM", "Radium"]
        },
        "measurements": {
            "idm": idm_res,
            "radium": radium_res
        }
    }

    out_json = os.path.join(BENCHMARK_DIR, "20261026_radium_vs_idm_500mb_validated.json")
    with open(out_json, "w") as f:
        json.dump(final_data, f, indent=2)

    print(f"\nSaved raw telemetry to: {out_json}")
