#!/usr/bin/env bash
# shots.sh ROUTE FRAMES(comma) [RATE]: captures of a route at those game frames (WWHD_DEBUG_* pass
# through), joined two by two into $OUT/shots-ROUTE/all.png. Captures are game data: view them in a
# scratch directory and delete them; tools/progress/publish.sh shot puts one on the progress page.
set -e
source "$(dirname "$0")/common.sh"
d=$OUT/shots-$1; rm -rf $d $OUT/shotrun-$1; mkdir -p $d
last=$(echo $2 | tr , '\n' | sort -n | tail -1)
CEMU_SHOT_FRAMES=$2 CEMU_SHOT_DIR=$d WWHD_RENDER=vk REF_GPU=llvmpipe SIXTY_FRAMES=$((last + 5)) tools/sixty/run.sh $1 $OUT/shotrun-$1 ${3:-30} > /dev/null 2>&1
cd $d; files=($(ls f*.tv.ppm)); rows=()
for ((i = 0; i < ${#files[@]}; i += 2)); do convert ${files[$i]} ${files[$((i+1))]:-${files[$i]}} -resize 640x360 +append row$i.png; rows+=(row$i.png); done
convert "${rows[@]}" -append all.png; rm -f row*.png; ls -d $d/*
