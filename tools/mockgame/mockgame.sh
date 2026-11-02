#!/bin/bash
# The non-Steam shortcut's target: mockgame.exe under Proton, in its own prefix.
d=$(dirname "$(readlink -f "$0")")
steam=$HOME/.local/share/Steam
export STEAM_COMPAT_DATA_PATH=$d/compat STEAM_COMPAT_CLIENT_INSTALL_PATH=$steam
mkdir -p "$STEAM_COMPAT_DATA_PATH"
[ -f "$d/env" ] && . "$d/env" # settings under test
exec "$steam/steamapps/common/SteamLinuxRuntime_4/_v2-entry-point" --verb=waitforexitandrun -- \
	"$steam/steamapps/common/Proton 11.0/proton" waitforexitandrun "$d/mockgame.exe"
