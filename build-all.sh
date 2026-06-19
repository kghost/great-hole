#!/bin/bash

set -e

cmake --workflow --preset debug
cmake --workflow --preset debug-gcc-snapshot
cmake --workflow --preset android-arm64-v8a
cmake --workflow --preset android-x86_64
