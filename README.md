# SFZ Player Module

Polyphonic SFZ and DecentSampler instrument player for Move Everything using [sfizz](https://github.com/sfztools/sfizz) by sfztools.

## Features

- Load SFZ (.sfz) and DecentSampler (.dspreset) instruments
- Browse instrument folders with jog wheel
- Multiple .sfz files per folder shown as variants (selectable from menu)
- Velocity layers, round robin, key switching
- Octave transpose (-4 to +4)
- Gain control
- Full polyphony with voice stealing
- Pitch bend, aftertouch, CC support
- Signal Chain compatible
- Debounced browsing for smooth jog wheel navigation

## Prerequisites

- [Move Everything](https://github.com/charlesvestal/move-everything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Installation

### Via Module Store (Recommended)

1. Launch Move Everything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Sound Generators** → **SFZ Player**
4. Select **Install**

### Manual Installation

```bash
./scripts/install.sh
```

## Loading Instruments

### Quick Setup

1. Download an SFZ instrument (see "Instrument Sources" below)
2. Copy the instrument folder to Move:
   ```bash
   scp -r MyInstrument ableton@move.local:/data/UserData/move-anything/modules/sound_generators/sfz/instruments/
   ```
3. Restart the SFZ module or browse to the new instrument

Each subfolder in `instruments/` is one instrument. The folder should contain `.sfz` or `.dspreset` files along with their sample files (WAV/FLAC).

### Folder Structure

```
instruments/
  MyPiano/
    piano.sfz
    samples/
      C3.wav
      D3.wav
      ...
  MiniFreak/
    presets/             ← .sfz files can be in subdirectories
      VerdantMeadows.sfz
      SereneLake.sfz
    Samples/
      Recorded/
        ...
```

The scanner automatically finds `.sfz` files in subdirectories like `presets/` or `Programs/`, so instruments from sfzinstruments.github.io and drolez.com work as-is without restructuring.

If sample paths don't resolve from the `.sfz` file's directory, the player automatically retries from the instrument root folder.

## Controls

| Control | Action |
|---------|--------|
| **Jog wheel** | Browse instrument folders |
| **Menu** | Choose variant (.sfz file) within instrument |
| **Knob 1** | Octave transpose (-4 to +4) |
| **Knob 2** | Gain (0.0 to 2.0) |
| **Pads** | Play notes (velocity sensitive) |

## Instrument Sources

Free SFZ instruments are available from:
- https://sfzinstruments.github.io/ — curated collection of free SFZ instruments
- https://github.com/sfzinstruments — GitHub repositories with samples included
- https://drolez.com/blog/music/ableton-free-sound-packs.php — free synth sample packs (MiniFreak, Pro-1, etc.) with SFZ presets

**Included categories:** Pianos, bass, guitars, drums/percussion, strings, brass/winds, synths/organs, folk/world instruments, melodic percussion.

## Troubleshooting

**No sound:**
- Ensure the instrument folder contains both `.sfz` files and sample audio files
- Check the error display in the preset browser — it shows load errors
- "Sample files not found" means the WAV/FLAC files are missing from the folder
- "0 regions" means the SFZ parsed but no samples were mapped

**Instrument not showing:**
- Folder must be inside `instruments/` directory
- Must contain at least one `.sfz` or `.dspreset` file (can be in a subdirectory)

**Clicking/glitching:**
- Very large instruments with many samples may take time to preload
- Try instruments with fewer velocity layers or smaller sample sets

## Technical Details

- Sample rate: 48000 Hz
- Block size: 128 frames
- Output: Stereo float
- Engine: sfizz (full SFZ v2 support with ARIA extensions)

## Building from Source

```bash
./scripts/build.sh
```

Requires Docker or ARM64 cross-compiler.

## Credits

- [sfizz](https://github.com/sfztools/sfizz) by sfztools (BSD-2-Clause license)
- Instrument samples from [sfzinstruments](https://sfzinstruments.github.io/) (various CC licenses)

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
