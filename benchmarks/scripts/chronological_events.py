import json

with open("benchmarks/final_500mb_comparison/20261026_radium_vs_idm_500mb_final.json") as f:
    data = json.load(f)

rad = data["measurements"]["radium"]
reqs = rad["telemetry"]["requests"]

# Sort reqs by start_ms
reqs_by_start = sorted(reqs, key=lambda r: r["start_ms"])
reqs_by_end = sorted(reqs, key=lambda r: r["end_ms"])

print("--- Chronological Events ---")
events = []
for r in reqs:
    events.append((r["start_ms"], f"START Req {r['id']} (W{r['worker']}) [{r['requested_start']} - {r['requested_end']}]"))
    events.append((r["end_ms"], f"END Req {r['id']} (W{r['worker']}) written={r['file_bytes_written']/(1024*1024):.2f}MB reason={r['cancel_reason']}"))

events.sort(key=lambda x: x[0])
for t, desc in events:
    print(f"t = {t/1000.0:6.2f}s | {desc}")
