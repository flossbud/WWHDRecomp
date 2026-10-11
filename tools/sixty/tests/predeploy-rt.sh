#!/usr/bin/env bash
# predeploy-rt.sh [ROUTE...]: predeploy's real-time part, for wwhd-play, the real-time build that deploys ship (t-codegen,
# session top). wwhd-play is the generated code compiled with WWHD_RT_LOOSE (tools/recomp/runtime/ppc_ops.h): blocks
# are charged but not checked, and a thread whose slice ran out yields at the next function entry or loop header, so a
# few blocks later than wwhd-null, where every check runs. It refuses the virtual clock, so predeploy.sh's 30-against-60
# routes can't cover it; this compares it with wwhd-null in real time, where neither is exact:
# 1. Routes (rt_routes.sh, three host threads, in real time, the null GPU): wwhd-null twice (mt1, mt2) and wwhd-play
#    twice (pl1, pl2). wwhd-null against itself is real time's noise for the route; FAIL when wwhd-play's runs both end
#    more than PREDEPLOY_FAIL (150) units from both of wwhd-null's and further than twice that noise, or a wwhd-play
#    run has no track (a crash or a hang); late starts (the title) are left out, as rt_routes does.
# 2. A soak (soak.sh, rendered): PREDEPLOY_RT_SOAK minutes (default 10; 0: none) of seeded random play with wwhd-play,
#    at least PREDEPLOY_RT_SOAK_MIN fps (45; a WARN instead when the machine is busy).
# RT_COMPARE_ONLY=1 (rt_routes.sh): the last runs judged again, nothing played.
# Exit status 1 on a FAIL. Default routes: a dozen of predeploy's, the sound and loader threads busiest (warps, doors,
# menus, the dungeons' and a fight). Run it as a job: tools/worker/job start prt tools/sixty/tests/predeploy-rt.sh
set -uo pipefail
cd "$(dirname "$0")/../../.."
source tools/sixty/tests/common.sh
play=${PLAY_BIN:-build/wwhd/wwhd-play}         # PLAY_BIN: another real-time build (tools/recomp/pgo.sh's)
[ -x "$play" ] || { echo "predeploy-rt: no $play (WWHD_PLAY_BUILD=0?)"; exit 2; }
routes=("$@"); [ ${#routes[@]} -eq 0 ] && routes=(warp back door house talk menus tour3 sail drcjar wtspring tgstatue en-rd)
fail=0
out=$(RT_PLAY_BIN="$play" RT_VARIANTS="mt1 mt2 pl1 pl2" tools/sixty/tests/rt_routes.sh "${routes[@]}")
echo "$out"
echo "$out" | python3 -c '
import re, sys
lim = float(sys.argv[1]); bad = 0
pairs = ["mt1-mt2", "mt1-pl1", "mt1-pl2", "mt2-pl1", "mt2-pl2", "pl1-pl2"]
rows = [l for l in sys.stdin if re.match(r"^\S+\s+(\s*([0-9.]+/\s*[0-9.]+\*?|late start|no track|no common keys)){6}\s*$", l)]
for l in rows:
    r = l.split()[0]
    raw = re.findall(r"[0-9.]+/\s*[0-9.]+\*?|late start|no track|no common keys", l[len(r):])
    c = dict(zip(pairs, raw))
    if any(c[p] == "no track" for p in pairs[1:5]):
        print(f"FAIL  {r:10s} a run has no track (a crash or a hang?)"); bad += 1; continue
    end = lambda p: float(c[p].split("/")[0]) if "/" in c[p] else None
    noise, cross = end("mt1-mt2"), [end(p) for p in pairs[1:5] if end(p) is not None]
    if not cross:
        print(f"skip  {r:10s} late starts only"); continue
    worst = min(cross)
    verdict = "FAIL" if worst > lim and (noise is None or worst > 2 * noise) else "ok"
    bad += verdict == "FAIL"
    ns = "-" if noise is None else f"{noise:.1f}"
    print(f"{verdict:5s} {r:10s} wwhd-play ends {worst:7.1f} units from wwhd-null at the nearest (wwhd-null against itself {ns})")
sys.exit(1 if bad else 0)
' "${PREDEPLOY_FAIL:-150}" || fail=1
if [ "${PREDEPLOY_RT_SOAK:-10}" != 0 ]; then
    soak=$(SOAK_BIN="$play" tools/sixty/tests/soak.sh "${PREDEPLOY_RT_SOAK:-10}" play:WWHD_CORES=3)
    echo "$soak"                                # (not tee /dev/stderr: it reopens a job's log and truncates it)
    grep -q ": ok, swap" <<< "$soak" || fail=1
    # its speed: under PREDEPLOY_RT_SOAK_MIN fps (default 45; the soak plays at 60) is a FAIL on a quiet machine, and on a
    # busy one (load over half its CPUs: other jobs, deploy 9's soak read 10.5 fps beside three capture runs) a WARN
    # to run it again when it's quiet
    fps=$(grep -o '[0-9.]* fps over' <<< "$soak" | head -1 | cut -d' ' -f1)
    load=$(cut -d' ' -f1 /proc/loadavg); cpus=$(nproc)
    if [ -n "$fps" ] && awk -v f="$fps" -v m="${PREDEPLOY_RT_SOAK_MIN:-45}" 'BEGIN { exit !(f < m) }'; then
        if awk -v l="$load" -v c="$cpus" 'BEGIN { exit !(l > c / 2) }'; then
            echo "WARN  soak: $fps fps, under ${PREDEPLOY_RT_SOAK_MIN:-45}, with the machine busy (load $load on $cpus CPUs): run it again when it's quiet"
        else
            echo "FAIL  soak: $fps fps, under ${PREDEPLOY_RT_SOAK_MIN:-45} (load $load on $cpus CPUs)"; fail=1
        fi
    fi
fi
[ $fail = 0 ] && echo "predeploy-rt: ok" || echo "predeploy-rt: FAIL"
exit $fail
