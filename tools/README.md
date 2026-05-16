# tools — converter ground-truth harness

Mac-side renderers and a comparator for A/B'ing our converter+xsynth
output against DecentSampler's own engine.

## sfz_render — render a .dspreset through our xsynth

```
cc -O2 sfz_render.c ../src/dsp/dspreset_to_xsynth_sfz.c \
    ../src/dsp/third_party/xsynth_shim/target/release/libxsynth_shim.a \
    -I ../src/dsp -lm -framework CoreFoundation -framework Security \
    -o sfz_render

./sfz_render preset.dspreset out.wav [note=60] [vel=100] [dur=2.0] [tail=2.0] [rate=44100]
```

Builds the same code paths the Move plugin uses — converter + xsynth-shim.
Output should match the device for the same MIDI input.

## ds_render (Swift) — partial; not currently functional

Stubbed AU host for offline rendering DecentSampler. Loads the AU and
sets `_libraryUrl` via JUCE plugin state, but DS doesn't actually
materialize the preset in `AVAudioEngine` offline rendering mode — the
async sample loader never progresses without a real audio thread or
some lifecycle event we haven't identified. Left in tree as a
starting point if/when we figure it out.

For now, capture DS reference WAVs manually:

1. Open DecentSampler.app (standalone)
2. Drag the .dspreset onto its window
3. In Logic / GarageBand / Audio Hijack: route DS output to record
4. Hit one note (e.g., C4 at velocity 100), hold 2 s, release, wait 2 s
5. Export as 16-bit / 44.1 kHz WAV

## wav_diff.py — broad-stroke comparator

```
python3 wav_diff.py reference.wav ours.wav
```

Reports per-side peak, RMS, onset frame, last-audible frame, and crude
band energy (sub/low/mid/hi/air). Doesn't aim for sample identity —
DS uses different envelopes, interpolation, and FX. Surfaces:

- gross level mismatches (we're 6 dB louder/quieter)
- onset timing differences (our attack is delayed by N ms)
- spectral character drift (we're missing high-end vs. DS, etc.)

## Workflow

```
# 1. capture DS reference once per preset (manual)
# 2. render through our pipeline
./sfz_render path/to/preset.dspreset /tmp/ours.wav 60 100 2.0 2.0 44100

# 3. compare
python3 wav_diff.py /tmp/ds_ref.wav /tmp/ours.wav
```
