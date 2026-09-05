#!/bin/bash
# run_ceiling.sh <label> -- drive an offered-load ladder ABOVE the paper's
# 30% cap against whatever DUT is currently running, to find the point where
# the DUT stops keeping up (its capacity ceiling).
#
# Runs on the Fedora load generator. Same latload.lua as the RFC 2544 grid;
# only the defaults differ (rates 30-100%, four frame sizes).
#
# Prerequisites (see ../REPRODUCE.md, "Capacity ceilings"):
#   - pktgen built with the latency-histogram patch AND Lua enabled
#     (meson -Denable_lua=true); set PKTGEN_DIR to its source tree.
#   - client Solarflare PF0 bound to vfio-pci (noiommu).
#   - the DUT arm launched on the Morello box.
#
# Environment overrides:
#   PKTGEN_DIR  pktgen source tree containing build/app/pktgen
#   OUT_DIR     where logs and the results file go
#               (default: ../results/rfc2544_logs)
#   SIZES       frame sizes, comma separated      (default 64,128,256,512)
#   RATES       offered rates, % of line rate     (default 30..100)
#   TRIAL_MS    trial length                      (paper used 30000)
#   LAT_US      latency probe interval            (default 500)
#
# Output: $OUT_DIR/arm_<label>.log (raw pktgen transcript) and one
# "LATLOAD <label> size=.. pct=.. ..." line per trial appended to
# $OUT_DIR/ceiling_results.txt. Summarise with:
#   python3 analyze.py $OUT_DIR/arm_<label>.log
# Convert to CSV with ../rfc2544_to_csv.py (pass every log you want kept).
#
# SAFETY: if a trial reports rx=0 the CAPIO echo has wedged. Kill pktgen
# immediately; continuing to flood a wedged DUT has hung the Morello kernel.
set -u
LABEL="${1:?usage: run_ceiling.sh <label>}"
HERE=$(cd "$(dirname "$0")" && pwd)
PKTGEN_DIR="${PKTGEN_DIR:?set PKTGEN_DIR to the pktgen source tree}"
OUT_DIR="${OUT_DIR:-$HERE/../results/rfc2544_logs}"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/ceiling_results.txt"
LOG="$OUT_DIR/arm_$LABEL.log"

cd "$PKTGEN_DIR"
sudo script -qec \
  "timeout 7200 env LABEL=$LABEL \
     SIZES=${SIZES:-64,128,256,512} \
     RATES=${RATES:-30,35,40,45,50,60,70,80,90,100} \
     TRIAL_MS=${TRIAL_MS:-30000} LAT_US=${LAT_US:-500} \
     ./build/app/pktgen -l 1-3 -n 4 --no-telemetry -- -P -m '[2:3].0' \
     -f $HERE/latload.lua" /dev/null > "$LOG" 2>&1
sed -e 's/\x1b\[[0-9;]*[A-Za-z]//g' "$LOG" \
  | grep -aoE "(LATLOAD|WARMUP|LATLOAD_[A-Z]+).*" >> "$OUT"
echo "$LABEL done: $(grep -c "LATLOAD $LABEL " "$OUT") rows in $OUT"
