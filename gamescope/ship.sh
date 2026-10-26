#!/bin/sh
# Build our patched gamescope on the build box (gamescope-build container,
# SteamOS's own package set) and copy the binary to the device as
# ~/gmstream/bin/gamescope. The Game Mode session picks it up on its next
# start. Hosts come from ../local.env (see local.env.example).
#
# First time on the build box:
#   podman build -t gamescope-build ~/gmstream/gamescope
set -e
cd "$(dirname "$0")"
. ../local.env
tar cf - Containerfile build.sh *.patch | build_host 'mkdir -p ~/gmstream/gamescope && tar xf - -C ~/gmstream/gamescope'
build_host 'podman run --rm -v ~/gmstream/gamescope:/work gamescope-build sh /work/build.sh'
build_host 'cat ~/gmstream/gamescope/out/gamescope' | device 'mkdir -p ~/gmstream/bin && cat > ~/gmstream/bin/gamescope.new && chmod +x ~/gmstream/bin/gamescope.new && mv ~/gmstream/bin/gamescope.new ~/gmstream/bin/gamescope && ls -la ~/gmstream/bin/gamescope'
