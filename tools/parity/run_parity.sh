#!/bin/bash
# run_parity.sh — full parity sweep. bash 3.2-compatible.
#
# Test definitions live in tests.txt as space-separated lines:
#   <id> <preset_basename> <note> <vel> <dur_s> <tail_s> [<scenario_file>]
# scenario_file optional; when present we capture via ds_capture_seq.sh
# with that scenario.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="/tmp/parity"
TESTS_FILE="$SCRIPT_DIR/tests.txt"
mkdir -p "$OUT_DIR"

pass=0
fail=0

printf "\n%-32s  %8s  %8s  %8s  %8s  %s\n" \
       "TEST" "gain_dB" "tail_s" "period" "band_dB" "STATUS"
printf '%.0s-' {1..80}; echo

# Read tests.txt; comments / blank lines skipped.
while IFS= read -r line; do
    line="${line%%#*}"
    [ -z "${line// }" ] && continue
    set -- $line
    name=$1; preset_base=$2; n=$3; v=$4; d=$5; t=$6
    scenario=""
    if [ $# -ge 7 ]; then scenario="$SCRIPT_DIR/scenarios/$7"; fi

    preset="$SCRIPT_DIR/presets/${preset_base}.dspreset"
    if [ ! -f "$preset" ]; then
        printf "%-32s  %s\n" "$name" "MISSING_PRESET"
        fail=$((fail+1)); continue
    fi
    ds_wav="$OUT_DIR/${name}_ds.wav"
    our_wav="$OUT_DIR/${name}_ours.wav"
    log="$OUT_DIR/${name}.log"

    if [ -n "$scenario" ]; then
        total=$(awk -v d="$d" -v t="$t" 'BEGIN{print d+t+1}')
        "$SCRIPT_DIR/../ds_capture_seq.sh" "$preset" "$ds_wav" "$total" "$scenario" >"$log" 2>&1
    else
        "$SCRIPT_DIR/../ds_capture.sh" "$preset" "$ds_wav" "$n" "$v" "$d" "$t" >"$log" 2>&1
    fi
    if [ $? -ne 0 ]; then
        printf "%-32s  %s\n" "$name" "DS_CAPTURE_FAIL"
        fail=$((fail+1)); continue
    fi

    # For our side we only support single-note today; scenario tests skip
    # the comparison and just record the DS capture for manual inspection.
    if [ -n "$scenario" ]; then
        printf "%-32s  %s\n" "$name" "DS_CAPTURED  (no-cmp)"
        continue
    fi

    "$SCRIPT_DIR/../sfz_render" "$preset" "$our_wav" "$n" "$v" "$d" "$t" 44100 >>"$log" 2>&1
    if [ $? -ne 0 ]; then
        printf "%-32s  %s\n" "$name" "SFZ_RENDER_FAIL"
        fail=$((fail+1)); continue
    fi

    python3 "$SCRIPT_DIR/../wav_diff.py" "$ds_wav" "$our_wav" >>"$log" 2>&1
    status=$?
    pline=$(grep "^PARITY:" "$log" | tail -1)
    gain=$(echo  "$pline" | sed -E 's/.*gain Δ=([0-9.]+).*/\1/')
    tail_d=$(echo "$pline" | sed -E 's/.*tail Δ=([0-9.]+).*/\1/')
    per=$(echo  "$pline" | sed -E 's/.*period (OK|MISMATCH).*/\1/')
    band=$(echo "$pline" | sed -E 's/.*max band Δ=([0-9.]+).*/\1/')
    if [ "$status" -eq 0 ]; then
        printf "%-32s  %8s  %8s  %8s  %8s  PASS\n" \
               "$name" "$gain" "$tail_d" "$per" "$band"
        pass=$((pass+1))
    else
        printf "%-32s  %8s  %8s  %8s  %8s  FAIL\n" \
               "$name" "$gain" "$tail_d" "$per" "$band"
        fail=$((fail+1))
    fi
done < "$TESTS_FILE"

echo
echo "PASS: $pass   FAIL: $fail"
echo "logs:  $OUT_DIR/<test>.log"
