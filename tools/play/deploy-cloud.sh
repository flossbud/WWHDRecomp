#!/usr/bin/env bash
# deploy-cloud.sh HOST  - deploy.sh for the cloud agent: the same build into the owner's existing ~/wwhd-play, through
# the PC's locked receiver (~/bin/wwhd-cloud-deploy there, the only command its key may run: status, put FILE,
# routes, du). The game, the saves and Cemu's resources are already on the PC; this only replaces the program and
# the files deploy.sh rewrites. Ask the owner before every run (their rule); it refuses while the game is running.
# Env: WWHD_DEPLOY_KEY, the deploy key's path. Run it after tools/recomp/build.sh && src/build.sh on the worker.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
host=${1:?usage: deploy-cloud.sh HOST}
key=${WWHD_DEPLOY_KEY:?WWHD_DEPLOY_KEY: the deploy key}
r() { ssh -i "$key" -o IdentitiesOnly=yes -o BatchMode=yes "$host" "$@"; }
[ "$(r status)" = idle ] || { echo "deploy-cloud: the game is running on the PC: not deploying" >&2; exit 3; }
w=$here/../worker/w
ref=$here/../reference
# wwhd-play, the real-time build (t-codegen; src/CMakeLists.txt), under the name play.sh runs; wwhd-null (every block's
# cycles counted, what the checks run) only when the build made no wwhd-play (WWHD_PLAY_BUILD=0)
if "$w" test -x build/wwhd/wwhd-play; then bin=wwhd-play; echo "deploying wwhd-play (the real-time build)"
else bin=wwhd-null; echo "warning: no build/wwhd/wwhd-play (WWHD_PLAY_BUILD=0?): deploying wwhd-null, the counting build" >&2; fi
"$w" bash -c "mkdir -p build/play && llvm-objcopy --strip-debug build/wwhd/$bin build/play/wwhd-null.new && mv build/play/wwhd-null.new build/play/wwhd-null"
"$w" cat build/play/wwhd-null | r put wwhd-null
r put portable/settings.xml < "$ref/settings.xml"
r put portable/gameProfiles/0005000010143500.ini < "$ref/0005000010143500.ini"
r put portable/controllerProfiles/controller0.xml < "$ref/controller0.xml"
tar -c -C "$ref/routes" . | r routes
list=$here/../../config/US_v0/shader_list.txt.gz
[ -f "$list" ] && r put cemu/wwhd/shader_list.txt.gz < "$list"
# the pause menu's assets (t-pausemenu: the design's fonts, images and copy), file by file next to the program
( cd "$here/../.." && find assets/pause_menu -type f ) | while read -r f; do r put "$f" < "$here/../../$f"; done
r put play.sh < "$here/play.sh"
echo "deployed to $host:wwhd-play ($(r du))"
