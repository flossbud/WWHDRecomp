#!/usr/bin/env bash
# tailnet-setup.sh - a cloud session joins the owner's tailnet and gets the workers (docs/cloud-handoff.md).
# Run it at the start of every cloud session (it does nothing when already set up): bash tools/cloud/tailnet-setup.sh
#
# Tailscale runs in userspace (no TUN device, no root network setup); SSH goes through `tailscale nc`. The tailnet's
# rules let this node (tag:wwhd-cloud, an ephemeral key) reach the worker's SSH and the owner's PC's SSH only, and the
# PC's key runs nothing but its deploy receiver (tools/play/deploy-cloud.sh). The cloud environment sets:
#   TS_AUTHKEY             the tailnet key (ephemeral, pre-approved, tagged tag:wwhd-cloud)
#   WWHD_CLOUD_SSH_KEY     the worker's key for this session (the private key, base64)
#   WWHD_CLOUD_DEPLOY_KEY  the PC's deploy key (the private key, base64)
#   WWHD_WORKER_USER     the login on the worker
#   WWHD_DESKTOP_SSH       user@DESKTOP_ADDR, the PC (for deploy-cloud.sh only)
#   WWHD_DESKTOP_HOME      that account's home (tools/worker/desktop-env.sh)
#   WWHD_CLOUD_DESKTOP_KEY the PC's full login key (base64; the owner's choice, 2026-10-08): with it the desktop
#                          worker is usable too (WWHD_ON unset: the desktop while its worker runs, else the worker)
#   WWHD_ON                optional: worker or desktop forces one
# None of these values go into git (the logins are the owner's account names).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
st=$HOME/.tailscale-wwhd
sock=$st/tailscaled.sock
for v in TS_AUTHKEY WWHD_CLOUD_SSH_KEY WWHD_CLOUD_DEPLOY_KEY WWHD_WORKER_USER WWHD_DESKTOP_SSH WWHD_DESKTOP_HOME; do
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
    HostName WORKER_ADDR
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

# 3. This checkout as a parallel session: its own worker directory and name, the desktop's login file, the remote
echo /wwhd/WWHDRecomp-cloud > "$root/.worker-dir"
echo cloud > "$root/.session"
printf 'WWHD_DESKTOP_SSH=%s\nWWHD_DESKTOP_HOME=%s\n' "$WWHD_DESKTOP_SSH" "$WWHD_DESKTOP_HOME" > "$root/tools/worker/desktop.env"
git -C "$root" remote get-url worker >/dev/null 2>&1 || git -C "$root" remote add worker worker:/wwhd/git/WWHDRecomp.git
git -C "$root" config user.name flossbud
git -C "$root" config user.email 224492734+flossbud@users.noreply.github.com

# 4. Check both paths
ssh -o BatchMode=yes -o ConnectTimeout=20 worker 'echo "worker: $(hostname), $(docker ps --format "{{.Names}}" | grep -c wwhd-worker) worker container"'
echo "deploy receiver: $(ssh -o BatchMode=yes -o ConnectTimeout=20 pc-deploy status)"
if [ -n "${WWHD_CLOUD_DESKTOP_KEY:-}" ]; then
    echo "desktop worker: $(ssh -o BatchMode=yes -o ConnectTimeout=20 "$WWHD_DESKTOP_SSH" "podman container inspect -f '{{.State.Running}}' wwhd-worker 2>/dev/null || echo 'not running'")"
fi
git -C "$root" fetch -q worker && echo "worker remote: ww-4 at $(git -C "$root" rev-parse --short worker/ww-4)"
echo "tailnet-setup: ready (WWHD_ON=${WWHD_ON:-unset: the desktop while its worker runs, else the worker})"
