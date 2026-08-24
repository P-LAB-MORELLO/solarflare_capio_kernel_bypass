#!/bin/bash
# Rerun all three F-Stack arms with net.inet.udp.recvspace=2000000
# (applied via [freebsd.sysctl] in example/f-stack.conf + the
# ff_freebsd_init EINVAL-retry fix). Same per-size window/csum split
# as the published grid. Labels: fsA_bigbuf / fsB_bigbuf / fsBrev_bigbuf.
set -u
SP=/tmp/claude-1000/-home-devel-Documents-cheri-workspace/0bee6b41-40bb-4861-a35b-b5e8e355ae56/scratchpad
M="ssh -o ConnectTimeout=10 -o ServerAliveInterval=15 -o StrictHostKeyChecking=no root@10.0.0.2"
log(){ echo "[bigbuf $(date +%H:%M:%S)] $*"; }
reboot_box(){ log "rebooting DUT"
  timeout 20 $M 'nohup sh -c "sleep 1; shutdown -r now" >/dev/null 2>&1 &' >/dev/null 2>&1
  sleep 25; local t=0
  while [ $t -lt 300 ]; do
    if timeout 15 $M 'exit 0' >/dev/null 2>&1; then log "box up (${t}s)"; sleep 10; return 0; fi
    sleep 10; t=$((t+10)); done
  log "ERROR: box did not come back"; exit 1; }

launch_capio(){ # <binary> <window> <csum> <log>
  $M "kenv hw.contigmem.num_buffers=8 >/dev/null; kenv hw.contigmem.buffer_size=268435456 >/dev/null; kldload contigmem >/dev/null 2>&1
kenv hw.sfc7120pol.tx_csum=$3 >/dev/null
kldload /root/sfc_main/sfc7120pol.ko >/dev/null 2>&1
rm -rf /var/run/dpdk; cd /root/f-stack/example
nohup sh -c \"SFC_RX_WINDOW=$2 FF_ECHO_PORT=11111 FF_EXTRA_EAL=\\\"--vdev=net_capio0,dev=/dev/sfc7120pol0 --no-shconf\\\" \
  cpuset -c -l 3 rtprio 0 ./$1 --conf f-stack.conf --proc-type=primary --proc-id=0 \
  > /root/$4 2>&1\" >/dev/null 2>&1 &
sleep 35"
  alive_check "$1"; }

launch_hyb(){ # <log>
  $M "kenv hw.contigmem.num_buffers=8 >/dev/null; kenv hw.contigmem.buffer_size=268435456 >/dev/null; kldload contigmem >/dev/null 2>&1
kenv hw.nic_uio.bdfs=\"3:0:0\" >/dev/null; kldload nic_uio >/dev/null 2>&1
rm -rf /var/run/dpdk; cd /root/f-stack/example
nohup sh -c \"FF_ECHO_PORT=11111 FF_EXTRA_EAL=\\\"--no-shconf\\\" \
  cpuset -c -l 3 rtprio 0 ./fstack_echo --conf f-stack.conf --proc-type=primary --proc-id=0 \
  > /root/$1 2>&1\" >/dev/null 2>&1 &
sleep 35"
  alive_check fstack_echo; }

alive_check(){ local pat=$(echo "$1" | cut -c1-19)
  local n=$(timeout 20 $M "ps -axo comm | grep -c '$pat'" 2>/dev/null | tr -d ' \r')
  if [ "$n" = "0" ]; then echo "[bigbuf] ERROR: DUT $1 did not start"; exit 1; fi
  log "DUT $1 up"; }

ladder_small(){ SIZES=64,128,256,512 TRIAL_MS=30000 WARM_RATE=1 "$SP/rfc2544/run_arm.sh" "$1"
  cp /tmp/arm_$1.log /tmp/arm_$1_small.log; }
ladder_big(){ SIZES=1024,1280,1514 TRIAL_MS=30000 WARM_RATE=1 "$SP/rfc2544/run_arm.sh" "$1"
  cp /tmp/arm_$1.log /tmp/arm_$1_big.log; }
merge(){ { sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' /tmp/arm_$1_small.log | grep -aoE "LATLOAD $1 size=(64|128|256|512) .*"
  sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' /tmp/arm_$1_big.log | grep -aoE "LATLOAD $1 size=(1024|1280|1514) .*"
} > /tmp/arm_$1_merged.log
  log "$1 merged: $(grep -c LATLOAD /tmp/arm_$1_merged.log)/84"; }

# ---- fsA (hybrid, nic_uio; no window/csum knobs apply) ----
log "arm 1/3: fsA_bigbuf small"; reboot_box; launch_hyb fsA_bb-small.log
ladder_small fsA_bigbuf
log "fsA_bigbuf big"; reboot_box; launch_hyb fsA_bb-big.log
ladder_big fsA_bigbuf; merge fsA_bigbuf

# ---- fsB (purecap, revocation off) ----
log "arm 2/3: fsB_bigbuf small"; reboot_box; launch_capio fstack_echo_pc 384 1 fsB_bb-small.log
ladder_small fsB_bigbuf
log "fsB_bigbuf big"; reboot_box; launch_capio fstack_echo_pc 256 0 fsB_bb-big.log
ladder_big fsB_bigbuf; merge fsB_bigbuf

# ---- fsBrev (purecap, revocation on) ----
log "arm 3/3: fsBrev_bigbuf small"; reboot_box; launch_capio fstack_echo_pc_revoke 384 1 fsBrev_bb-small.log
ladder_small fsBrev_bigbuf
log "fsBrev_bigbuf big"; reboot_box; launch_capio fstack_echo_pc_revoke 256 0 fsBrev_bb-big.log
ladder_big fsBrev_bigbuf; merge fsBrev_bigbuf
log "CAMPAIGN COMPLETE"
