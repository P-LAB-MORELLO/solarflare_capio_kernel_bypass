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
- rawB == rawA: ceiling 4.70 vs 4.69 Mpps, p99 34-40 vs 30-34us.
  The CAPIO mechanism costs a few microseconds and no throughput.
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

## Raw-arm capacity ceilings (above the 30% cap)

`results/raw_ceiling_results.csv` extends the raw arms to 30-100% of line
rate (20 s trials, `rfc2544/run_ceiling.sh`, summarise with
`rfc2544/analyze.py`). DUT-attributed max forwarded rate:

| frame | rawA (DPDK testpmd) | rawB (CAPIO echo) |
|-------|---------------------|-------------------|
| 64B   | 6.98 Mpps (47% line), loss cliff past 45% offered | 4.77 Mpps (32% line), flat at every offered rate |
| 128B  | 6.96 Mpps (82% line) | RX wedge at >=3.0 Mpps offered (rows `rawB_capio_r2`) |
| 256B  | 99.9% line, zero loss | 99.9% line, 0.003% loss (rows `rawB_capio_r3`) |
| 512B  | 100% line, zero loss | 100% line, 0.003% loss |

The CAPIO stub enables MAC flow control (FCNTL_AUTO), so under overload the
NIC pauses the generator instead of dropping: rawB 64B rows show
offered == forwarded == 4.77 Mpps with a ~516 us standing queue and <0.1%
DUT loss. Compare arms on forwarded pps, not loss. The 64B rawB rows after
the 128B wedge in the first ladder (`rawB_capio` at 128/256/512B) are
100%-loss and superseded by the `_r2`/`_r3` reruns on a fresh daemon.
The generator itself tops out near 8.3 Mpps at 64B (single TX core).

Follow-up rows (2026-09-05): `rawA_dpdk_30s` repeats the rawA ladder with
30 s trials (matches the 20 s ladder row for row). `rawB_capio_fc0` runs the
CAPIO echo with MAC flow control OFF (`kenv hw.sfc7120pol.fcntl=0` before
kldload; default 3 = auto, unchanged) and 30 s trials: the 64B ceiling is
the same 4.79 Mpps but now a drop curve (42% loss at the generator's 8.3
Mpps maximum), and at 256B the daemon wedges once offered load exceeds
its ~4.8 Mpps per-packet capacity (90% of line), which pause had been
masking. 128B/512B fc0 rows were not collected (DUT hung after the wedge).

Control (rows `rawB_capio_fc0b`, `rawB_capio_ctl_fc3`): with flow control
OFF the CAPIO echo shows a ring-sized standing backlog at 512B even at 30%
offered and wedges at 50% (1.18 Mpps), far below its per-packet ceiling;
the same rebuilt stub at the default fcntl=3 runs 512B to line rate with
56 us p50 and no wedge. The fcntl=0 anomaly is not root-caused, so the
tunable is diagnostic only; the one conclusion that survives it is that
the 64B ceiling (4.8 Mpps) is identical with pause on or off. For a
symmetric drop-based comparison, enable pause on the DPDK arm instead.
