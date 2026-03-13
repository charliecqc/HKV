import csv

INPUT_FILE = "benchmark1.csv"
OUTPUT_FILE = "pmm_bw.txt"

# Normalizer to ensure matching works even with spaces / weird cases
def norm(x):
    return x.strip().lower().replace(" ", "")

# Columns we want
TARGETS = ["systempmmread", "systempmmwrite", "systempmmwwrite"]

with open(INPUT_FILE, "r") as f:
    reader = csv.reader(f)

    # Read TWO header rows
    header1 = next(reader)
    header2 = next(reader)

    # Combine: "System" + "PMMREAD" -> "System PMMREAD"
    merged_header = [ (h1 + " " + h2).strip() for h1, h2 in zip(header1, header2) ]

    normalized = [norm(h) for h in merged_header]

    # Find columns
    col_indices = []
    for target in TARGETS:
        if target in normalized:
            col_indices.append(normalized.index(target))

    if len(col_indices) < 2:
        print("Merged header columns:")
        for i, h in enumerate(merged_header):
            print(i, repr(h))
        raise ValueError("Could not find PMMREAD/PMMWRITE columns!")

    pmmread_col, pmmwrite_col = col_indices[:2]

# Now parse data rows
time_sec = 0

with open(INPUT_FILE, "r") as f_in, open(OUTPUT_FILE, "w") as f_out:
    reader = csv.reader(f_in)

    # Skip the two header lines
    next(reader)
    next(reader)

    # Output format: time pmmread pmmwrite
    f_out.write("time pmmread pmmwrite\n")

    for row in reader:
        try:
            pmmread = row[pmmread_col].strip()
            pmmwrite = row[pmmwrite_col].strip()

            f_out.write(f"{time_sec} {pmmread} {pmmwrite}\n")
            time_sec += 1
        except:
            continue

print(f"Extracted PMMREAD + PMMWRITE with time → {OUTPUT_FILE}")