#!/usr/bin/env python3
"""Summarise ceiling ladders: per arm/size, forwarded pps, DUT-attributed loss, latency."""
import re, sys, collections
ansi = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
LINE = {64:14880952, 128:8445946, 256:4528986, 512:2349624, 1024:1197318, 1280:961538, 1514:815217}
rows = {}
for path in sys.argv[1:]:
    txt = ansi.sub("", open(path, errors="ignore").read()).replace("\r", "\n")
    for m in re.finditer(r"LATLOAD (\S+) size=(\d+) pct=([\d.]+) (.*?)(?=LATLOAD|\n|$)", txt):
        arm, size, pct, rest = m.group(1), int(m.group(2)), float(m.group(3)), m.group(4)
        kv = dict(re.findall(r"(\w+)=([\d.]+)", rest))
        if "off_pps" not in kv or "imissed" not in kv: continue
        rows[(arm, size, pct)] = kv
by = collections.defaultdict(list)
for (arm, size, pct), kv in sorted(rows.items(), key=lambda t:(t[0][0], t[0][1], t[0][2])):
    by[(arm, size)].append((pct, kv))
for (arm, size), lst in by.items():
    print(f"\n== {arm} {size}B  (line rate {LINE[size]/1e6:.2f} Mpps)")
    print(f"{'pct':>5} {'offered':>9} {'fwd Mpps':>9} {'%line':>6} {'DUTloss%':>9} {'rawloss%':>9} {'imissed':>8} {'p50':>6} {'p99':>7}")
    best = 0
    for pct, kv in lst:
        tx, rx, im = int(kv["tx"]), int(kv["rx"]), int(kv["imissed"])
        dut = max(0.0, (tx - rx - im) * 100.0 / tx) if tx else 0
        dur = 20.0 if arm in ("rawA_dpdk", "rawB_capio", "rawB_capio_r2", "rawB_capio_r3") else 30.0
        fwd = (rx + im) / dur
        best = max(best, fwd)
        print(f"{pct:5.0f} {int(kv['off_pps'])/1e6:9.3f} {fwd/1e6:9.3f} {fwd*100/LINE[size]:6.1f} {dut:9.3f} {float(kv['loss']):9.3f} {im:8d} {float(kv['p50']):6.0f} {float(kv['p99']):7.0f}")
    print(f"   max forwarded (echoed by DUT) = {best/1e6:.3f} Mpps = {best*100/LINE[size]:.1f}% of line rate")
