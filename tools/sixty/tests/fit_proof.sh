#!/usr/bin/env bash
# fit_proof.sh [ROUTE...]: WWHD_SURFACE_FIT's proof over the scripted routes (gpu-plan.md item 2; session bottom).
# Each route at 60 on the virtual clock (tools/sixty/run.sh) with the Vulkan renderer (FIT_GPU: REF_GPU, default
# the worker's own GPU; llvmpipe works, slower) and WWHD_SURFACE_FIT=proof, which changes nothing and logs every
# surface it would fit and every read past the rows it would fit to. A route passes when its log has no "surface
# fit: read past" line. Extra environment passes through (WWHD_BARRIERS=narrow, WWHD_RENDER_SCALE...).
# Default routes: predeploy.sh's. Output: $OUT/fitproof/ROUTE.log (the emulator's log), a verdict line each.
set -uo pipefail
source "$(dirname "$0")/common.sh"
routes=("$@"); [ ${#routes[@]} -eq 0 ] && routes=(warp back door house talk items cuts leaf hook ladder crawl carry spin bow shield swing land3 sidle2 pot plants slash sail menus tour3 boomerang bombs grapple boots hammer armor mirror heavy drcjar fwbud tgbeam wtspring medli gtrock en-rd en-ph en-pz en-bo tgstatue medliharp gtgrapple fwswitch crate slope warppot climb gohmatail gohmarock en-tn route tour)
R=$OUT/fitproof; mkdir -p "$R/bin"
cp build/wwhd/wwhd-null "$R/bin/wwhd-null.new" && mv "$R/bin/wwhd-null.new" "$R/bin/wwhd-null"
pass=0; fail=0; bad=()
for r in "${routes[@]}"; do
    w=; case $r in back) w=M_NewD2,5,14,-1 ;; talk) w=sea,0,11,-1 ;; house) w=sea,9,11,-1 ;; esac
    rm -rf "$R/run" "$R/bin/portable/log.txt" "$R/$r.log"; ok=1
    env ${w:+WWHD_DEBUG_STAGE=920:$w} ${FIT_GPU:+REF_GPU=$FIT_GPU} WWHD_RENDER=vk WWHD_SURFACE_FIT=proof \
        CEMU_BIN="$R/bin/wwhd-null" tools/sixty/run.sh "$r" "$R/run" 60 > "$R/$r.out" 2>&1 \
        || { echo "fit_proof: $r: the run failed (see $R/$r.out)"; ok=; }
    cp "$R/bin/portable/log.txt" "$R/$r.log" 2>/dev/null
    fits=$(grep -ac "surface fit: .* would be fitted" "$R/$r.log" 2>/dev/null); past=$(grep -ac "surface fit: read past" "$R/$r.log" 2>/dev/null)
    if [ -z "$ok" ] || [ ! -s "$R/$r.log" ]; then fail=$((fail + 1)); bad+=("$r"); v=FAILED
    elif [ "${past:-0}" = 0 ]; then pass=$((pass + 1)); v=ok; else fail=$((fail + 1)); bad+=("$r"); v=READ-PAST; fi
    echo "fit_proof: $r: $v (${fits:-0} surfaces would be fitted, ${past:-0} reads past) $(date +%H:%M)"
done
rm -rf "$R/run"
echo "fit_proof: $pass ok, $fail failed or with reads past${bad[*]:+: ${bad[*]}}"
