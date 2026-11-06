#!/bin/sh
# Builds the Android client (build/gmstream.apk) with the SDK's command-line
# tools only: the NDK's clang for the native core, javac + d8 for the Java
# shell, aapt2 to package, apksigner with a local debug key. No Gradle.
#
# Needs ANDROID_HOME (default ~/Library/Android/sdk) with build-tools 36,
# platforms;android-36 and an NDK (r27+), and a JDK (JAVA_HOME, or javac on
# the PATH). Riptide (riptide/ and its Granite checkout) is built in when cmake
# and ninja are there (riptide.sh); GM_RIPTIDE=0 leaves it out.
set -e
cd "$(dirname "$0")"
SDK=${ANDROID_HOME:-$HOME/Library/Android/sdk}
BT=$SDK/build-tools/$(ls "$SDK/build-tools" | sort -V | tail -1)
PLATFORM=$SDK/platforms/android-36/android.jar
NDK=${ANDROID_NDK:-$SDK/ndk/$(ls "$SDK/ndk" | sort -V | tail -1)}
TC=$NDK/toolchains/llvm/prebuilt/$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)/bin
API=33
if [ -z "$JAVA_HOME" ] && [ -d /opt/homebrew/opt/openjdk ]; then JAVA_HOME=/opt/homebrew/opt/openjdk; fi
[ -n "$JAVA_HOME" ] && export PATH="$JAVA_HOME/bin:$PATH"
OUT=build
mkdir -p $OUT/obj $OUT/classes $OUT/apk/lib/arm64-v8a

# ---- shaders (the .inc files are checked in; rebuilt when glslc is there) ----
GLSLC=$NDK/shader-tools/$(ls "$NDK/shader-tools" | head -1)/glslc
if [ -x "$GLSLC" ]; then
	for s in jni/shaders/*.vert jni/shaders/*.frag jni/shaders/*.comp; do
		inc=$(echo "$s" | sed 's/\.\([a-z]*\)$/_\1.inc/')
		[ "$inc" -nt "$s" ] || "$GLSLC" -O --target-env=vulkan1.1 -mfmt=num "$s" -o "$inc"
	done
fi

# ---- native ----
CC="$TC/clang --target=aarch64-linux-android$API"
CXX="$TC/clang++ --target=aarch64-linux-android$API"
CFLAGS="-O2 -g -fPIC -fvisibility=hidden -Wall"
objs=""
for c in monocypher monocypher-ed25519; do
	$CC $CFLAGS -c jni/third_party/monocypher/$c.c -o $OUT/obj/$c.o
	objs="$objs $OUT/obj/$c.o"
done
srcs="crypto conn phase stats panel presenter video audio session"
extra_libs=""
if [ -z "$GM_RIPTIDE" ] && command -v cmake >/dev/null && command -v ninja >/dev/null; then GM_RIPTIDE=1; fi
if [ "$GM_RIPTIDE" = 1 ]; then
	sh ./riptide.sh
	srcs="$srcs riptide_client gpuwarm gpu compositor"
	R=$OUT/riptide G=../riptide/Granite
	extra_libs="$R/libriptide.a $R/Granite/vulkan/libgranite-vulkan.a $R/Granite/math/libgranite-math.a $R/Granite/util/libgranite-util.a $R/Granite/third_party/libgranite-volk.a"
	RIPTIDE_FLAGS="-DGM_RIPTIDE=1 -DVK_USE_PLATFORM_ANDROID_KHR -DANDROID -DNDEBUG -DGRANITE_SHIPPING -DGRANITE_RENDERDOC_CAPTURE -DRIPTIDE_PRECISION=1 -I$G/application/global -I$G/third_party/renderdoc -I../riptide -I$G/vulkan -I$G/util -I$G/math -I$G/third_party/volk -I$G/third_party/khronos/vulkan-headers/include"
else
	srcs="$srcs riptide_stub compositor_stub"
fi
for s in $srcs; do
	$CXX $CFLAGS $RIPTIDE_FLAGS -Ijni -std=c++17 -c jni/$s.cpp -o $OUT/obj/$s.o
	objs="$objs $OUT/obj/$s.o"
done
$CXX -shared -o $OUT/apk/lib/arm64-v8a/libgmstream.so $objs $extra_libs \
	-static-libstdc++ -landroid -llog -lmediandk -laaudio -Wl,--gc-sections -Wl,-z,max-page-size=16384

# ---- Java ----
rm -rf $OUT/classes/*
javac --release 17 -Xlint:-options -cp "$PLATFORM" -d $OUT/classes $(find java -name '*.java')
"$BT/d8" --min-api $API --lib "$PLATFORM" --output $OUT/apk $(find $OUT/classes -name '*.class')

# ---- package and sign ----
"$BT/aapt2" link -o $OUT/unsigned.apk --manifest AndroidManifest.xml -I "$PLATFORM" --min-sdk-version $API --target-sdk-version 35
(cd $OUT/apk && zip -q -r ../unsigned.apk classes.dex lib)
"$BT/zipalign" -f -p 16 $OUT/unsigned.apk $OUT/aligned.apk
KS=${GM_KEYSTORE:-$HOME/.android/gmstream-debug.keystore}
if [ ! -f "$KS" ]; then
	mkdir -p "$(dirname "$KS")"
	keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias debug \
		-keyalg RSA -keysize 2048 -validity 10000 -dname "CN=gmstream debug" >/dev/null
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out $OUT/gmstream.apk $OUT/aligned.apk
echo "built $OUT/gmstream.apk"
