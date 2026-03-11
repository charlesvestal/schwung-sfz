# Sample Robot Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a tool module that autosamples external MIDI gear via USB MIDI out + line-in audio, producing SFZ instruments for the SFZ Player.

**Architecture:** Standalone tool module with C DSP plugin (state machine for sampling sequence, spectral+correlation loop finder, WAV/SFZ file writers) and JS UI (setup params, progress display). Reads audio from `audio_in_offset`, sends MIDI via `midi_send_external()`, writes output to SFZ Player's instruments folder.

**Tech Stack:** C (DSP plugin, Plugin API v2), JavaScript (QuickJS UI), KissFFT (embedded BSD FFT library)

**Design doc:** `docs/plans/2026-03-10-samplerobot-design.md`

---

## Task 0: Create Repository and Scaffold

**Files:**
- Create: `/Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything-samplerobot/`
- Create: `src/module.json`
- Create: `src/ui.js` (minimal stub)
- Create: `src/help.json`
- Create: `src/dsp/samplerobot_plugin.c` (minimal stub)
- Create: `scripts/build.sh`
- Create: `scripts/install.sh`
- Create: `scripts/Dockerfile`
- Create: `.github/workflows/release.yml`
- Create: `release.json`
- Create: `CLAUDE.md`
- Create: `.gitignore`

**Step 1: Create directory structure**

```bash
mkdir -p /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything-samplerobot/{src/dsp,scripts,.github/workflows,docs}
```

**Step 2: Create module.json**

```json
{
    "id": "samplerobot",
    "name": "Sample Robot",
    "version": "0.1.0",
    "description": "Autosample external MIDI gear to create SFZ instruments",
    "author": "Move Anything",
    "dsp": "dsp.so",
    "api_version": 2,
    "component_type": "tool",
    "tool_config": {
        "interactive": true,
        "command": null,
        "skip_file_browser": true,
        "allow_new_file": true
    },
    "capabilities": {
        "audio_in": true,
        "audio_out": true,
        "midi_in": true,
        "midi_out": true
    }
}
```

**Step 3: Create minimal DSP stub**

```c
/*
 * Sample Robot DSP Plugin
 *
 * Autosamples external MIDI gear via USB MIDI out + line-in audio.
 * Produces SFZ instruments compatible with the SFZ Player module.
 *
 * V2 API - instance-based.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define MOVE_PLUGIN_API_VERSION_2 2
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

static const host_api_v1_t *g_host = NULL;

typedef struct {
    int dummy;
} samplerobot_instance_t;

static void* sr_create_instance(const char *module_dir, const char *json_defaults) {
    samplerobot_instance_t *inst = calloc(1, sizeof(samplerobot_instance_t));
    return inst;
}

static void sr_destroy_instance(void *instance) {
    free(instance);
}

static void sr_on_midi(void *instance, const uint8_t *msg, int len, int source) {
}

static void sr_set_param(void *instance, const char *key, const char *val) {
}

static int sr_get_param(void *instance, const char *key, char *buf, int buf_len) {
    return 0;
}

static int sr_get_error(void *instance, char *buf, int buf_len) {
    return 0;
}

static void sr_render_block(void *instance, int16_t *out, int frames) {
    /* Passthrough: copy audio_in to audio_out */
    int16_t *audio_in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    memcpy(out, audio_in, frames * 2 * sizeof(int16_t));
}

static plugin_api_v2_t g_plugin_api = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = sr_create_instance,
    .destroy_instance = sr_destroy_instance,
    .on_midi = sr_on_midi,
    .set_param = sr_set_param,
    .get_param = sr_get_param,
    .get_error = sr_get_error,
    .render_block = sr_render_block,
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_plugin_api;
}
```

**Step 4: Create minimal UI stub**

