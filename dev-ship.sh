#!/bin/sh
# Sync sources to the build box, build there in the gmstream-dev container
# (image: build/Containerfile), and copy only the binary to the device as
# ~/gmstream/gmstream. Hosts come from local.env (see local.env.example).
#
# First time on the build box:
#   podman build -t gmstream-build ~/gmstream/build
#   podman run -d --name gmstream-dev -v ~/gmstream:/src \
#     -v gmstream-cargo:/root/.cargo/registry localhost/gmstream-build sleep infinity
set -e
cd "$(dirname "$0")"
. ./local.env
# Plain tar keeps source mtimes, so cargo only rebuilds what changed.
tar cf - Cargo.toml Cargo.lock build.rs src core web build examples | build_host 'mkdir -p ~/gmstream && tar xf - -C ~/gmstream'
build_host 'podman exec gmstream-dev cargo build --release --color=always 2>&1 | grep -v "^\s*Compiling\|^\s*Downloaded\|^\s*Downloading"; podman exec gmstream-dev test -x /src/target/release/gmstream'
build_host 'cat ~/gmstream/target/release/gmstream' | device 'mkdir -p ~/gmstream && cat > ~/gmstream/gmstream.new && chmod +x ~/gmstream/gmstream.new && mv ~/gmstream/gmstream.new ~/gmstream/gmstream && ls -la ~/gmstream/gmstream'
# CAP_SYS_NICE lets the PyroWave encoder get a realtime-priority GPU queue
# (core/pyrowave.c); a rewritten file loses it.
device_sudo setcap cap_sys_nice=eip /home/deck/gmstream/gmstream
device 'getcap ~/gmstream/gmstream'
