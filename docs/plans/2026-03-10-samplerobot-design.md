# Sample Robot Module Design

**Date**: 2026-03-10
**Module**: `move-anything-samplerobot`
**Category**: `tool`

## Overview

Tool module that autosamples external MIDI gear. Sends MIDI notes out USB to a connected synthesizer, records the resulting audio from Move's line-in, processes the recordings (trim, loop detection), and generates a complete SFZ instrument playable in the SFZ Player module.

## User Workflow

1. Connect external synth via USB MIDI (or MIDI adapter) and audio cable to Move's line-in
2. Set Move's audio input to line-in
3. Open Sample Robot module
4. Configure parameters: note range, key zones, velocity layers, hold duration, loop on/off, MIDI channel
5. Name the instrument
6. Press record — module runs fully automatically
7. When complete, switch to SFZ Player — new instrument is ready

## Parameters

| Parameter | Type | Range | Default | Description |
|-----------|------|-------|---------|-------------|
| `range_low` | int | 0-127 | 36 (C2) | Lowest MIDI note to sample |
| `range_high` | int | 0-127 | 84 (C6) | Highest MIDI note to sample |
| `key_zones` | int | 1-24 | 8 | Number of notes to sample across range |
| `velocity_layers` | int | 1-8 | 3 | Number of velocity levels per note |
| `hold_duration` | float | 0.5-30.0 | 3.0 | Seconds to hold each note |
| `loop_detect` | int | 0/1 | 1 | Enable loop point detection |
| `midi_channel` | int | 1-16 | 1 | MIDI output channel |
| `instrument_name` | string | — | — | Name for the instrument folder |

## Architecture

### Module Type

- Plugin API v2 (instanced)
- `audio_in: true` — reads from Move's line-in via `audio_in_offset` in shared memory
- `midi_out: true` — sends Note On/Off to external gear via `host->midi_send_external()`
- `midi_in: true` — receives pad/button input for UI control
- `audio_out: true` — passes through audio input for monitoring during setup
- `component_type: tool`
- `tool_config`: `{ "interactive": true, "skip_file_browser": true, "allow_new_file": true }`
- Accessed via Tools menu, installs to `modules/tools/samplerobot/`

### State Machine

```
IDLE → PRE_SAMPLE → RECORDING → RELEASE → PROCESSING → (next note or DONE)
```

1. **IDLE**: Waiting for user to configure and start. Audio passthrough for monitoring.
2. **PRE_SAMPLE**: 100ms of silence capture to establish noise floor (RMS baseline).
3. **RECORDING**: MIDI Note On sent. Recording audio for `hold_duration` seconds.
4. **RELEASE**: MIDI Note Off sent. Continue recording until silence detected.
5. **PROCESSING**: Trim silence, detect loops, write WAV file. Advance to next note/velocity or finish.
6. **DONE**: All samples captured. Generate .sfz file. Display results.

### Sampling Sequence

Notes and velocities are computed upfront:

- **Note selection**: Evenly spaced across range. E.g., range 36-84 with 8 zones → notes at 36, 42, 48, 55, 61, 67, 73, 79 (every ~6.9 semitones, rounded to nearest).
- **Velocity selection**: Evenly divided across 1-127. E.g., 3 layers → velocities 42, 85, 127.
- **Order**: All velocities for one note before moving to next note (low velocity first). This minimizes timbral discontinuity from the source synth's state.

### Audio Capture

- Read stereo interleaved int16 from `host->mapped_memory + host->audio_in_offset` every `render_block()` (128 frames at 44100 Hz).
- Accumulate into a ring buffer sized for max recording duration (hold_duration + max release time).
- Buffer size: 30s × 44100 × 2 channels × 2 bytes = ~5.3 MB max per sample.

### Silence Detection

- Compute RMS over a sliding window of 200ms (~8820 samples).
- **Noise floor**: Established during PRE_SAMPLE phase (100ms of silence before note-on).
- **Attack detection**: First sample where RMS exceeds noise_floor + 12dB. Back up 2ms (~88 samples) for transient safety.
- **Release end detection**: After Note Off, when RMS drops below noise_floor + 6dB for 200ms continuously.
- **Timeout**: If silence not detected within 10 seconds after Note Off, force-stop recording.

### Audio Passthrough

During IDLE state, the module copies audio_in to audio_out so the user can hear the connected instrument while adjusting levels and testing the connection. During sampling, passthrough is also active so the user hears what's being captured.

## Loop Detection Algorithm

