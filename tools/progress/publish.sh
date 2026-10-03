#!/usr/bin/env bash
# The progress page (tools/progress/README.md), served from the worker host on the tailnet.
#   publish.sh              collect the data (collect.py) and publish it with the page
#   publish.sh now TEXT     set this session's "working on" line (one small write: cheap to call often);
#                           the session is the name in .session (gitignored), "main" without one
#   publish.sh claim ID [NOTE] / done ID [NOTE] / release ID
#                           the work queue (plan.json "queue"): claim an item before starting it, so
#                           parallel sessions never take the same one; done when it's converted and
#                           committed; release to hand it back
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
session=$(cat "$root/.session" 2>/dev/null || echo main)

# claims.json on the host, changed under a lock: {id: {session, state, note, time}}
claims() {
	ssh $host "mkdir -p $dir && flock $dir/.claims.lock python3 - $dir/claims.json $(printf %q "$1") $(printf %q "$2") $(printf %q "$session") $(printf %q "$(json_str "${3:-}")") $(date +%s)" <<'PY'
import json, os, sys
path, op, item, session, note, t = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], json.loads(sys.argv[5]), int(sys.argv[6])
c = json.load(open(path)) if os.path.exists(path) else {}
cur = c.get(item)
if op == "claim":
    if cur and cur["session"] != session and cur["state"] == "working":
        sys.exit(f"{item} is claimed by {cur['session']} ({cur.get('note', '')})")
    c[item] = {"session": session, "state": "working", "note": note, "time": t}
elif op == "done":
    c[item] = {"session": session, "state": "done", "note": note, "time": t}
elif op == "release":
    c.pop(item, None)
json.dump(c, open(path + ".tmp", "w"), indent=1); os.replace(path + ".tmp", path)
print(f"{item}: {op} ({session})")
PY
}

case "${1:-}" in
	now)
		t=$(json_str "${2:-}")
		echo "{\"session\":\"$session\",\"text\":$t,\"time\":$(date +%s)}" | ssh $host "mkdir -p $dir/now && cat > $dir/now/$session.json.tmp && mv $dir/now/$session.json.tmp $dir/now/$session.json &&
			cd $dir/now && python3 -c 'import json,glob; json.dump([json.load(open(f)) for f in sorted(glob.glob(\"*.json\"))], open(\"../sessions.json.tmp\",\"w\"))' && mv ../sessions.json.tmp ../sessions.json"
		;;
	claim|done|release)
		claims "$1" "${2:?item id}" "${3:-}"
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
	*) echo "usage: publish.sh [now TEXT | claim|done|release ID [NOTE] | shot PPM CAPTION | serve]" >&2; exit 2 ;;
esac
