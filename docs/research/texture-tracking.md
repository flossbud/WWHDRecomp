# Texture change tracking by page write-protection: a design note

Session qa, 2026-10-08, for the Steam Deck work (B75). No code yet. The rival's change detection
(`docs/research/rival-study.md` §4.3): after a full check, a texture's host pages are made read-only; the first CPU
write faults once, stamps the page and makes it writable again; a lookup hashes the texture again only when one of
its pages carries a newer stamp. About 2 faults a frame in steady play, ~3.7 µs each on an M3 Max. They dropped
sampled hashing because a change "showed up to 63 frames late", and two runs reaching a scene at different frame
counts rendered it differently.

## What it replaces here

`src/gpu/vk/texture.cpp`, real time only (the virtual clock already hashes every texture whole every frame):

- `SampleMemory` on each texture's first use in a frame: its first and last 256 bytes and 64 words spread over it.
  A change between those words is missed until the texture's whole turn comes;
- the whole-texture rotation: one whose sample changed, and each at most every `kWholeEvery` (30) frames within
  `kWholeBudget` (4 MB) a frame: the lag of up to a second, and the frame-count dependence the rival saw.

With write-watch, a texture's check in a frame becomes "is any of its pages stamped after my last hash?" (an array
lookup per page, ~1-50 pages a texture), and a whole hash only when one is. Measured cost of what goes
(`docs/research/perf-baseline.md`): the memory check is ~19% of `SampleTexture`'s own time, ~1.3% of the render
thread on the save route; small on the desktop, larger in proportion on a Deck core. The main gain is exactness in
real time (no lag, the same picture whatever the frame count), and it drops a tuning knob (`kWholeEvery`,
`kWholeBudget`) whose right value differs by machine.

## How it sits with the signal handling

- **Who writes guest memory.** The recompiled code writes through `memory_base + ea` (`ppc_ops.h` `wr8`-`wr64`, plus
  `psq_store` and the `RT_STORE` journal hook, which stays before the store), so a protected page faults on the
  game's own write. Host code writing guest memory through host pointers (coreinit's `memcpy`/`memset` HLE, GX2 and
  the DMA engine, snd_core, the render thread's surface copies into guest memory) faults the same way: the handler
  doesn't care which thread. **Kernel writes don't fault**: a `read()`/`fread()` into a protected page fails with
  `EFAULT`. Cemu's file system path (FSReadFile and the `.wua` reader filling guest buffers) must bracket its
  destination: `WriteWatch::HostWrite(ptr, size)` unprotects and stamps first (the rival's `HostWrite`). That audit,
  every syscall whose buffer can be guest memory, is the risky part of the work.
- **Who handles SIGSEGV.** Ours today: only `SIGPROF` (the profiler, `runtime/profile.cpp`) and diff mode's
  `longjmp` path (no signal). Cemu's crash handler (`Common/ExceptionHandler/ExceptionHandler_posix.cpp`) takes
  `SIGSEGV` and `SIGBUS` for its crash dump. The write-watch handler installs after it with `SA_SIGINFO | SA_ONSTACK`,
  keeps the previous action, and for a fault outside the watched range (or on a page it didn't protect) restores and
  re-raises into the old handler, so crashes still dump. Windows: a vectored exception handler first in line; macOS:
  `SIGBUS` as well.
- **Fibers.** Guest threads run on our fibers (`runtime/fiber`), on stacks we allocate: the handler runs on the
  faulting stack unless each host thread that runs guest code has a `sigaltstack` (one per host thread, set where the
  thread starts). Required, since a fault can come deep in a fiber with little stack left.
- **The handler itself** is async-signal-safe: an atomic per-page stamp (4 GB / 4 KB = 1 M pages of `uint32`, 4 MB;
  or per region, MEM1 and MEM2 only) set to the frame counter, then `mprotect` back to read-write (a syscall, the
  standard pattern for this on Linux), then return; the faulting store re-executes.
- **Protecting.** The render thread, after a whole hash, `mprotect`s the texture's pages read-only (page-rounded:
  neighbours of a texture on the same page fault too, just a stamp). A write racing the hash faults after it and
  stamps: the next frame hashes again. Guest memory is Cemu's `util/MemMapper/MemMapperUnix.cpp`: a range reserved with
  `mmap(PROT_NONE)` and committed by `mprotect`, no huge-page hints, so pages are 4 KB and `mprotect` works per page.

## Staying exact for the checks build

The checks (traces, command streams, sound, captures) run on the virtual clock, where every texture is hashed
whole every frame. Keep that as the reference and add a verify mode: `WWHD_TEXTURE_HASH=verify` (on in the checks)
runs write-watch and the whole hash both, and counts a texture whose hash changed while none of its pages was
stamped (a missed writer). The checks report must show 0 misses; a miss names the texture's address, which leads to
the unbracketed writer. Captures stay byte-identical by construction (the whole hash still decides in checks).
Real time gets write-watch alone once the verify mode is clean on every route (predeploy's and the fights).

## Effort

- `write_watch.{h,cpp}` in `src/runtime`: stamps, protect/unprotect, the handler with chaining, `HostWrite`, a
  `sigaltstack` per guest host thread: ~250 lines, half a day.
- `texture.cpp`: the lookup by stamps, protect after hashing, the verify mode: ~80 lines, a few hours.
- The kernel-write audit (Cemu's FS and the `.wua` reader, anything `read()`ing into guest memory): half a day to a
  day; the verify mode finds what the audit misses.
- Gates: checks with verify on (0 misses), predeploy, an A/B (`perf-ab.sh`) of the render thread on continue and
  house, and a profile.
- Total ~2 days on Linux; Windows (vectored handler, `VirtualProtect`) and macOS (`SIGBUS`) half a day each later.

Risks: an unbracketed kernel write (caught by verify, but only on routes that reach it), a third-party library
catching `SIGSEGV` itself (Vulkan loaders and drivers can; chaining must stay first), and fault storms on a texture
the game rewrites every frame (a stamp-rate cap: a page faulting every frame stays unprotected and is hashed whole,
as today).
