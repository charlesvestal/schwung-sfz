# CLAUDE.md

## Project Overview

Multisampler module for Move Anything. Plays SFZ (.sfz) and DecentSampler
(.dspreset) libraries. Engine: a fork of [xsynth](https://github.com/arduano/xsynth)
(LGPL-3.0) with Move-specific changes — disk streaming, voice-side SRC,
DecentSampler→SFZ conversion, denormal flush, soft-clip output, auto-gain.

Was sfizz through v0.4.x; rewritten on xsynth for v0.5.0+. The on-device
module id stays `sfz` for seamless upgrades from old SFZ Player installs;
user-facing name is "Multisampler".

## Build Commands

```bash
./scripts/build.sh      # Build with Docker (cross-compiles xsynth-core + plugin)
./scripts/install.sh    # Deploy to Move
```

## Structure

```
src/
  module.json                       # Module metadata
  ui.js                             # JavaScript UI
  help.json                         # On-device help
  dsp/
    xsynth_plugin.c                 # Plugin wrapper around xsynth-shim
    dspreset_to_xsynth_sfz.{c,h}    # DecentSampler → SFZ converter
    third_party/
      xsynth/                       # Forked xsynth (LGPL-3.0)
      xsynth_shim/                  # C ABI shim around xsynth-core
      sfizz/                        # Vestigial; not built (kept for diff/reference)
```

## Streaming + .x44c cache

Samples larger than the resident head buffer are streamed off disk. Each
sample picks one of two `SampleLayout` variants at load time
(`src/dsp/third_party/xsynth/core/src/soundfont/streaming.rs`):

- `Flac` — direct from `.flac`. No prebake required. Voice spawn primes
  the head buffer via a Symphonia FLAC decode pass; per-voice CPU cost
  during play. 48k FLACs use the streaming SRC path to hit Move's 44.1k.
- `Separated` — backed by a `.x44c` cache file alongside the source
  audio. Pre-decoded PCM at 44.1k, channels stored contiguously. Stream
  reads are raw `read()`s with no decode and no SRC.

`.x44c` files are an optional perf cache, not a requirement. Build them
off-device with the `prebake_cache` binary
(`src/dsp/third_party/xsynth/core/src/bin/prebake_cache.rs`) when a
specific library shows CPU pressure under high polyphony or release
tails — otherwise ship FLAC-only.

## Instrument Organization

`instruments/` is scanned recursively (up to 5 levels) into a single flat
preset list. Each top-level entry maps to one "instrument" (a library or
loose file), and every `.sfz` / `.dspreset` inside becomes one preset
belonging to that instrument.

- Examples:
  - `Cosmos/COSMOS.dspreset` → instrument "Cosmos", 1 preset
  - `Raw Violin/{Pad,Granular Pad,Harmonic Pad,Raw Violin}.dspreset` →
    instrument "Raw Violin", 4 presets
  - `K4Coll-1.01/K4-Acoustic/K4-Acoustic.dspreset` (and ~60 siblings) →
    instrument "K4Coll-1.01", 60 presets
  - `DS - The Synths/DS_The Synths/*.dspreset` → instrument "DS - The Synths",
    45 presets
- Normal L/R scrolls the flat preset list across all libraries. Shift+L/R
  jumps to the next/previous library's first preset. Each preset loads with
  `dirname(preset.path)` as the sample-resolution root.

## DecentSampler Conversion Spec Adherence

`.dspreset` files are converted to SFZ at load time by
`convert_dspreset_to_sfz` in `src/dsp/dspreset_to_xsynth_sfz.c`. Spec source:
the official
[DecentSampler Developers Guide](https://decentsampler-developers-guide.readthedocs.io/).

**Supported (verified against docs):**
- `<sample>` mapping: `rootNote` / `loNote` / `hiNote` / `loVel` / `hiVel` /
  `start` / `end` / `tuning` / `pan` / `loopEnabled` / `loopStart` / `loopEnd`
- `<group>` ADSR: `attack` (0–10s), `decay` (0–25s), `sustain` (0–1 ratio),
  `release` (0–25s); `volume` (linear 0–16 or `NdB`); `ampVelTrack` (0–1)
- `<groups>` wrapper attrs propagate to `<global>`
- Round-robin: `seqMode="round_robin"` + `seqPosition` → SFZ `seq_length`/`seq_position`
- Effects (xsynth-native only):
  - `lowpass` / `lowpass_4pl` → region `cutoff` / `fil_type=lpf_2p`/`lpf_4p` / `resonance`. Defaults: 22000 Hz, Q=0.7.
  - `reverb` → xsynth's reverb bus on bus `fx1` with proper wet/dry crossfade via `directtomain` / `fx1tomain`. Defaults: room=0.7, damping=0.3, **wet=0**.
  - `gain` → `global_volume` (in dB).
- UI knob default values: `<control>`/`<labeled-knob>` `value=` is applied to
  the bound parameter at load time, with `factor=` and
  `translation="linear"`/`"table"` (with `translationTable=`) honored.
- **Dynamic per-preset knob mapping**: every supported `<labeled-knob>` /
  `<control>` in the dspreset is enumerated at convert time and exposed as a
  Move parameter (`knob_0`…`knob_15`). The first `DS_KNOB_LIVE_COUNT-2` knobs
  (default 6, after `octave_transpose` + `gain`) join the live encoder row;
  every knob shows in the params menu. Each knob owns one synthetic MIDI CC
  (allocated from 102..117) and the converter emits `<param>_oncc<N>=delta`
  opcodes for every supported binding inside the control — a single knob can
  drive multiple xsynth targets when its dspreset has multiple `<binding>`
  children (e.g. WörliTzer's "Line" knob driving group positions 1 AND 5).
  Runtime: `set_param("knob_3", "0.75")` calls
  `xsynth_send_hdcc` through the shim — zero glitch, sample-accurate. Knob
  position resets to the dspreset's `value=` on every preset load (no
  per-preset persistence yet).
- AMP_VOLUME-on-group knobs claim the group's amplitude — the static
  `modVolume`/`group_amp_db` contribution is skipped (would double-attenuate
  on top of the CC-driven amplitude).
- Knobs targeting `FX_REVERB_WET_LEVEL` stay static-at-load (no CC binding) —
  the static reverb path crossfades via `directtomain`/`fx1tomain` and a CC
  on `reverb_wet` would multiply against that crossfade.
- Knobs whose every binding hits an unsupported target (FX_DELAY_*, FX_CHORUS_*,
  parameterName= per-tag bars, etc.) are skipped entirely — the UI doesn't
  show a knob that can't move sound.
- MIDI CC bindings (`<midi><cc>`): emit `<param>_oncc<N>` opcodes with
  the load-time CC value derived from the target knob's `value=`. `level=ui`
  bindings chain through the target control's own translation. Degenerate
  bindings (zero delta) are skipped.
- Sample paths: Windows backslashes normalized to `/`.

**Not supported / partially supported:**
- ADSR defaults are **not documented in DS docs**. We fall back to
  `ampeg_release=0.5` when no release is set anywhere (otherwise xsynth's
  ~1ms cuts piano samples abruptly). Known guess.
- `level="tag"` bindings (with `<tags>` element + `tags="..."` on groups)
  collapse to global parameters. Per-tag mixing not honored.
- Effects without xsynth equivalents: `delay`, `chorus`, `phaser`,
  `pitch_shift`, `convolution`, `wave_folder`, `wave_shaper`,
  `stereo_simulator`, `bit_crusher`. (Use the ecosystem's chain modules
  — CloudSeed, SpaceEcho, Junologue Chorus, etc. — after the Multisampler.)
- `<modulators>` (LFOs, envelopes, MIDI CC modulators, MPE, random) — none
  honored.
- `<button>` / button-state bindings, animations, `<note>` and `<velocity>`
  modulators inside `<midi>`, X-Y pads, oscillators, FM6 operators.
- Group-level bindings other than ADSR (per-group volume/pan/tuning).
- Filter `Q` (DS 0–5) → SFZ `resonance` (dB) — passed through with the
  hand-built Q→dB conversion (see commit dd737b4). Within typical
  author-set values it's roughly OK.
- `attackCurve` / `decayCurve` / `releaseCurve` — xsynth uses linear curves.
- Extended filter types: `lowpass_1pl`, `notch`, `peak`, `bandpass`,
  `highpass` — `lpf_2p`/`lpf_4p` are the only filters we emit today.

**Standalone test tool:** there's a converter CLI at `/tmp/dstest/dsconvert`
(rebuilt from `/tmp/dstest/dsconvert.c` which copies the converter functions
out of `dspreset_to_xsynth_sfz.c` plus a tiny `main`) for inspecting
converted SFZ without deploying. Pull the official DS examples to
`/tmp/dstest/dspresets/` to verify behavior.

## DSP Plugin API

Standard Move Anything plugin_api_v2 (implemented in `src/dsp/xsynth_plugin.c`):
- `on_load()`: Initialize xsynth synth via the shim, scan instruments
- `on_midi()`: Forward MIDI to xsynth-shim
- `set_param()`: Set instrument_index, preset, gain, octave_transpose, knob_0..15
- `get_param()`: Get instrument/preset info, state
- `render_block()`: Render 128 frames stereo via xsynth-shim
