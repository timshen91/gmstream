#!/bin/sh
# Builds and runs the unit tests (Tests/*.swift) with Xcode's XCTest: an
# .xctest bundle of the app's sources (../apple, and Sources but main.swift,
# the app itself, and Settings.swift, its settings window) and the tests, compiled together
# so the tests see internal types. Needs Xcode (not only the Command Line
# Tools) and the PyroWave objects from build.sh.
#
#   ./test.sh                   all tests
#   ./test.sh PhaseLockTests    one class (or Class/testMethod)
set -e
cd "$(dirname "$0")"
pw=build/pyrowave
[ -d build/pw-obj ] && ls build/pw-obj/*.o build/rt-obj/*.o >/dev/null 2>&1 || ./build.sh

platform=$(xcrun --show-sdk-platform-path)
bundle=build/GMStreamTests.xctest
rm -rf "$bundle"
mkdir -p "$bundle/Contents/MacOS"
cat > "$bundle/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>CFBundleExecutable</key><string>GMStreamTests</string>
	<key>CFBundleIdentifier</key><string>io.github.timshen91.gmstream.tests</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
</dict></plist>
EOF
sources="../apple/*.swift $(ls Sources/*.swift | grep -v -e '/main.swift$' -e '/Settings.swift$')"
xcrun swiftc -Onone -swift-version 5 -target arm64-apple-macos14.0 -sdk "$(xcrun --show-sdk-path)" \
	-parse-as-library -import-objc-header ../apple/Bridge.h -Xcc -I$pw/metal -Xcc -I../riptide/metal \
	-F "$platform/Developer/Library/Frameworks" -I "$platform/Developer/usr/lib" -L "$platform/Developer/usr/lib" \
	-Xlinker -bundle \
	-Xlinker -rpath -Xlinker "$platform/Developer/Library/Frameworks" -Xlinker -rpath -Xlinker "$platform/Developer/usr/lib" \
	$sources Tests/*.swift build/pw-obj/*.o build/rt-obj/*.o -lc++ -framework Metal -framework IOSurface -framework XCTest \
	-o "$bundle/Contents/MacOS/GMStreamTests"
xcrun xctest ${1:+-XCTest "$1"} "$bundle"
