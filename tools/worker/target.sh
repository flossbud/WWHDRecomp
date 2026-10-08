# Sourced by sync.sh, w, job and publish.sh shot: which worker a command goes to. Two machines run
# the same container image (wwhd-worker) with the same paths inside (/wwhd/...):
#   desktop    podman on the owner's PC, the desktop worker (24 threads, all the worker's), /wwhd
#              being ~/wwhd-desk there: everything runs there while the owner lends it (builds,
#              tests, captures, checks: its references give the same verdicts as the worker's, Ghidra).
#   worker     docker on the worker (10 of 12 threads), /wwhd on the host too: the fallback.
# Without WWHD_ON the desktop is used when its worker is running (tools/worker/desktop.sh status),
# else the worker. WWHD_ON=worker or WWHD_ON=desktop forces one. When the owner takes the desktop
# back, commands go to the worker by themselves: sync and build there first (its build may be old).
# The machines' addresses come from tools/worker/hosts.env (tools/worker/hosts.sh).
source "$(dirname "${BASH_SOURCE[0]}")/hosts.sh"
if [ -z "${WWHD_ON:-}" ]; then              # -n: the probe must not read the caller's stdin (w python3 - < script)
    if ssh -n -o BatchMode=yes -o ConnectTimeout=4 $WWHD_DESKTOP_SSH \
        "podman container inspect -f '{{.State.Running}}' wwhd-worker 2>/dev/null" 2>/dev/null | grep -q true; then
        WWHD_ON=desktop
    else
        WWHD_ON=worker
    fi
fi
case "$WWHD_ON" in
    worker)  W_SSH=$WWHD_WORKER_SSH; W_ENGINE=docker; W_ROOT=/wwhd ;;
    desktop) W_SSH=$WWHD_DESKTOP_SSH; W_ENGINE=podman; W_ROOT=$WWHD_DESKTOP_HOME/wwhd-desk ;;
    *) echo "WWHD_ON=$WWHD_ON: worker or desktop" >&2; exit 2 ;;
esac
# a path inside the container (/wwhd/...) as the host sees it
hostpath() { echo "$W_ROOT${1#/wwhd}"; }
# ssh to the worker's host (BatchMode: never prompt)
on_host() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$W_SSH" "$@"; }
