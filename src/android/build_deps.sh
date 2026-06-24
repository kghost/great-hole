#!/bin/bash
set -e

# Configuration
NDK="$HOME/android-sdk/ndk/30.0.14904198"
API="26"

echo "NDK: $NDK"
echo "API: $API"

SRC_DIR="$PWD/build-android/src"
mkdir -p "$SRC_DIR"

# 1. Download c-ares
if [ ! -d "$SRC_DIR/c-ares-1.30.0" ]; then
    echo "Downloading c-ares..."
    pushd "$SRC_DIR"
    curl -L https://github.com/c-ares/c-ares/releases/download/v1.30.0/c-ares-1.30.0.tar.gz | tar -xz
    popd
fi

# 2. Download Boost
if [ ! -d "$SRC_DIR/boost_1_86_0" ]; then
    echo "Downloading Boost..."
    pushd "$SRC_DIR"
    curl -L -O https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz
    tar -xzf boost_1_86_0.tar.gz
    rm boost_1_86_0.tar.gz
    popd
fi

# 3. Bootstrap Boost if b2 is missing
if [ ! -f "$SRC_DIR/boost_1_86_0/b2" ]; then
    echo "Bootstrapping Boost..."
    cd "$SRC_DIR/boost_1_86_0"
    ./bootstrap.sh
fi

for ABI in arm64-v8a x86_64; do
    for BUILD_TYPE in debug release; do
        PREFIX="$PWD/build-android-$ABI-$BUILD_TYPE/dependencies/install"
        BUILD_DIR="$PWD/build-android-$ABI-$BUILD_TYPE/build"

        mkdir -p "$PREFIX"
        mkdir -p "$BUILD_DIR"

        echo "Building dependencies for Android..."
        echo "PREFIX: $PREFIX"
        echo "BUILD_DIR: $BUILD_DIR"

        # 1. Build c-ares
        if [ ! -f "$PREFIX/lib/libcares.a" ]; then
            echo "Building c-ares for $ABI..."
            mkdir -p "$BUILD_DIR/c-ares"
            pushd "$BUILD_DIR/c-ares"
            cmake -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
                -DANDROID_ABI="$ABI" \
                -DANDROID_PLATFORM=android-"$API" \
                -DCMAKE_INSTALL_PREFIX="$PREFIX" \
                -DCARES_STATIC=ON \
                -DCARES_SHARED=OFF \
                -DCARES_BUILD_TOOLS=OFF \
                -DCARES_BUILD_TESTS=OFF \
                "$SRC_DIR/c-ares-1.30.0"
            make -j$(nproc) install
            popd
        fi

        # 2. Build boost
        if [ ! -f "$PREFIX/lib/libboost_log.a" ]; then
            echo "Building Boost for $ABI..."
            
            # Determine Boost build variables based on ABI
            case "$ABI" in
                "arm64-v8a")
                    BOOST_ARCH="arm"
                    BOOST_ADDR="64"
                    COMPILER_NAME="aarch64-linux-android${API}-clang++"
                    ;;
                "armeabi-v7a")
                    BOOST_ARCH="arm"
                    BOOST_ADDR="32"
                    COMPILER_NAME="armv7a-linux-androideabi${API}-clang++"
                    ;;
                "x86")
                    BOOST_ARCH="x86"
                    BOOST_ADDR="32"
                    COMPILER_NAME="i686-linux-android${API}-clang++"
                    ;;
                "x86_64")
                    BOOST_ARCH="x86"
                    BOOST_ADDR="64"
                    COMPILER_NAME="x86_64-linux-android${API}-clang++"
                    ;;
                *)
                    echo "Unsupported ABI: $ABI"
                    exit 1
                    ;;
            esac

            # Setup user-config.jam in BUILD_DIR
            COMPILER="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/$COMPILER_NAME"
            echo "using clang : android : $COMPILER ;" > "$BUILD_DIR/user-config.jam"

            # Build Boost
            pushd "$SRC_DIR/boost_1_86_0"
            ./b2 --user-config="$BUILD_DIR/user-config.jam" \
                --build-dir="$BUILD_DIR/boost" \
                variant=$BUILD_TYPE \
                toolset=clang-android \
                target-os=android \
                architecture="$BOOST_ARCH" \
                address-model="$BOOST_ADDR" \
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
            popd
        fi
    done
done

echo "Dependencies built successfully!"
