#!/usr/bin/env bash
# Cross-compile the two benchmark CLIs for aarch64. Run inside the
# move-anything-sfz-builder Docker image so we get both Rust+aarch64
# and the aarch64 gcc cross.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-sfz-builder"

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== bench build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Image missing — run scripts/build.sh first to bootstrap it."
        exit 1
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./bench/build-bench.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

mkdir -p build

# --- xsynth shim (Rust → staticlib) ---
echo "=== Building xsynth shim ==="
cd src/dsp/third_party/xsynth_shim
cargo +nightly build --release --target aarch64-unknown-linux-gnu
cd "$REPO_ROOT"
XSHIM_A="src/dsp/third_party/xsynth_shim/target/aarch64-unknown-linux-gnu/release/libxsynth_shim.a"

# --- sfizz library (CMake) ---
if [ ! -f "build/sfizz-build/library/lib/libsfizz.a" ]; then
    echo "=== Building sfizz ==="
    mkdir -p build/sfizz-build
    cat > build/aarch64-toolchain.cmake << 'TOOLCHAIN_EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a -mtune=cortex-a72")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a -mtune=cortex-a72")
TOOLCHAIN_EOF
    (cd build/sfizz-build && \
     cmake ../../src/dsp/third_party/sfizz \
        -DCMAKE_TOOLCHAIN_FILE=../aarch64-toolchain.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DSFIZZ_JACK=OFF -DSFIZZ_RENDER=OFF -DSFIZZ_BENCHMARKS=OFF \
        -DSFIZZ_TESTS=OFF -DSFIZZ_DEMOS=OFF -DSFIZZ_DEVTOOLS=OFF \
        -DPLUGIN_LV2=OFF -DPLUGIN_LV2_UI=OFF -DPLUGIN_VST3=OFF \
        -DPLUGIN_AU=OFF -DPLUGIN_PUREDATA=OFF \
        -DSFIZZ_SHARED=OFF -DBUILD_SHARED_LIBS=OFF && \
     cmake --build . -j$(nproc))
fi
SFIZZ_LIB_DIR="build/sfizz-build/library/lib"

# --- xsynth bench ---
echo "=== Linking xsynth_bench ==="
${CROSS_PREFIX}gcc -O3 -static \
    -march=armv8-a -mtune=cortex-a72 \
    bench/xsynth_bench.c \
    "$XSHIM_A" \
    -o build/xsynth_bench \
    -lm -lpthread -ldl

# --- sfizz bench ---
echo "=== Linking sfizz_bench ==="
${CROSS_PREFIX}gcc -O3 \
    -march=armv8-a -mtune=cortex-a72 \
    -Isrc/dsp/third_party/sfizz/src \
    bench/sfizz_bench.c \
    -Wl,--whole-archive \
    "$SFIZZ_LIB_DIR"/lib*.a \
    -Wl,--no-whole-archive \
    -o build/sfizz_bench \
    -lm -lpthread -ldl -lstdc++ -latomic

echo ""
echo "=== Done ==="
ls -lh build/xsynth_bench build/sfizz_bench
