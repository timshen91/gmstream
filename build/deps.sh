#!/bin/sh
# Builds the static C libraries gmstream links into its single binary, into
# /opt/gm. Only the GPU and audio stacks (EGL, GLESv2, GBM, the VA driver,
# libpipewire) and glibc are left to the host: they have to match its drivers.
set -eux
P=/opt/gm
export PKG_CONFIG_PATH=$P/lib/pkgconfig:$P/share/pkgconfig
cd /tmp

fetch() { git -c advice.detachedHead=false clone -q --depth 1 --branch "$2" "$1" "$3"; }
mesonb() {
	dir=$1; shift
	meson setup "$dir/b" "$dir" --prefix=$P --libdir=lib --buildtype=release \
		--default-library=static -Db_staticpic=true "$@"
	ninja -C "$dir/b" install
	rm -rf "$dir"
}

fetch https://gitlab.freedesktop.org/wayland/wayland.git 1.26.0 wayland
mesonb wayland -Ddocumentation=false -Dtests=false -Ddtd_validation=false

fetch https://gitlab.freedesktop.org/wayland/wayland-protocols.git 1.49 wayland-protocols
mesonb wayland-protocols -Dtests=false

fetch https://gitlab.freedesktop.org/mesa/drm.git libdrm-2.4.134 drm
mesonb drm -Dintel=disabled -Damdgpu=disabled -Dradeon=disabled \
	-Dnouveau=disabled -Dvmwgfx=disabled -Dfreedreno=disabled -Dvc4=disabled \
	-Detnaviv=disabled -Dexynos=disabled -Domap=disabled -Dtegra=disabled \
	-Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled \
	-Dtests=false -Dinstall-test-programs=false

fetch https://gitlab.freedesktop.org/pixman/pixman.git pixman-0.46.4 pixman
mesonb pixman -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled

fetch https://github.com/xkbcommon/libxkbcommon.git xkbcommon-1.13.2 xkbcommon
mesonb xkbcommon -Denable-x11=false -Denable-wayland=false -Denable-docs=false \
	-Denable-xkbregistry=false -Denable-tools=true -Denable-bash-completion=false \
	-Dxkb-config-root=/usr/share/X11/xkb

fetch https://gitlab.freedesktop.org/wlroots/wlroots.git 0.20.2 wlroots
mesonb wlroots -Dbackends= -Drenderers=gles2 -Dallocators=gbm \
	-Dsession=disabled -Dxwayland=disabled -Dcolor-management=disabled \
	-Dlibliftoff=disabled -Dxcb-errors=disabled -Dexamples=false

# libva looks for host drivers in each of these at runtime.
fetch https://github.com/intel/libva.git 2.24.1 libva
sed -i 's/= shared_library(/= library(/' libva/va/meson.build # it hardcodes shared
mesonb libva -Dwith_x11=no -Dwith_glx=no -Dwith_wayland=no -Dwith_win32=no \
	-Ddriverdir=/usr/lib/dri:/usr/lib64/dri:/usr/lib/x86_64-linux-gnu/dri

# The git tree lacks the DNN model data that release tarballs carry.
curl -sSfL https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz | tar xz
mv opus-1.6.1 opus
mesonb opus -Dtests=disabled -Ddocs=disabled -Dextra-programs=disabled

fetch https://github.com/FFmpeg/FFmpeg.git n8.1.2 ffmpeg
cd ffmpeg
# Mesa (radeonsi, 26.1) parses the packed H.264 headers FFmpeg passes and
# mishandles emulation-prevention bytes in them: depending on where fields
# fall (frame size, crop, frame rate) it misparses and spins forever in an
# Exp-Golomb read, holding the VA lock (seen at 640x328@30, 2928x1600@30).
# - The SPS's 32-bit VUI timing fields are the usual source of 00 00 0x
#   bytes; nothing needs them, so leave them out.
perl -0pi -e 's/sps->vui.timing_info_present_flag = 1;/sps->vui.timing_info_present_flag = 0;/; s/sps->vui.fixed_frame_rate_flag = 1;/sps->vui.fixed_frame_rate_flag = 0;/' \
	libavcodec/hw_base_encode_h264.c
grep -q 'timing_info_present_flag = 0;' libavcodec/hw_base_encode_h264.c
# Same for HEVC's VPS and VUI timing (Mesa parses packed HEVC headers alike).
perl -0pi -e 's/vps->vps_timing_info_present_flag = 1;/vps->vps_timing_info_present_flag = 0;/; s/vui->vui_timing_info_present_flag        = 1;/vui->vui_timing_info_present_flag        = 0;/' \
	libavcodec/hw_base_encode_h265.c
grep -q 'vps_timing_info_present_flag = 0;' libavcodec/hw_base_encode_h265.c
grep -q 'vui_timing_info_present_flag        = 0;' libavcodec/hw_base_encode_h265.c
./configure --prefix=$P --enable-static --disable-shared --enable-pic \
	--pkg-config-flags=--static --disable-autodetect --disable-everything \
	--disable-programs --disable-doc --disable-network --disable-avdevice \
	--disable-avformat --disable-swscale --disable-swresample --disable-avfilter \
	--enable-vaapi --enable-libdrm \
	--enable-encoder=h264_vaapi,hevc_vaapi,av1_vaapi
make -j"$(nproc)" install
cd .. && rm -rf ffmpeg

# A fixed US keymap, compiled now so the binary needs no xkb data at runtime.
$P/bin/xkbcli-compile-keymap --layout us > $P/share/gm-keymap.xkb ||
	$P/libexec/xkbcommon/xkbcli-compile-keymap --layout us > $P/share/gm-keymap.xkb
test -s $P/share/gm-keymap.xkb
