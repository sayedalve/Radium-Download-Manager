import os
import sys
import time
import hashlib
import psutil
import subprocess
import json

URL = "https://mirror.xeonbd.com/archlinux/iso/latest/archlinux-2026.10.01-x86_64.iso"
EXPECTED_BYTES = 1640497152
REFERENCE_SHA256 = "684ded26c63240ff4a41e8c25ee84ea6da233f557364821f13d12c2b0a9059a5"
WORK_DIR = os.path.abspath("benchmarks/xeonbd_arch_benchmark")
RADIUM_EXE = os.path.abspath("build/win-x64/Release/radium.exe")
IDM_EXE = "C:\\Program Files (x86)\\Internet Download Manager\\IDMan.exe"

def compute_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1024 * 1024):
            h.update(chunk)
    return h.hexdigest()

def monitor_process(pid, stop_event, metrics):
    cpu_samples = []
    mem_samples = []
    try:
        p = psutil.Process(pid)
        while not stop_event["stop"]:
            try:
                cpu = p.cpu_percent(interval=0.25)
                mem = p.memory_info().rss / (1024 * 1024)
                for child in p.children(recursive=True):
                    try:
                        cpu += child.cpu_percent(interval=None)
                        mem += child.memory_info().rss / (1024 * 1024)
                    except Exception:
                        pass
                cpu_samples.append(cpu)
                mem_samples.append(mem)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                break
    except Exception:
        pass
    metrics["cpu_samples"] = cpu_samples
    metrics["mem_samples"] = mem_samples

def run_radium_trial(trial_num):
    print(f"\n========================================================")
    print(f"--- [Trial {trial_num}] RADIUM DOWNLOAD STARTING ---")
    print(f"========================================================")
    out_file = os.path.join(WORK_DIR, f"arch_radium_t{trial_num}.iso")
    db_file = os.path.join(WORK_DIR, f"radium_t{trial_num}.db")
    log_file = os.path.join(WORK_DIR, f"radium.log") # Radium logs to radium.log next to db

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
        "--out", WORK_DIR,
        "--connections", "8",
        "--download", URL
    ]

    t_start = time.perf_counter()
    proc = subprocess.Popen(cmd, env=env)

    stop_event = {"stop": False}
    metrics = {}
    import threading
    t_mon = threading.Thread(target=monitor_process, args=(proc.pid, stop_event, metrics))
    t_mon.start()

    proc.wait(timeout=1200) # up to 20 minutes for 1.64 GB
    t_end = time.perf_counter()
    stop_event["stop"] = True
    t_mon.join(timeout=2)

    elapsed = t_end - t_start

    # Move default filename to trial-specific name
    default_out = os.path.join(WORK_DIR, "archlinux-2026.10.01-x86_64.iso")
    if os.path.exists(default_out):
        if os.path.exists(out_file):
            os.remove(out_file)
        os.rename(default_out, out_file)

    size = os.path.getsize(out_file) if os.path.exists(out_file) else 0
    sha = compute_sha256(out_file) if size == EXPECTED_BYTES else None

    # Save log file specific to trial
    saved_log = os.path.join(WORK_DIR, f"radium_t{trial_num}.log")
    if os.path.exists(log_file):
        if os.path.exists(saved_log):
            os.remove(saved_log)
        os.rename(log_file, saved_log)

    cpu_list = metrics.get("cpu_samples", [0.0])
    mem_list = metrics.get("mem_samples", [0.0])
    avg_cpu = sum(cpu_list) / max(1, len(cpu_list))
    peak_cpu = max(cpu_list) if cpu_list else 0.0
    peak_mem = max(mem_list) if mem_list else 0.0

    # Parse telemetry from saved log
    telemetry = {
        "range_requests_issued": 0,
        "range_splits": 0,
        "cancellations": 0,
        "retries": 0,
        "client_bytes_received": 0,
        "file_bytes_written": 0,
        "probe_duration_ms": 0
    }
    if os.path.exists(saved_log):
        with open(saved_log, "r", errors="ignore") as f:
            for line in f:
                if "HTTP_RANGE_TRACE" in line:
                    telemetry["range_requests_issued"] += 1
                    try:
                        j_str = line.split("HTTP_RANGE_TRACE ", 1)[1].strip()
                        rec = json.loads(j_str)
                        telemetry["client_bytes_received"] += rec.get("client_bytes_received", 0)
                        telemetry["file_bytes_written"] += rec.get("file_bytes_written", 0)
                        if rec.get("outcome") == "cancelled":
                            telemetry["cancellations"] += 1
                        if rec.get("cancel_reason") == "range_split":
                            telemetry["range_splits"] += 1
                    except Exception:
                        pass
                if "Probe completed in" in line:
                    try:
                        ms = int(line.split("Probe completed in ")[1].split(" ms")[0])
                        telemetry["probe_duration_ms"] = ms
                    except Exception:
                        pass

    print(f"Radium Trial {trial_num} Finished: elapsed={elapsed:.2f}s, size={size}, sha={sha}")
    res = {
        "trial": trial_num,
        "app": "Radium Download Manager",
        "version": "0.3.2 (with Segment Guard)",
        "executable": RADIUM_EXE,
        "elapsed_s": round(elapsed, 2),
        "size_bytes": size,
        "throughput_mb_s": round((size / (1000 * 1000)) / elapsed, 2) if elapsed > 0 else 0,
        "throughput_mib_s": round((size / (1024 * 1024)) / elapsed, 2) if elapsed > 0 else 0,
        "avg_cpu_percent": round(avg_cpu, 1),
        "peak_cpu_percent": round(peak_cpu, 1),
        "peak_mem_mb": round(peak_mem, 1),
        "sha256": sha,
        "sha256_match": (sha == REFERENCE_SHA256),
        "status": "COMPLETED" if (size == EXPECTED_BYTES and sha == REFERENCE_SHA256) else "FAILED",
        "telemetry": telemetry
    }
    # Clean up output file to preserve disk space
    if os.path.exists(out_file):
        os.remove(out_file)
    return res

