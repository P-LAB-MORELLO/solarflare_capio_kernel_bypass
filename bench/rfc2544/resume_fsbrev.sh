#!/bin/bash
# Resume fsBrev after the comm-truncation false abort. The small-frame DUT
# (window 384, csum 1, revoke binary) is already up and fresh; use it, then
# reboot for the big-frame segment.
set -u
SP=/tmp/claude-1000/-home-devel-Documents-cheri-workspace/0bee6b41-40bb-4861-a35b-b5e8e355ae56/scratchpad
M="ssh -o ConnectTimeout=10 -o ServerAliveInterval=15 -o StrictHostKeyChecking=no root@10.0.0.2"
log(){ echo "[fsbrev-resume $(date +%H:%M:%S)] $*"; }
alive(){ [ "$(timeout 20 $M 'ps -axo comm | grep -c fstack_echo_pc_revo' 2>/dev/null | tr -d ' \r')" != "0" ]; }

alive || { log "ERROR: DUT not up at start"; exit 1; }
log "fsBrev small frames (reusing live DUT)"
SIZES=64,128,256,512 TRIAL_MS=30000 WARM_RATE=1 "$SP/rfc2544/run_arm.sh" fsBrev_txfix
cp /tmp/arm_fsBrev_txfix.log /tmp/arm_fsBrev_txfix_small.log

log "fsBrev big frames"
timeout 20 $M 'nohup sh -c "sleep 1; shutdown -r now" >/dev/null 2>&1 &' >/dev/null 2>&1
sleep 25; t=0
while [ $t -lt 300 ]; do
  timeout 15 $M 'exit 0' >/dev/null 2>&1 && { log "box up (${t}s)"; sleep 10; break; }
  sleep 10; t=$((t+10)); done
[ $t -ge 300 ] && { log "ERROR: no boot"; exit 1; }
$M 'kenv hw.contigmem.num_buffers=8 >/dev/null; kenv hw.contigmem.buffer_size=268435456 >/dev/null; kldload contigmem >/dev/null 2>&1
kenv hw.sfc7120pol.tx_csum=0 >/dev/null
kldload /root/sfc_main/sfc7120pol.ko >/dev/null 2>&1
rm -rf /var/run/dpdk; cd /root/f-stack/example
nohup sh -c "SFC_RX_WINDOW=256 FF_ECHO_PORT=11111 FF_EXTRA_EAL=\"--vdev=net_capio0,dev=/dev/sfc7120pol0 --no-shconf\" \
  cpuset -c -l 3 rtprio 0 ./fstack_echo_pc_revoke --conf f-stack.conf --proc-type=primary --proc-id=0 \
  > /root/fsBrev_txfix-big.log 2>&1" >/dev/null 2>&1 &
sleep 35'
alive || { log "ERROR: big-frame DUT did not start"; exit 1; }
log "big-frame DUT up (window=256 csum=0)"
SIZES=1024,1280,1514 TRIAL_MS=30000 WARM_RATE=1 "$SP/rfc2544/run_arm.sh" fsBrev_txfix
cp /tmp/arm_fsBrev_txfix.log /tmp/arm_fsBrev_txfix_big.log
{ sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' /tmp/arm_fsBrev_txfix_small.log | grep -aoE "LATLOAD fsBrev_txfix size=(64|128|256|512) .*"
  sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' /tmp/arm_fsBrev_txfix_big.log   | grep -aoE "LATLOAD fsBrev_txfix size=(1024|1280|1514) .*"
} > /tmp/arm_fsBrev_txfix_merged.log
log "fsBrev merged: $(grep -c LATLOAD /tmp/arm_fsBrev_txfix_merged.log)/84"
log "RESUME COMPLETE"
