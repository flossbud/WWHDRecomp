#!/usr/bin/env bash
# The progress page (tools/progress/README.md), served from the worker host on the tailnet.
#   publish.sh              collect the data (collect.py) and publish it with the page
#   publish.sh now TEXT     set this session's "working on" line (one small write: cheap to call often);
#                           the session is the name in .session (gitignored), "main" without one
#   publish.sh retire [SESSION]
#                           take a session's line off the page when it ends (this session without a name)
#   publish.sh claim ID [NOTE] / done ID [NOTE] / release ID
#                           the work queue (plan.json "queue"): claim an item before starting it, so
#                           parallel sessions never take the same one; done when it's converted and
#                           committed; release to hand it back
#   publish.sh step ID DONE TOTAL
#                           an item's progress for its bar on the page (e.g. step ui30 3 5: three of five
#                           parts done); an item with "ids" in plan.json fills its bar from the actor types
#                           converted without it
#   publish.sh bug add TITLE [DETAILS]       record a bug the owner reported (prints its id, B1...)
#   publish.sh bug start|ready|fixed|verified|wontfix|reopen ID [NOTE]
#                           its state: open -> working (by this session) -> ready (the fix is in ww-4,
#                           waiting to be deployed) -> fixed (deployed, waiting for the owner's
#                           retest) -> verified (the owner
#                           confirmed); wontfix with the reason; reopen if the retest fails
#   publish.sh bug note ID TEXT              add a finding to it
#   publish.sh bug needs ID TEXT             what an open bug waits on from the owner, shown as "Needs TEXT"
#                                            (e.g. "your F9 recording", "a yes/no: shooting stars?")
#   publish.sh bug short ID TEXT             a one-line summary for its card (the full note shows on a tap)
#   publish.sh bug list                      the bugs, newest first (also on the page; "you:" marks the owner's
#                                            latest note, from the testing page: verified, still broken, a note)
#   publish.sh bug show ID                   one bug in full: its details, every note, the image links
#   publish.sh shot PPM CAPTION [PLACE]
#                           PLACE: the shot's heading on the page (e.g. "Wind Temple, the fans' room"); without
#                           it the page takes the caption's text before its first ": "
#                           add a capture: PPM is a path on the worker (/wwhd/...: the desktop's while
#                           it's lent, else the worker's; WWHD_ON= forces one); it becomes a JPEG
#                           on the worker, never on the editing machine or in git (captures are game data); none is ever deleted
#   publish.sh usage        the Claude account's usage meters (5 h, week, per model) from the editing machine's
#                           /api/usage/claude ($WWHD_USAGE_URL, default http://127.0.0.1:7690) to usage.json:
#                           percentages and reset times only, the login token never leaves the editing machine. A
#                           crontab entry on the editing machine (tagged wwhd-usage) runs it every 5 minutes
#   publish.sh notes                         the owner's testing notes (written on the page), newest first
#   publish.sh notes reply ID TEXT           answer one (shown under it on the page, as this session)
#   publish.sh notes done|reopen ID [TEXT]   mark it handled (with an optional reply) or open again
#   publish.sh serve        install server.py and start it if it isn't running, or restart it if server.py
#                           changed (http://TAILNET_IP:8765; it serves the page and stores the notes)
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
elif op == "step":
    done, total = (int(x) for x in note.split("/"))
    if not 0 <= done <= total or not total:
        sys.exit(f"step: {done} of {total}?")
    c.setdefault(item, {"session": session, "state": "working", "note": "", "time": t})["progress"] = [done, total]
    c[item]["time"] = t
json.dump(c, open(path + ".tmp", "w"), indent=1); os.replace(path + ".tmp", path)
print(f"{item}: {op} ({session})")
PY
}

