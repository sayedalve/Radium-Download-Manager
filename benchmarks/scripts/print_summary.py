import json

with open("benchmarks/results/local_telemetry_audit/summary.json") as f:
    data = json.load(f)

print(f"{'Scenario':<15} {'Conns':<6} {'Median Transfer (ms)':<22} {'Useful MiB/s':<15} {'Peak Active':<12} {'Discarded Bytes':<15}")
print("-" * 85)
for s in data["summary"]:
    sc = s["scenario"]
    c = s["configured_connections"]
    transfers = sorted(s["transfer_ms_each"])
    med_t = transfers[len(transfers)//2]
    useful = sorted(s["useful_mib_s_each"])
    med_u = useful[len(useful)//2]
    peak = max(s["client_peak_active_each"])
    discarded = sum(s["discarded_client_bytes_each"])
    print(f"{sc:<15} {c:<6} {med_t:<22.2f} {med_u:<15.1f} {peak:<12} {discarded:<15}")
