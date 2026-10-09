#!/usr/bin/env bash
# droptest [DROP_ENV]: routes at 60 on the virtual clock, with and without dropped half frames (DROP_ENV, default
# WWHD_60FPS_DROPTEST=2; the frame rate cap: WWHD_FPS_CAP=40 or 50); whole-tick state compared. DROP_ROUTES: the routes
source tools/sixty/tests/common.sh
D=$OUT/droptest; mkdir -p $D/bin0 $D/bin2
cp build/wwhd/wwhd-null $D/bin0/; cp build/wwhd/wwhd-null $D/bin2/
drop=${1:-WWHD_60FPS_DROPTEST=2}
routes=(${DROP_ROUTES:-tour3 en-bo sail})
for r in "${routes[@]}"; do
  ( WWHD_STATE_TRACK=168,73,194,214,453,407 CEMU_BIN=$D/bin0/wwhd-null SIXTY_OUT=$D/s0 tools/sixty/run.sh $r $D/$r-0 60 > $D/$r-0.log 2>&1 ) &
  ( env WWHD_STATE_TRACK=168,73,194,214,453,407 $drop CEMU_BIN=$D/bin2/wwhd-null SIXTY_OUT=$D/s2 tools/sixty/run.sh $r $D/$r-2 60 > $D/$r-2.log 2>&1 ) &
  wait
  a=$D/$r-0/60/hashes.txt b=$D/$r-2/60/hashes.txt
  n=$(wc -l < $a); m=$(wc -l < $b)
  wa=$(awk '$1 % 2 == 0' $a | md5sum | cut -c1-8); wb=$(awk '$1 % 2 == 0' $b | md5sum | cut -c1-8)
  dw=$(diff <(awk '$1 % 2 == 0' $a) <(awk '$1 % 2 == 0' $b) | grep -c '^<')
  first=$(diff <(awk '$1 % 2 == 0' $a) <(awk '$1 % 2 == 0' $b) | grep -m1 '^<' | cut -c1-60)
  echo "$r: lines $n vs $m; whole-tick lines differing: $dw ${first:+(first: $first)}; drops logged: $(grep -ac 'half frames dropped' $D/bin2/portable/log.txt)"
done
python3 - $D "${routes[@]}" <<'PY'
import math, os, struct, sys
sys.path.insert(0, "tools/sixty")
import compare
D = sys.argv[1]
pos = lambda b: struct.unpack(">3f", b[0x314:0x320])
for r in sys.argv[2:]:
    try:
        A, B = compare.load_track(f"{D}/{r}-0/60/track.bin"), compare.load_track(f"{D}/{r}-2/60/track.bin")
    except OSError as e:
        print(r, "no track", e); continue
    worst = {}
    for k in A:
        if k not in B: continue
        for t in A[k]:
            if t % 2 or t not in B[k]: continue
            d = math.dist(pos(A[k][t]), pos(B[k][t]))
            name = k[0]
            if d > worst.get(name, (0, 0))[0]: worst[name] = (d, t)
    print(r, " ".join(f"{n}:{d:.2f}@{t}" for n, (d, t) in sorted(worst.items())))
PY
echo droptest done