Spectral + waveform hybrid approach, running in PROCESSING state. Analysis performed on mono mixdown (L+R)/2; resulting loop points applied to the stereo WAV.

### Step 1: Identify Sustain Region

- Skip attack: first 200ms of trimmed sample.
- Skip release: everything after the Note Off point (known from hold_duration).
- The sustain region is where loop candidates are searched.
- If sustain region is < 200ms, skip loop detection for this sample.

### Step 2: Spectral Similarity Search

- Divide sustain into overlapping windows of ~50ms (2048 samples at 44100 Hz), 50% overlap.
- Compute FFT magnitude spectrum for each window (real FFT, keep magnitudes only).
- Compare all pairs of windows separated by at least 100ms.
- Spectral distance = sum of squared log-magnitude differences (log scale weights harmonics appropriately).
- Keep top 20 most similar pairs as loop candidates.

### Step 3: Waveform Refinement

- For each candidate pair, define a search region of ±3ms around each candidate position.
- Find all zero-crossings within each search region.
- For each combination of zero-crossings (start × end), compute normalized cross-correlation over a 10ms window centered on each point.
- Select the zero-crossing pair with highest correlation.

### Step 4: Select Best Loop

- Score = spectral_similarity × waveform_correlation × length_bonus
- `length_bonus = min(loop_length_ms / 500.0, 1.0)` — prefer loops ≥ 500ms.
- Minimum acceptable correlation: 0.9. Below this, mark as `loop_mode=no_loop`.

### Step 5: Crossfade Length

- `loop_crossfade` = 10% of loop length, clamped to range 220-2205 samples (5ms-50ms).
- Written to the .sfz region as a sample count.

### FFT Implementation

Use embedded KissFFT (BSD license, single-file, ~500 lines of C). No external dependencies.

### Performance Estimate

- 3-second sustain → ~130 windows of 2048 samples
- ~8400 spectral comparisons (upper triangle, excluding adjacent)
- ~20 candidate pairs × ~50 zero-crossing combos × correlation
- Total: well under 1 second per sample on ARM Cortex-A7.

## WAV File Writing

