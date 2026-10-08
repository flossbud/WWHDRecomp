#!/usr/bin/env bash
# tailnet-setup.sh - a cloud session joins the owner's tailnet and gets the workers (docs/cloud-handoff.md).
# Run it at the start of every cloud session (it does nothing when already set up): bash tools/cloud/tailnet-setup.sh [NAME]
# NAME (or WWHD_CLOUD_SESSION) names this session: cloud, cloud2, cloud3... Each needs its own, because it picks the
# session's worker directory (/wwhd/WWHDRecomp-NAME: sync.sh mirrors the checkout there with --delete, so two
# sessions sharing one would overwrite each other's trees and builds) and its name on the progress page.
#
# Tailscale runs in userspace (no TUN device, no root network setup); SSH goes through `tailscale nc`. The tailnet's
# rules let this node (tag:wwhd-cloud, an ephemeral key) reach the worker's SSH and the owner's PC's SSH only, and the
# PC's key runs nothing but its deploy receiver (tools/play/deploy-cloud.sh). The cloud environment sets:
#   TS_AUTHKEY             the tailnet key (ephemeral, pre-approved, tagged tag:wwhd-cloud)
#   WWHD_CLOUD_SSH_KEY     the worker's key for this session (the private key, base64)
#   WWHD_CLOUD_DEPLOY_KEY  the PC's deploy key (the private key, base64)
#   WWHD_WORKER_ADDR       the worker's tailnet address
#   WWHD_WORKER_USER       the login on the worker
#   WWHD_DESKTOP_SSH       user@address of the PC
#   WWHD_DESKTOP_HOME      that account's home
#   WWHD_CLOUD_DESKTOP_KEY the PC's full login key (base64; the owner's choice, 2026-10-08): with it the desktop
#                          worker is usable too (WWHD_ON unset: the desktop while its worker runs, else the worker)
#   WWHD_ON                optional: worker or desktop forces one
# The script writes tools/worker/hosts.env (gitignored) from them. None of these values go into git (the addresses
# are the owner's machines', the logins the owner's account names).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
st=$HOME/.tailscale-wwhd
sock=$st/tailscaled.sock
for v in TS_AUTHKEY WWHD_CLOUD_SSH_KEY WWHD_CLOUD_DEPLOY_KEY WWHD_WORKER_ADDR WWHD_WORKER_USER WWHD_DESKTOP_SSH WWHD_DESKTOP_HOME; do
    [ -n "${!v:-}" ] || { echo "tailnet-setup: $v isn't set (the cloud environment's variables)" >&2; exit 2; }
done
sudo=; [ "$(id -u)" = 0 ] || sudo=sudo

# 1. Tailscale, in userspace
if ! command -v tailscaled >/dev/null; then
    echo "tailnet-setup: installing Tailscale"
    curl -fsSL https://tailscale.com/install.sh | $sudo sh >/dev/null
fi
command -v ssh >/dev/null || { $sudo apt-get install -y openssh-client >/dev/null 2>&1 || { $sudo apt-get update >/dev/null && $sudo apt-get install -y openssh-client >/dev/null; }; }
mkdir -p "$st"
if ! tailscale --socket="$sock" status >/dev/null 2>&1; then
    # a container restart leaves a daemon or state from the last start, whose ephemeral node is gone: start clean
    [ -f "$st.pid" ] && kill "$(cat "$st.pid")" 2>/dev/null && sleep 2
    rm -rf "$st" && mkdir -p "$st"
    setsid nohup tailscaled --tun=userspace-networking --state="$st/tailscaled.state" --socket="$sock" \
        --statedir="$st" > "$st/tailscaled.log" 2>&1 < /dev/null &
    echo $! > "$st.pid"
    for _ in $(seq 1 30); do [ -S "$sock" ] && break; sleep 1; done
    tailscale --socket="$sock" up --authkey="$TS_AUTHKEY" --hostname="wwhd-cloud" --accept-dns=false
fi
tailscale --socket="$sock" status | sed -n 1,3p   # sed reads it all: head would SIGPIPE under pipefail