```javascript
import {
    MidiCC,
    MoveShift, MoveMainKnob, MoveMainButton, MoveBack,
    MoveCapture, MoveRec,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    White, Black, DarkGrey, LightGrey
} from '/data/UserData/move-anything/shared/constants.mjs';

import { decodeDelta } from '/data/UserData/move-anything/shared/input_filter.mjs';
import { announce } from '/data/UserData/move-anything/shared/screen_reader.mjs';
import { log as uniLog } from '/data/UserData/move-anything/shared/logger.mjs';

function debugLog(msg) { uniLog("SampleRobot", msg); }

globalThis.onModuleLoad = function() {
    debugLog("Sample Robot loaded");
};

globalThis.onDspReady = function() {
    debugLog("DSP ready");
};

globalThis.onTick = function() {
    clear_screen();
    print(4, 4, "SAMPLE ROBOT", White);
    print(4, 20, "Module loaded", LightGrey);
    print(4, 36, "Setup coming soon", LightGrey);
};

globalThis.onMidi = function(data) {
};
```

**Step 5: Create build.sh**

```bash
#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="samplerobot"
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"
echo "=== Building Sample Robot Module ==="

mkdir -p build
${CROSS_PREFIX}gcc -Ofast -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    src/dsp/samplerobot_plugin.c \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm

rm -rf "dist/$MODULE_ID"
mkdir -p "dist/$MODULE_ID"

cp src/module.json "dist/$MODULE_ID/"
cp src/ui.js "dist/$MODULE_ID/"
cp build/dsp.so "dist/$MODULE_ID/"
[ -f src/help.json ] && cp src/help.json "dist/$MODULE_ID/"
chmod +x "dist/$MODULE_ID/dsp.so"

cd dist
tar -czvf "$MODULE_ID-module.tar.gz" "$MODULE_ID/"
cd ..

echo "=== Build Complete ==="
echo "Output: dist/$MODULE_ID/"
```

**Step 6: Create install.sh**

```bash
#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="samplerobot"

cd "$REPO_ROOT"
if [ ! -d "dist/$MODULE_ID" ]; then
    echo "Error: dist/$MODULE_ID not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing Sample Robot Module ==="
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/tools/$MODULE_ID"
scp -r dist/$MODULE_ID/* ableton@move.local:/data/UserData/move-anything/modules/tools/$MODULE_ID/
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/tools/$MODULE_ID"
echo "=== Install Complete ==="
```

**Step 7: Create Dockerfile, release.yml, release.json, .gitignore, CLAUDE.md, help.json**

- Dockerfile: same as waveform-editor (ubuntu:22.04, gcc-aarch64-linux-gnu)
- release.yml: same pattern as waveform-editor, replacing `waveform-editor` with `samplerobot`
- release.json: `{ "version": "0.1.0", "download_url": "" }`
- .gitignore: `build/`, `dist/`
- CLAUDE.md: brief project description and build commands
- help.json: basic help structure

**Step 8: Init git repo and commit**

```bash
cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything-samplerobot
git init
git add -A
git commit -m "feat: scaffold Sample Robot tool module"
```

**Step 9: Build and verify it compiles**

```bash
./scripts/build.sh
```

Expected: `dist/samplerobot/` with module.json, ui.js, dsp.so, help.json

---

## Task 1: WAV Writer

**Files:**
- Create: `src/dsp/wav_writer.h`
- Create: `src/dsp/wav_writer.c`

Writes stereo 16-bit 44.1kHz WAV files with optional smpl chunk for loop points.

**Step 1: Create wav_writer.h**

```c
#ifndef WAV_WRITER_H
#define WAV_WRITER_H

#include <stdint.h>

/* Write a stereo 16-bit 44100Hz WAV file.
 * If loop_start >= 0 and loop_end > loop_start, includes a smpl chunk.
 * Returns 0 on success, -1 on error. */
int wav_write(const char *path,
              const int16_t *interleaved_stereo,
              int num_frames,
              int loop_start,   /* -1 for no loop */
              int loop_end);    /* sample frame (not byte offset) */

#endif
```

**Step 2: Create wav_writer.c**

Implement standard RIFF/WAV writing with:
- 44-byte standard header (RIFF, fmt chunk)
- data chunk with raw PCM
- Optional smpl chunk (if loop_start >= 0): 36-byte smpl header + 24-byte loop struct
- Use `fopen`/`fwrite` — straightforward file I/O