- Format: 16-bit PCM, 44100 Hz, stereo (matching Move's native format).
- Include `smpl` chunk with loop point data (loop start, loop end, loop type = forward).
- Filename convention: `<NoteName><Octave>_v<Velocity>.wav` (e.g., `C2_v042.wav`, `Fs2_v085.wav`).

## SFZ File Generation

### Structure

```sfz
// Generated by Sample Robot
// <InstrumentName> - <n> zones, <m> velocity layers
// <timestamp>

<control>
default_path=samples/

<global>
ampeg_release=0.5
loop_mode=loop_sustain

// --- C2 (root=36) range 33-38 ---
<group> lokey=33 hikey=38 pitch_keycenter=36

<region> sample=C2_v042.wav lovel=1 hivel=63
xfout_lovel=43 xfout_hivel=63

<region> sample=C2_v085.wav lovel=32 hivel=106
xfin_lovel=32 xfin_hivel=63
xfout_lovel=86 xfout_hivel=106

<region> sample=C2_v127.wav lovel=64 hivel=127
xfin_lovel=64 xfin_hivel=106
```

### Key Zone Boundaries

For N zones across range [low, high]:
- Sampled notes are evenly spaced: `note[i] = low + round(i * (high - low) / (N - 1))`
- Zone boundary between note[i] and note[i+1]: `midpoint = floor((note[i] + note[i+1]) / 2)`
- Zone i: `lokey = prev_midpoint + 1`, `hikey = next_midpoint`
- First zone extends down to `max(0, note[0] - half_interval)`
- Last zone extends up to `min(127, note[N-1] + half_interval)`
- `pitch_keycenter` = sampled note

### Velocity Crossfade Regions

For M velocity layers with trigger velocities v[0] < v[1] < ... < v[M-1]:

- Layer 0 (softest): `lovel=1 hivel=midpoint(v[0],v[1])`, fade out at top
- Layer i (middle): `lovel=midpoint(v[i-1],v[i]) hivel=midpoint(v[i],v[i+1])`, fade in at bottom, fade out at top
- Layer M-1 (loudest): `lovel=midpoint(v[M-2],v[M-1]) hivel=127`, fade in at bottom

Crossfade overlap zone = the space between adjacent velocity midpoints, split evenly.

### Loop Opcodes (per region, when loop detected)

```sfz
loop_mode=loop_sustain
loop_start=<sample_frame>
loop_end=<sample_frame>
loop_crossfade=<sample_count>
```

When no loop found: `loop_mode=no_loop` (overrides the global default).

## Output File Structure

```
/data/UserData/move-anything/modules/sound_generators/sfz/instruments/
  <InstrumentName>/
    <InstrumentName>.sfz
    samples/
      C2_v042.wav
      C2_v085.wav
      C2_v127.wav
      Fs2_v042.wav
      Fs2_v085.wav
      Fs2_v127.wav
      ...
```

## UI Screens

### Screen 1: Setup (IDLE state)

```
┌────────────────────────┐
│ SAMPLE ROBOT           │
│ Range:  C2 - C6        │
│ Zones:  8              │
│ Layers: 3              │
│ Hold:   3.0s           │
│ Loop:   ON   Ch: 1     │
│                        │
│ [Press ▶ to name]      │
└────────────────────────┘
```

- Jog wheel scrolls between parameters
- Knobs edit selected parameter value
- Record/play button advances to naming screen

### Screen 2: Naming

- Standard Move text input for instrument name
- Confirm to begin sampling

### Screen 3: Sampling (PRE_SAMPLE / RECORDING / RELEASE states)

```
┌────────────────────────┐
│ SAMPLING: Rhodes       │
│ ████████░░░░░░  14/24  │
│                        │
│ Note: F#3  Vel: 85     │
│ Recording...    2.1s   │
│                        │
│ [Hold STOP to cancel]  │
└────────────────────────┘
```

- Progress bar shows overall completion (sample N of total)
- Current note, velocity, and sub-state displayed
- Hold stop button to abort (keeps samples captured so far)

### Screen 4: Processing

```
┌────────────────────────┐
│ PROCESSING: Rhodes     │
│ ████████████░░  20/24  │
│                        │
│ Finding loops...       │
│ C3_v085: loop 4482ms   │
└────────────────────────┘
```

### Screen 5: Complete (DONE state)

```
┌────────────────────────┐
│ COMPLETE!              │
│ Rhodes                 │
│ 24 samples, 8 zones    │
│ 22/24 loops found      │
│                        │
│ Ready in SFZ Player    │
│ [Press any to restart] │
└────────────────────────┘
```

## Implementation Files

```
move-anything-samplerobot/
  src/
    module.json              # Module metadata and parameter definitions
    ui.js                    # JavaScript UI (setup, progress, results screens)
    help.json                # On-device help text
    dsp/
      samplerobot_plugin.c   # Main plugin: state machine, audio capture, MIDI send
      loop_finder.c          # Spectral+correlation loop detection algorithm
      loop_finder.h
      sfz_writer.c           # .sfz file generation from sample metadata
      sfz_writer.h
      wav_writer.c           # WAV file writing with smpl chunk (loop points)
      wav_writer.h
      silence_detect.c       # RMS-based silence detection and trimming
      silence_detect.h
      kiss_fft.c             # KissFFT (embedded, BSD license)
      kiss_fft.h
  scripts/
    build.sh                 # Build script (native ARM compilation, no Docker needed)
    install.sh               # Deploy to Move
  release.json               # Module Store release metadata
```

## Dependencies

- **KissFFT** (BSD license) — embedded single-file FFT for spectral analysis. ~500 lines of C.
- **No other external dependencies**. Standard C math library only.
- Build uses the same ARM cross-compilation toolchain as other modules.

## Estimated Size

- ~1500 lines C (DSP plugin + state machine)
- ~400 lines C (loop finder)
- ~200 lines C (WAV writer + SFZ writer + silence detection)
- ~200 lines JS (UI)
- ~2300 lines total

## Edge Cases and Error Handling

- **No audio input**: If no signal detected after Note On for 2 seconds, skip that sample and log warning. Show count of skipped samples in results.
- **Audio clipping**: If samples hit ±32767 repeatedly, warn user to lower input gain. Don't auto-adjust (user controls levels).
- **Disk full**: Check available space before starting. Each sample ≈ 300KB, so 48 samples ≈ 15MB. Warn if < 50MB free.
- **Instrument name collision**: If folder already exists, append ` (2)`, ` (3)`, etc.
- **Abort mid-session**: Keep all WAV files written so far. Generate .sfz with whatever was captured (partial instrument is better than nothing).
- **Loop detection failure**: Per-sample fallback to `loop_mode=no_loop`. Report count of failed loops in results screen.
- **Very short sustain**: If hold_duration < 0.5s or sustain region < 200ms, skip loop detection for that sample.
