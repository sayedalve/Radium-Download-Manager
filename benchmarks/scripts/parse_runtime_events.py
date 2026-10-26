import re

events = []
with open("benchmarks/final_500mb_comparison/radium_range_trace.log") as f:
    for line in f:
        if "HTTP_RUNTIME_TRACE" in line:
            m = re.search(r'HTTP_RUNTIME_TRACE (\{.*\})', line)
            if m:
                events.append(m.group(1))

print(f"Total runtime trace events: {len(events)}")
