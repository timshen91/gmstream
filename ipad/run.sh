#!/bin/sh
# Installs the built app on the connected iPad and launches it with ARGS (the
# macOS client's options, e.g. --mock or a host), printing its output until
# it exits or Ctrl+C. Environment: GM_* variables are passed to the app;
# GM_IPAD picks the device (default: the first connected iPad).
set -e
cd "$(dirname "$0")"
app=build/dd/Build/Products/Release-iphoneos/GMStream.app
device=${GM_IPAD:-$(xcrun devicectl list devices --json-output /dev/stdout 2>/dev/null | python3 -c '
import json, sys
for d in json.load(sys.stdin)["result"]["devices"]:
    if d["hardwareProperties"].get("deviceType") == "iPad" and d["connectionProperties"].get("tunnelState") != "unavailable":
        print(d["identifier"]); break')}
[ -n "$device" ] || { echo "no iPad connected" >&2; exit 1; }
xcrun devicectl device install app --device "$device" "$app" >/dev/null
env=$(python3 -c 'import json, os; print(json.dumps({k: v for k, v in os.environ.items() if k.startswith("GM_") and k != "GM_IPAD" and k != "GM_TEAM"}))')
[ "$env" = "{}" ] && set -- io.github.timshen91.gmstream -- "$@" || set -- -e "$env" io.github.timshen91.gmstream -- "$@"
exec xcrun devicectl device process launch --device "$device" --terminate-existing --console "$@"
