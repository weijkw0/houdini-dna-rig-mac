#!/bin/bash
# Make Epic's MetaHuman for Houdini usable on macOS: copy its cross-platform part (HDAs, help, icons,
# masks) into a package folder, put the macOS dylibs where the Linux .so files were, and register the
# package with Houdini 22.0.
#
#   ./install.sh path/to/MetaHumanForHoudini     (the folder from Epic's download: lib/, package/, ...)
#
# The package goes next to the dylibs (build/package in a source checkout, package/ in a release zip).
# Uninstall: delete ~/Library/Preferences/houdini/22.0/packages/houdini_dna_rig_mac.json
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NAME=houdini_dna_rig_mac
TESTED=1.2.11
OFFICIAL=$(cd "${1:?usage: ./install.sh path/to/MetaHumanForHoudini}" && pwd)
if [ -d "$HERE/bin" ]; then BIN=$HERE/bin; else BIN=$HERE/build/bin; fi
for f in metahuman_dna_import.dylib metahuman_rig_logic.dylib; do
  [ -f "$BIN/$f" ] || { echo "missing $BIN/$f (run ./build.sh first)" >&2; exit 1; }
done
SRC=$(ls -d "$OFFICIAL"/lib/mh_hou_character_assembler/*/platform-linux/houdini-22.0/package 2>/dev/null | sort -V | tail -1 || true)
[ -n "$SRC" ] || { echo "no lib/mh_hou_character_assembler/*/platform-linux/houdini-22.0/package in $OFFICIAL" >&2; exit 1; }
VER=$(echo "$SRC" | sed -E 's|.*/mh_hou_character_assembler/([^/]+)/.*|\1|')
[ "$VER" = "$TESTED" ] || echo "warning: character assembler $VER found; this build was tested with $TESTED"

PKG=$(dirname "$BIN")/package
A=$PKG/mh_hou_character_assembler
mkdir -p "$A/dso" "$A/apexdso"
rsync -a --delete --exclude '*.so' --exclude '*.dylib' "$SRC/mh_hou_character_assembler/" "$A/"
cp "$SRC/mh_hou_character_assembler.json" "$PKG/"
# Copy-then-rename, so a Houdini that still has the old dylib loaded is not disturbed.
for pair in dso/metahuman_dna_import.dylib apexdso/metahuman_rig_logic.dylib; do
  cp "$BIN/$(basename "$pair")" "$A/$pair.tmp" && mv -f "$A/$pair.tmp" "$A/$pair"
done
# Files from a downloaded zip carry the quarantine flag, which stops Houdini from loading the dylibs.
xattr -dr com.apple.quarantine "$A/dso" "$A/apexdso" 2>/dev/null || true

PREFS=$HOME/Library/Preferences/houdini/22.0/packages
mkdir -p "$PREFS"
printf '{\n    "package_path": "%s"\n}\n' "$PKG" > "$PREFS/$NAME.json"
echo "package: $PKG (character assembler $VER)"
echo "registered: $PREFS/$NAME.json - restart Houdini 22.0 to load it"
[ ! -f "$PREFS/MetaHumanForHoudini.json" ] || echo "warning: $PREFS/MetaHumanForHoudini.json (Epic's Linux/Windows package) is also installed; remove it to avoid duplicate HDAs"
