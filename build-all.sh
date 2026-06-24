#!/bin/bash

set -e

cmake --workflow --preset debug
cmake --workflow --preset debug-gcc-snapshot
src/android/build_deps.sh
cmake --workflow --preset android-arm64-v8a-debug
cmake --workflow --preset android-x86_64-debug
cmake --workflow --preset android-arm64-v8a-release
cmake --workflow --preset android-x86_64-release
