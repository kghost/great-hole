#!/bin/bash
set -e

# Configuration
NDK="${1:-$HOME/android-sdk/ndk/30.0.14904198}"
ABI="${2:-arm64-v8a}"
API="${3:-26}"
PREFIX="${4:-$PWD/build-android/dependencies/install}"

SRC_DIR="$PWD/build-android/dependencies/src"
mkdir -p "$SRC_DIR"
mkdir -p "$PREFIX"

echo "Building dependencies for Android..."
echo "NDK: $NDK"
echo "ABI: $ABI"
echo "API: $API"
echo "PREFIX: $PREFIX"

# 1. Build c-ares
if [ ! -f "$PREFIX/lib/libcares.a" ]; then
    echo "Downloading and building c-ares..."
    cd "$SRC_DIR"
    if [ ! -d "c-ares-1.30.0" ]; then
        curl -L https://github.com/c-ares/c-ares/releases/download/v1.30.0/c-ares-1.30.0.tar.gz | tar -xz
    fi
    cd c-ares-1.30.0
    mkdir -p build && cd build
    cmake -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
          -DANDROID_ABI="$ABI" \
          -DANDROID_PLATFORM=android-"$API" \
          -DCMAKE_INSTALL_PREFIX="$PREFIX" \
          -DCARES_STATIC=ON \
          -DCARES_SHARED=OFF \
          -DCARES_BUILD_TOOLS=OFF \
          -DCARES_BUILD_TESTS=OFF \
          ..
    make -j$(nproc) install
fi

# 2. Build boost
if [ ! -f "$PREFIX/lib/libboost_log.a" ]; then
    echo "Downloading and building Boost..."
    cd "$SRC_DIR"
    if [ ! -d "boost_1_86_0" ]; then
        curl -L -O https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz
        tar -xzf boost_1_86_0.tar.gz
        rm boost_1_86_0.tar.gz
    fi
    cd boost_1_86_0
    
    # Bootstrap b2
    if [ ! -f "./b2" ]; then
        ./bootstrap.sh
    fi
    
    # Setup user-config.jam
    COMPILER="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${API}-clang++"
    echo "using clang : android : $COMPILER ;" > user-config.jam
    
    # Build Boost
    ./b2 --user-config=user-config.jam \
         toolset=clang-android \
         target-os=android \
         architecture=arm \
         address-model=64 \
         link=static \
         threading=multi \
         cxxflags="-fPIC" \
         cflags="-fPIC" \
         --with-log \
         --with-program_options \
         --with-filesystem \
         --with-thread \
         install \
         --prefix="$PREFIX" \
         -j$(nproc)
fi

echo "Dependencies built successfully!"
