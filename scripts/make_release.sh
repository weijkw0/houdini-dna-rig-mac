#!/bin/bash
# Zip the built dylibs with install.sh for a GitHub release:
#   scripts/make_release.sh v1.0.0  ->  dist/houdini-dna-rig-mac-v1.0.0-houdini22.0-arm64.zip
set -euo pipefail
cd "$(dirname "$0")/.."
TAG=${1:?usage: scripts/make_release.sh <tag>}
NAME=houdini-dna-rig-mac-$TAG-houdini22.0-arm64
D=dist/$NAME
rm -rf "$D" "dist/$NAME.zip"
mkdir -p "$D/bin"
cp build/bin/metahuman_dna_import.dylib build/bin/metahuman_rig_logic.dylib "$D/bin/"
cp install.sh README.md LICENSE THIRD_PARTY_NOTICES.md "$D/"
(cd dist && zip -qry "$NAME.zip" "$NAME")
shasum -a 256 "dist/$NAME.zip"
