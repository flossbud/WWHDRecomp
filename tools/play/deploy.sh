#!/usr/bin/env bash
# deploy.sh HOST [DIR]  - put the current build on a desktop machine, to play it there with
# DIR/play.sh (tools/play/play.sh). Run on the editing machine after src/build.sh on the worker: files
# stream from the worker to HOST (an ssh destination) and nothing is stored here. DIR is relative
# to HOST's home (default wwhd-play). The game (.wua) and the worker's test saves are copied once.
# Everything it copies is generated output or game data: it lives on HOST only, never in git.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
host=${1:?usage: deploy.sh HOST [DIR]}; dir=${2:-wwhd-play}
w=$here/../worker/w
on() { ssh -o BatchMode=yes "$host" "cd && $*"; }
on "mkdir -p $dir/game $dir/saves $dir/routes $dir/cemu $dir/portable/gameProfiles $dir/portable/controllerProfiles"
# the program without debug info (made on the worker), replaced by rename so a running copy is untouched
"$w" bash -c 'mkdir -p build/play && llvm-objcopy --strip-debug build/wwhd/wwhd-null build/play/wwhd-null.new && mv build/play/wwhd-null.new build/play/wwhd-null'
"$w" cat build/play/wwhd-null | on "cat > $dir/wwhd-null.new && chmod +x $dir/wwhd-null.new && mv -f $dir/wwhd-null.new $dir/wwhd-null"
on "test -d $dir/cemu/resources" || "$w" tar -c -C /wwhd/opt/cemu-src/bin resources gameProfiles | on "tar -x -C $dir/cemu"
on "test -f $dir/game/wwhd.wua" || { echo "copying the game (1.5 GB)"; "$w" bash -c 'cat /wwhd/data/rom/*.wua' | on "cat > $dir/game/wwhd.wua.part && mv $dir/game/wwhd.wua.part $dir/game/wwhd.wua"; }
on "test -d $dir/saves/wwhd_100" || "$w" tar -c -C /wwhd/data/saves wwhd_100 | on "tar -x -C $dir/saves"
ref=$here/../reference
on "cat > $dir/portable/settings.xml" < "$ref/settings.xml"
on "cat > $dir/portable/gameProfiles/0005000010143500.ini" < "$ref/0005000010143500.ini"
on "cat > $dir/portable/controllerProfiles/controller0.xml" < "$ref/controller0.xml"
tar -c -C "$ref/routes" . | on "tar -x -C $dir/routes"
# the shader list (D20: what to prepare on a first start; no game content), where the game reads it
list=$here/../../config/US_v0/shader_list.txt
[ -f "$list" ] && on "mkdir -p $dir/cemu/wwhd && cat > $dir/cemu/wwhd/shader_list.txt" < "$list"
on "cat > $dir/play.sh && chmod +x $dir/play.sh" < "$here/play.sh"
echo "deployed to $host:$dir ($(on "du -sh $dir | cut -f1"))"
