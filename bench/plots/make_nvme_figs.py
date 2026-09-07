#!/usr/bin/env python3
"""Restyle the two NVMe evaluation figures to match the paper's house style.

Data:
  microbench : nvmepol/results.csv   (fio 4KB QD1, four configurations)
  TPC-H      : capio_sqlite/tpch_four_way_sf1.csv (three backends used here)
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
WS = "/home/devel/Documents/cheri_workspace"

plt.rcParams.update({
    "font.size": 8,
    "axes.titlesize": 8,
    "axes.labelsize": 8,
    "legend.fontsize": 7,
    "xtick.labelsize": 7,
    "ytick.labelsize": 7,
    "figure.dpi": 150,
    "pdf.fonttype": 42,
    "axes.spines.top": False,
    "axes.spines.right": False,
})

C_UNSAFE = "#444444"   # unsafe bypass baseline (SPDK), as for DPDK raw
C_KERNEL = "#5b6d92"   # kernel path
C_CAPIO  = "#ff7461"   # CAPIO sliced (the real configuration)
C_UNSL   = "#2a9d8f"   # CAPIO unsliced (ablation)

# ------------------------------------------------------------- microbenchmark
def fig_micro():
    rows = {}
    with open(os.path.join(WS, "nvmepol/results.csv")) as f:
        for r in csv.DictReader(f):
            rows[(r["config"], r["operation"])] = r
    configs = (("SPDK-bypass", "SPDK (unsafe)", C_UNSAFE),
               ("kernel-nvme4", "kernel nvme(4)", C_KERNEL),
               ("CAPIO-unsliced", "CAPIO unsliced (ablation)", C_UNSL),
               ("CAPIO-sliced", "CAPIO sliced (safe)", C_CAPIO))
    panels = (("mean latency (µs)", lambda r: float(r["mean_us"])),
              ("p99 latency (µs)", lambda r: float(r["p99_us"])),
              ("IOPS (thousands)", lambda r: float(r["iops"]) / 1e3))
    ops = (("randread", "read"), ("randwrite", "write"))
    fig, axes = plt.subplots(1, 3, figsize=(3.6, 1.55))
    w = 0.19
    for ax, (ylabel, get) in zip(axes, panels):
        for k, (key, label, color) in enumerate(configs):
            xs = [i + (k - 1.5) * w for i in range(len(ops))]
            ys = [get(rows[(key, op)]) for op, _ in ops]
            ax.bar(xs, ys, width=w, color=color,
                   label=label if ax is axes[0] else None)
        ax.set_xticks(range(len(ops)))
        ax.set_xticklabels([n for _, n in ops], fontsize=6.5)
        ax.set_ylabel(ylabel, fontsize=6.5, labelpad=1)
        ax.tick_params(axis="y", labelsize=6)
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, frameon=False, ncol=4, loc="upper center",
               bbox_to_anchor=(0.54, 1.0), fontsize=5.5, columnspacing=0.6,
               handlelength=1.1, handletextpad=0.3)
    fig.tight_layout(pad=0.35, rect=(0, 0, 1, 0.85), w_pad=0.8)
    # bbox_inches="tight" grows the canvas around every artist, so the
    # figure-level legend can never be clipped whatever its anchor.
    fig.savefig(os.path.join(HERE, "nvme_perf.pdf"), bbox_inches="tight",
                pad_inches=0.02)
    plt.close(fig)

# ------------------------------------------------------------ NVMe QD sweep
def fig_qd():
    """IOPS and mean latency versus queue depth (1, 8, 32, 128) for the four
    storage stacks, 4 KB random reads (top row) and writes (bottom row).
    Data: nvmepol/results_qd.csv (config,operation,qd,iops,...)."""
    rows = {}
    with open(os.path.join(WS, "nvmepol/results_qd.csv")) as f:
        for r in csv.DictReader(f):
            if r["iops"] == "ERROR":
                continue
            rows[(r["config"], r["operation"], int(r["qd"]))] = r
    configs = (("SPDK-bypass", "SPDK (unsafe)", C_UNSAFE),
               ("kernel-nvme4", "kernel nvme(4)", C_KERNEL),
               ("CAPIO-unsliced", "CAPIO unsliced (ablation)", C_UNSL),
               ("CAPIO-sliced", "CAPIO sliced (safe)", C_CAPIO))
    qds = sorted({k[2] for k in rows})
    ops = (("randread", "read"), ("randwrite", "write"))
    fig, axes = plt.subplots(2, 3, figsize=(7.2, 4.4), sharex=True)
    panels = (("IOPS (thousands)", lambda r: float(r["iops"]) / 1e3),
              ("mean latency (µs)", lambda r: float(r["mean_us"])),
              ("p99 latency (µs)", lambda r: float(r["p99_us"])))
    markers = ("o", "s", "^", "D")
    for row, (op, opname) in enumerate(ops):
        for col, (ylabel, get) in enumerate(panels):
            ax = axes[row][col]
            for k, (key, label, color) in enumerate(configs):
                xs = [q for q in qds if (key, op, q) in rows]
                ys = [get(rows[(key, op, q)]) for q in xs]
                ax.plot(xs, ys, marker=markers[k], ms=5, lw=1.4, color=color,
                        label=label if (row == 0 and col == 0) else None)
            ax.set_xscale("log", base=2)
            ax.set_xticks(qds)
            ax.set_xticklabels([str(q) for q in qds], fontsize=11)
            ax.minorticks_off()
            if col > 0:
                ax.set_yscale("log")
            ax.tick_params(axis="y", labelsize=11)
            if row == 0:
                ax.set_title(ylabel, fontsize=12, pad=5)
            if col == 0:
                ax.set_ylabel(f"4 KB {opname}", fontsize=12, labelpad=5)
            if row == 1:
                ax.set_xlabel("queue depth", fontsize=12, labelpad=4)
    h, l = axes[0][0].get_legend_handles_labels()
    fig.legend(h, l, frameon=False, ncol=4, loc="upper center",
               bbox_to_anchor=(0.52, 1.0), fontsize=11.5, columnspacing=1.6,
               handlelength=2.0, handletextpad=0.6)
    fig.tight_layout(pad=0.4, rect=(0, 0, 1, 0.91), w_pad=1.2, h_pad=1.4)
    fig.savefig(os.path.join(HERE, "nvme_qd.pdf"), bbox_inches="tight",
                pad_inches=0.02)
    plt.close(fig)

# --------------------------------------------------------------------- TPC-H
def fig_tpch():
    data = {}
    with open(os.path.join(WS, "capio_sqlite/tpch_four_way_sf1.csv")) as f:
        for r in csv.DictReader(f):
            if r["phase"] == "query":
                data.setdefault(r["backend"], {})[r["name"]] = \
                    float(r["wall_s"])
    backends = (("spdk", "SPDK (unsafe)", C_UNSAFE),
                ("capio", "CAPIO (safe)", C_CAPIO),
                ("unix_direct", "kernel FS (O_DIRECT)", C_KERNEL))
    # q13/q14/q22 need SQL dialect features SQLite lacks and fail on every
    # backend; exclude them as the text describes.
    queries = sorted(q for q in set(data["spdk"]) & set(data["capio"])
                     & set(data["unix_direct"])
                     if q not in ("q13", "q14", "q22")
                     and min(data[b][q] for b in
                             ("spdk", "capio", "unix_direct")) > 0.01)
    fig, ax = plt.subplots(figsize=(7.2, 2.05))
    w = 0.27
    for k, (key, label, color) in enumerate(backends):
        xs = [i + (k - 1) * w for i in range(len(queries))]
        ax.bar(xs, [data[key][q] for q in queries], width=w, color=color,
               label=label)
    # Label the CAPIO and kernel bars with their wall time relative to the
    # SPDK bar of the same query, in the series colour, so the reader can
    # tell what each number is a ratio of without consulting the caption.
    for i, q in enumerate(queries):
        for k, key, color, fmt in ((1, "capio", C_CAPIO, "{:.2f}x"),
                                   (2, "unix_direct", C_KERNEL, "{:.1f}x")):
            ratio = data[key][q] / data["spdk"][q]
            ax.text(i + (k - 1) * w, data[key][q] * 1.18, fmt.format(ratio),
                    ha="center", va="bottom", fontsize=5.6, color=color,
                    fontweight="bold", rotation=90)
    ax.text(0.0, 1.02, "bold numbers: wall time relative to SPDK "
            "(SPDK = 1x)", transform=ax.transAxes, ha="left", va="bottom",
            fontsize=6.5, color="#333333")
    ax.set_yscale("log")
    ax.set_yticks([1, 10, 100, 1000])
    ax.set_yticklabels(["1", "10", "100", "1000"])
    ax.minorticks_off()
    ax.set_ylim(top=15000)
    ax.set_ylabel("wall time (s)")
    ax.set_xticks(range(len(queries)))
    ax.set_xticklabels(queries, fontsize=6.5)
    ax.legend(frameon=False, ncol=3, loc="upper center",
              bbox_to_anchor=(0.5, 1.22), fontsize=6.5, columnspacing=1.0,
              handlelength=1.4)
    fig.tight_layout(pad=0.4)
    fig.savefig(os.path.join(HERE, "tpch_three_way_sf1.pdf"))
    plt.close(fig)

fig_micro()
if os.path.exists(os.path.join(WS, "nvmepol/results_qd.csv")):
    fig_qd()
fig_tpch()
print("nvme figures written")
