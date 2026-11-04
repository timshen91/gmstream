# Sourced by mac/build.sh and ipad/build.sh: PyroWave's Metal decoder
# (github.com/Themaister/pyrowave, MIT) from the same commit as the host's
# encoder (build/pyrowave.sh).
PYROWAVE_COMMIT=79a3232

# pyrowave_checkout DIR: clones into DIR once, then checks out the commit.
pyrowave_checkout() {
	[ -d "$1" ] || git clone -q https://github.com/Themaister/pyrowave.git "$1"
	git -C "$1" -c advice.detachedHead=false checkout -q -f $PYROWAVE_COMMIT
	git -C "$1" clean -fdq # nothing of ours in it
}
