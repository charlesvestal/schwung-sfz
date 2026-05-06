# CLAUDE.md

## Project Overview

SFZ sample player module for Move Anything. Uses sfizz library (BSD-2-Clause).

## Build Commands

```bash
./scripts/build.sh      # Build with Docker (cross-compiles sfizz + plugin)
./scripts/install.sh    # Deploy to Move
```

## Structure

```
src/
  module.json           # Module metadata
  ui.js                 # JavaScript UI
  help.json             # On-device help
  dsp/
    sfz_plugin.c        # Plugin wrapper around sfizz
    third_party/
      sfizz/            # Git submodule - sfizz library
```

## Instrument Organization

- Each top-level entry in `instruments/` becomes one instrument:
  - A folder = one instrument named after the folder. Variants are all
    `.sfz` / `.dspreset` files found anywhere inside (recursive, up to 5 levels).
  - A loose `.sfz` / `.dspreset` file at the top of `instruments/` = one
    instrument with one variant.
- Examples:
  - `Cosmos/COSMOS.dspreset` → instrument "Cosmos", variant "COSMOS"
  - `Raw Violin/{Pad,Granular Pad,Harmonic Pad,Raw Violin}.dspreset` →
    instrument "Raw Violin", 4 variants
  - `K4Coll-1.01/K4-Acoustic/K4-Acoustic.dspreset` (and ~60 sibling folders)
    → instrument "K4Coll-1.01", 60 variants
  - `DS - The Synths/DS_The Synths/*.dspreset` → instrument "DS - The Synths",
    45 variants
- Shift+L/R switches instruments; preset nav switches variants. Each variant
  loads with its own folder as the sample-resolution root (so nested layouts
  like K4Coll work).

## DSP Plugin API

Standard Move Anything plugin_api_v2:
- `on_load()`: Initialize sfizz synth, scan instruments
- `on_midi()`: Process MIDI input via sfizz
- `set_param()`: Set instrument_index, preset, gain, octave_transpose
- `get_param()`: Get instrument/preset info, state
- `render_block()`: Render 128 frames stereo via sfizz
