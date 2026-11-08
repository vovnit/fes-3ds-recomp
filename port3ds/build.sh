#!/bin/bash
# Builds the New 3DS test build with the official devkitARM image. Needs Docker (or OrbStack).
#   port3ds/build.sh [path to the ps2-web-recomp folder]     default: ../ps2-web-recomp next to this repository
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
RECOMP="${1:-$HERE/../ps2-web-recomp}"
RECOMP="$(cd "$RECOMP" && pwd)"
docker run --rm -v "$HERE:/p" -v "$RECOMP:/w" devkitpro/devkitarm:20260610 make -f /p/port3ds/Makefile -j8 --output-sync=target VU="${VU:-1}" PROF="${PROF:-0}" IOPRECORD="${IOPRECORD:-0}" GAMEOPT="${GAMEOPT:-}"
ls -la "$RECOMP/build/3ds/p3fes.3dsx"