def run_idm_trial(trial_num):
    print(f"\n========================================================")
    print(f"--- [Trial {trial_num}] IDM DOWNLOAD STARTING ---")
    print(f"========================================================")
    filename = f"arch_idm_t{trial_num}.iso"
    out_file = os.path.join(WORK_DIR, filename)
    if os.path.exists(out_file):
        os.remove(out_file)

    cmd = [
        IDM_EXE,
        "/d", URL,
        "/p", WORK_DIR,
        "/f", filename,
        "/n"
    ]

    t_start = time.perf_counter()
    subprocess.Popen(cmd)

    # Monitor IDMan process
    time.sleep(0.5)
    idm_pids = [p.info["pid"] for p in psutil.process_iter(["pid", "name"]) if p.info["name"] and "IDMan" in p.info["name"]]
    idm_pid = idm_pids[0] if idm_pids else None

    stop_event = {"stop": False}
    metrics = {}
    import threading
    if idm_pid:
        t_mon = threading.Thread(target=monitor_process, args=(idm_pid, stop_event, metrics))
        t_mon.start()

    # Poll for target file creation and complete size
    max_wait = 1200 # up to 20 minutes
    elapsed = 0
    poll_start = time.perf_counter()
    while time.perf_counter() - poll_start < max_wait:
        if os.path.exists(out_file):
            sz = os.path.getsize(out_file)
            if sz == EXPECTED_BYTES:
                time.sleep(0.5)
                elapsed = time.perf_counter() - t_start
                break
        time.sleep(0.5)

    stop_event["stop"] = True
    if idm_pid:
        t_mon.join(timeout=2)

    size = os.path.getsize(out_file) if os.path.exists(out_file) else 0
    sha = compute_sha256(out_file) if size == EXPECTED_BYTES else None

    cpu_list = metrics.get("cpu_samples", [0.0])
    mem_list = metrics.get("mem_samples", [0.0])
    avg_cpu = sum(cpu_list) / max(1, len(cpu_list))
    peak_cpu = max(cpu_list) if cpu_list else 0.0
    peak_mem = max(mem_list) if mem_list else 0.0

    print(f"IDM Trial {trial_num} Finished: elapsed={elapsed:.2f}s, size={size}, sha={sha}")
    res = {
        "trial": trial_num,
        "app": "Internet Download Manager",
        "version": "6.43.15",
        "executable": IDM_EXE,
        "elapsed_s": round(elapsed, 2),
        "size_bytes": size,
        "throughput_mb_s": round((size / (1000 * 1000)) / elapsed, 2) if elapsed > 0 else 0,
        "throughput_mib_s": round((size / (1024 * 1024)) / elapsed, 2) if elapsed > 0 else 0,
        "avg_cpu_percent": round(avg_cpu, 1),
        "peak_cpu_percent": round(peak_cpu, 1),
        "peak_mem_mb": round(peak_mem, 1),
        "sha256": sha,
        "sha256_match": (sha == REFERENCE_SHA256),
        "status": "COMPLETED" if (size == EXPECTED_BYTES and sha == REFERENCE_SHA256) else "FAILED"
    }
    # Clean up output file
    if os.path.exists(out_file):
        os.remove(out_file)
    return res

if __name__ == "__main__":
    results = []

    # Trial 1: Radium then IDM
    r1 = run_radium_trial(1)
    results.append(r1)
    if r1["status"] != "COMPLETED":
        print("Trial 1 Radium failed. Stopping.")
        sys.exit(1)

    time.sleep(3)
    i1 = run_idm_trial(1)
    results.append(i1)
    if i1["status"] != "COMPLETED":
        print("Trial 1 IDM failed. Stopping.")
        sys.exit(1)

    # Trial 2: IDM then Radium
    time.sleep(3)
    i2 = run_idm_trial(2)
    results.append(i2)
    if i2["status"] != "COMPLETED":
        print("Trial 2 IDM failed. Stopping.")
        sys.exit(1)

    time.sleep(3)
    r2 = run_radium_trial(2)
    results.append(r2)
    if r2["status"] != "COMPLETED":
        print("Trial 2 Radium failed. Stopping.")
        sys.exit(1)

    final_payload = {
        "benchmark_date": "2026-10-26",
        "url": URL,
        "expected_bytes": EXPECTED_BYTES,
        "reference_sha256": REFERENCE_SHA256,
        "trials": results
    }
    with open("benchmarks/xeonbd_arch_benchmark/results.json", "w") as f:
        json.dump(final_payload, f, indent=2)

    print("\n========================================================")
    print("ALL 4 DOWNLOADS COMPLETED AND VERIFIED")
    print("========================================================")
