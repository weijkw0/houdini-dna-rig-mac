#!/bin/bash
# Build the two native components of Epic's MetaHuman for Houdini plugin for macOS (Apple Silicon):
#   build/bin/metahuman_dna_import.dylib   SOP  mhdna_import
#   build/bin/metahuman_rig_logic.dylib    APEX mh::RigLogic, mh::DrivenKeys
# Then, if an official MetaHuman for Houdini download is given (or found in vendor/), assemble and
# register a Houdini package with install.sh.
#
#   ./build.sh [path/to/MetaHumanForHoudini]
#
# Env: HFS  Houdini 22.0 Resources dir (default: newest /Applications/Houdini/Houdini22.0.*)
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD

if [ -z "${HFS:-}" ]; then
  HFS=$(ls -d /Applications/Houdini/Houdini22.0.*/Frameworks/Houdini.framework/Versions/22.0/Resources 2>/dev/null | sort -V | tail -1 || true)
fi
[ -x "${HFS:-}/bin/hcustom" ] || { echo "Houdini 22.0 not found; set HFS to its .../Versions/22.0/Resources dir" >&2; exit 1; }
export HFS PATH=$HFS/bin:$PATH
echo "Houdini: $HFS"

ORL=$ROOT/third_party/openriglogic
[ -f "$ORL/include/riglogic/RigLogic.h" ] || git submodule update --init third_party/openriglogic
export TMPDIR=$ROOT/build/tmp
mkdir -p "$TMPDIR" build/obj build/bin

# The feature set UE 5.8 builds RigLogicLib with on Mac (RigLogicLib.Build.cs). Without
# RL_BUILD_WITH_XYZ_ROTATION_ORDER the Euler->quaternion conversion is a no-op and joints don't move.
RL_DEFS=(-DRL_BUILD_WITH_HALF_FLOATS -DTRIO_MMAP_AVAILABLE -DRL_BUILD_WITH_XYZ_ROTATION_ORDER -DRL_AUTODETECT_SSE -DRL_AUTODETECT_HALF_FLOATS)

# 1) OpenRigLogic (DNA reader + RigLogic) as a static library. Both dylibs link it, so its symbols are
#    hidden: otherwise dyld binds the second dylib's status-code registry to the first one and aborts.
LIB=build/librigdna.a
if [ ! -f "$LIB" ]; then
  FLAGS=(-std=c++17 -O2 -fPIC -arch arm64 -mmacosx-version-min=11.0 -I"$ORL/include" -I"$ORL/src"
         "${RL_DEFS[@]}" -fvisibility=hidden -fvisibility-inlines-hidden -w)
  objs=()
  while IFS= read -r f; do
    case "$f" in *Windows*) continue;; esac
    o=build/obj/$(echo "$f" | md5 | cut -c1-12).o
    clang++ "${FLAGS[@]}" -c "$f" -o "$o"
    objs+=("$o")
  done < <(find "$ORL/src/dna" "$ORL/src/pma" "$ORL/src/status" "$ORL/src/trio" "$ORL/src/riglogic" -name "*.cpp" | sort)
  ar rcs "$LIB" "${objs[@]}"
  echo "built $LIB (${#objs[@]} objects)"
fi

# 2) The two Houdini DSOs.
export HCUSTOM_CFLAGS="-I$ORL/include -I$ORL/src -DRL_BUILD_WITH_XYZ_ROTATION_ORDER -w"
hcustom -q -L "$ROOT/build" -l rigdna -i "$TMPDIR/dso" src/SOP_MHDNAImport.C
mv "$TMPDIR/dso/SOP_MHDNAImport.dylib" build/bin/metahuman_dna_import.dylib
hcustom -q -L "$ROOT/build" -l rigdna -i "$TMPDIR/apexdso" src/apex_metahuman_rig_logic.C
mv "$TMPDIR/apexdso/apex_metahuman_rig_logic.dylib" build/bin/metahuman_rig_logic.dylib
echo "built build/bin/metahuman_dna_import.dylib, build/bin/metahuman_rig_logic.dylib"

# 3) Package.
OFFICIAL=${1:-$ROOT/vendor/MetaHumanForHoudini}
if [ -d "$OFFICIAL" ]; then
  ./install.sh "$OFFICIAL"
else
  echo "No official MetaHuman for Houdini download given; run ./install.sh <path/to/MetaHumanForHoudini> to package."
fi
