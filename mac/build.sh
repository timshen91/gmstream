#!/bin/sh
# Builds GMStream.app (native macOS client) with the Command Line Tools'
# swiftc and clang; no Xcode project. Output: mac/build/GMStream.app
#
#   ./build.sh && open build/GMStream.app --args steamtab.local
set -e
cd "$(dirname "$0")"
mkdir -p build

# PyroWave's Metal decoder (github.com/Themaister/pyrowave, MIT), built from
# the same commit as the host's encoder (build/pyrowave.sh).
PYROWAVE_COMMIT=79a3232
pw=build/pyrowave
if [ ! -d $pw ]; then
	git clone -q https://github.com/Themaister/pyrowave.git $pw
fi
git -C $pw -c advice.detachedHead=false checkout -q $PYROWAVE_COMMIT
mkdir -p build/pw-obj
for f in $pw/metal/*.mm $pw/metal/*.cpp; do
	o=build/pw-obj/$(basename "$f").o
	[ "$o" -nt "$f" ] || clang++ -c -O2 -std=c++17 -fobjc-arc -arch arm64 -mmacosx-version-min=14.0 \
		-I$pw/metal -I$pw/metal/shaders "$f" -o "$o"
done

app=build/GMStream.app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cp Info.plist "$app/Contents/"
swiftc -O -swift-version 5 -target arm64-apple-macos14.0 -o "$app/Contents/MacOS/GMStream" \
	-import-objc-header Bridge.h -Xcc -I$pw/metal \
	Sources/*.swift build/pw-obj/*.o -lc++ -framework Metal -framework IOSurface
# Ad-hoc signed, but with a designated requirement naming only the bundle
# identifier: macOS's privacy permissions (Local Network, Bluetooth) then
# survive rebuilds instead of following each build's code hash.
codesign --force --sign - -r='designated => identifier "io.github.timshen91.gmstream"' "$app"
echo "built $app"