# bugs.json on the host, changed under the claims' lock: [{id, title, details, state, session, notes, reported, time}]
bugs() {
	ssh $host "mkdir -p $dir && flock $dir/.claims.lock python3 - $dir/bugs.json $(printf %q "$1") $(printf %q "$(json_str "${2:-}")") $(printf %q "$(json_str "${3:-}")") $(printf %q "$session") $(date +%s)" <<'PY'
import json, os, sys, time
path, op, a, b, session, t = sys.argv[1], sys.argv[2], json.loads(sys.argv[3]), json.loads(sys.argv[4]), sys.argv[5], int(sys.argv[6])
bugs = json.load(open(path)) if os.path.exists(path) else []
def find(i):
    for x in bugs:
        if x["id"].lower() == i.lower():
            return x
    sys.exit(f"no bug {i}")
if op == "add":
    n = max([int(x["id"][1:]) for x in bugs] or [0]) + 1
    bugs.append({"id": f"B{n}", "title": a, "details": b, "state": "open", "session": "", "notes": [], "reported": t, "time": t})
    print(f"B{n}: {a}")
elif op == "list":
    for x in sorted(bugs, key=lambda x: -x["time"]):
        last = x["notes"][-1] if x["notes"] else None
        who = "you: " if last and last.get("session") == "owner" else ""
        pics = sum(len(n.get("images", [])) for n in x["notes"]) + len(x.get("images", []))
        print(f"{x['id']:5} {x['state']:9} {x['session'] or '-':7} {x['title']}" + (f"  [{who}{last['text']}]" if last else "")
              + (f"  ({pics} image{'s' * (pics > 1)}: bug show {x['id']})" if pics else ""))
    sys.exit(0)
elif op == "show":
    x = find(a)
    url = lambda f: "http://WORKER_ADDR:8765/" + f
    print(f"{x['id']} {x['state']} {x['session'] or '-'}{' (reported by the owner)' if x.get('by') == 'owner' else ''}: {x['title']}")
    if x["details"]:
        print("  " + x["details"])
    for f in x.get("images", []):
        print("  image: " + url(f))
    for n in x["notes"]:
        print(f"  {'owner' if n.get('session') == 'owner' else n.get('session') or '?'}: {n['text']}")
        for f in n.get("images", []):
            print("    image: " + url(f))
    sys.exit(0)
else:
    x = find(a)
    if op == "note":
        x["notes"].append({"text": b, "session": session, "time": t})
    elif op in ("needs", "short"):
        x[op] = b
    else:
        state = {"start": "working", "ready": "ready", "fixed": "fixed", "verified": "verified", "wontfix": "wontfix", "reopen": "open"}.get(op)
        if not state:
            sys.exit(f"unknown bug command {op}")
        x["state"] = state
        if op == "start":
            x["session"] = session
        if b:
            x["notes"].append({"text": f"{state}: {b}", "session": session, "time": t})
    if op not in ("needs", "short"):   # its card's wording, not news about the bug
        x["time"] = t
    print(f"{x['id']}: {x['state']} ({session})")
json.dump(bugs, open(path + ".tmp", "w"), indent=1); os.replace(path + ".tmp", path)
PY
}

case "${1:-}" in
	now)
		t=$(json_str "${2:-}")
		echo "{\"session\":\"$session\",\"text\":$t,\"time\":$(date +%s)}" | ssh $host "mkdir -p $dir/now && cat > $dir/now/$session.json.tmp && mv $dir/now/$session.json.tmp $dir/now/$session.json &&
			cd $dir/now && python3 -c 'import json,glob; json.dump([json.load(open(f)) for f in sorted(glob.glob(\"*.json\"))], open(\"../sessions.json.tmp\",\"w\"))' && mv ../sessions.json.tmp ../sessions.json"
		;;
	retire)
		who=${2:-$session}
		ssh $host "rm -f $dir/now/$(printf %q "$who").json &&
			cd $dir/now && python3 -c 'import json,glob; json.dump([json.load(open(f)) for f in sorted(glob.glob(\"*.json\"))], open(\"../sessions.json.tmp\",\"w\"))' && mv ../sessions.json.tmp ../sessions.json"
		echo "$who: retired"
		;;
	bug)
		bugs "${2:?bug add|start|ready|fixed|verified|wontfix|reopen|note|needs|short|list|show}" "${3:-}" "${4:-}"
		;;
	claim|done|release)
		claims "$1" "${2:?item id}" "${3:-}"
		;;
	step)
		claims step "${2:?item id}" "${3:?done}/${4:?total}"
		;;
	shot)
		src=$2; cap=$(json_str "${3:-}"); place=$(json_str "${4:-}"); name=shot-$(date +%Y%m%d-%H%M%S-%3N).jpg   # ms: two shots in one second
		# would share a file (one picture under both captions, deleted with the older one)
		source "$root/tools/worker/target.sh"                # the worker the capture was made on (as for job)
		if [ "$WWHD_ON" = desktop ]; then
			ssh -o BatchMode=yes owner@DESKTOP_ADDR "podman exec wwhd-worker convert '$src' -resize 960x540 -quality 82 jpg:-" |
				ssh $host "mkdir -p $dir/shots && cat > $dir/shots/$name"
		else
			ssh $host "mkdir -p $dir/shots && docker exec wwhd-worker convert '$src' -resize 960x540 -quality 82 /wwhd/data/progress/shots/$name"
		fi
		# every shot is kept: shots.json holds the newest 24, older ones move to shots-archive.json (the page
		# shows both, a page at a time). Kept short so a checkout from before this (it deleted the files of
		# all but the newest 48 in shots.json) can't reach an archived shot's file
		ssh $host "flock $dir/.claims.lock python3 - $dir/shots.json $name $(printf %q "$cap") $(date +%s) $(printf %q "$session") $(printf %q "$place")" <<'PY'
