import re
import json

def analyze():
    # Let's inspect the active segments state at t=68s, 71s, 73s, 88s, 110s, 114s
    # In radium_range_trace.log, get snapshots or check what was happening
    snapshots = []
    splits = []
    with open("benchmarks/final_500mb_comparison/radium_range_trace.log") as f:
        for line in f:
            if "range_split" in line:
                m = re.search(r'HTTP_RUNTIME_TRACE (\{.*\})', line)
                if m:
                    splits.append(json.loads(m.group(1)))

    print("--- All Splits Recorded ---")
    for s in splits:
        print(s)

analyze()
