# SFZ Module Design

## Overview

New `move-anything-sfz` module providing SFZ format sample playback using the [sfizz](https://github.com/sfztools/sfizz) library. Separate module from the existing SF2 player.

## Architecture

- **Engine**: sfizz (BSD-2-Clause, C API, proven on ARM64)
- **Plugin API**: V2 instance-based
- **Format**: SFZ files with associated sample directories

## Instrument Organization

- `instruments/` directory on device
- Each subfolder = one instrument (folder name = display name)
- Multiple `.sfz` files within a folder = variants (selected from menu)
- Jog wheel browses instrument folders, menu selects variant
- Last-used variant remembered per instrument

## File Structure

```
move-anything-sfz/
├── src/
│   ├── dsp/
│   │   ├── sfz_plugin.c        # Plugin API V2 wrapper
│   │   └── third_party/
│   │       └── sfizz/           # Git submodule
│   ├── module.json
│   ├── ui.js
│   └── help.json
├── scripts/
│   ├── build.sh                 # Docker + CMake cross-compile
│   ├── install.sh
│   └── Dockerfile
├── release.json
└── CLAUDE.md
```

## Parameters

| Parameter | R/W | Description |
|---|---|---|
| preset / instrument_index | RW | Current instrument folder index (preset browser) |
| preset_count / instrument_count | R | Number of instrument folders |
| preset_name / instrument_name | R | Current folder name |
| instrument_list | R | JSON array of folder names |
| variant | RW | Current .sfz index within instrument |
| variant_count | R | Number of .sfz files in instrument |
| variant_name | R | Current .sfz filename (sans ext) |
| variant_list | R | JSON array of variant names |
| gain | RW | Output level 0.0-2.0 |
| octave_transpose | RW | -4 to +4 |
| state | RW | Full JSON state for save/restore |

## Sample Path Resolution

When loading `.sfz` files from a subdirectory (e.g. `presets/`), sample paths are first resolved relative to the `.sfz` file directory. If no samples are found, the loader retries with the instrument root folder as the base path. This handles packs like drolez/SHLD that put `.sfz` files in `presets/` but samples in `Samples/` at the root.

## Build

sfizz built from source via CMake in Docker, linked as static library into dsp.so.
