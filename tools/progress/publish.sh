#!/usr/bin/env bash
# The progress page (tools/progress/README.md), served from the worker host on the tailnet.
#   publish.sh              collect the data (collect.py) and publish it with the page
#   publish.sh now TEXT     set the "working on" line (one small write: cheap to call often)
#   publish.sh shot PPM CAPTION
#                           add a capture: PPM is a path on the worker (/wwhd/...); it becomes a JPEG
#                           on the worker, never on the editing machine or in git (captures are game data)
#   publish.sh serve        start the server if it isn't running (http://TAILNET_IP:8765)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
host=worker
dir=/wwhd/data/progress
port=8765

json_str() { python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$1"; }

case "${1:-}" in
	now)
		t=$(json_str "${2:-}")
		echo "{\"text\":$t,\"time\":$(date +%s)}" | ssh $host "mkdir -p $dir && cat > $dir/now.json.tmp && mv $dir/now.json.tmp $dir/now.json"
		;;
	shot)
		src=$2; cap=$(json_str "${3:-}"); name=shot-$(date +%Y%m%d-%H%M%S).jpg
		ssh $host "mkdir -p $dir/shots && docker exec wwhd-worker convert '$src' -resize 960x540 -quality 82 /wwhd/data/progress/shots/$name"
		ssh $host "python3 - $dir/shots.json $name $(printf %q "$cap") $(date +%s)" <<'PY'
import json, os, sys
path, name, cap, t = sys.argv[1], sys.argv[2], json.loads(sys.argv[3]), int(sys.argv[4])
shots = json.load(open(path)) if os.path.exists(path) else []
shots.insert(0, {"file": "shots/" + name, "caption": cap, "time": t})
for old in shots[48:]:
    try: os.remove(os.path.join(os.path.dirname(path), old["file"]))
    except OSError: pass
json.dump(shots[:48], open(path + ".tmp", "w")); os.replace(path + ".tmp", path)
PY
		;;
	serve)
		ssh $host "bash -s" <<EOF
mkdir -p $dir
if [ -f $dir/.pid ] && kill -0 \$(cat $dir/.pid) 2>/dev/null; then echo "running (pid \$(cat $dir/.pid))"; exit 0; fi
ip=\$(tailscale ip -4 | head -1)
cd $dir && setsid -f sh -c 'echo \$\$ > .pid; exec python3 -m http.server $port --bind '\$ip > $dir/.server.log 2>&1 < /dev/null
sleep 1; echo "serving http://\$ip:$port (pid \$(cat $dir/.pid))"
EOF
		;;
	"")
		names=$(mktemp)
		ssh $host "docker exec wwhd-worker cat /wwhd/data/ghidra-out/actor_names.tsv" > "$names" || true
		python3 "$root/tools/progress/collect.py" "$names" | ssh $host "mkdir -p $dir && cat > $dir/progress.json.tmp && mv $dir/progress.json.tmp $dir/progress.json"
		rm -f "$names"
		ssh $host "cat > $dir/index.html" < "$here/index.html"
		;;
	*) echo "usage: publish.sh [now TEXT | shot PPM CAPTION | serve]" >&2; exit 2 ;;
esac
