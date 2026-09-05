# RFC 2544 benchmark harness and results

Five-configuration latency/throughput grid over a Solarflare SFN7322F
(EF10) link: pktgen-DPDK generator (Fedora) against a Morello CheriBSD DUT.
84 cells per arm: frame sizes 64/128/256/512/1024/1280/1514B x offered
rates 1-30% of line rate, 30s trials, latency percentiles from a patched
pktgen histogram (`pktgen-latency-histogram.patch`, 2us buckets).

## Arms
| arm          | DUT                                             | ABI     |
|--------------|-------------------------------------------------|---------|
| rawA_dpdk    | testpmd macswap, stock sfc PMD via nic_uio      | hybrid  |
| rawB_capio   | userlib/sockperf_echo (plain), CAPIO stub       | purecap |
| fsA_dpdk     | F-Stack UDP echo, stock sfc PMD                 | hybrid  |
| fsB_capio    | F-Stack UDP echo, CAPIO PMD (`fstack-capio/`)   | purecap |
| fsBrev_capio | fsB binary with CHERI heap revocation enabled   | purecap |

## Headline results (64B row)
- rawB == rawA inside the 30% grid: both forward 4.70 Mpps at the top 64B
  cell, p99 34-40 vs 30-34us. The CAPIO mechanism costs a few microseconds
  and no throughput here. NOTE: 30% is NOT the ceiling of either arm; see
  "Capacity ceilings" below for what happens above the grid.
- fsB = 0.57x fsA uniformly (ceiling 502 vs 876 kpps; p50, p99 and
  throughput all shift ~1.75x together): the purecap ABI tax on the stack.
- fsBrev = fsB - 25% further, and has NO loss-free rate at any size:
  heap-revocation quarantine stalls drop burst tails at all loads.

## Known caveats (also see commit messages)
- RESOLVED: the earlier large-frame standing queue was the RX descriptor
  window exceeding the NIC's descriptor-fetch capacity. W<=256 is clean at
  every size and rate; W=384 randomly picks 8ms-stale replies or a wedge per
  boot; W>=448 kills the RXQ outright. fsB/fsBrev big-frame rows were
  re-collected at SFC_RX_WINDOW=256 (small-frame rows are window-insensitive:
  values byte-identical at 256 and 384). The userlib default of 64 suits the
  tight raw-echo loop; F-Stack's bursty consumer wants 256.
- The PMD posts replacement RX descriptors at the ring TAIL (never rewriting
  a doorbell-covered slot) and carries a CAPIO_STATS heartbeat plus a
  CAPIO_RX_COPY diagnostic mode.
- fsBrev = same binary as fsB; only `elfctl -e +nocherirevoke` differs.
  Its 1024/1280/1514 rows were collected in a separate 18-min run because
  ARP expiry (~20 min) under revocation stalls breaks re-resolution.
- Raw logs in `results/*.xz`; canonical rawB is arm_rawB_merged.log
  (1514B row re-collected after a one-off DUT process crash).

## Reproducing
1. Patch pktgen-DPDK 24.x with `pktgen-latency-histogram.patch`, build.
2. Start the DUT for the arm under test (see `rfc2544/campaign_rerun.sh`
   for exact launch lines; reboot the Morello box between arms - never
   kldunload the CAPIO stub, its teardown panics the kernel).
3. `LABEL=<arm> SIZES=... RATES=... rfc2544/run_arm.sh <arm>` drives
   `rfc2544/latload.lua`; `../rfc2544_to_csv.py` merges arm logs to CSV.

## Capacity ceilings: how fast can each raw arm go?

The paper's grid stops at 30% of line rate. This section answers the
question it leaves open: if you keep pushing, where does each raw arm top
out? Data: `results/raw_ceiling_results.csv`. Script: `rfc2544/run_ceiling.sh`.
Summary tool: `rfc2544/analyze.py`.

### How to read the numbers

Think of the DUT as a funnel. We pour packets in faster and faster and
measure how many come out the bottom (are echoed back). The **ceiling** is
the most that ever came out, whatever we poured in. It is not the RFC 2544
"throughput under 1% loss" number, because the two arms handle overload
differently (see "Flow control" below), which makes a loss threshold unfair.

"DUT-attributed" means the generator's own receive misses are added back,
since the DUT did echo those frames. `analyze.py` does this for you.

### Results (paper configuration: 30 s trials, flow control at its default)

