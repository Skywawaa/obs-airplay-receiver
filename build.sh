#!/bin/bash
# ============================================
# OBS AirPlay Receiver - Linux/macOS Build
# ============================================
#
# Prerequisites:
#   - cmake, gcc/clang, pkg-config
#   - obs-studio development headers (libobs-dev)
#   - ffmpeg development headers (libavcodec-dev, libavutil-dev, libswscale-dev)
#
# Ubuntu/Debian:
#   sudo apt install cmake build-essential pkg-config \
#     libobs-dev libavcodec-dev libavutil-dev libswscale-dev
#
# macOS (Homebrew):
#   brew install cmake ffmpeg obs
#

set -e

BUILD_DIR="build"
BUILD_TYPE="${BUILD_TYPE:-Release}"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    ${OBS_DIR:+-DOBS_DIR="$OBS_DIR"} \
    ${FFMPEG_DIR:+-DFFMPEG_DIR="$FFMPEG_DIR"}

cmake --build . --config "$BUILD_TYPE" -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo ""
echo "============================================"
echo "Build successful!"
echo ""
echo "To install:"
echo "  cmake --install . --config $BUILD_TYPE"
echo ""
echo "Or manually copy:"
echo "  ${BUILD_DIR}/obs-airplay-receiver.so"
echo "  to: ~/.config/obs-studio/plugins/obs-airplay-receiver/bin/64bit/"
echo "============================================"
