#!/usr/bin/env bash
# (Re)build the worker image on the worker from this directory's Dockerfile.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
source "$here/hosts.sh"                     # the worker's address (tools/worker/hosts.env)
ssh "$WWHD_WORKER_SSH" 'mkdir -p /wwhd/worker'
rsync -a "$here/Dockerfile" "$WWHD_WORKER_SSH:/wwhd/worker/Dockerfile"
ssh "$WWHD_WORKER_SSH" 'docker build -t wwhd-worker:latest /wwhd/worker'
