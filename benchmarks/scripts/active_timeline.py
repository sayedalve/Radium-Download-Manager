import json

with open("benchmarks/final_500mb_comparison/20261026_radium_vs_idm_500mb_final.json") as f:
    data = json.load(f)

rad = data["measurements"]["radium"]
reqs = rad["telemetry"]["requests"]

print("--- Active Requests Timeline (at 10s intervals) ---")
for t in range(0, 165, 10):
    t_ms = t * 1000.0
    active = [r for r in reqs if r["start_ms"] <= t_ms <= r["end_ms"]]
    workers = [r["worker"] for r in active]
    bytes_written_so_far = sum(
        min(r["file_bytes_written"], int(r["file_bytes_written"] * ((t_ms - r["start_ms"])/(r["end_ms"] - r["start_ms"]))))
        for r in active
    )
    print(f"t = {t:3d}s | Active requests: {len(active):2d} | Workers: {str(sorted(workers)):<24} | Bytes so far: {bytes_written_so_far/(1024*1024):.1f} MB")
