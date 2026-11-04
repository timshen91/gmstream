# Sourced by mac/build.sh and ipad/build.sh: PyroWave's Metal decoder
# (github.com/Themaister/pyrowave, MIT) from the same commit as the host's
# encoder (build/pyrowave.sh), with the same patch on top: inter frames
# (build/pyrowave-inter.patch; bitstream/inter.md in the patched tree).
PYROWAVE_COMMIT=79a3232
# The build scripts source this from mac/ or ipad/.
PYROWAVE_PATCH=$(cd .. && pwd)/build/pyrowave-inter.patch

# pyrowave_checkout DIR: clones into DIR once, then checks out the commit and
# applies the patch (again only when either changes).
pyrowave_checkout() {
	[ -d "$1" ] || git clone -q https://github.com/Themaister/pyrowave.git "$1"
	want="$PYROWAVE_COMMIT $(shasum "$PYROWAVE_PATCH" | cut -c1-16)"
	[ "$(cat "$1/.gmstream-patch" 2>/dev/null)" = "$want" ] && return
	git -C "$1" -c advice.detachedHead=false checkout -q -f $PYROWAVE_COMMIT
	git -C "$1" clean -fdq # the files the last patch added
	git -C "$1" apply "$PYROWAVE_PATCH"
	echo "$want" > "$1/.gmstream-patch"
}
