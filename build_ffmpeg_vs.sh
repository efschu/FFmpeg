#!/bin/bash
# Build script for FFmpeg with VapourSynth filter support
# Fork: https://github.com/efschu/FFmpeg
# 
# Usage: ./build_ffmpeg_vs.sh [options]
#
# Options:
#   --install-deps     Install VapourSynth and dependencies
#   --clean            Clean and rebuild
#   --enable-gpl       Enable GPL licensed components
#   --enable-nonfree   Enable non-free components (for NVIDIA NVENC/NVDEC)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG_DIR="${SCRIPT_DIR}"
BUILD_DIR="${FFMPEG_DIR}/build"
INSTALL_PREFIX="${FFMPEG_DIR}/install"

# Default options
CLEAN=0
INSTALL_DEPS=0
ENABLE_GPL=1
ENABLE_NONFREE=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --clean)
            CLEAN=1
            shift
            ;;
        --install-deps)
            INSTALL_DEPS=1
            shift
            ;;
        --enable-gpl)
            ENABLE_GPL=1
            shift
            ;;
        --disable-gpl)
            ENABLE_GPL=0
            shift
            ;;
        --enable-nonfree)
            ENABLE_NONFREE=1
            shift
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Install dependencies
install_deps() {
    echo "=== Installing VapourSynth and dependencies ==="
    
    if command -v apt-get &> /dev/null; then
        sudo apt-get update
        sudo apt-get install -y \
            python3 python3-pip \
            vapoursynth vapoursynth-python \
            libvapoursynth-dev libvapoursynth-script-dev \
            nasm yasm \
            cmake pkg-config \
            libssl-dev \
            libx264-dev libx265-dev libvpx-dev \
            libfdk-aac-dev libmp3lame-dev libopus-dev \
            libass-dev libfreetype6-dev libfribidi-dev libharfbuzz-dev \
            libsdl2-dev \
            || echo "Some packages may not be available"
    elif command -v pacman &> /dev/null; then
        sudo pacman -S --noconfirm \
            python python-pip \
            vapoursynth \
            nasm yasm \
            cmake pkg-config \
            openssl \
            x264 x265 libvpx \
            fdk-aac lame opus \
            freetype2 fribidi harfbuzz \
            sdl2 \
            || echo "Some packages may not be available"
    fi
    
    # Install Python VapourSynth plugins (TensorRT support)
    pip3 install --user vapoursynth
    pip3 install --user numpy
    
    # Common TensorRT VapourSynth plugins
    # pip3 install --user vs-rife
    
    echo "=== Dependencies installed ==="
}

# Clean build
clean_build() {
    echo "=== Cleaning build ==="
    rm -rf "${BUILD_DIR}"
    rm -rf "${INSTALL_PREFIX}"
    mkdir -p "${BUILD_DIR}"
    mkdir -p "${INSTALL_PREFIX}"
    echo "=== Build cleaned ==="
}

# Configure
configure() {
    echo "=== Configuring FFmpeg ==="
    
    cd "${FFMPEG_DIR}"
    
    # Base configuration
    CONFIG_OPTIONS="
        --prefix=${INSTALL_PREFIX}
        --bindir=${INSTALL_PREFIX}/bin
        --libdir=${INSTALL_PREFIX}/lib
        --incdir=${INSTALL_PREFIX}/include
        --datadir=${INSTALL_PREFIX}/share
        --mandir=${INSTALL_PREFIX}/share/man
        --docdir=${INSTALL_PREFIX}/share/doc
        --htmldir=${INSTALL_PREFIX}/share/doc/ffmpeg
        --disable-doc
        --disable-static
        --enable-shared
        --enable-gpl
        --enable-version3
        --enable-libx264
        --enable-libx265
        --enable-libvpx
        --enable-libmp3lame
        --enable-libopus
        --enable-libfdk-aac
        --enable-libass
        --enable-libfreetype
        --enable-libfribidi
        --enable-libharfbuzz
        --enable-sdl2
    "
    
    # VapourSynth support (the key feature!)
    CONFIG_OPTIONS="${CONFIG_OPTIONS}
        --enable-vapoursynth
    "
    
    # Non-free components (optional)
    if [ $ENABLE_NONFREE -eq 1 ]; then
        CONFIG_OPTIONS="${CONFIG_OPTIONS}
            --enable-nonfree
            --enable-cuda-nvcc
            --enable-cuda-llvm
            --enable-libnpp
            --enable-cuvid
            --enable-nvenc
            --enable-nvdec
            --enable-ffnvcodec
        "
    fi
    
    # GPL components
    if [ $ENABLE_GPL -eq 1 ]; then
        CONFIG_OPTIONS="${CONFIG_OPTIONS}
            --enable-gpl
            --enable-postproc
            --enable-libx264
            --enable-libx265
            --enable-libvidstab
        "
    fi
    
    # Create build directory and configure
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"
    
    echo "Running: ../configure ${CONFIG_OPTIONS}"
    ../configure ${CONFIG_OPTIONS}
    
    echo "=== Configuration complete ==="
}

# Build
build() {
    echo "=== Building FFmpeg ==="
    cd "${BUILD_DIR}"
    
    # Get number of CPU cores
    if command -v nproc &> /dev/null; then
        NJOBS=$(nproc)
    elif command -v sysctl &> /dev/null; then
        NJOBS=$(sysctl -n hw.ncpu)
    else
        NJOBS=4
    fi
    
    echo "Building with ${NJOBS} parallel jobs..."
    
    make -j${NJOBS}
    
    echo "=== Build complete ==="
}

# Install
install() {
    echo "=== Installing FFmpeg ==="
    cd "${BUILD_DIR}"
    make install
    
    echo "=== Installation complete ==="
    echo "FFmpeg installed to: ${INSTALL_PREFIX}"
    echo ""
    echo "To use VapourSynth filter, run:"
    echo "  ${INSTALL_PREFIX}/bin/ffmpeg -i input.mkv -vf 'vapoursynth=file=script.vpy' output.mkv"
    echo ""
    echo "Or for TensorRT-based upscaling:"
    echo "  ${INSTALL_PREFIX}/bin/ffmpeg -i input.mkv -vf 'vapoursynth=file=upscale_tensorrt.vpy' -threads 8 output.mkv"
}

# Main
main() {
    echo "=== FFmpeg VS Build Script ==="
    echo "Build directory: ${BUILD_DIR}"
    echo "Install prefix: ${INSTALL_PREFIX}"
    echo ""
    
    if [ $INSTALL_DEPS -eq 1 ]; then
        install_deps
    fi
    
    if [ $CLEAN -eq 1 ]; then
        clean_build
    fi
    
    configure
    build
    install
    
    echo ""
    echo "=== FFmpeg with VapourSynth filter built successfully! ==="
    echo ""
    echo "Key features:"
    echo "  - VapourSynth filter: -vf 'vapoursynth=file=script.vpy'"
    echo "  - TensorRT plugins can be loaded directly via VapourSynth scripts"
    echo "  - Multithreaded processing with configurable thread count"
    echo "  - Direct in-process execution (no pipes)"
    echo ""
    echo "Example VapourSynth script (upscale.vpy):"
    echo '  import vapoursynth as vs'
    echo '  from vapoursynth import core'
    echo '  # Load TensorRT plugin'
    echo '  core.std.LoadPlugin("/path/to/vsplugin.so")'
    echo '  clip = core.imwri.Read()  # or your source filter'
    echo '  # Apply upscaling'
    echo '  upscaled = core.trt.Model(clip, model="path/to/model.engine")'
    echo '  upscaled.set_output()'
}

main
