#!/usr/bin/env bash
# regress.sh: every converted enemy's 30-against-60 test, to rerun after a change to shared code
# (a helper override, a shared rule). Compare with the numbers in docs/handoff.md.
cd "$(dirname "$0")"
for p in 215 206 188 191 181 224; do ./spawn_test.sh $p 0 190 | tail -1; done
./stage_test.sh 216 M_NewD2,0,12,-1 1000 1400 | tail -1
./stage_test.sh 209 kindan,0,9,-1 1000 1300 | grep "^(209" | head -1
./stage_test.sh 214 kindan,0,9,-1 1010 1300 "1000:214,0,-278,6052,-6700" | grep "^(214" | tail -1
WWHD_DEBUG_BOSS=1 ./stage_test.sh 234 M_DragB,0,0,-1 1050 1450 | tail -1
