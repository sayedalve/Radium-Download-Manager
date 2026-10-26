import json

with open("benchmarks/final_500mb_comparison/20261026_radium_vs_idm_500mb_final.json") as f:
    data = json.load(f)

rad = data["measurements"]["radium"]
print(f"Elapsed: {rad['elapsed_s']} s")
print(f"Avg TCP: {rad['avg_tcp_conns']}, Peak TCP: {rad['peak_tcp_conns']}")
print("\nRequests:")
for req in rad["telemetry"]["requests"]:
    dur = (req["end_ms"] - req["start_ms"]) / 1000.0
    print(f"Req {req['id']:2d} | W{req['worker']} | range: [{req['requested_start']:10d} - {req['requested_end']:10d}] | "
          f"written: {req['file_bytes_written']:10d} B ({req['file_bytes_written']/(1024*1024):.2f} MB) | "
          f"start: {req['start_ms']/1000.0:6.2f}s | end: {req['end_ms']/1000.0:6.2f}s | dur: {dur:6.2f}s | "
          f"outcome: {req['outcome']:9s} | reason: {req['cancel_reason']}")
