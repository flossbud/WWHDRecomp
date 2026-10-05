# Sourced by sync.sh, w and job: which worker a command goes to. Two machines run the same
# container image (wwhd-worker) with the same paths inside (/wwhd/...):
#   worker (default)        docker on the worker; /wwhd on the host too. Has everything, including the
#                             reference traces and captures the checks compare with (~140 GB).
#   desktop (WWHD_ON=desktop) podman on the owner's desktop (24 threads); /wwhd is
#                             ~/wwhd-desk there. Builds and tests (spawn/stage/route tests, captures,
#                             regress) run there; checks.sh runs on the worker only. The owner may
#                             take it back: tools/worker/desktop.sh status says whether it's on.
WWHD_ON=${WWHD_ON:-worker}
case "$WWHD_ON" in
    worker) W_SSH=worker; W_ENGINE=docker; W_ROOT=/wwhd ;;
    desktop)  W_SSH=owner@DESKTOP_ADDR; W_ENGINE=podman; W_ROOT=/home/owner/wwhd-desk ;;
    *) echo "WWHD_ON=$WWHD_ON: worker or desktop" >&2; exit 2 ;;
esac
# a path inside the container (/wwhd/...) as the host sees it
hostpath() { echo "$W_ROOT${1#/wwhd}"; }
# ssh to the worker's host (BatchMode: never prompt)
on_host() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$W_SSH" "$@"; }
