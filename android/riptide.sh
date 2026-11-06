#!/bin/sh
# Riptide (../riptide) and the Granite libraries it uses, cross-compiled for
# Android arm64 with the NDK's CMake toolchain, into build/riptide. Called by
# build.sh with GM_RIPTIDE=1. Needs cmake and ninja; Granite is checked out
# into riptide/Granite the first time (riptide/checkout_granite.sh).
set -eu
cd "$(dirname "$0")"
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/$(ls "$SDK/ndk" | sort -V | tail -1)}
[ -d ../riptide/Granite ] || (cd ../riptide && bash checkout_granite.sh > /dev/null)
cmake -S ../riptide -B build/riptide -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=33 -DANDROID_STL=c++_static \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON > build/riptide-cmake.log
ninja -C build/riptide riptide granite-vulkan granite-math granite-util granite-volk
