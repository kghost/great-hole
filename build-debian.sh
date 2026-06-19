#!/bin/bash

set -e

gbp buildpackage -S -us -uc --git-export-dir=build/debian --git-submodules

Version=$(dpkg-parsechangelog --show-field Version)

for dist in trixie sid; do
    echo "Building for $dist"
    mkdir -p build/debian/$dist
    pushd build/debian/$dist
    sbuild -d $dist --build-dep-resolver=aptitude ../great-hole_${Version}.dsc
    popd
done
