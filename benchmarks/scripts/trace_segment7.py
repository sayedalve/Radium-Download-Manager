import re
import json

def analyze_timeline():
    # Let's see what was happening at t=68.39s
    # In radium_range_trace.log, find range_split line around t=68.4s
    with open("benchmarks/final_500mb_comparison/radium_range_trace.log") as f:
        lines = f.readlines()

    for i, line in enumerate(lines):
        if "range_split" in line and "parent_segment\":7" in line:
            print("Found split of segment 7:")
            for j in range(max(0, i-5), min(len(lines), i+15)):
                print(lines[j].strip())
            break

analyze_timeline()
