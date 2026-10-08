# WWHD worker

**Every heavy WWHD job runs on the worker, never on the editing machine.** A Cemu determinism run
plus a trace reader that loaded the whole trace into RAM once ran the editing machine's container
out of memory, and it had to be shut down.

The Claude session and git history stay on the editing machine. Builds, Ghidra, Cemu, traces and
compiling generated code run in a resource-capped Docker container on the worker, reached over
SSH. The machines' addresses and logins live in `tools/worker/hosts.env` (gitignored; copy
`hosts.env.example` and fill it in), which `tools/worker/hosts.sh` reads for every script here.

## What is where

| | Where | Limit |
|---|---|---|
| Container `wwhd-worker` | the worker's Docker (`tools/worker/start.sh`) | 24 GB RAM with no swap, 10 of 12 CPU threads, 4096 processes |
| GPU | the Intel iGPU via `/dev/dri` (host groups video 44, render 992) | shared |
| Volume `/wwhd` | 250 GB sparse ext4 image `/var/lib/wwhd/wwhd.img`, loop-mounted (`/etc/fstab`, `nofail`) | hard 250 GB cap, so it can't fill the host's root disk |
| Repo checkout | `/wwhd/WWHDRecomp` (pushed by `sync.sh up`), or the path in the local checkout's `.worker-dir` (parallel sessions: one each, with its own `build/`) | |
| Bare repo | `/wwhd/git/WWHDRecomp.git` (git remote `worker`) | |
| Game dump | `/wwhd/data/rom/*.wua`, extracted to `/wwhd/data/orig` (never in git) | |
| Tools | `/wwhd/opt`: Ghidra 12.0.4 + RPX loader, `cemu-src` (patched reference Cemu) | |
| Caches, home | `/wwhd/home`, `/wwhd/cache` | |

The host keeps about 7 GB of RAM and 2 threads for its own jobs. An out-of-memory job is killed
inside the container; the host and the editing machine keep running.

**Power cap (read this before blaming a build).** The worker's CPU is capped by
`/etc/systemd/system/cpu-power-cap.service` at 45 W sustained and 60 W burst, at every boot,
because uncapped all-core load draws more than its power supply delivers. Don't undo it. Under a
10-thread burn the package holds 45 W after ~20 s at 57-58 W. So the worker is slower and noisier
than its thread count suggests: compare timings of the same build only, with more rounds.

## Use (from the editing machine's checkout)

```sh
tools/worker/sync.sh                     # push the working tree (committed or not) to the worker
tools/worker/w tools/ghidra/rebuild.sh   # run any command in the container, from the checkout
tools/worker/w -t bash                   # interactive shell
tools/worker/job start NAME CMD...       # long jobs: detached, own process group, log /wwhd/logs/NAME.log
tools/worker/job wait NAME [MIN]         #   block until done (timeout), print tail; status / stop / tail too
tools/worker/sync.sh down config/US_v0/functions.csv   # bring generated results back
git push worker <branch>                 # off-machine copy of the history
```

**Long jobs always go through `tools/worker/job`**, never `docker exec -d … &` plus `pkill`.
A pattern-based kill once matched another job's wrapper and silently cancelled it. `job stop`
kills exactly one job's process group.

## Setup (already done once; kept for rebuilding)

```sh
# on the worker (owner has passwordless sudo): the capped volume
sudo truncate -s 250G /var/lib/wwhd/wwhd.img && sudo mkfs.ext4 -L wwhd -m 1 /var/lib/wwhd/wwhd.img
echo '/var/lib/wwhd/wwhd.img /wwhd ext4 loop,nofail,noatime 0 2' | sudo tee -a /etc/fstab
sudo mkdir /wwhd && sudo mount /wwhd && sudo chown owner:owner /wwhd
# from the editing machine (tools/worker/hosts.env filled in)
tools/worker/image.sh                     # build the image (Dockerfile here)
tools/worker/start.sh                     # create the capped container
tools/worker/sync.sh                      # push the repo
rsync the .wua to the worker's /wwhd/data/rom/
tools/worker/w tools/worker/setup-volume.sh all   # extract game files, install Ghidra, build Cemu and SDL3
```

SSH from the editing machine uses a key of its own (an `~/.ssh/config` alias, `WWHD_WORKER_SSH` in
`hosts.env`), authorized on the worker only from the editing machine's addresses.

**Growing the volume:** stop the container, then `sudo umount /wwhd`,
`sudo truncate -s 400G /var/lib/wwhd/wwhd.img`, `sudo e2fsck -f` and `sudo resize2fs` on the
image, then `sudo mount /wwhd`.

## The second worker: the owner's desktop (the default while it's lent)

`tools/worker/desktop.sh start|stop|status` runs the same image on the owner's PC under rootless podman
(`/wwhd` = `~/wwhd-desk`; all 24 threads, 28 GB; a `wwhd-awake` user unit blocks sleep while it runs). While
it runs, `sync.sh`, `w`, `job start` and `publish.sh shot` use it by themselves (`tools/worker/target.sh`);
otherwise the worker. `WWHD_ON=worker|desktop` forces one. It has the checks' references and the Ghidra
project, so everything runs there. `tools/worker/cleanup.sh [--apply] [install]` frees old test outputs on
both (cron on the worker, a user timer on the desktop).
