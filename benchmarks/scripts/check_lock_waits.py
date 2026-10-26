import json

wait_nonzero = []
with open("benchmarks/final_500mb_comparison/radium_range_trace.log") as f:
    for line in f:
        if "lock_timing" in line and '"lock":"segment_write"' in line:
            data = json.loads(line.split("HTTP_RUNTIME_TRACE ")[1])
            if data.get("wait_us", 0) > 0:
                wait_nonzero.append(data)

print(f"Total segment_write lock acquisitions with wait > 0: {len(wait_nonzero)}")
for item in wait_nonzero[:10]:
    print(item)
