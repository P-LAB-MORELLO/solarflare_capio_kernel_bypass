# Reproducing the paper's networking evaluation

Everything below runs against two directly cabled machines: the Morello DUT
(CheriBSD, Solarflare SFN7322F-R2, PF0 = pci0:3:0:0) and a Fedora load
generator (Solarflare SFC9120). Results in `results/` were produced exactly
this way; `rfc2544_results.csv` carries the five-arm grid (the `*_txfix`
rows are the post-TX-fix reruns cited by the paper).

## Build (Morello)

Morello builds must be `-j1` (parallel builds silently produce empty
objects).

- Kernel stub: `cd sfc_main && make -j1` -> `sfc7120pol.ko`
- F-Stack purecap: `fsbuild_pc.sh` (cleans shared `lib/` first; the rm is
  load-bearing because hybrid and purecap builds share the directory)
- F-Stack hybrid: `fsbuild_hybrid.sh`
- DPDK: `ninja -C f-stack/dpdk/build-purecap install` (the CAPIO PMD lives
  in `drivers/net/capio/`)
- nginx / redis: see `fstack-capio/README.md` and
  `fstack-capio/redis-port/` (redis caches build flags in
  `src/.make-settings`; delete it after any Makefile change)

Binary feature flags (set with the process stopped; "Text file busy"
failures are silent in scripts):

- every purecap benchmark binary: `elfctl -e +noaslr` (cores do not
  symbolize without it)
- fsB arms: `elfctl -e +nocherirevoke`; fsBrev arms: leave revocation on

## DUT launch recipes (one arm at a time; reboot between arms)

Reboot before every CAPIO launch; never `kldunload sfc7120pol` (kernel
panic in teardown).

Common preamble after boot:

    kenv hw.contigmem.num_buffers=8
    kenv hw.contigmem.buffer_size=268435456
    kldload contigmem

CAPIO arms add:

    kldload /root/sfc_main/sfc7120pol.ko          # csum on by default
    kenv hw.sfc7120pol.tx_csum=0                  # only for the RFC2544
                                                  # large-frame segment
    rm -rf /var/run/dpdk

DPDK arms instead bind the NIC:

    kenv hw.nic_uio.bdfs="3:0:0"
    kldload nic_uio

Launches (hold stdin open; DPDK apps exit when stdin closes):

    # rawB (CAPIO raw echo)
    sleep 100000 | cpuset -c -l 3 rtprio 0 \
      ./sockperf_echo_plain /dev/sfc7120pol0

    # fsB/fsBrev echo (RFC2544): window 384 for 64-512B frames,
    # window 256 + tx_csum=0 for 1024-1514B
    sleep 100000 | SFC_RX_WINDOW=384 FF_ECHO_PORT=11111 \
      FF_EXTRA_EAL="--vdev=net_capio0,dev=/dev/sfc7120pol0 --no-shconf" \
      cpuset -c -l 3 rtprio 0 ./fstack_echo_pc \
      --conf f-stack.conf --proc-type=primary --proc-id=0

    # nginx / redis on CAPIO (window 256)
    sleep 100000 | SFC_RX_WINDOW=256 \
      FF_EXTRA_EAL="--vdev=net_capio0,dev=/dev/sfc7120pol0 --no-shconf" \
      cpuset -c -l 3 /root/nginx-pc/sbin/nginx -p /root/nginx-pc
    sleep 100000 | SFC_RX_WINDOW=256 \
      FF_EXTRA_EAL="--vdev=net_capio0,dev=/dev/sfc7120pol0 --no-shconf" \
      cpuset -c -l 3 /root/redis-pc/redis-server \
      --conf f-stack.conf --proc-type=primary --proc-id=0 redis.conf

    # hybrid (fsA) variants: same lines minus SFC_RX_WINDOW/--vdev, with
    # FF_EXTRA_EAL="--no-shconf", binaries from the hybrid build, nic_uio
    # loaded instead of sfc7120pol

## Client (Fedora)

- Run `bench_env.sh` after every reboot (C-states off; without it P99
  spread is ~80x worse).
- pktgen needs the client NIC on vfio-pci (noiommu):
  unbind from `sfc`, `driver_override` to vfio-pci, bind. Rebind to `sfc`
  and re-add 10.0.1.1/24 afterwards for the TCP benchmarks.
- RFC2544 grid: `rfc2544/run_arm.sh <label>` per arm segment;
  `rfc2544/rerun_txfix.sh` reproduces the paper's fsB/fsBrev rows end to
  end (reboots the DUT itself). Convert logs with `rfc2544_to_csv.py`
  (NOTE: it overwrites the CSV with only the logs given; always pass every
  arm log).
- Applications: `ab` (nginx grid in `results/nginx/README.md`),
  `redis-benchmark`, memtier_benchmark (built from git), YCSB 0.17.0
  (`bin/ycsb.sh`, no python2 needed). Cells as named in the results CSVs.

## Revocation arms: warm-up protocol

Revocation throughput depends on cumulative allocation churn. Warm every
fsBrev measurement with ~300k operations of the same workload and report
steady state; for nginx run each cell twice and report the second pass.
Fresh-process numbers are up to 5x higher and unrepresentative.

## Figures

`plots/make_figs.py` regenerates the networking figures from
`results/*.csv`; `plots/make_nvme_figs.py` regenerates the NVMe figures
from the `nvmepol` and `capio_sqlite` repositories' CSVs (paths at the top
of each script).

