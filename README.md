# Multisampler

Polyphonic multisample instrument player for [Schwung](https://github.com/charlesvestal/move-anything) on Ableton Move. Loads SFZ (.sfz) and DecentSampler (.dspreset) sample libraries — pianos, keys, drums, strings, brass, synths, drum kits, and anything else shipped in those formats.

Built on a fork of [xsynth](https://github.com/arduano/xsynth) (LGPL-3.0), with extensive Move-specific changes: disk streaming for libraries larger than RAM, voice-side sample rate conversion, DecentSampler-to-SFZ conversion, denormal-flush in audio threads, and many small ergonomic fixes.

## Features

- **SFZ v2 + ARIA** opcodes: regions, groups, key/velocity layers, round robin, key switches, loops with crossfade, release samples, filter/cutoff, ADSR, pitch/amp LFOs, filter envelope, knob CC bindings, curve tables.
- **DecentSampler (.dspreset)** support via on-load conversion to SFZ — covers the common multisampled-instrument subset (envelopes, looping, filter, reverb, group amps, knob bindings).
- **Disk streaming** — libraries larger than RAM (e.g. Salamander Grand Piano, ~5 GB) play directly off the SD card. Three streaming paths:
  - `.x44c` cache (prebaked on Mac for maximum first-load speed)
  - Direct 16-bit PCM WAV
  - Direct FLAC (no prebake required)
- **Voice-side SRC** for mismatched sample rates (e.g. 48 kHz FLAC played at 44.1 kHz target).
- **Loop wrap support** — looped regions extend their always-resident head buffer to cover the whole loop, so wrap reads never fall out of the bounded streaming window.
- **Per-preset auto-gain** — bright libraries (Salamander, etc.) are automatically attenuated to land in the same loudness ballpark as DecentSampler reference renders.
- **Sample-edge fade** — automatic ramp-to-zero on samples whose recording ends mid-decay, suppresses EOF clicks.
- **Stateless soft-clip** waveshaper on the output stage; transparent below -0.92 dBFS, smooth saturation above.

## Installation

### Module Store (recommended)

1. Open Schwung on your Move
2. Module Store → Sound Generators → **Multisampler**
3. Install

### Manual

```bash
./scripts/build.sh   # cross-compiles via Docker
./scripts/install.sh # rsync to move.local
```

## Loading Sample Libraries

Each top-level folder inside `instruments/` is one "library" in the preset browser. Every `.sfz` or `.dspreset` file inside it (up to 5 levels deep) becomes one preset.

```
instruments/
  MyPiano/                  ← one library, one or more presets
    piano.sfz
    Samples/
      C3.wav  D3.wav  …
  Salamander Grand Piano/   ← one library, one preset
    Salamander Grand Piano V3.sfz
    Samples/
      A0v1.flac  A0v2.flac …
```

The scanner finds `.sfz` files in subdirectories like `presets/` or `Programs/`, so libraries from sfzinstruments.github.io, drolez.com, and pianobook.co.uk work as-is.

If sample paths in the SFZ don't resolve from the SFZ file's directory, the loader retries from the library root.

### Faster cold loads

Decoded sample data is cached as `.x44c` files alongside the sources. Run `prebake_cache` on Mac to bake these for very large libraries before uploading — it parallelizes FLAC decode at ~400 files/sec. Without prebake, first-load decode runs on Move's CPU and takes ~50 ms per FLAC source.

## Controls

| Control | Action |
|---|---|
| Jog wheel | Browse preset list across all libraries |
| Shift + L/R | Jump to next/previous library |
| Knob: Octave | Transpose -4 to +4 octaves |
| Knob: Gain | Output level 0.0 to 2.0 |
| Knob: Voices | Polyphony cap 4 to 128 |
| Knobs 3-8 | Per-library macros (DecentSampler knob bindings) |
| Pads | Velocity-sensitive notes |

## Library Sources

| Source | Format | Notes |
|---|---|---|
| [sfzinstruments.github.io](https://sfzinstruments.github.io/) | SFZ | Curated free SFZ libraries (pianos, keys, drums, etc.) |
| [pianobook.co.uk](https://pianobook.co.uk/) | DecentSampler | Large community library of free DS presets |
| [Salamander Grand Piano](https://archive.org/details/SalamanderGrandPianoV3) | SFZ | 48 kHz FLAC, streams direct (~5 GB on disk) |
| [drolez.com/blog/music](https://drolez.com/blog/music/) | SFZ | Free synth packs (MiniFreak, Pro-1) |

## Troubleshooting

**Silent / "no sound":**
- Confirm the library folder contains both `.sfz`/`.dspreset` AND sample audio (`.wav`/`.flac`)
- Check the preset browser error display — load errors surface there
- "Sample not found" → WAV/FLAC missing from the folder
- "0 regions" → the SFZ parsed but no samples mapped to any key

**Library not appearing:**
- Folder must be inside `instruments/`
- Must contain a `.sfz` or `.dspreset` (up to 5 levels deep)
- `.converted.sfz` files (DecentSampler conversion outputs) are filtered from the preset list

**Audio glitches:**
- First note after preset load may have a brief delay as samples page into the OS cache
- Heavy chord bursts on multi-layered libraries can exceed the audio frame budget — reduce voice cap
- Long-tail releases use FZ (flush-to-zero) to avoid denormal CPU traps; this is automatic

**Long release tails cut off abruptly:**
- The sample EOF reached before the envelope completed. SFZ libraries with short samples and long `ampeg_decay` exhibit this. An automatic 256-frame edge fade smooths the cutoff.

## Building from Source

```bash
./scripts/build.sh    # cross-compile via Docker (aarch64-linux-gnu)
./scripts/install.sh  # rsync dist/sfz/ to /data/UserData/schwung/modules/sound_generators/sfz on move.local
```

The `sfz/` directory name in build output and on-device is intentional — it's the module id, kept stable so upgrades from previous SFZ Player versions are seamless.

Requires Docker + the cross-compiler image (built automatically on first run).

## Technical Details

- Sample rate: 44.1 kHz target output
- Block size: 128 frames per render call
- Output: stereo 16-bit
- Engine: forked xsynth-core with disk streaming, voice-side SRC, FZ-in-rayon
- Streaming: per-voice ring buffer + 3-thread I/O pool; head buffer covers chord-burst spawn latency

## Credits

- [xsynth](https://github.com/arduano/xsynth) by Arduano + contributors — LGPL-3.0
- DecentSampler conversion path written for this project; covers the documented `.dspreset` subset
- Sample libraries linked above retain their own licenses (most CC-BY or CC-0)

## License

LGPL-3.0 (inherited from xsynth-core).

## AI Assistance Disclaimer

Developed with AI assistance (Claude). All architecture, implementation, and release decisions are reviewed by human maintainers. AI-assisted content may still contain errors — please validate functionality, security, and license compatibility before production use.
