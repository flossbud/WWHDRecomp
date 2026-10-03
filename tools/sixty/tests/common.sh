# Sourced by the 60 fps test scripts (run on the worker, from a worker checkout): ROOT is the
# checkout, OUT its scratch directory under /wwhd/data/m6 (one per checkout, so parallel sessions'
# outputs never meet; game memory and captures: they stay on the worker).
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=/wwhd/data/m6/$(basename "$ROOT")
mkdir -p "$OUT"
cd "$ROOT"
# defaults: the processes sixty.cpp converts by default
defaults() { grep -o 'kConvertedByDefault = "[0-9,]*"' src/overrides/sixty.cpp | cut -d'"' -f2; }
# base: what spawn and stage tests convert besides the actor tested: the camera, the ship and its
# sail, not Link (so an actor that reads Link sees the 30-tick run's Link; the numbers in
# docs/handoff.md were measured so). BASE=defaults uses the default list instead.
base() { if [ "${BASE:-}" = defaults ]; then defaults; else echo ${BASE:-476,165,171}; fi; }
# trial lines worth reading: stores stepping about twice, without the helpers' derived copies
trial_filter() {
    grep -v "f_028E8F64\|f_028E90D4\|f_028E9108\|only half\|not moved\|f_025D475C\|f_02018D40\|f_020182E0\|f_02018808\|f_024F08A8\|f_024EFF50\|f_020180A8" |
        grep -E "twice| x1\.[6-9]| x2\.| x3\." | cut -c1-190
}
