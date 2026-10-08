# Sourced by the worker scripts: the owner's desktop's SSH login and home, from tools/worker/desktop.env (not in
# git: the login is the owner's own account name). desktop.env holds, for example:
#   WWHD_DESKTOP_SSH=user@DESKTOP_ADDR
#   WWHD_DESKTOP_HOME=/home/user
_wwhd_env="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/desktop.env"
[ -f "$_wwhd_env" ] && . "$_wwhd_env"
: "${WWHD_DESKTOP_SSH:?tools/worker/desktop.env must set WWHD_DESKTOP_SSH (user@host of the owner's desktop)}"
: "${WWHD_DESKTOP_HOME:?tools/worker/desktop.env must set WWHD_DESKTOP_HOME (that account's home directory)}"
