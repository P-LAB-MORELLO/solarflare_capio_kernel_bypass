#!/bin/bash
# run_arm.sh <label> — run the latency-vs-load ladder for the currently
# running DUT and append machine-readable results to rfc2544_results.txt.
#
# The ladder doubles as the RFC 2544 throughput source: loss-vs-rate comes from
# the same trials, and the DUT's loss curve is non-monotonic (a real medium-
# rate stall on this hardware), so a binary NDR search would mislead; we report
# the whole curve and derive max-clean-rate from it.
set -u
LABEL="${1:?usage: run_arm.sh <label>}"
SP=/tmp/claude-1000/-home-devel-Documents-cheri-workspace/0bee6b41-40bb-4861-a35b-b5e8e355ae56/scratchpad
OUT=/home/devel/Documents/cheri_workspace/sfc_bench/rfc2544_results.txt
cd "$SP/Pktgen-DPDK"
echo devel1234 | sudo -S script -qec \
  "timeout 7200 env LABEL=$LABEL \
     SIZES=${SIZES:-64,512,1518} \
     RATES=${RATES:-1,2,3,4,5,6,8,10,12,15,20,30} \
     TRIAL_MS=${TRIAL_MS:-30000} LAT_US=${LAT_US:-500} \
     ./build/app/pktgen -l 1-3 -n 4 --no-telemetry -- -P -m '[2:3].0' \
     -f $SP/rfc2544/latload.lua" /dev/null > /tmp/arm_$LABEL.log 2>&1
sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' /tmp/arm_$LABEL.log | grep -aoE "(LATLOAD|WARMUP|LATLOAD_[A-Z]+).*" >> "$OUT"
echo "$LABEL done: $(grep -c "LATLOAD $LABEL" "$OUT") rows" 
