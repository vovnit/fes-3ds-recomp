#!/bin/sh
# Builds the iOS / iPadOS app from ps2-web-recomp and the C++ generated from your own disc, and packs it as an unsigned .ipa.
#   portios/build.sh            device build  -> <ps2-web-recomp>/build/ios-p3fes/P3FES.ipa
#   portios/build.sh sim        Simulator build -> <ps2-web-recomp>/build/ios-sim-p3fes/P3FES.app
# The game code is translated again for iOS into <ps2-web-recomp>/generated/p3fes-ios, without the 3DS options (no hot copies,
# no 3DS natives, no 32-bit narrowing, no loop locals, stack accesses checked as usual): generated/p3fes is tuned for the 3DS.
# That takes about a second and runs again when the translator, the boot program, entries.txt or this script is newer than the last one.
# Environment:
#   PS2_ROOT        ps2-web-recomp checkout (default: ../ps2-web-recomp next to this repository)
#   P3FES_WGPU_DIR  unpacked wgpu-native iOS release for GPU drawing (see portios/README.md). Default:
#                   <ps2-web-recomp>/build/wgpu-ios/device or .../sim when that folder exists; none found: CPU renderer
#   JOBS            parallel compile jobs (default 6)
# The app holds code translated from the game: keep the .ipa on your own machine and devices.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PS2_ROOT=${PS2_ROOT:-$(cd "$HERE/../../ps2-web-recomp" && pwd)}
SHARED=$PS2_ROOT/generated/p3fes
GENERATED=$PS2_ROOT/generated/p3fes-ios
EERECOMP=$PS2_ROOT/build/tools/eerecomp
ELF=$PS2_ROOT/games/p3fes/disc/SLUS_216.21
ENTRIES=$PS2_ROOT/games/p3fes/entries.txt
JOBS=${JOBS:-6}

if [ "${1:-}" = sim ]; then
    SYSROOT=iphonesimulator
    OUT=$PS2_ROOT/build/ios-sim-p3fes
else
    SYSROOT=iphoneos
    OUT=$PS2_ROOT/build/ios-p3fes
fi
if [ $SYSROOT = iphonesimulator ]; then KIND=sim; else KIND=device; fi
WGPU=${P3FES_WGPU_DIR:-}
[ -z "$WGPU" ] && [ -f "$PS2_ROOT/build/wgpu-ios/$KIND/lib/libwgpu_native.a" ] && WGPU=$PS2_ROOT/build/wgpu-ios/$KIND
[ -x "$EERECOMP" ] && [ -f "$ELF" ] && [ -f "$ENTRIES" ] || { echo "need $EERECOMP, $ELF and $ENTRIES: build the native version first (docs/install.md)"; exit 1; }
# a stamp file says when the translation last ran
STAMP=$GENERATED/.translated
if [ ! -f "$GENERATED/table.cpp" ] || [ ! -f "$STAMP" ] || [ "$EERECOMP" -nt "$STAMP" ] || [ "$ELF" -nt "$STAMP" ] ||
   [ "$ENTRIES" -nt "$STAMP" ] || [ "$0" -nt "$STAMP" ]; then
    echo "translating the game for iOS into $GENERATED"
    rm -f "$GENERATED"/unit_*.cpp "$GENERATED"/hot_*.cpp "$GENERATED"/hot.hpp
    EERECOMP_WIDE=1 EERECOMP_NOLOOPS=1 EERECOMP_SPCHECK=1 "$EERECOMP" "$ELF" --entries "$ENTRIES" --out "$GENERATED" | tail -1
    touch "$STAMP"
fi
# the vector unit programs are the desktop ones (the 3DS has its own, made with --scalar, in generated/p3fes-vu3ds)
for f in "$SHARED"/vu1_*.cpp; do
    [ -f "$f" ] || continue
    [ "$GENERATED/$(basename "$f")" -nt "$f" ] || cp -p "$f" "$GENERATED/"
done

cmake -S "$HERE" -B "$OUT" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SYSROOT -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
    -DCMAKE_BUILD_TYPE=Release -DPS2_ROOT="$PS2_ROOT" -DPS2_GENERATED="$GENERATED" -DP3FES_WGPU_DIR="$WGPU"
ninja -C "$OUT" -j "$JOBS" p3fes

if [ $SYSROOT = iphonesimulator ]; then
    echo "built $OUT/P3FES.app"
    exit 0
fi
# an .ipa is the app inside a Payload folder, zipped; sideloading tools sign it with your own Apple ID
rm -rf "$OUT/ipa" "$OUT/P3FES.ipa"
mkdir -p "$OUT/ipa/Payload"
cp -R "$OUT/P3FES.app" "$OUT/ipa/Payload/"
xcrun strip -S -x "$OUT/ipa/Payload/P3FES.app/P3FES"
(cd "$OUT/ipa" && zip -qry "$OUT/P3FES.ipa" Payload)
rm -rf "$OUT/ipa"
ls -l "$OUT/P3FES.ipa"
