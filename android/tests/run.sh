#!/bin/sh
# Host-side tests of the portable core (crypto, protocol parsing): builds with
# the build machine's own compiler and runs them.
set -e
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/gmstream-android-tests
mkdir -p "$out"
for c in monocypher monocypher-ed25519; do
	cc -O2 -c jni/third_party/monocypher/$c.c -o "$out/$c.o"
done
c++ -std=c++17 -O1 -Wall -o "$out/crypto_test" tests/crypto_test.cpp jni/crypto.cpp "$out"/monocypher*.o
"$out/crypto_test"
c++ -std=c++17 -O1 -Wall -pthread -o "$out/probe" tests/probe.cpp jni/conn.cpp jni/crypto.cpp "$out"/monocypher*.o
echo "built $out/probe"
