# WWHD worker (the worker)

**Every heavy WWHD job runs here, never in the editing machine container.** On 2026-09-28 a Cemu
determinism run plus a trace reader that loaded the whole trace into RAM pushed the editing machine
(a container on the host, 10 GB while the Windows VM runs) to 8 GB of RAM plus 1.9 GB of swap. The
container thrashed and had to be shut down.

The Claude session and git history stay on the editing machine. Builds, Ghidra, Cemu, traces and
compiling generated code run in a resource-capped Docker container on the worker, reached over
SSH.

## What is where

| | Where | Limit |
|---|---|---|
| Container `wwhd-worker` | the worker Docker (`tools/worker/start.sh`) | 24 GB RAM with no swap, 10 of 12 CPU threads, 4096 processes |
| GPU | Intel Intel iGPU via `/dev/dri` (host groups video 44, render 992) | shared |
| Volume `/wwhd` | 250 GB sparse ext4 image `/var/lib/wwhd/wwhd.img`, loop-mounted (`/etc/fstab`, `nofail`) | hard 250 GB cap, so it can't fill the worker's root disk |
| Repo checkout | `/wwhd/WWHDRecomp` (pushed by `sync.sh up`) | |
| Bare repo | `/wwhd/git/WWHDRecomp.git` (git remote `worker`) | |
| Game dump | `/wwhd/data/rom/*.wua`, extracted to `/wwhd/data/orig` (never in git) | |
| Tools | `/wwhd/opt`: Ghidra 12.0.4 + RPX loader, `cemu-src` (patched reference Cemu) | |
| Caches, home | `/wwhd/home`, `/wwhd/cache` | |

the worker keeps about 7 GB of RAM and 2 threads for its own jobs: Tailscale subnet router, backup
monitoring, other services. An out-of-memory job is killed inside the container; the worker and
the editing machine keep running.

**Power cap (read this before blaming a build).** the worker is an a mini PC
(6-core CPU, 65 W desktop CPU) on a 90 W adapter.

* **The problem:** the firmware allows 65 W sustained and 96 W bursts for the CPU alone, so
  all-core load pushes the whole machine past 90 W. The adapter latched off, dead until unplugged
  and replugged, on 2026-09-07 and again on 2026-09-28 during this worker's first Cemu build.
* **The fix:** `/etc/systemd/system/cpu-power-cap.service` caps the package at 45 W sustained and
  60 W burst, at every boot.
* **Verified:** under a 10-thread burn the package measured 57–58 W for the first ~20 s, then held
  45 W at 71–75 °C.
* **Undo:** `sudo systemctl disable --now cpu-power-cap && sudo reboot`.
* **If the adapter latches off again anyway**, replace it.

## Use (from the editing machine checkout)

```sh
tools/worker/sync.sh                     # push the working tree (committed or not) to the worker
tools/worker/w tools/ghidra/rebuild.sh   # run any command in the container, from the checkout
tools/worker/w -t bash                   # interactive shell
tools/worker/job start NAME CMD...       # long jobs: detached, own process group, log /wwhd/logs/NAME.log
tools/worker/job wait NAME [MIN]         #   block until done (timeout), print tail; status / stop / tail too
tools/worker/sync.sh down config/US_v0/functions.csv   # bring generated results back
git push worker <branch>               # off-machine copy of the history
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
# from the editing machine
tools/worker/image.sh                     # build the image (Dockerfile here)
tools/worker/start.sh                     # create the capped container
tools/worker/sync.sh                      # push the repo
rsync the .wua to worker:/wwhd/data/rom/
tools/worker/w tools/worker/setup-volume.sh all   # extract game files, install Ghidra, build Cemu
```

SSH from the editing machine uses `~/.ssh/worker_ed25519` (alias `worker`). That key is
authorized on the worker only from the editing machine's IPs (`from="EDITING_ADDR,LAN_ADDR"`).

**Growing the volume:** stop the container, then `sudo umount /wwhd`,
`sudo truncate -s 400G /var/lib/wwhd/wwhd.img`, `sudo e2fsck -f` and `sudo resize2fs` on the
image, then `sudo mount /wwhd`.
