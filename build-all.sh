#!/bin/bash

set -e

[ ! -L build ] && ln -s build-clang-debug build

cmake --workflow --preset clang-debug
cmake --workflow --preset gcc-14-debug
cmake --workflow --preset gcc-snapshot-debug
src/android/build_deps.sh
cmake --workflow --preset android-arm64-v8a-debug
cmake --workflow --preset android-x86_64-debug
cmake --workflow --preset android-arm64-v8a-release
cmake --workflow --preset android-x86_64-release
