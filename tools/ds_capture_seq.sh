#!/bin/bash
# ds_capture_seq — like ds_capture, but the MIDI sequence comes from
# a "scenario" file passed via stdin or as an arg. Each line of the
# scenario: "<delay_ms> <on|off> <note> [<vel>]" (same format as
# send_midi_seq).
#
# Usage:
#   ./ds_capture_seq.sh <preset.dspreset> <out.wav> <total_seconds> < scenario.txt
set -e

PRESET="${1:?preset}"
OUT="${2:?out wav}"
TOTAL="${3:?total seconds}"
SCENARIO="${4:-/dev/stdin}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SETTLE=6.0

BH_INDEX=$(ffmpeg -f avfoundation -list_devices true -i "" 2>&1 \
           | sed -nE 's/.*\[([0-9]+)\] BlackHole 2ch[[:space:]]*$/\1/p' | head -1)
[ -z "$BH_INDEX" ] && { echo "BlackHole 2ch not found"; exit 2; }

open -a DecentSampler "$PRESET"
sleep "$SETTLE"

TMP="$(mktemp -t ds_capture_seq).wav"
ffmpeg -y -f avfoundation -i ":${BH_INDEX}" -ar 44100 -ac 2 -t "$TOTAL" \
       -acodec pcm_s16le "$TMP" >/tmp/ds_capture_seq_ffmpeg.log 2>&1 &
FFPID=$!
sleep 0.6

swift "$SCRIPT_DIR/send_midi_seq.swift" < "$SCENARIO"
wait $FFPID
mv "$TMP" "$OUT"
