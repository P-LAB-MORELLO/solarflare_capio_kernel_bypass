#!/usr/bin/env python3
"""Convert LATLOAD result lines (from the pktgen RFC2544 harness) to CSV.

Reads every file given on argv (raw pktgen logs or the appended results file),
strips ANSI, extracts LATLOAD lines, and writes one row per trial.
"""
import csv
import os, re, sys

COLS = ["arm","size","pct","off_pps","tx","rx","loss_pct",
        "lat_min_us","p50_us","p90_us","p99_us","p999_us",
        "lat_n","skipped","hist_n","jitter_n","imissed","ierrors","oerrors"]
KEYMAP = {"size":"size","pct":"pct","off_pps":"off_pps","tx":"tx","rx":"rx",
          "loss":"loss_pct","lat_min":"lat_min_us","p50":"p50_us","p90":"p90_us",
          "p99":"p99_us","p999":"p999_us","lat_n":"lat_n","skipped":"skipped",
          "hist_n":"hist_n","jitter_n":"jitter_n","imissed":"imissed",
          "ierrors":"ierrors","oerrors":"oerrors"}

ansi = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
rows = []
for path in sys.argv[1:]:
    try: text = open(path, errors="ignore").read()
    except FileNotFoundError: continue
    for line in ansi.sub("", text).splitlines():
        m = re.search(r"LATLOAD (\S+) (.*)", line)
        if not m or m.group(1).endswith("_BEGIN"): continue
        row = {"arm": m.group(1)}
        for k, v in re.findall(r"(\w+)=([\d.]+)", m.group(2)):
            if k in KEYMAP: row[KEYMAP[k]] = v
        if "size" in row and "loss_pct" in row: rows.append(row)

# de-duplicate (results file may repeat log content), keep last occurrence
seen = {}
for r in rows: seen[(r["arm"], r["size"], r["pct"])] = r
# Absolute path: this script is invoked from orchestrators whose cwd varies,
# and a relative path silently drops the CSV wherever they happened to start.
_OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rfc2544_results.csv")
out = csv.DictWriter(open(_OUT,"w",newline=""), fieldnames=COLS)
out.writeheader()
for k in sorted(seen, key=lambda t:(t[0], int(t[1]), float(t[2]))):
    out.writerow({c: seen[k].get(c,"") for c in COLS})
print(f"rfc2544_results.csv: {len(seen)} rows")