**Step 3: Update build.sh to include wav_writer.c**

Add `src/dsp/wav_writer.c` to the gcc command line.

**Step 4: Test compilation**

```bash
./scripts/build.sh
```

**Step 5: Commit**

```bash
git add src/dsp/wav_writer.h src/dsp/wav_writer.c scripts/build.sh
git commit -m "feat: add WAV file writer with smpl chunk loop support"
```

---

## Task 2: Silence Detection

**Files:**
- Create: `src/dsp/silence_detect.h`
- Create: `src/dsp/silence_detect.c`

**Step 1: Create silence_detect.h**

```c
#ifndef SILENCE_DETECT_H
#define SILENCE_DETECT_H

#include <stdint.h>

/* Compute RMS of stereo interleaved int16 buffer (mono mixdown).
 * Returns RMS as float (0.0 to 32768.0 range). */
float silence_rms(const int16_t *stereo, int num_frames);

/* Find first frame where signal exceeds threshold.
 * Uses sliding window RMS. Returns frame index, or -1 if not found.
 * Backs up safety_frames before the detected onset. */
int silence_find_onset(const int16_t *stereo, int num_frames,
                       float threshold_rms, int window_frames,
                       int safety_frames);

/* Find frame where signal drops below threshold for duration_frames.
 * Searches backwards from end. Returns the frame where silence begins,
 * or num_frames if no silence found. */
int silence_find_tail(const int16_t *stereo, int num_frames,
                      float threshold_rms, int window_frames,
                      int duration_frames);

#endif
```

**Step 2: Create silence_detect.c**

Implement:
- `silence_rms()`: sum squares of (L+R)/2, divide by count, sqrt
- `silence_find_onset()`: slide window forward, find first window above threshold, back up by safety_frames
- `silence_find_tail()`: slide window backward from end, find where RMS stays below threshold for duration_frames

**Step 3: Update build.sh, compile, commit**

```bash
git add src/dsp/silence_detect.h src/dsp/silence_detect.c scripts/build.sh
git commit -m "feat: add RMS-based silence detection for trim and tail"
```

---

## Task 3: SFZ Writer

**Files:**
- Create: `src/dsp/sfz_writer.h`
- Create: `src/dsp/sfz_writer.c`

**Step 1: Create sfz_writer.h**

```c
#ifndef SFZ_WRITER_H
#define SFZ_WRITER_H

#include <stdint.h>

#define SFZ_MAX_ZONES 24
#define SFZ_MAX_LAYERS 8

typedef struct {
    int midi_note;          /* MIDI note that was sampled */
    int velocity;           /* velocity that was sampled */
    char filename[128];     /* relative path: samples/C2_v042.wav */
    int has_loop;           /* 0 or 1 */
    int loop_start;         /* sample frame */
    int loop_end;           /* sample frame */
    int loop_crossfade;     /* sample count for crossfade */
} sfz_sample_info_t;

typedef struct {
    char instrument_name[128];
    int num_zones;
    int num_layers;
    int range_low;          /* lowest MIDI note */
    int range_high;         /* highest MIDI note */
    int sample_notes[SFZ_MAX_ZONES];        /* MIDI notes sampled */
    int sample_velocities[SFZ_MAX_LAYERS];  /* velocities sampled */
    sfz_sample_info_t samples[SFZ_MAX_ZONES * SFZ_MAX_LAYERS];
    int sample_count;
} sfz_instrument_t;

/* Write .sfz file. Returns 0 on success, -1 on error. */
int sfz_write(const char *path, const sfz_instrument_t *inst);

#endif
```

**Step 2: Create sfz_writer.c**

Implement:
- Compute key zone boundaries (midpoints between sampled notes)
- Compute velocity crossfade ranges (overlapping regions with xfin/xfout)
- Write `<control>`, `<global>`, then `<group>` + `<region>` blocks
- Include loop opcodes when `has_loop` is set
- Use `fprintf` for text output

