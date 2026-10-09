#!/usr/bin/env bash
# soak.sh [MINUTES] [NAME:K=V,K=V...]: one long real-time run at 60 (session top, for three host threads by default,
# threads.md): the 100% save, continue's start, then MINUTES (default 60) of seeded random play on Outset: the stick
# in random directions and lengths, the camera stick, A (roll, talk, read on), B (sword), R (shield), ZL (target),
# X and Y (the items the save holds there), now and then the pause menu (PLUS, a page or two, PLUS). The run must
# reach its last swap with no crash, panic or assert. Each NAME:K=V,... is a variant run after the other, its
# environment added (default one, "cores3:WWHD_CORES=3"); every variant gets the same inputs (SOAK_SEED, default 1).
# With SOAK_MIN unset the route ends at swap 900 + MINUTES x 3600 (60 fps), so a run below 60 fps takes longer.
# Out ($OUT/soak/NAME): frames.txt (WWHD_FRAME_LOG), threads.txt (WWHD_THREAD_STATS), census.txt (WWHD_CORE_CENSUS),
# the emulator's log; a verdict line per variant. Rendered (WWHD_RENDER=vk, headless on the machine's GPU). SOAK_BIN: another binary than build/wwhd/wwhd-null. Run it as a job: tools/worker/job start soak tools/sixty/tests/soak.sh 60
set -uo pipefail
source "$(dirname "$0")/common.sh"
minutes=${1:-60}; shift || true
variants=("$@"); [ ${#variants[@]} -eq 0 ] && variants=(cores3:WWHD_CORES=3)
S=$OUT/soak; mkdir -p "$S"
frames=$((900 + minutes * 1800))                  # game frames (ticks at 30 a second)
script=$S/soak-${SOAK_SEED:-1}-$minutes.txt
python3 - "$frames" "${SOAK_SEED:-1}" > "$script" <<'PY'
import random, sys
end, seed = int(sys.argv[1]), int(sys.argv[2])
rnd = random.Random(seed)
print("# soak route (tools/sixty/tests/soak.sh), seed", seed)
for line in ("420 A 5", "540 A 5", "660 A 5", "780 A 5"):   # continue-100.txt's start: gameplay on the Outset dock
    print(line)
f = 960
while f < end - 300:
    r = rnd.random()
    if r < 0.03:                                   # the pause menu: open, a page or two, close
        print(f"{f} PLUS 5"); f += 120
        for _ in range(rnd.randint(0, 2)):
            print(f"{f} {rnd.choice(['R', 'L'])} 5"); f += 90
        print(f"{f} PLUS 5"); f += 90
        continue
    n = rnd.randint(20, 150)
    a = rnd.uniform(-1, 1); b = rnd.uniform(-1, 1)
    parts = [f"LS={a:.2f},{b:.2f}"]
    if rnd.random() < 0.3:
        parts.append(f"RS={rnd.uniform(-1, 1):.2f},{rnd.uniform(-1, 1):.2f}")
    if rnd.random() < 0.15:
        parts.append(rnd.choice(["R", "ZL"]))
    print(f"{f} {'+'.join(parts)} {n}")
    g = f + rnd.randint(5, max(6, n - 5))
    if rnd.random() < 0.5:                         # a button in the middle of the move
        print(f"{g} {rnd.choice(['A', 'A', 'B', 'B', 'B', 'X', 'Y'])} {rnd.randint(2, 8)}")
    f += n + rnd.randint(0, 30)
PY
swaps=$((900 + 2 * (frames - 900)))
fail=0
for v in "${variants[@]}"; do
    name=${v%%:*}; envs=()
    IFS=, read -ra kvs <<< "$([[ $v == *:* ]] && echo "${v#*:}")"
    for kv in "${kvs[@]}"; do envs+=("$kv"); done
    d=$S/$name; rm -rf "$d"; mkdir -p "$d/bin"
    cp "${SOAK_BIN:-build/wwhd/wwhd-null}" "$d/bin/wwhd-null" || exit 2
    echo "soak.sh: $name: $minutes min ($frames game frames, swap $swaps), ${envs[*]}"
    start=$(date +%s)
    env WWHD_DEBUG_BOOT=1 "${envs[@]}" WWHD_60FPS=1 WWHD_60FPS_FROM=900 WWHD_EXIT_FRAME=$swaps WWHD_LAZY_DRAWDONE=${WWHD_LAZY_DRAWDONE:-1} \
        WWHD_FRAME_LOG=$d/frames.txt WWHD_THREAD_STATS=$d/threads.txt WWHD_CORE_CENSUS=$d/census.txt WWHD_NATIVE=on WWHD_RENDER=vk \
        REF_PIDFILE=$d/pid REF_FRESH=1 REF_SAVE=/wwhd/data/saves/wwhd_100 CEMU_INPUT_SCRIPT=$script CEMU_BIN=$d/bin/wwhd-null \
        tools/reference/run.sh > "$d/run.log" 2>&1 || { echo "soak.sh: $name: the game didn't start (see $d/run.log)"; fail=1; continue; }
    pid=$(cat "$d/pid")
    while kill -0 "$pid" 2>/dev/null; do sleep 30; done
    cp "$d/bin/portable/log.txt" "$d/log.txt" 2>/dev/null
    last=$(grep -v '^#' "$d/frames.txt" 2>/dev/null | tail -1 | cut -d' ' -f1)
    bad=$(grep -ciE 'panic|assert|sigsegv|segmentation|crash|fatal' "$d/log.txt" 2>/dev/null)
    mins=$(( ($(date +%s) - start) / 60 ))
    if grep -q "reached frame $swaps, exiting" "$d/log.txt" 2>/dev/null && [ "${bad:-0}" = 0 ]; then
        echo "soak.sh: $name: ok, swap $swaps reached in $mins min"
    else
        echo "soak.sh: $name: FAIL, no exit at swap $swaps (the frame log ends at ${last:-none}) after $mins min; $bad suspect log lines (see $d/log.txt)"
        fail=1
    fi
    python3 - "$d/frames.txt" <<'PY'
import statistics, sys
fr = [l.split() for l in open(sys.argv[1]) if not l.startswith("#")]
fr = [p for p in fr if int(p[0]) >= 1800]
if fr:
    span = (float(fr[-1][2]) - float(fr[0][2])) / 1000
    shown = [p for p in fr if p[1] != "d"]
    late = sum(1 for p in fr if int(p[8]) > 0)
    worst = sorted(float(p[3]) for p in fr)[-max(1, len(fr) // 1000)]
    c = lambda i: sum(float(p[i]) for p in fr) / span / 10 if len(fr[0]) > i else 0
    print(f"soak.sh:   {len(shown) / span:.1f} fps over {span / 60:.1f} min, {late} late frames ({100 * late / len(fr):.1f}%), "
          f"work 99.9th percentile {worst:.1f} ms; CPU % of a core: game thread {c(5):.0f}, core 0 {c(13):.0f}, core 2 {c(14):.0f}")
PY
done
exit $fail
