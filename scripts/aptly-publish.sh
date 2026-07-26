#!/bin/bash

set -e
set -x

DEBIAN_APTLY_SERVER="$1"

SCRIPT_DIR=$(dirname -- "${BASH_SOURCE[0]}")
echo "SCRIPT_DIR: $SCRIPT_DIR"
cd "$SCRIPT_DIR/.."

Version=$(dpkg-parsechangelog --show-field Version)

scp -r build-debian "$DEBIAN_APTLY_SERVER:"

ssh "$DEBIAN_APTLY_SERVER" /bin/bash << EOF
cd build-debian
aptly repo include -accept-unsigned -repo trixie great-hole_${Version}_source.changes
aptly repo copy trixie sid great-hole_${Version}_source
aptly repo include -accept-unsigned -repo trixie trixie/great-hole_${Version}_amd64.changes
aptly repo include -accept-unsigned -repo sid sid/great-hole_${Version}_amd64.changes
aptly publish update sid sid
aptly publish update trixie trixie
EOF
