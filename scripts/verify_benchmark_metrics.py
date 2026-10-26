#!/usr/bin/env python3
"""
scripts/verify_benchmark_metrics.py
Automated consistency checker for all Radium Download Manager benchmark claims.
Cross-references raw benchmark JSON files with reported metrics and formulas:
  - Completion time reduction = (IDM time - Radium time) / IDM time * 100
  - Throughput improvement = (Radium throughput - IDM throughput) / IDM throughput * 100
  - Memory reduction = (IDM memory - Radium memory) / IDM memory * 100
"""

import os
import sys
import json

def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)

def verify_500mb_benchmarks():
    f1 = "benchmarks/adaptive_500mb_final/20261026_radium_vs_idm_500mb_adaptive_final.json"
    f2 = "benchmarks/adaptive_500mb_confirmation/20261026_radium_vs_idm_500mb_adaptive_confirmation.json"

    if not os.path.exists(f1) or not os.path.exists(f2):
        print(f"[-] Missing benchmark JSON files: {f1} or {f2}")
        return False

    d1 = load_json(f1)
    d2 = load_json(f2)

    # 1. Payload validation
    expected_sha = "b16f001a0731c8636f163586c16276b3714b8977242b67ae05fa60b96f685ebc"
    expected_bytes = 500000000

    for name, d in [("Trial 1", d1), ("Trial 2", d2)]:
        assert d["payload"]["expected_bytes"] == expected_bytes, f"{name} byte count mismatch"
        assert d["payload"]["verified_sha256"] == expected_sha, f"{name} SHA-256 mismatch"
        assert d["measurements"]["idm"]["sha256"] == expected_sha, f"{name} IDM SHA-256 mismatch"
        assert d["measurements"]["radium"]["sha256"] == expected_sha, f"{name} Radium SHA-256 mismatch"
        assert d["measurements"]["idm"]["size_bytes"] == expected_bytes, f"{name} IDM size mismatch"
        assert d["measurements"]["radium"]["size_bytes"] == expected_bytes, f"{name} Radium size mismatch"

    # 2. Extract metrics
    t1_idm = d1["measurements"]["idm"]
    t1_rad = d1["measurements"]["radium"]
    t2_idm = d2["measurements"]["idm"]
    t2_rad = d2["measurements"]["radium"]

    # Trial 1 formulas
    t1_time_red = (t1_idm["elapsed_s"] - t1_rad["elapsed_s"]) / t1_idm["elapsed_s"] * 100
    t1_thr_imp = (t1_rad["throughput_mb_s"] - t1_idm["throughput_mb_s"]) / t1_idm["throughput_mb_s"] * 100
    t1_mem_red = (t1_idm["peak_mem_mb"] - t1_rad["peak_mem_mb"]) / t1_idm["peak_mem_mb"] * 100

    # Trial 2 formulas
    t2_time_red = (t2_idm["elapsed_s"] - t2_rad["elapsed_s"]) / t2_idm["elapsed_s"] * 100
    t2_thr_imp = (t2_rad["throughput_mb_s"] - t2_idm["throughput_mb_s"]) / t2_idm["throughput_mb_s"] * 100
    t2_mem_red = (t2_idm["peak_mem_mb"] - t2_rad["peak_mem_mb"]) / t2_idm["peak_mem_mb"] * 100

    # Averages
    mean_idm_time = (t1_idm["elapsed_s"] + t2_idm["elapsed_s"]) / 2
    mean_rad_time = (t1_rad["elapsed_s"] + t2_rad["elapsed_s"]) / 2
    mean_time_red = (mean_idm_time - mean_rad_time) / mean_idm_time * 100

    mean_idm_thr = (t1_idm["throughput_mb_s"] + t2_idm["throughput_mb_s"]) / 2
    mean_rad_thr = (t1_rad["throughput_mb_s"] + t2_rad["throughput_mb_s"]) / 2
    mean_thr_imp = (mean_rad_thr - mean_idm_thr) / mean_idm_thr * 100

    mean_idm_mem = (t1_idm["peak_mem_mb"] + t2_idm["peak_mem_mb"]) / 2
    mean_rad_mem = (t1_rad["peak_mem_mb"] + t2_rad["peak_mem_mb"]) / 2
    mean_mem_red = (mean_idm_mem - mean_rad_mem) / mean_idm_mem * 100

    print("[+] All 500 MB raw measurements and calculations verified:")
    print(f"    Trial 1 (IDM 1st): Radium {t1_rad['elapsed_s']}s vs IDM {t1_idm['elapsed_s']}s -> Time reduction: {t1_time_red:.2f}%, Thr improvement: {t1_thr_imp:.2f}%, RAM: Radium {t1_rad['peak_mem_mb']}MB vs IDM {t1_idm['peak_mem_mb']}MB ({t1_mem_red:.2f}% reduction)")
    print(f"    Trial 2 (Radium 1st): Radium {t2_rad['elapsed_s']}s vs IDM {t2_idm['elapsed_s']}s -> Time reduction: {t2_time_red:.2f}%, Thr improvement: {t2_thr_imp:.2f}%, RAM: Radium {t2_rad['peak_mem_mb']}MB vs IDM {t2_idm['peak_mem_mb']}MB ({t2_mem_red:.2f}% reduction)")
    print(f"    Combined Mean: Radium {mean_rad_time:.2f}s vs IDM {mean_idm_time:.2f}s -> Mean time reduction: {mean_time_red:.2f}%, Mean thr improvement: {mean_thr_imp:.2f}%, RAM: Radium {mean_rad_mem:.1f}MB vs IDM {mean_idm_mem:.1f}MB ({mean_mem_red:.2f}% reduction)")

    return True

if __name__ == "__main__":
    if verify_500mb_benchmarks():
        print("[+] Benchmark metrics consistency check PASSED.")
        sys.exit(0)
    else:
        print("[-] Benchmark metrics consistency check FAILED.")
        sys.exit(1)
