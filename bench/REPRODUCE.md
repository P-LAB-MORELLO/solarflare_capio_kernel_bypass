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
