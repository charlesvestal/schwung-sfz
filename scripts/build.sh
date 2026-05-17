#!/usr/bin/env bash
# Build Multisampler module for Schwung (ARM64).
#
# Module id stays "sfz" for seamless upgrades from the previous SFZ Player
# (the build paths, tarball name, and on-device install dir keep that name
# too). User-facing name is "Multisampler"; see module.json.
#
# Uses xsynth as the engine. Cross-compile via Docker, which carries both
# aarch64-linux-gnu-gcc and a Rust nightly + the aarch64-unknown-linux-gnu
# target.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-sfz-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Multisampler Build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    echo ""
    echo "=== Done ==="
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

echo "=== Building Multisampler (xsynth) ==="
echo "Cross prefix: $CROSS_PREFIX"

mkdir -p build dist/sfz

# --- Step 1: Build xsynth_shim (Rust staticlib) ---
echo ""
echo "=== Building xsynth shim ==="
SHIM_DIR="src/dsp/third_party/xsynth_shim"
if [ ! -f "$SHIM_DIR/Cargo.toml" ]; then
    echo "Error: xsynth_shim crate missing. Did you run 'git submodule update --init'?"
    exit 1
fi
( cd "$SHIM_DIR" && cargo +nightly build --release --target aarch64-unknown-linux-gnu )
XSHIM_A="$SHIM_DIR/target/aarch64-unknown-linux-gnu/release/libxsynth_shim.a"
if [ ! -f "$XSHIM_A" ]; then
    echo "Error: shim staticlib missing at $XSHIM_A"
    exit 1
fi

# --- Step 2: Compile and link the plugin ---
echo ""
echo "=== Compiling DSP plugin ==="
for src in src/dsp/xsynth_plugin.c src/dsp/dspreset_to_xsynth_sfz.c; do
    obj="build/$(basename "$src" .c).o"
    ${CROSS_PREFIX}gcc -O3 -fPIC \
        -march=armv8-a -mtune=cortex-a72 \
        -DNDEBUG \
        -c "$src" \
        -o "$obj" \
        -Isrc/dsp
done

# Link the plugin as a shared lib. The shim staticlib pulls in xsynth-core,
# symphonia (audio decoders), rayon, and the rest of the Rust runtime. C++
# stdlib isn't needed anymore (sfizz dropped). libdl, libpthread, libm, librt
# all required by Rust's std and rayon.
echo "=== Linking dsp.so ==="
${CROSS_PREFIX}gcc -O3 -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    build/xsynth_plugin.o \
    build/dspreset_to_xsynth_sfz.o \
    "$XSHIM_A" \
    -o build/dsp.so \
    -lm -lpthread -ldl -lrt

echo "DSP plugin linked"

# --- Step 3: Package ---
echo ""
echo "=== Packaging ==="
cat src/module.json > dist/sfz/module.json
cat src/ui.js > dist/sfz/ui.js
cat build/dsp.so > dist/sfz/dsp.so
[ -f src/help.json ] && cat src/help.json > dist/sfz/help.json
chmod +x dist/sfz/dsp.so
mkdir -p dist/sfz/instruments

cd dist
tar -czvf sfz-module.tar.gz sfz/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/sfz/"
echo "Tarball: dist/sfz-module.tar.gz"
echo ""
echo "To install on Move:  ./scripts/install.sh"