import json, os, sys
path, name, cap, t, session, place = sys.argv[1], sys.argv[2], json.loads(sys.argv[3]), int(sys.argv[4]), sys.argv[5], json.loads(sys.argv[6])
arch = os.path.join(os.path.dirname(path), "shots-archive.json")
def load(p): return json.load(open(p)) if os.path.exists(p) else []
def save(p, v): json.dump(v, open(p + ".tmp", "w")); os.replace(p + ".tmp", p)
shots = load(path)
shots.insert(0, dict({"file": "shots/" + name, "caption": cap, "time": t, "session": session}, **({"place": place} if place else {})))
if len(shots) > 24:
    save(arch, shots[24:] + load(arch))   # the archive first: a crash between leaves a duplicate, not a loss
save(path, shots[:24])
PY
		;;
	notes)
		ssh $host "python3 - $dir/notes.json $(printf %q "${2:-list}") $(printf %q "${3:-}") $(printf %q "$(json_str "${4:-}")") $(printf %q "$session") $(date +%s)" <<'PY'
import fcntl, json, os, sys
path, op, nid, text, session, t = sys.argv[1], sys.argv[2], sys.argv[3], json.loads(sys.argv[4]), sys.argv[5], int(sys.argv[6])
lock = open(os.path.join(os.path.dirname(path), ".notes.lock"), "a")
fcntl.flock(lock, fcntl.LOCK_EX)   # the server takes it too
notes = json.load(open(path)) if os.path.exists(path) else []
if op == "list":
    for n in notes:
        print(f"{n['id']} {'done' if n['done'] else 'open'} {n['time']}: {n['text']}")
        for f in n.get("images", []):
            print(f"   image: http://WORKER_ADDR:8765/{f}")
        for r in n.get("replies", []):
            print(f"   {r['session']}: {r['text']}")
    sys.exit(0)
n = next((x for x in notes if x["id"].lower() == nid.lower()), None)
if not n:
    sys.exit(f"no note {nid}")
if op in ("done", "reopen"):
    n["done"] = op == "done"
elif op != "reply" or not text:
    sys.exit("notes reply ID TEXT | notes done|reopen ID [TEXT]")
if text:
    n.setdefault("replies", []).append({"text": text, "session": session, "time": t})
json.dump(notes, open(path + ".tmp", "w"), indent=1); os.replace(path + ".tmp", path)
print(f"{n['id']}: {op} ({session})")
PY
		;;
	usage)
		u=$(curl -sf -m 20 "${WWHD_USAGE_URL:-http://127.0.0.1:7690}/api/usage/claude") || { echo "usage: the editing machine didn't answer" >&2; exit 1; }
		printf '%s' "$u" | python3 -c 'import json,sys,time; d=json.load(sys.stdin); s=d["snapshot"]
json.dump({"meters": s["meters"], "fetchedAt": s["fetchedAt"] // 1000, "stale": d.get("stale", False), "published": int(time.time())}, sys.stdout)' |
			ssh $host "cat > $dir/usage.json.tmp && mv $dir/usage.json.tmp $dir/usage.json"
		;;
	serve)
		ssh $host "mkdir -p $dir && cat > $dir/.server.py.new" < "$here/server.py"
		ssh $host "bash -s" <<EOF
cd $dir
running() { [ -f .pid ] && grep -q server.py /proc/\$(cat .pid)/cmdline 2>/dev/null; }
if running && cmp -s .server.py .server.py.new; then rm .server.py.new; echo "running (pid \$(cat .pid))"; exit 0; fi
mv .server.py.new .server.py
if running; then kill \$(cat .pid); sleep 1; fi
ip=\$(tailscale ip -4 | head -1)
setsid -f python3 .server.py $port \$ip >> .server.log 2>&1 < /dev/null
sleep 1; running && echo "serving http://\$ip:$port (pid \$(cat .pid))" || { echo "didn't start: tail .server.log"; tail -5 .server.log; exit 1; }
EOF
		;;
	"")
		names=$(mktemp)
		ssh $host "docker exec wwhd-worker cat /wwhd/data/ghidra-out/actor_names.tsv" > "$names" || true
		python3 "$root/tools/progress/collect.py" "$names" | ssh $host "mkdir -p $dir && cat > $dir/progress.json.tmp && mv $dir/progress.json.tmp $dir/progress.json"
		rm -f "$names"
		ssh $host "cat > $dir/index.html" < "$here/index.html"
		ssh $host "cat > $dir/testing.html" < "$here/testing.html"
		;;
	*) echo "usage: publish.sh [now TEXT | retire [SESSION] | claim|done|release ID [NOTE] | step ID DONE TOTAL | bug ... | shot PPM CAPTION [PLACE] | notes ... | usage | serve]" >&2; exit 2 ;;
esac