# 2. SSH: the keys and the two hosts, through the tailnet
mkdir -p ~/.ssh && chmod 700 ~/.ssh
echo "$WWHD_CLOUD_SSH_KEY" | base64 -d > ~/.ssh/wwhd_cloud_worker
echo "$WWHD_CLOUD_DEPLOY_KEY" | base64 -d > ~/.ssh/wwhd_cloud_deploy
chmod 600 ~/.ssh/wwhd_cloud_worker ~/.ssh/wwhd_cloud_deploy
desk_key=~/.ssh/wwhd_cloud_deploy                 # without the full key the PC answers only the deploy receiver
if [ -n "${WWHD_CLOUD_DESKTOP_KEY:-}" ]; then
    echo "$WWHD_CLOUD_DESKTOP_KEY" | base64 -d > ~/.ssh/wwhd_cloud_desktop && chmod 600 ~/.ssh/wwhd_cloud_desktop
    desk_key=~/.ssh/wwhd_cloud_desktop            # deploy-cloud.sh still names the deploy key with -i
fi
desk_user=${WWHD_DESKTOP_SSH%@*}; desk_ip=${WWHD_DESKTOP_SSH#*@}
cat > ~/.ssh/config <<EOF
Host worker
    HostName $WWHD_WORKER_ADDR
    User $WWHD_WORKER_USER
    IdentityFile ~/.ssh/wwhd_cloud_worker
    IdentitiesOnly yes
    ProxyCommand tailscale --socket=$sock nc %h %p
    StrictHostKeyChecking accept-new
    ServerAliveInterval 30
Host $desk_ip
    User $desk_user
    IdentityFile $desk_key
    IdentitiesOnly yes
    ProxyCommand tailscale --socket=$sock nc %h %p
    StrictHostKeyChecking accept-new
    ServerAliveInterval 30
Host pc-deploy
    HostName $desk_ip
    User $desk_user
    IdentityFile ~/.ssh/wwhd_cloud_deploy
    IdentitiesOnly yes
    ProxyCommand tailscale --socket=$sock nc %h %p
    StrictHostKeyChecking accept-new
EOF
chmod 600 ~/.ssh/config

# 3. This checkout as a parallel session: its own worker directory and name, hosts.env, the worker remote
name=${1:-${WWHD_CLOUD_SESSION:-$(cat "$root/.session" 2>/dev/null || echo cloud)}}
case "$name" in cloud|cloud[0-9]*) ;; *) echo "tailnet-setup: the session name must be cloud, cloud2, cloud3..." >&2; exit 2 ;; esac
echo "/wwhd/WWHDRecomp-$name" > "$root/.worker-dir"
echo "$name" > "$root/.session"
echo "tailnet-setup: session $name, worker directory /wwhd/WWHDRecomp-$name"
printf 'WWHD_WORKER_SSH=worker\nWWHD_DESKTOP_SSH=%s\nWWHD_DESKTOP_HOME=%s\nWWHD_PROGRESS_URL=http://%s:8765\n' \
    "$WWHD_DESKTOP_SSH" "$WWHD_DESKTOP_HOME" "$WWHD_WORKER_ADDR" > "$root/tools/worker/hosts.env"
rm -f "$root/tools/worker/desktop.env"            # the older file: hosts.env has its two lines
git -C "$root" remote get-url worker >/dev/null 2>&1 || git -C "$root" remote add worker worker:/wwhd/git/WWHDRecomp.git
git -C "$root" config user.name flossbud
git -C "$root" config user.email 224492734+flossbud@users.noreply.github.com

# 4. Check both paths
ssh -o BatchMode=yes -o ConnectTimeout=20 worker 'echo "worker: $(docker ps --format "{{.Names}}" | grep -c wwhd-worker) worker container"'
echo "deploy receiver: $(ssh -o BatchMode=yes -o ConnectTimeout=20 pc-deploy status)"
if [ -n "${WWHD_CLOUD_DESKTOP_KEY:-}" ]; then
    echo "desktop worker: $(ssh -o BatchMode=yes -o ConnectTimeout=20 "$WWHD_DESKTOP_SSH" "podman container inspect -f '{{.State.Running}}' wwhd-worker 2>/dev/null || echo 'not running'")"
fi
git -C "$root" fetch -q worker && echo "worker remote: main at $(git -C "$root" rev-parse --short worker/main 2>/dev/null || echo '(none yet)')"
echo "tailnet-setup: ready (WWHD_ON=${WWHD_ON:-unset: the desktop while its worker runs, else the worker})"
