#!/bin/bash
# Rerun fsB and fsBrev RFC2544 rows with the TX-completion-fixed PMD.
# Config faithful to the final grid: sizes 64-512 @ window 384 + csum ON;
# sizes 1024-1514 @ window 256 + csum OFF (kenv hw.sfc7120pol.tx_csum=0).
set -u
SP=/tmp/claude-1000/-home-devel-Documents-cheri-workspace/0bee6b41-40bb-4861-a35b-b5e8e355ae56/scratchpad
BENCH=/home/devel/Documents/cheri_workspace/sfc_bench
M="ssh -o ConnectTimeout=10 -o ServerAliveInterval=15 -o StrictHostKeyChecking=no root@10.0.0.2"
log(){ echo "[txfix-rerun $(date +%H:%M:%S)] $*"; }
reboot_box(){ log "rebooting DUT"
  timeout 20 $M 'nohup sh -c "sleep 1; shutdown -r now" >/dev/null 2>&1 &' >/dev/null 2>&1
  sleep 25; local t=0
  while [ $t -lt 300 ]; do
    if timeout 15 $M 'exit 0' >/dev/null 2>&1; then log "box up (${t}s)"; sleep 10; return 0; fi
    sleep 10; t=$((t+10)); done
  log "ERROR: box did not come back"; exit 1; }
launch(){ # launch <binary> <window> <csum 0|1> <logname>
  $M "kenv hw.contigmem.num_buffers=8 >/dev/null; kenv hw.contigmem.buffer_size=268435456 >/dev/null; kldload contigmem >/dev/null 2>&1
kenv hw.sfc7120pol.tx_csum=$3 >/dev/null
kldload /root/sfc_main/sfc7120pol.ko >/dev/null 2>&1
rm -rf /var/run/dpdk; cd /root/f-stack/example
nohup sh -c \"SFC_RX_WINDOW=$2 FF_ECHO_PORT=11111 FF_EXTRA_EAL=\\\"--vdev=net_capio0,dev=/dev/sfc7120pol0 --no-shconf\\\" \
  cpuset -c -l 3 rtprio 0 ./$1 --conf f-stack.conf --proc-type=primary --proc-id=0 \
  > /root/$4 2>&1\" >/dev/null 2>&1 &
sleep 35"
  local n=$(timeout 20 $M "ps -axo comm | grep -c '$1'" 2>/dev/null | tr -d ' \r')
  if [ "$n" = "0" ]; then log "ERROR: DUT $1 did not start"; exit 1; fi
  log "DUT $1 up (window=$2 csum=$3)"; }

run_split_arm(){ # run_split_arm <binary> <label>
  local BIN="$1" LABEL="$2"
  log "$LABEL: small frames"
  reboot_box; launch "$BIN" 384 1 "${LABEL}-small.log"
  SIZES=64,128,256,512 TRIAL_MS=30000 WARM_RATE=1 "$SP/rfc2544/run_arm.sh" "$LABEL"
  cp /tmp/arm_${LABEL}.log /tmp/arm_${LABEL}_small.log
  log "$LABEL: big frames"
  reboot_box; launch "$BIN" 256 0 "${LABEL}-big.log"
  SIZES=1024,1280,1514 TRIAL_MS=30000 WARM_RATE=1 "$SP/rfc2544/run_arm.sh" "$LABEL"
  cp /tmp/arm_${LABEL}.log /tmp/arm_${LABEL}_big.log
  { sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' /tmp/arm_${LABEL}_small.log | grep -aoE "LATLOAD ${LABEL} size=(64|128|256|512) .*"
    sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' /tmp/arm_${LABEL}_big.log   | grep -aoE "LATLOAD ${LABEL} size=(1024|1280|1514) .*"
  } > /tmp/arm_${LABEL}_merged.log
  log "$LABEL merged rows: $(grep -c LATLOAD /tmp/arm_${LABEL}_merged.log)/84"; }

run_split_arm fstack_echo_pc        fsB_txfix
run_split_arm fstack_echo_pc_revoke fsBrev_txfix
log "CAMPAIGN COMPLETE"
