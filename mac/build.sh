#!/bin/sh
# Builds GMStream.app (native macOS client) with the Command Line Tools'
# swiftc and clang; no Xcode project. Output: mac/build/GMStream.app, which
# is also installed to ~/Applications so it can be launched like any other
# app. Both copies carry the same designated requirement (the bundle id), so
# they share the Local Network permission.
# Sources: ../apple (shared with the iPad app) and Sources (the AppKit app).
#
#   ./build.sh && open build/GMStream.app --args steamdeck.local
set -e
cd "$(dirname "$0")"
mkdir -p build

. ../apple/pyrowave.sh
pw=build/pyrowave
pyrowave_checkout $pw
mkdir -p build/pw-obj
for f in $pw/metal/*.mm $pw/metal/*.cpp; do
	o=build/pw-obj/$(basename "$f").o
	[ "$o" -nt "$f" ] || clang++ -c -O2 -std=c++17 -fobjc-arc -arch arm64 -mmacosx-version-min=14.0 \
		-I$pw/metal -I$pw/metal/shaders "$f" -o "$o"
done
# Riptide's Metal decoder (../riptide, our codec), beside PyroWave's.
rt=../riptide
mkdir -p build/rt-obj
for f in $rt/metal/*.mm $rt/metal/*.cpp $rt/riptide_inter.cpp; do
	o=build/rt-obj/$(basename "$f").o
	[ "$o" -nt "$f" ] || clang++ -c -O2 -std=c++17 -fobjc-arc -arch arm64 -mmacosx-version-min=14.0 \
		-I$rt -I$rt/metal -I$rt/metal/shaders "$f" -o "$o"
done

app=build/GMStream.app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cp Info.plist "$app/Contents/"
swiftc -O -swift-version 5 -target arm64-apple-macos14.0 -o "$app/Contents/MacOS/GMStream" \
	-import-objc-header ../apple/Bridge.h -Xcc -I$pw/metal -Xcc -I$rt/metal \
	../apple/*.swift Sources/*.swift build/pw-obj/*.o build/rt-obj/*.o -lc++ -framework Metal -framework IOSurface
# Ad-hoc signed, but with a designated requirement naming only the bundle
# identifier: macOS's privacy permissions (Local Network, Bluetooth) then
# survive rebuilds instead of following each build's code hash.
codesign --force --sign - -r='designated => identifier "io.github.timshen91.gmstream"' "$app"
echo "built $app"

# Install it, so it is in Spotlight and the Dock like any other app. ditto
# keeps the bundle's signature and extended attributes; a plain cp does not.
installed=$HOME/Applications/GMStream.app
mkdir -p "$HOME/Applications"
rm -rf "$installed"
ditto "$app" "$installed"
echo "installed $installed"
