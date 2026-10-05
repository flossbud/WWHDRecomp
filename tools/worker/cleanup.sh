#!/usr/bin/env bash
# cleanup.sh [--apply] [install]: free the workers' disk of old test outputs (the shared /wwhd once
# filled to 100% and broke a build). Without --apply it only lists what it would delete.
#   /wwhd/data/gx2/<run>/      a check run's command streams (300-600 MB each): deleted when nothing
#                              in it changed for 12 h. The references (gx2/*.txt files) stay.
#   /wwhd/data/m6/<dir>/<x>/   test outputs (tracks, captures, trials) of a checkout, and the old
#                              scratch dirs: deleted when nothing in them changed for 48 h.
# Never touched: references, traces, g3 captures, saves, the game, the progress page, logs, and
# m6/ui (probe_menu.sh's baseline).
# A run in progress keeps writing into its directory, so it is never old enough to go.
#   install    put it on the worker (~/bin/wwhd-cleanup.sh) with a crontab entry every 6 hours
#              (tagged wwhd-cleanup) that cleans the worker's /wwhd, and on the desktop with a systemd
#              user timer (wwhd-cleanup.timer) that cleans ~/wwhd-desk
# Run from the editing machine: it cleans the worker, and the desktop worker's ~/wwhd-desk when it's reachable.
set -euo pipefail
apply=0; [ "${1:-}" = --apply ] && { apply=1; shift; }
if [ "${1:-}" = --here ]; then
    root=${2:?root}
    freed=0
    old() { [ -z "$(find "$1" -mmin -"$2" -print -quit 2>/dev/null)" ]; }
    drop() {
        local kb; kb=$(du -sk "$1" 2>/dev/null | cut -f1)
        freed=$((freed + kb))
        if [ $apply = 1 ]; then rm -rf -- "${1:?}"; else echo "  would delete $1 ($((kb / 1024)) MB)"; fi
    }
    if [ -d "$root/data/gx2" ]; then
        for d in "$root"/data/gx2/*/; do [ -d "$d" ] && old "$d" 720 && drop "${d%/}"; done
    fi
    if [ -d "$root/data/m6" ]; then
        for d in "$root"/data/m6/*/; do
            [ -d "$d" ] || continue
            case "$(basename "$d")" in
                ui) ;;                                  # tools/sixty/probe_menu.sh's 30-tick baseline
                WWHDRecomp*) for x in "$d"*/; do [ -d "$x" ] && old "$x" 2880 && drop "${x%/}"; done ;;
                *) old "$d" 2880 && drop "${d%/}" ;;
            esac
        done
    fi
    echo "$root: $([ $apply = 1 ] && echo freed || echo would free) $((freed / 1048576)) GB; $(df -h "$root" | tail -1 | awk '{print $4 " free, " $5 " used"}')"
    exit 0
fi
self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
a=(); [ $apply = 1 ] && a=(--apply)
if [ "${1:-}" = install ]; then
    ssh -o BatchMode=yes worker 'mkdir -p ~/bin && cat > ~/bin/wwhd-cleanup.sh && chmod +x ~/bin/wwhd-cleanup.sh' < "$self"
    ssh -o BatchMode=yes worker '(crontab -l 2>/dev/null | grep -v wwhd-cleanup; echo "17 */6 * * * $HOME/bin/wwhd-cleanup.sh --apply --here /wwhd >> /wwhd/logs/cleanup.log 2>&1 # wwhd-cleanup") | crontab - && crontab -l | grep wwhd-cleanup'
    # the desktop: a systemd user timer (no cron there), every 6 hours
    ssh -o BatchMode=yes owner@DESKTOP_ADDR 'mkdir -p ~/bin ~/.config/systemd/user && cat > ~/bin/wwhd-cleanup.sh && chmod +x ~/bin/wwhd-cleanup.sh' < "$self"
    ssh -o BatchMode=yes owner@DESKTOP_ADDR 'cat > ~/.config/systemd/user/wwhd-cleanup.service <<EOT
[Unit]
Description=WWHD worker: delete old test outputs in ~/wwhd-desk
[Service]
Type=oneshot
ExecStart=%h/bin/wwhd-cleanup.sh --apply --here %h/wwhd-desk
EOT
cat > ~/.config/systemd/user/wwhd-cleanup.timer <<EOT
[Unit]
Description=WWHD worker cleanup every 6 hours
[Timer]
OnCalendar=*-*-* 00/6:47:00
Persistent=true
[Install]
WantedBy=timers.target
EOT
systemctl --user daemon-reload && systemctl --user enable --now wwhd-cleanup.timer && systemctl --user list-timers wwhd-cleanup.timer --no-pager | head -2'
    exit 0
fi
ssh -o BatchMode=yes worker "bash -s -- ${a[*]:-} --here /wwhd" < "$self"
ssh -o BatchMode=yes -o ConnectTimeout=5 owner@DESKTOP_ADDR "test -d ~/wwhd-desk && bash -s -- ${a[*]:-} --here \$HOME/wwhd-desk" < "$self" 2>/dev/null || echo "desktop: not reachable or no worker there"
