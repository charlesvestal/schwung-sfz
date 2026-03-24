#!/bin/bash
# Install SFZ module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/sfz" ]; then
    echo "Error: dist/sfz not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing SFZ Module ==="

# Deploy to Move - sound_generators subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/sound_generators/sfz"
scp -r dist/sfz/* ableton@move.local:/data/UserData/schwung/modules/sound_generators/sfz/

# Install chain presets if they exist
if [ -d "src/chain_patches" ]; then
    echo "Installing chain presets..."
    scp src/chain_patches/*.json ableton@move.local:/data/UserData/schwung/patches/
fi

# Create instruments directory for user SFZ instruments
echo "Creating instruments directory..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/sound_generators/sfz/instruments"

# Set permissions so Module Store can update later
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/sound_generators/sfz"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/schwung/modules/sound_generators/sfz/"
echo ""
echo "Upload SFZ instrument folders to the instruments/ subdirectory."
echo "Restart Move Anything to load the new module."
