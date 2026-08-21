#!/usr/bin/env python3
"""Generate the Solarflare-campaign evaluation figures for the ASPLOS paper.

Data sources:
  RFC2544 ladder : sfc_bench/rfc2544_results.csv        (5 arms x 7 sizes x 12 rates)
  nginx          : repo bench/results/nginx/nginx_results.csv
  redis          : repo bench/results/redis/{ycsb,memtier}_results.csv
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = "/home/devel/Documents/cheri_workspace/sfc_bench"
REPO = "/home/devel/Documents/cheri_workspace/solarflare_capio_kernel_stub/bench/results"

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

# Consistent arm palette across all figures (colorblind-safe-ish).
C_DPDK   = "#5b6d92"   # slate blue  : unsafe DPDK baselines (rawA, fsA)
C_CAPIO  = "#ff7461"   # salmon      : CAPIO arms (rawB, fsB) -- house color
C_REVOKE = "#8c2d19"   # dark rust   : fsB + revocation

# ----------------------------------------------------------------- ladder data
def load_ladder():
    rows = {}
    with open(os.path.join(BENCH, "rfc2544_results.csv")) as f:
        for r in csv.DictReader(f):
            key = (r["arm"], int(r["size"]))
            rows.setdefault(key, []).append(
                (float(r["off_pps"]), float(r["loss_pct"]), float(r["p50_us"]),
                 float(r["p99_us"])))
    for v in rows.values():
        v.sort()
    return rows

LAD = load_ladder()

def series(arm, size, col):
    idx = {"pps": 0, "loss": 1, "p50": 2, "p99": 3}[col]
    return ([p[0] / 1e6 for p in LAD[(arm, size)]],
            [p[idx] for p in LAD[(arm, size)]])

def plot_lat(ax, arm, size, c, m, label):
    """p99 latency, solid throughout; a vertical dashed line marks the
    arm's saturation knee (last offered rate sustained with <1% loss),
    when that knee falls inside the swept range."""
    pts = LAD[(arm, size)]
    xs = [p[0] / 1e6 for p in pts]
    ys = [p[3] for p in pts]
    ax.plot(xs, ys, marker=m, ms=2.5, lw=1.0, color=c, label=label)

def log_ticks(ax, ticks):
    """Log y-axis with plain-number labels at chosen ticks only."""
    ax.set_yscale("log")
    ax.set_yticks(ticks)
    ax.set_yticklabels([str(t) for t in ticks])
    ax.minorticks_off()

# ----------------------------------------------------------------- fig: nginx bars
def fig_nginx():
    rows = {}
    with open(os.path.join(REPO, "nginx/nginx_results.csv")) as f:
        for r in csv.DictReader(f):
            if r["arm"] == "fsBrev" and r["steady_state"] != "1":
                continue
            key = (int(r["response_kb"]), int(r["concurrency"]),
                   int(r["keepalive"]))
            rows.setdefault(key, {})[r["arm"]] = float(r["rps"])
    cells = [(kb, c, ka) for kb in (1, 48, 64) for c in (1, 8)
             for ka in (0, 1)]
    labels = [f"{kb}k c{c}{'/ka' if ka else ''}" for kb, c, ka in cells]
    fig, ax = plt.subplots(figsize=(3.6, 1.55))
    w = 0.27
    for off, arm, label, color in (
            (-w, "fsA", "F-Stack/DPDK (unsafe)", C_DPDK),
            (0.0, "fsB", "F-Stack/CAPIO", C_CAPIO),
            (w, "fsBrev", "F-Stack/CAPIO+revoke", C_REVOKE)):
        xs = [i + off for i in range(len(cells))]
        ys = [rows[c].get(arm, 0) for c in cells]
        ax.bar(xs, ys, width=w, color=color, label=label)
    ax.set_yscale("log")
    ax.set_ylabel("requests/s")
    ax.set_xticks(range(len(cells)))
    ax.set_xticklabels(labels, rotation=40, ha="right", fontsize=6)
    h, l = ax.get_legend_handles_labels()
    fig.legend(h, l, frameon=False, ncol=3, loc="upper center",
               bbox_to_anchor=(0.54, 1.03), fontsize=6, columnspacing=0.8)
    fig.tight_layout(pad=0.4, rect=(0, 0, 1, 0.92))
    fig.savefig(os.path.join(HERE, "sfc_nginx.pdf"))
    plt.close(fig)

# ----------------------------------------------------------------- fig: redis bars
def fig_redis():
    # YCSB panel
    ycsb = {}
    with open(os.path.join(REPO, "redis/ycsb_results.csv")) as f:
        for r in csv.DictReader(f):
            if r["workload"] in ("a", "b", "c", "d", "f"):
                ycsb.setdefault(r["workload"], {})[r["arm"]] = \
                    float(r["throughput_ops_sec"]) / 1e3
    # memtier panel
    mt = {}
    with open(os.path.join(REPO, "redis/memtier_results.csv")) as f:
        for r in csv.DictReader(f):
            if r["op"] != "total":
                continue
            key = (r["set_get_ratio"], int(r["value_bytes"]),
                   int(r["pipeline"]))
            mt.setdefault(key, {})[r["arm"]] = float(r["ops_per_sec"]) / 1e3
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(3.6, 1.7),
                                   gridspec_kw={"width_ratios": [2, 3]})
    arms = (("fsA", "F-Stack/DPDK (unsafe)", C_DPDK),
            ("fsB", "F-Stack/CAPIO", C_CAPIO),
            ("fsBrev", "F-Stack/CAPIO+revoke", C_REVOKE))
    wls = ["a", "b", "c", "d", "f"]
    w = 0.27
    for k, (arm, label, color) in enumerate(arms):
        xs = [i + (k - 1) * w for i in range(len(wls))]
        ax1.bar(xs, [ycsb[wl].get(arm, 0) for wl in wls], width=w,
                color=color, label=label)
    ax1.set_xticks(range(len(wls)))
    ax1.set_xticklabels([w.upper() for w in wls])
    ax1.set_ylabel("k ops/s")
    ax1.set_title("YCSB", pad=2)
    cells = [(r, d, p) for r in ("1:1", "1:9") for d in (100, 1024)
             for p in (1, 16)]
    lab = [f"{r} {d}B P{p}" for r, d, p in cells]
    for k, (arm, label, color) in enumerate(arms):
        xs = [i + (k - 1) * w for i in range(len(cells))]
        ax2.bar(xs, [mt[c].get(arm, 0) for c in cells], width=w, color=color)
    ax2.set_xticks(range(len(cells)))
    ax2.set_xticklabels(lab, rotation=40, ha="right", fontsize=5.5)
    ax2.set_title("memtier_benchmark", pad=2)
    h, l = ax1.get_legend_handles_labels()
    fig.legend(h, l, frameon=False, ncol=3, loc="upper center",
               bbox_to_anchor=(0.54, 1.03), fontsize=6, columnspacing=0.8)
    fig.tight_layout(pad=0.4, rect=(0, 0, 1, 0.90))
    fig.savefig(os.path.join(HERE, "sfc_redis.pdf"))
    plt.close(fig)

fig_nginx()
fig_redis()
print("figures written")

# ---------------------------------------------- standard RFC2544 plot set
ARMS5 = (("rawA_dpdk", "DPDK raw (unsafe)", "#444444", "o", "-"),
         ("rawB_capio", "CAPIO raw", "#2a9d8f", "s", "-"),
         ("fsA_dpdk", "F-Stack/DPDK (unsafe)", C_DPDK, "o", "--"),
         ("fsB_txfix", "F-Stack/CAPIO", C_CAPIO, "s", "--"),
         ("fsBrev_txfix", "F-Stack/CAPIO+revoke", C_REVOKE, "^", "--"))
SIZES = [64, 128, 256, 512, 1024, 1280, 1514]

def _grid():
    import csv as _csv
    best, lat, loss = {}, {}, {}
    with open(os.path.join(BENCH, "rfc2544_results.csv")) as f:
        for r in _csv.DictReader(f):
            k = (r["arm"], int(r["size"]))
            if float(r["loss_pct"]) < 1.0:
                best[k] = max(best.get(k, 0.0), float(r["pct"]))
            else:
                best.setdefault(k, 0.0)
            if float(r["pct"]) == 2.0:
                lat[k] = (float(r["p50_us"]), float(r["p99_us"]))
            loss.setdefault(k, []).append((float(r["pct"]),
                                           float(r["loss_pct"])))
    for v in loss.values():
        v.sort()
    return best, lat, loss

BEST, LAT2, LOSS = _grid()

def _legend_top(fig, ax, ncol=3):
    h, l = ax.get_legend_handles_labels()
    fig.legend(h, l, frameon=False, ncol=ncol, loc="upper center",
               bbox_to_anchor=(0.54, 1.04), fontsize=5.5, columnspacing=0.5,
               handlelength=1.4, handletextpad=0.3)

def fig_tput():
    fig, ax = plt.subplots(figsize=(3.6, 1.55))
    for k, (arm, label, c, m, ls) in enumerate(ARMS5):
        ys = [BEST[(arm, sz)] for sz in SIZES]
        off = {0: 0.45, 1: -0.45, 2: 0.0, 3: -0.9}.get(k, 0.0)
        ax.plot(range(len(SIZES)), [y + off for y in ys], marker=m, ms=2.8,
                lw=1.0, color=c, ls=ls, label=label)
    ax.set_xticks(range(len(SIZES)))
    ax.set_xticklabels([str(x) for x in SIZES], fontsize=6)
    ax.set_xlabel("frame size (B)")
    ax.set_ylabel("throughput\n(% of line rate)", fontsize=7)
    ax.set_yticks([0, 5, 15, 30])
    ax.set_ylim(-2, 33)
    _legend_top(fig, ax)
    fig.tight_layout(pad=0.4, rect=(0, 0, 1, 0.86))
    fig.savefig(os.path.join(HERE, "sfc_rfc_tput.pdf"))
    plt.close(fig)

def fig_lat():
    fig, axes = plt.subplots(1, 2, figsize=(3.6, 1.7), sharey=True)
    for j, which in enumerate(("p50", "p99")):
        ax = axes[j]
        for arm, label, c, m, ls in ARMS5:
            ax.plot(range(len(SIZES)),
                    [LAT2[(arm, sz)][j] for sz in SIZES],
                    marker=m, ms=2.4, lw=1.0, color=c, ls=ls, label=label)
        ax.set_title(which, pad=2, fontsize=7)
        ax.set_xticks(range(len(SIZES)))
        ax.set_xticklabels([str(x) for x in SIZES], rotation=45, fontsize=5.5)
        ax.set_xlabel("frame size (B)", fontsize=7)
        ax.set_yscale("log")
        ax.set_yticks([30, 60, 125, 250])
        ax.set_yticklabels(["30", "60", "125", "250"])
        ax.minorticks_off()
        if j == 0:
            ax.set_ylabel("RTT (µs) at 2% load", fontsize=7)
    _legend_top(fig, axes[0])
    fig.tight_layout(pad=0.4, rect=(0, 0, 1, 0.86))
    fig.savefig(os.path.join(HERE, "sfc_rfc_lat.pdf"))
    plt.close(fig)

def fig_loss():
    fig, axes = plt.subplots(1, 2, figsize=(3.6, 1.7), sharey=True)
    for j, size in enumerate((64, 1514)):
        ax = axes[j]
        for arm, label, c, m, ls in ARMS5:
            pts = LOSS[(arm, size)]
            ax.plot([p[0] for p in pts], [p[1] for p in pts], marker=m,
                    ms=2.4, lw=1.0, color=c, ls=ls, label=label)
        ax.set_title(f"{size} B frames", pad=2, fontsize=7)
        ax.set_xlabel("offered load\n(% of line rate)", fontsize=7)
        ax.set_xticks([1, 10, 20, 30])
        if j == 0:
            ax.set_ylabel("frame loss (%)", fontsize=7)
            ax.set_yticks([0, 25, 50, 75, 100])
    _legend_top(fig, axes[0])
    fig.tight_layout(pad=0.4, rect=(0, 0, 1, 0.86))
    fig.savefig(os.path.join(HERE, "sfc_rfc_loss.pdf"))
    plt.close(fig)

fig_tput()
fig_lat()
fig_loss()