Key zone boundary algorithm:
```
For note[i] and note[i+1]:
  boundary = floor((note[i] + note[i+1]) / 2)
  zone[i].hikey = boundary
  zone[i+1].lokey = boundary + 1
First zone: lokey = max(0, note[0] - half_interval)
Last zone:  hikey = min(127, note[N-1] + half_interval)
```

Velocity crossfade algorithm:
```
For layers v[0] < v[1] < ... < v[M-1]:
  midpoint(i, i+1) = floor((v[i] + v[i+1]) / 2)
  Layer 0:   lovel=1,                   hivel=midpoint(0,1)
             xfout_lovel=v[0]+1,        xfout_hivel=midpoint(0,1)
  Layer i:   lovel=midpoint(i-1,i),     hivel=midpoint(i,i+1)
             xfin_lovel=midpoint(i-1,i), xfin_hivel=v[i]-1
             xfout_lovel=v[i]+1,         xfout_hivel=midpoint(i,i+1)
  Layer M-1: lovel=midpoint(M-2,M-1),   hivel=127
             xfin_lovel=midpoint(M-2,M-1), xfin_hivel=v[M-1]-1
```

**Step 3: Update build.sh, compile, commit**

```bash
git add src/dsp/sfz_writer.h src/dsp/sfz_writer.c scripts/build.sh
git commit -m "feat: add SFZ file writer with key zones and velocity crossfades"
```

---

## Task 4: KissFFT Integration

**Files:**
- Create: `src/dsp/third_party/kiss_fft.h`
- Create: `src/dsp/third_party/kiss_fft.c`
- Create: `src/dsp/third_party/kiss_fftr.h`
- Create: `src/dsp/third_party/kiss_fftr.c`

**Step 1: Add KissFFT as files (BSD license)**

Download or copy KissFFT source files. We need the real-FFT wrapper (`kiss_fftr`) for efficient real-valued transforms.

Can be added as a git submodule: `git submodule add https://github.com/mborgerding/kissfft.git src/dsp/third_party/kissfft`

Or just copy the 4 needed files (kiss_fft.h, kiss_fft.c, kiss_fftr.h, kiss_fftr.c) directly — they're BSD licensed and self-contained.

**Step 2: Update build.sh to include KissFFT sources**

Add `-Isrc/dsp/third_party` and the .c files to gcc.

**Step 3: Compile, commit**

```bash
git add src/dsp/third_party/
git commit -m "feat: add KissFFT library for spectral analysis"
```

---

## Task 5: Loop Finder

**Files:**
- Create: `src/dsp/loop_finder.h`
- Create: `src/dsp/loop_finder.c`

**Step 1: Create loop_finder.h**

```c
#ifndef LOOP_FINDER_H
#define LOOP_FINDER_H

#include <stdint.h>

typedef struct {
    int found;              /* 1 if a good loop was found, 0 otherwise */
    int loop_start;         /* sample frame */
    int loop_end;           /* sample frame */
    int loop_crossfade;     /* recommended crossfade in samples */
    float quality;          /* 0.0-1.0, correlation quality */
} loop_result_t;

/* Find optimal loop points in a stereo sample.
 * Analyzes mono mixdown of the sustain region (skip_frames..sustain_end_frame).
 * Returns result with found=0 if no acceptable loop found. */
loop_result_t loop_find(const int16_t *stereo_samples,
                        int num_frames,
                        int skip_frames,        /* frames to skip (attack) */
                        int sustain_end_frame);  /* end of sustain region */

#endif
```

**Step 2: Create loop_finder.c**

Implement the spectral+waveform hybrid algorithm:

1. **Mono mixdown** of sustain region: `(L[i] + R[i]) / 2`
2. **Window the sustain** into 2048-sample overlapping windows (50% overlap)
3. **FFT each window** using kiss_fftr, keep magnitude spectrum
4. **Compare all non-adjacent pairs** (>100ms apart): spectral distance = sum of squared log-magnitude differences
5. **Keep top 20 candidates** by spectral similarity
6. **Refine each candidate**: search ±3ms for zero-crossing pairs, compute normalized cross-correlation over 10ms window
7. **Score**: spectral_similarity × correlation × length_bonus
8. **Return best** if correlation > 0.9, else found=0
9. **Crossfade**: 10% of loop length, clamped 220-2205 samples (5-50ms)

