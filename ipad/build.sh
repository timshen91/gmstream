#!/bin/sh
# Builds GMStream.app for iPad (Release) with Xcode: sources ../apple (shared
# with the macOS app) and Sources (the UIKit app). Output:
# ipad/build/dd/Build/Products/Release-iphoneos/GMStream.app
#
#   ./build.sh && ./run.sh [ARGS...]
#
# Signing: the Apple Development team in GM_TEAM (a free Apple ID's
# profiles last 7 days: build again after that).
set -e
cd "$(dirname "$0")"
mkdir -p build
. ../apple/pyrowave.sh
pyrowave_checkout build/pyrowave
xcodegen generate --quiet
xcodebuild -project GMStream.xcodeproj -scheme GMStream -configuration Release \
	-destination 'generic/platform=iOS' -derivedDataPath build/dd -allowProvisioningUpdates \
	DEVELOPMENT_TEAM="${GM_TEAM:-XXXXXXXXXX}" -quiet build
echo "built build/dd/Build/Products/Release-iphoneos/GMStream.app"
