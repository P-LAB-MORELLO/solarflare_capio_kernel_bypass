#!/bin/bash
# Re-run rawB and fsB after the RX descriptor-window fix.
# rawB: fixed userlib (default window 64) built into sockperf_echo_plain.
# fsB:  rebuilt purecap PMD (window + TX-completion range-free + runt padding);
#       needs SFC_RX_WINDOW=384 -- F-Stack consumes in bursts and the 64
#       default (right for the tight raw echo loop) drops burst tails.
set -u
export SIZES=64,128,256,512,1024,1280,1514
export TRIAL_MS=30000
SP=/tmp/claude-1000/-home-devel-Documents-cheri-workspace/0bee6b41-40bb-4861-a35b-b5e8e355ae56/scratchpad
BENCH=/home/devel/Documents/cheri_workspace/sfc_bench
SSHO="-o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 -o StrictHostKeyChecking=no"
M="ssh $SSHO root@10.0.0.2"
CONTIG='kenv hw.contigmem.num_buffers=8 >/dev/null; kenv hw.contigmem.buffer_size=268435456 >/dev/null; kldload contigmem >/dev/null 2>&1'
log(){ echo "[campaign $(date +%H:%M:%S)] $*"; }
csv(){ python3 "$BENCH/rfc2544_to_csv.py" /tmp/arm_rawB_capio.log /tmp/arm_rawA_dpdk.log \
        /tmp/arm_fsA_dpdk.log /tmp/arm_fsB_capio.log 2>/dev/null || true; }
wait_boot(){ local t=0
  while [ $t -lt 300 ]; do
    if timeout 15 $M 'exit 0' >/dev/null 2>&1; then log "box up (${t}s)"; sleep 10; return 0; fi
    sleep 10; t=$((t+10)); done
  log "ERROR: box did not come back"; return 1; }
reboot_box(){ log "rebooting DUT"
  timeout 20 $M 'nohup sh -c "sleep 1; shutdown -r now" >/dev/null 2>&1 &' >/dev/null 2>&1
  sleep 25; wait_boot || exit 1; }
require_dut(){ local n="$1"
  if [ "$(timeout 20 $M "ps -axo comm | grep -c '$n'" 2>/dev/null | tr -d ' \r')" = "0" ]; then
    log "ERROR: DUT '$n' did not start; aborting"; exit 1; fi
  log "DUT '$n' up"; }

log "arm 1/2: rawB (fixed userlib, window 64)"
reboot_box
$M 'kldload /root/sfc_main/sfc7120pol.ko >/dev/null 2>&1
cd /root/sfc_main/userlib
nohup sh -c "cpuset -c -l 3 rtprio 0 ./sockperf_echo_plain /dev/sfc7120pol0 > /root/rawB-rerun.log 2>&1" >/dev/null 2>&1 &
sleep 8'
require_dut sockperf_echo_plain
"$SP/rfc2544/run_arm.sh" rawB_capio; csv

log "arm 2/2: fsB (purecap F-Stack + CAPIO, window 384)"
reboot_box
$M "$CONTIG
kldload /root/sfc_main/sfc7120pol.ko >/dev/null 2>&1
rm -rf /var/run/dpdk; cd /root/f-stack/example
nohup sh -c \"SFC_RX_WINDOW=384 FF_ECHO_PORT=11111 FF_EXTRA_EAL=\\\"--vdev=net_capio0,dev=/dev/sfc7120pol0 --no-shconf\\\" \
  cpuset -c -l 3 rtprio 0 ./fstack_echo_pc --conf f-stack.conf --proc-type=primary --proc-id=0 \
  > /root/fsB-rerun.log 2>&1\" >/dev/null 2>&1 &
sleep 35"
require_dut fstack_echo_pc
"$SP/rfc2544/run_arm.sh" fsB_capio; csv
log "RERUN COMPLETE"