**Step 3: Update build.sh, compile, commit**

```bash
git add src/dsp/loop_finder.h src/dsp/loop_finder.c scripts/build.sh
git commit -m "feat: add spectral+correlation loop finder"
```

---

## Task 6: DSP Plugin State Machine

**Files:**
- Modify: `src/dsp/samplerobot_plugin.c` (replace stub with full implementation)

**Step 1: Define instance state**

```c
typedef enum {
    STATE_IDLE = 0,
    STATE_PRE_SAMPLE,
    STATE_RECORDING,
    STATE_RELEASE,
    STATE_PROCESSING,
    STATE_DONE
} sample_state_t;

typedef struct {
    /* Config params */
    int range_low;
    int range_high;
    int key_zones;
    int velocity_layers;
    float hold_duration;
    int loop_detect;
    int midi_channel;
    char instrument_name[128];

    /* Sampling state */
    sample_state_t state;
    int current_zone;
    int current_layer;
    int total_samples;
    int completed_samples;
    int loops_found;

    /* Audio capture buffer */
    int16_t *capture_buf;       /* stereo interleaved */
    int capture_frames;
    int capture_max_frames;

    /* Noise floor */
    float noise_floor_rms;

    /* Timing */
    int block_counter;
    int hold_blocks;            /* hold_duration in blocks */

    /* Computed schedule */
    int sample_notes[SFZ_MAX_ZONES];
    int sample_velocities[SFZ_MAX_LAYERS];

    /* Results */
    sfz_instrument_t instrument;

    /* Output path */
    char output_dir[512];
    char samples_dir[512];

    /* Status for UI */
    char status_text[128];
    int current_note;
    int current_velocity;
} samplerobot_instance_t;
```

**Step 2: Implement create_instance**

- Allocate instance, set defaults
- Compute output_dir: `/data/UserData/move-anything/modules/sound_generators/sfz/instruments/`

**Step 3: Implement set_param / get_param**

- `set_param`: handle range_low, range_high, key_zones, velocity_layers, hold_duration, loop_detect, midi_channel, instrument_name, and "start" (triggers sampling), "stop" (aborts)
- `get_param`: return state, progress (completed/total), status_text, current_note, current_velocity, loops_found, plus all config params

**Step 4: Implement render_block — state machine**

```c
static void sr_render_block(void *instance, int16_t *out, int frames) {
    samplerobot_instance_t *inst = (samplerobot_instance_t *)instance;
    int16_t *audio_in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);

    /* Always passthrough for monitoring */
    memcpy(out, audio_in, frames * 2 * sizeof(int16_t));

    switch (inst->state) {
    case STATE_IDLE:
        break;

    case STATE_PRE_SAMPLE:
        /* Accumulate audio for noise floor measurement */
        /* After 100ms (~4410 frames, ~34 blocks), compute RMS and transition */
        capture_append(inst, audio_in, frames);
        inst->block_counter++;
        if (inst->block_counter >= 35) {
            inst->noise_floor_rms = silence_rms(inst->capture_buf, inst->capture_frames);
            inst->capture_frames = 0;
            /* Send MIDI Note On */
            send_note_on(inst);
            inst->state = STATE_RECORDING;
            inst->block_counter = 0;
        }
        break;

    case STATE_RECORDING:
        capture_append(inst, audio_in, frames);
        inst->block_counter++;
        if (inst->block_counter >= inst->hold_blocks) {
            send_note_off(inst);
            inst->state = STATE_RELEASE;
            inst->block_counter = 0;
        }
        break;

    case STATE_RELEASE:
        capture_append(inst, audio_in, frames);
        inst->block_counter++;
        /* Check for silence or timeout (10s) */
        if (check_silence_or_timeout(inst)) {
            inst->state = STATE_PROCESSING;
        }
        break;

    case STATE_PROCESSING:
        /* Process one sample per render_block to avoid blocking audio */
        process_current_sample(inst);
        advance_to_next(inst);
        break;

    case STATE_DONE:
        break;
    }
}
```

