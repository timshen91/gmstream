#!/bin/sh
# PyroWave (github.com/Themaister/pyrowave, MIT): GPU wavelet intra codec with
# exact per-frame size control. Static libraries and the C API header into
# $P (default /opt/gm). Run from build/deps.sh; needs cmake, ninja and a C++
# compiler. Vulkan is loaded at run time (volk), so nothing links against it.
set -eu
P=${P:-/opt/gm}
PYROWAVE_COMMIT=79a3232
cd "${TMPDIR:-/tmp}"
rm -rf pyrowave
git clone -q https://github.com/Themaister/pyrowave.git pyrowave
cd pyrowave
git -c advice.detachedHead=false checkout -q $PYROWAVE_COMMIT
bash checkout_granite.sh > /dev/null
cmake -S . -B b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
ninja -C b pyrowave-shared
# The C API and the video scaler are only built into the shared library
# upstream; archive their objects.
ar rcs $P/lib/libpyrowave-c.a b/CMakeFiles/pyrowave-shared.dir/pyrowave_c.cpp.o \
	b/CMakeFiles/pyrowave-shared.dir/Granite/video/scaler.cpp.o
cp b/libpyrowave.a b/Granite/vulkan/libgranite-vulkan.a b/Granite/math/libgranite-math.a \
	b/Granite/util/libgranite-util.a b/Granite/third_party/libgranite-volk.a $P/lib/
mkdir -p $P/include/pyrowave
cp pyrowave.h $P/include/pyrowave/
cp -r Granite/third_party/khronos/vulkan-headers/include/vulkan Granite/third_party/khronos/vulkan-headers/include/vk_video $P/include/ 2>/dev/null ||
	cp -r Granite/third_party/khronos/vulkan-headers/include/vulkan $P/include/
cd .. && rm -rf pyrowave
