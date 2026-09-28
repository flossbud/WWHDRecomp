#!/usr/bin/env bash
# press.sh KEY [HOLD_SECONDS]  - press a key in the reference Cemu window (Pro Controller, controller0.xml):
#   A=x B=z X=s Y=a L=q R=w ZL=1 ZR=2 Plus=Return Minus=BackSpace, D-pad=arrows,
#   left stick=i/k/j/l, right stick=t/g/f/h
set -euo pipefail
export DISPLAY=${DISPLAY:-:99}
win=$(xdotool search --onlyvisible --name '^Cemu' | head -1)
xdotool windowactivate --sync "$win"
xdotool keydown "$1"; sleep "${2:-0.15}"; xdotool keyup "$1"
