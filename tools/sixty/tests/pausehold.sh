#!/usr/bin/env bash
# pausehold.sh OUT RATE (t-pausemenu, session top): the pause menu held in real time (windowed, on our own Xvfb, the
# save route, WWHD_FRAME_LOG): opened (F2) OPEN_AT seconds in (40; in play on llvmpipe: 160-200), held HOLD_S (62), then
# closed (Esc). ok: the release's first five frames no longer than the frames before the hold (their 99th percentile, at
# least 35 ms) and no burst in the first second (session qa's review: the guest clock runs on while held). Run it on
# the worker: tools/worker/job start hold tools/sixty/tests/pausehold.sh /wwhd/tmp/hold 60
set -uo pipefail
out=$1; rate=$2
rm -rf "$out"; mkdir -p "$out"; export DISPLAY=${PAUSEHOLD_DISPLAY:-:95}
xv=
if ! xdpyinfo >/dev/null 2>&1; then Xvfb $DISPLAY -screen 0 3900x2200x24 -nolisten tcp >/dev/null 2>&1 & xv=$!; sleep 2; fi
win() { xdotool search --onlyvisible --name "Wind Waker" 2>/dev/null | head -1; }
key() { local id; id=$(win); [ -n "$id" ] && { xdotool windowfocus --sync "$id" 2>/dev/null; xdotool keydown --window "$id" "$1"; sleep 0.12; xdotool keyup --window "$id" "$1"; }; echo "$1 $(date +%s.%N)" >> "$out/keys.txt"; }
( sleep ${OPEN_AT:-40}; key F2; sleep ${HOLD_S:-62}; key Escape ) & k=$!
env WWHD_WINDOW=1 WWHD_RENDER=vk SIXTY_REALTIME=1 SIXTY_FRAMES=${FRAMES:-2400} WWHD_FRAME_LOG="$out/frames.txt" \
    tools/sixty/run.sh save "$out/run" "$rate" > "$out/run.log" 2>&1
wait $k; [ -n "$xv" ] && kill $xv
python3 - "$out" "$rate" "${HOLD_S:-62}" <<'PY'
import os, sys
out, rate, held_s = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
rows = [(int(f[0]), float(f[2])) for f in (l.split() for l in open(os.path.join(out, "frames.txt")) if not l.startswith("#") and l.strip())]
gaps = [(rows[i + 1][1] - rows[i][1], i) for i in range(len(rows) - 1)]
hold = max(gaps)                                   # the hold: the longest gap
i = hold[1] + 1                                    # the first frame after the release
before = sorted(rows[j + 1][1] - rows[j][1] for j in range(max(0, i - 301), i - 2))
after = [rows[j + 1][1] - rows[j][1] for j in range(i, min(len(rows) - 1, i + 300))]
first = after[:5]                                  # the release's own frames, against the frames before the hold
limit = max(before[int(len(before) * 0.99)] if before else 0, 35)
first_sec = sum(1 for r in rows[i:] if r[1] - rows[i][1] <= 1000)
bad = hold[0] < held_s * 800 or not after or max(first) > limit or first_sec > rate * 1.25
print(f"{'FAIL' if bad else 'ok':5s} hold at {rate}: held {hold[0] / 1000:.1f} s (swap {rows[hold[1]][0]}); the release's first frames "
      f"{[round(x) for x in first]} ms against the frames before it (p99 {limit:.0f}, median {before[len(before) // 2] if before else 0:.0f}); "
      f"{first_sec} in the first second (at most {rate * 1.25:.0f})")
sys.exit(1 if bad else 0)
PY
