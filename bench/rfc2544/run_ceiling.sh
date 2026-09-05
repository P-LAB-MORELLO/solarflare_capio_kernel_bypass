#!/bin/bash
# run_ceiling.sh <label> -- offered-load ladder above the paper's 30% cap, to locate the raw arms' capacity ceilings.
set -u
LABEL="${1:?usage: run_ceiling.sh <label>}"
S=/tmp/claude-1000/-home-devel-Documents-cheri-workspace/5cac86db-674f-48f3-b0be-11fce9509156/scratchpad
OUT=$S/ceiling/ceiling_results.txt
cd "$S/Pktgen-DPDK"
echo devel1234 | sudo -S script -qec \
  "timeout 7200 env LABEL=$LABEL \
     SIZES=${SIZES:-64,128,256,512} \
     RATES=${RATES:-30,35,40,45,50,60,70,80,90,100} \
     TRIAL_MS=${TRIAL_MS:-20000} LAT_US=${LAT_US:-500} \
     ./build/app/pktgen -l 1-3 -n 4 --no-telemetry -- -P -m '[2:3].0' \
     -f $S/ceiling/latload.lua" /dev/null > $S/ceiling/arm_$LABEL.log 2>&1
sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' $S/ceiling/arm_$LABEL.log | grep -aoE "(LATLOAD|WARMUP|LATLOAD_[A-Z]+).*" >> "$OUT"
echo "$LABEL done: $(grep -c "LATLOAD $LABEL" "$OUT") rows"
