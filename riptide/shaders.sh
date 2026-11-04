#!/bin/sh
# Regenerates the compiled shaders after a change to shaders/*.comp: the
# Vulkan table (shaders/slangmosh.hpp, with Granite's slangmosh: build it with
# Granite's CMake, target slangmosh, and put it on PATH) and the Metal sources
# (metal/shaders/transpile.sh: glslang and SPIRV-Cross).
set -e
cd "$(dirname "$0")"
slangmosh --output shaders/slangmosh.hpp shaders/slangmosh.json --namespace Riptide -O --strip
slangmosh --output shaders/slangmosh_scaler.hpp Granite/video/slangmosh_encode.json --namespace Riptide -O --strip
./metal/shaders/transpile.sh
