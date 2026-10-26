import json

with open("benchmarks/final_500mb_comparison/20261026_radium_vs_idm_500mb_final.json") as f:
    data = json.load(f)

rad = data["measurements"]["radium"]
reqs = rad["telemetry"]["requests"]

print(f"{'Req':<4} {'Worker':<7} {'Requested Start':<16} {'Requested End':<16} {'Size (MB)':<10} {'Written (MB)':<12} {'Start (s)':<10} {'End (s)':<10} {'Dur (s)':<10} {'Outcome':<10} {'Cancel Reason':<15}")
print("-" * 125)
for r in reqs:
    sz = (r["requested_end"] - r["requested_start"] + 1) / (1024 * 1024)
    wr = r["file_bytes_written"] / (1024 * 1024)
    dur = (r["end_ms"] - r["start_ms"]) / 1000.0
    print(f"{r['id']:<4} {r['worker']:<7} {r['requested_start']:<16} {r['requested_end']:<16} {sz:<10.2f} {wr:<12.2f} {r['start_ms']/1000.0:<10.2f} {r['end_ms']/1000.0:<10.2f} {dur:<10.2f} {r['outcome']:<10} {r['cancel_reason']:<15}")
