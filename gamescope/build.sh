#!/bin/sh
# Builds Valve's SteamOS gamescope package (the exact version the device runs)
# with our patches, and leaves the gamescope binary in out/. Runs inside the
# gamescope-build container (see Containerfile); /work is this directory.
#
#   podman run --rm -v ~/gmstream/gamescope:/work gamescope-build sh /work/build.sh
set -eu
VERSION=3.16.30.1-2
SRCPKG=https://steamdeck-packages.steamos.cloud/archlinux-mirror/sources/jupiter-3.9/gamescope-$VERSION.src.tar.gz

mkdir -p /work/cache /work/out
cd /work/cache
[ -f gamescope-$VERSION.src.tar.gz ] || curl -sSfLO "$SRCPKG"
rm -rf build && mkdir build && tar xzf gamescope-$VERSION.src.tar.gz -C build
cd build/gamescope
cp /work/*.patch .

# Valve's prepare() applies every *.patch listed in source=().
patches=$(ls /work/*.patch | xargs -n1 basename | sed "s/.*/\"&\"/" | tr '\n' ' ')
skips=$(ls /work/*.patch | sed "s/.*/'SKIP'/" | tr '\n' ' ')
sed -i "s/^\t)$/\t$patches)/" PKGBUILD
sed -i "s/^\(sha256sums=(.*\)/\1/" PKGBUILD
# Append one SKIP per patch to the last line of sha256sums.
awk -v skips="$skips" '
	/^sha256sums=\(/ { in_sums = 1 }
	in_sums && /\)$/ { sub(/\)$/, " " skips ")"); in_sums = 0 }
	{ print }' PKGBUILD > PKGBUILD.new && mv PKGBUILD.new PKGBUILD
grep -q "$(ls /work/*.patch | head -1 | xargs basename)" PKGBUILD

chown -R builder: /work/cache/build
su builder -c 'makepkg --nocheck --nodeps --noconfirm -f' > /work/out/makepkg.log 2>&1 || {
	tail -40 /work/out/makepkg.log
	exit 1
}
cp src/gamescope/build/src/gamescope /work/out/gamescope
echo "built: $(ls -la /work/out/gamescope)"
