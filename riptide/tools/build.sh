#!/bin/sh
# Builds the Metal Riptide library objects and tools/rtrd into build/.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
FL="-O2 -std=c++17 -fobjc-arc -Imetal -DRIPTIDE_EXPORT_SYMBOLS -Wno-unused-parameter"
for s in riptide_common riptide_decoder riptide_encoder; do clang++ $FL -c metal/$s.mm -o build/$s.o; done
clang++ $FL -c metal/riptide_bitstream.cpp -o build/riptide_bitstream.o
clang++ $FL -c riptide_inter.cpp -o build/riptide_inter.o
clang++ $FL tools/rtrd.mm build/riptide_*.o -o build/rtrd -framework Metal -framework Foundation -framework IOSurface -framework QuartzCore
clang++ $FL tools/rtdec.mm build/riptide_*.o -o build/rtdec -framework Metal -framework Foundation -framework IOSurface -framework QuartzCore
