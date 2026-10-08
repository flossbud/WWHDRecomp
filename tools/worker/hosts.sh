# Sourced by the worker, progress and cloud scripts: where the machines are, from tools/worker/hosts.env
# (gitignored: it holds the machines' addresses and the owner's logins; copy hosts.env.example and fill it in).
#   WWHD_WORKER_SSH     ssh destination of the worker (an ~/.ssh/config alias or user@address)
#   WWHD_DESKTOP_SSH    user@address of the desktop worker (the owner's PC)
#   WWHD_DESKTOP_HOME   that account's home directory
#   WWHD_PROGRESS_URL   the progress page, as the owner opens it (http://ADDRESS:8765)
# A checkout from before hosts.env may still have tools/worker/desktop.env (the desktop's two lines): it's read
# first, so hosts.env wins where both set a value.
_wwhd_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$_wwhd_dir/desktop.env" ] && . "$_wwhd_dir/desktop.env"
[ -f "$_wwhd_dir/hosts.env" ] && . "$_wwhd_dir/hosts.env"
: "${WWHD_WORKER_SSH:?tools/worker/hosts.env must set WWHD_WORKER_SSH (the worker's ssh destination; see hosts.env.example)}"
: "${WWHD_DESKTOP_SSH:?tools/worker/hosts.env must set WWHD_DESKTOP_SSH (user@address of the desktop worker)}"
: "${WWHD_DESKTOP_HOME:?tools/worker/hosts.env must set WWHD_DESKTOP_HOME (that account's home directory)}"
: "${WWHD_PROGRESS_URL:=}"
