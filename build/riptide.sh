#!/bin/sh
# Riptide (../riptide): its static libraries and C API header into $P (default
# /opt/gm), beside PyroWave's (build/pyrowave.sh, which provides the Granite
# libraries both use: the same Granite commit). Run from the source tree, in the
# build image: sh build/riptide.sh. Needs cmake, ninja, a C++ compiler and, the
# first time, git (Granite is checked out into riptide/Granite).
set -eu
P=${P:-/opt/gm}
src=$(cd "$(dirname "$0")/../riptide" && pwd)
out=${RIPTIDE_BUILD:-$src/build}
cd "$src"
[ -d Granite ] || bash checkout_granite.sh > /dev/null
cmake -S . -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON > /dev/null
ninja -C "$out" riptide riptide-c rtenc
cp "$out/libriptide.a" "$out/libriptide-c.a" $P/lib/
mkdir -p $P/include/riptide
cp riptide.h $P/include/riptide/
