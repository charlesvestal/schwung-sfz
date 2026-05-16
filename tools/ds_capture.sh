#!/bin/bash
# ds_capture — drive DecentSampler standalone, send MIDI via a virtual
# CoreMIDI source, capture audio from BlackHole 2ch.
#
# Prereq: DS standalone must be configured (one time) to output to
#         "BlackHole 2ch" — Options → Audio/MIDI Settings.
#
# Usage:
#   ./ds_capture.sh <preset.dspreset> <out.wav> [note=60] [vel=100] \
#                   [duration_s=2.0] [tail_s=2.0]
set -e

PRESET="${1:?need preset path}"
OUT="${2:?need output wav}"
NOTE="${3:-60}"
VEL="${4:-100}"
DUR="${5:-2.0}"
TAIL="${6:-2.0}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOTAL=$(python3 -c "print(${DUR}+${TAIL})")
SETTLE=6.0   # let DS load the preset (samples can be many MB on disk)

# Compute the ffmpeg input index for BlackHole 2ch (varies by machine).
BH_INDEX=$(ffmpeg -f avfoundation -list_devices true -i "" 2>&1 \
           | sed -nE 's/.*\[([0-9]+)\] BlackHole 2ch[[:space:]]*$/\1/p' | head -1)
if [ -z "$BH_INDEX" ]; then
    echo "BlackHole 2ch not found in ffmpeg device list"; exit 2
fi
echo "[ds_capture] BlackHole 2ch is avfoundation index $BH_INDEX"

# Open the preset in DS. If DS is already running, it switches the
# loaded preset in place (preserving Audio/MIDI settings).
open -a DecentSampler "$PRESET"
echo "[ds_capture] opened DS with $PRESET, waiting ${SETTLE}s for load..."
sleep "$SETTLE"

# Start ffmpeg recording from BlackHole. Background it; we'll send MIDI
# and stop it after enough audio.
TMPOUT="$(mktemp -t ds_capture).wav"
ffmpeg -y -f avfoundation -i ":${BH_INDEX}" \
       -ar 44100 -ac 2 -t "$TOTAL" \
       -acodec pcm_s16le "$TMPOUT" \
       >/tmp/ds_capture_ffmpeg.log 2>&1 &
FFPID=$!
# ffmpeg needs ~0.3s to open the device before we can rely on it
# recording our NoteOn.
sleep 0.6

echo "[ds_capture] NoteOn $NOTE @ vel $VEL"
swift "$SCRIPT_DIR/send_midi.swift" on "$NOTE" "$VEL" >/dev/null
sleep "$DUR"
echo "[ds_capture] NoteOff"
swift "$SCRIPT_DIR/send_midi.swift" off "$NOTE" >/dev/null

wait $FFPID
mv "$TMPOUT" "$OUT"
echo "[ds_capture] wrote $OUT"

# Leave DS running; next capture reuses the same session/settings.
