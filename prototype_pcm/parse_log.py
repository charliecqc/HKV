import re

INPUT_FILE  = "output2.log"
OUTPUT_FILE = "parsed_output2.txt"

pattern = re.compile(
    r"\[(Running|Running Txn)\]\s*Time:\s*([\d\.]+)s,.*?"
    r"Interval Throughput:\s*([\d\.Ee\+\-]+)\s*ops/sec,.*?"
    r"Interval Avg Latency:\s*([\d\.]+)\s*us"
)

with open(INPUT_FILE, "r") as f_in, open(OUTPUT_FILE, "w") as f_out:
    for line in f_in:
        match = pattern.search(line)
        if match:
            time_val = float(match.group(2))
            throughput_raw = float(match.group(3))
            throughput_scaled = throughput_raw / 1_000_000
            latency = float(match.group(4))

            f_out.write(f"{time_val:.3f} {throughput_scaled:.5f} {latency}\n")

print(f"Parsed data written to {OUTPUT_FILE}")