**Step 5: Implement helper functions**

- `capture_append()`: copy audio_in to capture buffer
- `send_note_on()` / `send_note_off()`: 3-byte MIDI via `g_host->midi_send_external()`
- `check_silence_or_timeout()`: use silence_detect functions
- `process_current_sample()`: trim audio, run loop_find if enabled, call wav_write, populate sfz_sample_info
- `advance_to_next()`: increment layer/zone, if all done call sfz_write and set STATE_DONE
- `compute_schedule()`: calculate evenly-spaced notes and velocities

**Step 6: Compile, commit**

```bash
git add src/dsp/samplerobot_plugin.c
git commit -m "feat: implement sampling state machine with audio capture and MIDI send"
```

---

## Task 7: UI — Setup Screen

**Files:**
- Modify: `src/ui.js` (replace stub)

**Step 1: Implement setup view**

The setup screen shows all parameters, navigable with jog wheel, editable with knobs.

```javascript
var VIEW_SETUP = 0;
var VIEW_NAMING = 1;
var VIEW_SAMPLING = 2;
var VIEW_PROCESSING = 3;
var VIEW_DONE = 4;

var currentView = VIEW_SETUP;

/* Setup params */
var params = [
    { key: 'range_low',       label: 'Low Note',  val: 36,  min: 0,   max: 127, fmt: noteNameFmt },
    { key: 'range_high',      label: 'Hi Note',   val: 84,  min: 0,   max: 127, fmt: noteNameFmt },
    { key: 'key_zones',       label: 'Zones',     val: 8,   min: 1,   max: 24,  fmt: null },
    { key: 'velocity_layers', label: 'Layers',    val: 3,   min: 1,   max: 8,   fmt: null },
    { key: 'hold_duration',   label: 'Hold (s)',  val: 3.0, min: 0.5, max: 30,  fmt: null, step: 0.5 },
    { key: 'loop_detect',     label: 'Loop',      val: 1,   min: 0,   max: 1,   fmt: onOffFmt },
    { key: 'midi_channel',    label: 'MIDI Ch',   val: 1,   min: 1,   max: 16,  fmt: null },
];
var selectedParam = 0;
```

- Jog wheel: scroll selectedParam
- Knob 1: adjust selected param value
- Record button (MoveRec CC): advance to naming view
- Display: list params with highlight on selected, show computed total samples

**Step 2: Implement note name formatter**

```javascript
var NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
function noteNameFmt(n) {
    return NOTE_NAMES[n % 12] + (Math.floor(n / 12) - 2);
}
function onOffFmt(v) { return v ? 'ON' : 'OFF'; }
```

**Step 3: Compile (no build needed for JS), commit**

```bash
git add src/ui.js
git commit -m "feat: add setup screen UI with parameter editing"
```

---

## Task 8: UI — Naming Screen

**Files:**
- Modify: `src/ui.js`

**Step 1: Implement text input**

Simple character-by-character naming:
- Jog wheel: scroll through characters (A-Z, a-z, 0-9, space, -, _)
- Jog click: confirm character, advance cursor
- Back: delete last character
- Record/Capture: confirm name and start sampling

Display:
```
NAME INSTRUMENT
> Rhodes_______
  [A] ← jog to change
```

**Step 2: On confirm, send all params to DSP**

```javascript
function startSampling() {
    for (var p of params) {
        host_module_set_param(p.key, String(p.val));
    }
    host_module_set_param('instrument_name', instrumentName);
    host_module_set_param('start', '1');
    currentView = VIEW_SAMPLING;
}
```

**Step 3: Commit**

```bash
git add src/ui.js
git commit -m "feat: add instrument naming screen"
```

---

## Task 9: UI — Sampling and Progress Screens

**Files:**
- Modify: `src/ui.js`

**Step 1: Implement sampling view**

Poll DSP state each tick:
```javascript
function drawSamplingView() {
    var state = host_module_get_param('state');
    var progress = host_module_get_param('progress');
    var note = host_module_get_param('current_note');
    var vel = host_module_get_param('current_velocity');
    var status = host_module_get_param('status_text');
    // ... draw progress bar, note name, velocity, status
}
```

