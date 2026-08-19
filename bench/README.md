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
