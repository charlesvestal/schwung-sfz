#!/bin/bash
# run_parity.sh — full parity sweep. bash 3.2-compatible (no -A arrays).
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="/tmp/parity"
mkdir -p "$OUT_DIR"

# Per-test params via case lookup. Echoes "note vel duration tail".
test_params() {
    case "$1" in
        1)  echo 60 100 1.0 0.5;;
        2)  echo 60 100 1.5 1.0;;
        3)  echo 60 100 1.0 0.5;;
        4)  echo 60 100 1.0 0.5;;
        5)  echo 60 100 0.05 2.0;;
        6)  echo 60 100 0.05 2.0;;
        7)  echo 60 100 2.0 0.5;;
        8)  echo 60 100 2.0 0.5;;
        9)  echo 60 100 2.0 0.5;;
        10) echo 60 100 2.0 0.5;;
        11) echo 60 100 2.0 0.5;;
        12) echo 60 100 2.0 0.5;;
        13) echo 60 100 2.0 0.5;;
        14) echo 60 100 1.5 1.0;;
        15) echo 60 100 2.0 0.5;;
        16) echo 60 100 1.0 0.5;;
        17) echo 60 100 0.5 1.5;;
        18) echo 60 100 1.0 0.5;;
        19) echo 60 100 1.0 0.5;;
        20) echo 60 100 1.0 0.5;;
        *)  echo ""
    esac
}

pass=0
fail=0

printf "\n%-30s %10s %s\n" "PRESET" "Δ_dB" "STATUS"
printf '%.0s-' {1..60}; echo

for preset in "$SCRIPT_DIR"/presets/*.dspreset; do
    base=$(basename "$preset" .dspreset)
    raw_id="${base%%_*}"
    id=$((10#$raw_id))
    params=$(test_params "$id")
    [ -z "$params" ] && continue
    set -- $params
    n=$1; v=$2; d=$3; t=$4
    ds_wav="$OUT_DIR/${base}_ds.wav"
    our_wav="$OUT_DIR/${base}_ours.wav"
    log="$OUT_DIR/${base}.log"

    "$SCRIPT_DIR/../ds_capture.sh" "$preset" "$ds_wav" "$n" "$v" "$d" "$t" >"$log" 2>&1
    if [ $? -ne 0 ]; then
        printf "%-30s %10s %s\n" "$base" "-" "DS_CAPTURE_FAIL"
        fail=$((fail+1)); continue
    fi
    "$SCRIPT_DIR/../sfz_render" "$preset" "$our_wav" "$n" "$v" "$d" "$t" 44100 >>"$log" 2>&1
    if [ $? -ne 0 ]; then
        printf "%-30s %10s %s\n" "$base" "-" "SFZ_RENDER_FAIL"
        fail=$((fail+1)); continue
    fi

    python3 "$SCRIPT_DIR/../wav_diff.py" "$ds_wav" "$our_wav" >>"$log" 2>&1
    status=$?
    score=$(grep "PARITY SCORE" "$log" | tail -1 | awk '{print $7}')
    if [ "$status" -eq 0 ]; then
        printf "%-30s %10s %s\n" "$base" "${score:-?}" "PASS"
        pass=$((pass+1))
    else
        printf "%-30s %10s %s\n" "$base" "${score:-?}" "FAIL"
        fail=$((fail+1))
    fi
done

echo
echo "PASS: $pass   FAIL: $fail"
echo "logs:  $OUT_DIR/<test>.log"
