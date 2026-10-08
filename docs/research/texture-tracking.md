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

## The runtime half, built (session cloud2, 2026-10-08)

`src/runtime/write_watch.{h,cpp}` (namespace `wwhd::rt::write_watch`), in `wwhd_runtime` but called by nothing yet.
Off unless `WWHD_WRITE_WATCH=1`; unit tests without the game or Cemu: `src/runtime/tests/run.sh` (clang and gcc,
-O2 and -O0, all pass in the cloud sandbox).

- **API.** `Init(base, size)` (the region, page tables, the handler), `ThreadInit()` (a 256 KB `sigaltstack` with a
  guard page, freed at thread exit), `Mark()`, `Protect(p, n)` (returns false when `mprotect` fails, e.g.
  `vm.max_map_count`: hash whole then), `Unprotect`, `WrittenSince(p, n, mark)`, `LastStamp`, `HostWrite` (a scope),
  `GetStats()`.
- **Stamps are a global sequence, not the frame counter**: every way a protected page becomes writable (a fault,
  `Unprotect`, `HostWrite`'s start) takes the next number, and `HostWrite` stamps again at its end. So "written after my
  mark" is exact, whatever the frame count, and a texture sharing a page with one that was unprotected re-hashes once
  instead of missing a later write.
- **Races.** Page states and `mprotect`s change under one spinlock, which the handler takes too (no thread can fault
  while holding it, so no self-deadlock). Two threads faulting on the same page: the second finds it already writable
  and retries the store (`raced`); the same address faulting 1000 times in a row from one thread is taken as not
  writable after all and chained. Pages inside a `HostWrite` scope are pinned: `Protect` skips them, so a kernel write
  in progress never gets `EFAULT` because a watcher protected its page meanwhile.
- **Chaining.** The previous `SIGSEGV` action is read before ours goes in. A fault outside the region, on a page we
  never protected, or one we can't make writable goes to it: an `SA_SIGINFO` handler (Cemu's
  `handlerDumpingSignal`) is called directly with the same `siginfo` and context; `SIG_DFL` is put back and the access
  re-faults into a core dump. Cemu's handler resets to `SIG_DFL` and re-raises, so its crash log and dump still
  happen. The handler runs with `SA_ONSTACK`; its own stack use is tiny, the 256 KB is for Cemu's handler
  (`backtrace_symbols`, demangling, fmt) when a crash is chained.
- **Threads and fibers.** The handler keeps no per-fiber state, and its per-thread state (the race counter, the
  alternate stack's bounds for the stats) belongs to the host thread. A fiber that moves between host threads faults
  on whichever it's on, on that thread's alternate stack. Tested: 8 threads writing protected pages (a shared one
  each round, ~1250 raced faults of ~1650), and a fiber writing a protected page with under 1.5 KB of its 64 KB stack
  left, on one host thread, then parked and resumed on another; a control in a child process shows that same write
  without `ThreadInit` kills the process (the signal frame doesn't fit), so the alternate stack is what saves it.

**How it plugs in (needs the game; not done):**
1. `ThreadInit()` at the top of `OSSchedulerCoreEmulationThread` (`src/os/coreinit/coreinit_Thread.cpp`, after
   `enableFlushDenormalsToZero`): every host thread that runs guest code starts there, one or Cemu's three. Other
   threads that write guest memory without running guest code (the GX2/render thread, sound) fault on their own
   ordinary stacks, which is fine; `ThreadInit` there is optional.
2. `Init(memory_base, 4 GB)` once on the main thread, after Cemu's `ExceptionHandler_Init` (in its `main`) and after
   the title is prepared (`frontend/cemu_boot.cpp`), before the scheduler threads start; only in real time
   (`!PPCTimer_isVirtualClock()`), so the checks never run it.
3. `texture.cpp` (`Get`'s check, around `SampleMemory`): when `Active()`, a texture keeps `mark`; its check is
   `t.protectedOk && !WrittenSince(p, n, t.mark)` (skip), else `t.mark = Mark(); t.protectedOk = Protect(p, n);` then
   the whole hash as today. `kWholeEvery`/`kWholeBudget`/`SampleMemory` stay for when it's off. A stamp-rate cap: a
   texture re-stamped in, say, 8 frames running stays unprotected and hashes whole (no fault storm).
4. `HostWrite` around kernel writes into guest memory: `fsc_readFile` into `destPtr` in `FSAProcessCmd_read`
   (`src/os/iosu/iosu_fsa.cpp`) first; then the audit (anything `read()`/`fread()`/`recv()`ing into guest memory,
   the `.wua` reader below `fsc`).
5. The verify mode (`WWHD_TEXTURE_HASH=verify`): Init forced on with the virtual clock too, every texture hashed whole
   as now, and a count of hash changes with no stamp after the mark (the missed writers), reported by the checks.
6. `GetStats()` in the frame log or at exit: faults a frame, raced, chained (should stay 0 on a clean run).

Not covered yet: Windows (a vectored handler and `VirtualProtect`) and macOS (`SIGBUS`, compiled in but untested).
The handler assumes a page it protected was read-write before (true of committed guest memory).