| frame | rawA (DPDK testpmd)                 | rawB (CAPIO echo)                  |
|-------|-------------------------------------|------------------------------------|
| 64B   | 7.0 Mpps = 47% of line rate         | 4.8 Mpps = 32% of line rate        |
| 128B  | 7.0 Mpps = 82% of line rate         | not measured cleanly (see below)   |
| 256B  | line rate (4.53 Mpps), zero loss    | line rate (4.53 Mpps), 0.003% loss |
| 512B  | line rate (2.35 Mpps), zero loss    | line rate (2.35 Mpps), 0.003% loss |

In words:

- Both arms are limited by per-packet CPU work on one Morello core, so each
  has a fixed packets-per-second cap regardless of frame size: about 7.0 Mpps
  for DPDK, about 4.8 Mpps for the CAPIO echo (1.46x apart).
- With small frames the cap is below what the 10 GbE wire can carry, so
  neither arm reaches line rate at 64B, and only DPDK does at 128B.
- With frames of 256B and up the wire is the limit, so both arms hit 100% of
  line rate with essentially no loss.
- The paper's 30% cap (4.46 Mpps at 64B) sits at 64% of DPDK's ceiling but
  right on top of CAPIO's. That is why the paper's 64B/30% rawB cell shows a
  522 us p99: it is CAPIO-raw at saturation.

The load generator (pktgen, one TX core) cannot offer more than ~8.3 Mpps at
64B, so 100% offered at 64B really means ~56%. This does not affect the
results because both DUTs plateau below 8.3 Mpps.

### Flow control (why the two arms look different under overload)

The CAPIO stub programs the NIC with Ethernet flow control on auto (the
sfxge default). When the DUT falls behind, its NIC sends PAUSE frames and
the generator slows down to exactly the DUT's drain rate. So under overload
rawB shows almost no loss but a large standing queue (~516 us at 64B), while
DPDK-raw simply drops the excess. Either way the amount echoed is the same
ceiling; only the fate of the excess differs. The client NIC's
`port_rx_pause` counter confirms the pause frames.

We tried turning flow control off in the stub to make both arms drop
(`kenv hw.sfc7120pol.fcntl=0` before kldload; default 3 = auto). Result:
the 64B ceiling is unchanged (4.79 Mpps), so the number is robust. But with
flow control off the CAPIO echo misbehaves at larger frames: at 512B it
carries a constant backlog of about one RX ring even at 30% load with zero
loss, then stops echoing entirely at 50% offered, far below its capacity,
and the kernel hangs soon after. A control run with the same stub build and
flow control back at its default was clean (line rate, 56 us p50). The cause
is not known. Treat `fcntl=0` as a diagnostic knob, not a benchmark
configuration. If a symmetric drop-based comparison is ever wanted, enable
pause on the DPDK arm instead of disabling it on CAPIO.

### The 128B gap and the wedge

Twice, at 128B with 3.0-3.4 Mpps offered, the CAPIO echo daemon kept
busy-polling but stopped echoing (rx=0 for every later trial), with nothing
in the kernel log; the first time the kernel hard-hung during the trials
that followed. This is a robustness bug in the raw userlib, not a capacity
number, and it is not root-caused. It did not happen at 64B, nor at 256/512B
with flow control at its default. Until it is fixed, do not report a 128B
ceiling for rawB, and never let a ladder keep running after an rx=0 row.

### Row sets in `raw_ceiling_results.csv`

| arm label            | what it is                                                   | use |
|----------------------|--------------------------------------------------------------|-----|
| rawA_dpdk            | DPDK ladder, 20 s trials                                     | ok  |
| rawA_dpdk_30s        | same with 30 s trials; matches row for row                   | ok (cite this) |
| rawB_capio           | CAPIO ladder, 20 s; 64B rows valid, 128B+ rows are post-wedge (100% loss) | 64B only |
| rawB_capio_r2        | CAPIO 128B rerun on a fresh daemon; wedged again at 35%      | evidence of the wedge |
| rawB_capio_r3        | CAPIO 256B/512B rerun on a fresh daemon, 20 s                | ok (cite this) |
| rawB_capio_fc0       | CAPIO 64B/256B, flow control OFF, 30 s                       | 64B ceiling only |
| rawB_capio_fc0b      | CAPIO 512B, flow control OFF, 30 s; wedged at 50%            | diagnostic |
| rawB_capio_ctl_fc3   | CAPIO 512B, flow control default, rebuilt stub; clean        | control |
