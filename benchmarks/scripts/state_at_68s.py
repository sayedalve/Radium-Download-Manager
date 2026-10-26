import json

with open("benchmarks/final_500mb_comparison/20261026_radium_vs_idm_500mb_final.json") as f:
    data = json.load(f)

# At t=68.39s (which is 68394ms into the run):
t_target = 68394.0

print(f"--- State of all requests at t = 68.39s ---")
for r in data["measurements"]["radium"]["telemetry"]["requests"]:
    st = r["start_ms"]
    end = r["end_ms"]
    if st <= t_target <= end:
        # Request is active!
        # How much had it downloaded by t_target?
        dur = end - st
        frac = (t_target - st) / max(1.0, dur)
        bytes_wr = r["file_bytes_written"]
        est_curr = r["requested_start"] + int(bytes_wr * frac)
        rem = r["requested_end"] - est_curr
        print(f"Req {r['id']:2d} (W{r['worker']}, Seg {r['worker']}): range=[{r['requested_start']} - {r['requested_end']}] | "
              f"total_wr={bytes_wr/(1024*1024):.2f}MB | est_downloaded={est_curr/(1024*1024):.2f}MB | est_rem={rem/(1024*1024):.2f}MB")