## Known artifact: 1514B frame loss step at >=20% of line (both F-Stack arms)

Diagnosed 2026-08-24. FreeBSD's default UDP receive buffer
(net.inet.udp.recvspace = 42080 bytes) holds only 28 maximum-size
datagrams; a single 32-frame burst delivered between poll-loop drains
overflows it (32 x 1472 = 47104 bytes). At 1280B a full burst fits, which
is why no other frame size shows the step. Loss is a flat 1.1-3.3%
independent of overload (unlike true saturation), NIC and PMD counters
show near-zero drops, and the loss sits between stack input and echo
output. Fix: net.inet.udp.recvspace=2000000 in [freebsd.sysctl]
(0.52%/0.33% loss at 20%/30% of line, vs 3.27% default). Requires the
ff_freebsd_init.c EINVAL-retry fix: integer config values are applied as
4-byte writes and u_long sysctls reject them.

RESOLUTION (2026-08-24): all three F-Stack arms were rerun in full with
recvspace=2MB (`rfc2544/rerun_bigbuf.sh`, rows `*_bigbuf` in the CSV,
raw logs in `results/rfc2544_logs/`). The paper's figures now use the
`*_bigbuf` rows. Verdict: the 1514B step vanishes on fsA and fsB (both
sustain the 30% sweep ceiling, ~0.1-0.3% loss); every other column
reproduces the old grid within run-to-run variation, INCLUDING all of
the revocation arm's small-frame losses, which are therefore genuine
poll-loop-stall drops at the NIC ring and not a buffering artifact.
One knife-edge cell moved for the worse (fsA 128B@10%: 0.69% -> 2.57%
with the cliff at 12% in both runs) - near-criterion variance, not a
buffer effect. Default-configuration rows (`fsA_dpdk`, `fs*_txfix`)
are retained for comparison.

Diagnostic method (reusable): pktgen blasts line-rate frames during
startup before the Lua script paces it, so bracketing DUT counters around
a whole run is meaningless; align per-second CSTAT (CAPIO_STATS=1) and
the MAC-stats sampler by rate signature and integrate over the steady
window only.

## Loss attribution

The CSV's tx/rx/loss_pct columns are raw generator-side counts. The
`imissed` column is the generator NIC's own receive-miss counter for the
trial: returned frames the client dropped for lack of RX descriptors.
Figures and paper claims use DUT-attributed loss, (tx - rx - imissed)/tx,
because those frames were successfully echoed by the DUT. This matters in
18 of 420 trials (clustered at 256B@5-6% and 512B@10%); worst case is
rawB 256B@5%, where the raw 11.1% "loss" is 100% client-side (DUT-
attributed 0.000%). No sustained-rate figure changes under attribution;
the raw arms' worst DUT-attributed trial is 0.158% (DPDK) / 0.026% (CAPIO).

## Capacity ceilings (rates above the 30% grid)

What it measures: how many packets per second each raw arm can echo when
you keep raising the offered rate past the paper's 30% cap. Results and
their interpretation are in `README.md`, "Capacity ceilings".

Client side, once:

1. Build pktgen 24.03.1 with the histogram patch and Lua enabled. The
   files in `pktgen-modified/app/` are drop-in replacements for that tag:

       git clone https://github.com/pktgen/Pktgen-DPDK && cd Pktgen-DPDK
       git checkout pktgen-24.03.1
       cp <repo>/bench/pktgen-modified/app/*.{c,h} app/
       meson setup build -Denable_lua=true && ninja -C build

   Without `-Denable_lua=true` pktgen starts but silently ignores the
   script ("please build with Lua enabled" in the log, no rows).
2. Run `bench_env.sh` (C-states off) and bind the client PF0 to vfio-pci
   as described above.

Per arm:

3. Launch the DUT exactly as for the RFC 2544 grid (recipes above). For a
   CAPIO arm reboot the Morello box first; the stub cannot be unloaded and
   only the first daemon after a load receives RX events.
4. On the client:

       PKTGEN_DIR=~/Pktgen-DPDK TRIAL_MS=30000 rfc2544/run_ceiling.sh <label>

   Defaults sweep 64/128/256/512B at 30-100% of line rate. Override with
   SIZES= and RATES=. Put 128B last for CAPIO arms.
5. Watch the log while it runs:

       grep -ao "LATLOAD <label> size=[^ ]* pct=[^ ]* .*rx=[0-9]*" \
           results/rfc2544_logs/arm_<label>.log

   If a row shows `rx=0`, the CAPIO echo has wedged. Kill pktgen at once
   (`sudo pkill -9 -f 'app/pktgen -l'`). Flooding a wedged DUT has hung the
   Morello kernel hard (console dead, not in ddb). Recovery: the board
   controller CLI on `/dev/ttyUSB1` (115200 8N1) accepts `power reboot -d`;
   the kernel console is `/dev/ttyUSB2`.
6. Summarise and convert:

       python3 rfc2544/analyze.py results/rfc2544_logs/arm_<label>.log
       python3 rfc2544_to_csv.py results/rfc2544_logs/arm_*.log

   (`rfc2544_to_csv.py` overwrites its CSV with only the logs given.)

Stub tunable: `kenv hw.sfc7120pol.fcntl=<0|1|2|3>` before kldload sets the
NIC's Ethernet flow-control mode (0 off, 3 auto = default). Leave it at the
default for benchmarks; 0 is diagnostic only (see README, "Flow control").
