#!/usr/bin/env python3
"""
Heatmaps of CapIO-vs-DPDK RTT difference across UDP payload size x inter-packet
delay, styled to match figs/capio_raw_rtt_speedup.pdf in the ASPLOS paper.

Layout matches the paper figure: packet size on the vertical axis, inter-packet
delay on the horizontal, annotated cells, colorbar on the right.

Two deliberate departures from the reference figure, both forced by the data:

  * Diverging colormap centred on zero. The reference plots improvements over
    the FreeBSD kernel, which are large and single-signed, so a sequential map
    is fine there. Here both arms are kernel-bypass and the difference is near
    zero with both signs, so a sequential map would render noise as a gradient.
  * Fixed symmetric limits (default +/-25%) shared by all three figures, so p50,
    p90 and p99 are directly comparable and near-zero cells read as neutral.
    Auto-scaling to each panel's own range would stretch +/-2% across the full
    colormap and manufacture structure that is not there.

Sign convention follows the paper: positive = CapIO is faster than the baseline.

Usage: ./plot_sfc_heatmaps.py sweep_v8.csv --outdir heatmaps
"""
import argparse
import csv
import os

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

METRICS = [
    ("hw_p50", "P50", "Median"),
    ("hw_p90", "P90", "90th percentile"),
    ("hw_p99", "P99", "99th percentile"),
]

ap = argparse.ArgumentParser()
ap.add_argument("csv", nargs="?", default="sweep_v8.csv")
ap.add_argument("--target", default="capio")
ap.add_argument("--baseline", default="dpdk")
ap.add_argument("--outdir", default="heatmaps")
ap.add_argument("--lim", type=float, default=25.0,
                help="colour scale limit in %%, symmetric about zero")
a = ap.parse_args()

rows = list(csv.DictReader(open(a.csv)))
sizes = sorted({int(r["payload_bytes"]) for r in rows})
delays = sorted({int(r["delay_ms"]) for r in rows})
index = {(r["stack"], int(r["payload_bytes"]), int(r["delay_ms"])): r for r in rows}

os.makedirs(a.outdir, exist_ok=True)


def improvement(metric, sz, d):
    """Percent by which the target's latency is lower than the baseline's."""
    t = index.get((a.target, sz, d))
    b = index.get((a.baseline, sz, d))
    if t is None or b is None:
        return np.nan
    tv, bv = float(t[metric]), float(b[metric])
    if bv <= 0:
        return np.nan
    return (bv - tv) / bv * 100.0


for metric, label, longname in METRICS:
    M = np.array([[improvement(metric, sz, d) for d in delays] for sz in sizes])

    fig, ax = plt.subplots(figsize=(6.4, 4.8))
    im = ax.imshow(M, cmap="RdBu", vmin=-a.lim, vmax=a.lim, aspect="auto")

    ax.set_xticks(range(len(delays)))
    ax.set_xticklabels(delays, fontsize=13)
    ax.set_yticks(range(len(sizes)))
    ax.set_yticklabels(sizes, fontsize=13)
    ax.set_xlabel("Inter-packet Delay (ms)", fontsize=14, labelpad=7)
    ax.set_ylabel("UDP Packet Size (Bytes)", fontsize=14, labelpad=7)
    ax.set_title(f"CapIO-raw RTT Improvement over DPDK ({label})",
                 fontsize=15, pad=11)

    # thin white cell separators, as in the reference figure
    ax.set_xticks(np.arange(-0.5, len(delays), 1), minor=True)
    ax.set_yticks(np.arange(-0.5, len(sizes), 1), minor=True)
    ax.grid(which="minor", color="white", linewidth=1.2)
    ax.tick_params(which="minor", length=0)

    for i in range(len(sizes)):
        for j in range(len(delays)):
            v = M[i, j]
            if np.isnan(v):
                continue
            ax.text(j, i, f"{v:+.1f}%", ha="center", va="center", fontsize=11,
                    color="white" if abs(v) > a.lim * 0.55 else "black")

    cb = fig.colorbar(im, ax=ax, pad=0.02)
    cb.set_label(f"% lower {label} RTT than DPDK", fontsize=12)
    cb.ax.tick_params(labelsize=11)

    stem = os.path.join(a.outdir, f"capio_vs_dpdk_{metric.replace('hw_', '')}")
    fig.savefig(stem + ".pdf", bbox_inches="tight")
    fig.savefig(stem + ".png", dpi=200, bbox_inches="tight")
    plt.close(fig)

    finite = M[np.isfinite(M)]
    print(f"{label}: wrote {stem}.pdf/.png  "
          f"range {finite.min():+.1f}%..{finite.max():+.1f}%  "
          f"mean {finite.mean():+.2f}%")