- Progress bar: filled proportion = completed / total
- Current note + velocity displayed
- Status text from DSP (e.g., "Recording...", "Waiting for silence...")
- Hold stop to abort: send `set_param('stop', '1')`

**Step 2: Implement processing view**

Same polling, shows "Finding loops..." and per-sample results.

**Step 3: Implement done view**

Shows summary: name, sample count, zones, loops found. "Ready in SFZ Player". Any button returns to setup.

**Step 4: Handle state transitions**

In `onTick()`, check DSP state to auto-advance view:
```javascript
if (currentView === VIEW_SAMPLING || currentView === VIEW_PROCESSING) {
    var dspState = host_module_get_param('state');
    if (dspState === 'processing') currentView = VIEW_PROCESSING;
    if (dspState === 'done') currentView = VIEW_DONE;
}
```

**Step 5: Commit**

```bash
git add src/ui.js
git commit -m "feat: add sampling progress, processing, and done screens"
```

---

## Task 10: Help File and Polish

**Files:**
- Modify: `src/help.json`

**Step 1: Write help content**

```json
{
    "title": "Sample Robot",
    "children": [
        {
            "title": "Overview",
            "lines": [
                "Autosample external",
                "MIDI gear to create",
                "SFZ instruments.",
                "",
                "Connect synth via",
                "USB MIDI + line in.",
                "Set audio input to",
                "line-in first."
            ]
        },
        {
            "title": "Setup",
            "lines": [
                "Low/Hi Note: range",
                "Zones: # of keys",
                "Layers: velocity lvls",
                "Hold: note duration",
                "Loop: find loop pts",
                "MIDI Ch: output ch",
                "",
                "Jog: select param",
                "Knob 1: adjust value",
                "Rec: start sampling"
            ]
        },
        {
            "title": "Sampling",
            "lines": [
                "Fully automatic.",
                "Progress shown on",
                "screen.",
                "",
                "Hold Stop to cancel.",
                "Partial instruments",
                "are kept.",
                "",
                "Output goes to SFZ",
                "Player instruments."
            ]
        }
    ]
}
```

**Step 2: Commit**

```bash
git add src/help.json
git commit -m "feat: add help documentation"
```

---

## Task 11: Integration Test on Device

**Step 1: Build**

```bash
cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything-samplerobot
./scripts/build.sh
```

**Step 2: Install**

```bash
./scripts/install.sh
```

**Step 3: Test on Move**

1. Verify module appears in Tools menu
2. Verify setup screen displays and params are editable
3. Connect external synth, verify audio passthrough (hear the synth)
4. Name instrument, start sampling
5. Verify MIDI notes are sent (check external synth responds)
6. Verify WAV files written to correct location
7. Verify .sfz file generated with correct structure
8. Switch to SFZ Player, verify instrument loads and plays

**Step 4: Fix any issues found**

**Step 5: Final commit**

```bash
git add -A
git commit -m "fix: integration test fixes"
```

---

## Task Order Summary

| Task | Component | Dependencies |
|------|-----------|-------------|
| 0 | Scaffold | None |
| 1 | WAV Writer | Task 0 |
| 2 | Silence Detection | Task 0 |
| 3 | SFZ Writer | Task 0 |
| 4 | KissFFT | Task 0 |
| 5 | Loop Finder | Task 4 |
| 1-3 can be done in parallel | | |
| 6 | DSP State Machine | Tasks 1, 2, 3, 5 |
| 7 | UI Setup | Task 0 |
| 8 | UI Naming | Task 7 |
| 9 | UI Progress | Task 8 |
| 10 | Help | Task 0 |
| 7-10 can be done in parallel with 1-5 | | |
| 11 | Integration Test | All above |

**Parallelizable groups:**
- Group A (DSP components): Tasks 1, 2, 3, 4 (all independent)
- Group B (UI): Tasks 7, 8, 9, 10 (sequential but independent of Group A)
- Task 5 depends on Task 4
- Task 6 depends on Tasks 1-5
- Task 11 depends on all
