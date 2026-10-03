#!/usr/bin/env bash
# framediff.sh ROUTE FIRST LAST: captures of every swap FIRST..LAST at 60 (tick N is swap 900 + 2(N - 900)),
# then each one's RMSE from the one before (small images): 0 = a repeated frame, so a stretch where every
# other value is 0 is a scene showing at 30
set -e
source "$(dirname "$0")/common.sh"
d=$OUT/fdiff; rm -rf $d $OUT/fdiffrun; mkdir -p $d
CEMU_SHOT_FRAMES=$(seq -s, $2 $3) CEMU_SHOT_DIR=$d WWHD_RENDER=vk REF_GPU=llvmpipe SIXTY_FRAMES=$(( 900 + ($3 - 900) / 2 + 3 )) tools/sixty/run.sh $1 $OUT/fdiffrun 60 > /dev/null 2>&1
cd $d; prev=
for f in $(ls f*.tv.ppm); do
  convert $f -resize 320x180 ${f%.ppm}.png; rm -f $f
  if [ -n "$prev" ]; then echo "${f:1:6} $(compare -metric RMSE $prev ${f%.ppm}.png null: 2>&1 | cut -d' ' -f1)"; fi
  prev=${f%.ppm}.png
done
