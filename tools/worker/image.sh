#!/usr/bin/env bash
# (Re)build the worker image on the worker from this directory's Dockerfile.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ssh worker 'mkdir -p /wwhd/worker'
rsync -a "$here/Dockerfile" worker:/wwhd/worker/Dockerfile
ssh worker 'docker build -t wwhd-worker:latest /wwhd/worker'
