# ZeldaWWHDRecomp: technical study and comparison with WWHDRecomp

*Read-only study, 2026-10-07. Rival: https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp, shallow
clone (50 commits) at `ffb7a12` ("README: What's new in v0.2.6", 2026-10-07 17:52 +0200). Repository
created 2026-10-01; 239 stars, 26 forks, about 60 issues and PRs in its first week. Nothing from it was
built or run. Paths below are relative to its root unless they start with `src/`, `docs/`,
`config/` or `tools/recomp/runtime` (ours).*

---

## 0. Summary

* **The same binary.** They target USA v0 `cking.rpx`, with the same SHA-256 as ours. Text addresses are
  identical: their hook list names the same functions at the same addresses as our symbols and
  overrides. Guest *data* layouts differ: their OS objects, import data and heaps are in other places,
  so their guest pointers and traces can't be compared with Cemu's.
* **The CPU side is close to ours.** Python emits one C function per guest function, taking a
  `Cpu*` register struct with `__restrict`. Branches inside a function are `goto`s and tail calls use
  `musttail`. Memory sits at a *compile-time constant* host base. Generated code is built at `-O3`
  with no cycle counting, and preemption is checked only at function entry.
  The real difference is threads: **every guest thread is a host pthread**, and a per-emulated-core
  ownership lock reproduces Cafe OS priority rules. So the three guest cores run in parallel on the
  host, but nothing is deterministic.
* **"Native graphics" is not API-level mapping.** Their GX2 calls are hand-written. Each one writes
  *Latte register values* (built with Cemu's register structs) into a command stream of their own
  (native-endian, about 20 opcodes). A render thread replays that stream into a Latte-layout register
  file, and a Cemu-style renderer turns registers into Vulkan or Metal state. Shaders are translated
  at runtime by Cemu's vendored decompiler (GLSL then glslang for Vulkan, MSL for Metal), with a disk
  cache. So it is our architecture minus the PM4 encode and parse step, and minus exactness: their
  front half is a reimplementation that has had real divergence bugs.
* **Their 60 fps is mostly interpolation.** "60 fps" (the recommended mode) keeps logic at 30 Hz and
  draws every other frame halfway: camera inputs, J3D model world matrices, particles, sea, weather,
  cloth and material animation frames are blended. "True 60" (experimental) covers only Link's
  locomotion and sword procedures and the follow camera, as a *preview that is rolled back*: half
  passes compute a dt=0.5 preview, it is drawn, and at the next full pass it is undone and the exact
  30 Hz step runs. That is the speculative rollback design our owner ruled out in D21. We have
  converted about 401 process types with a real mixed-rate step whose results stand. They have
  converted 2 subsystems.
* **Where they are clearly ahead:** product and platforms (macOS, Windows, Linux x86-64 and arm64,
  Android build-it-yourself, a portable installer that compiles the game on the user's machine in
  about 2 minutes), features (save states, crash recovery with input replay, ImGui settings overlay,
  aspect ratio Hor+ with HUD anchoring, internal resolution 1-3x, AO fixes, FXAA, AF, mods and Cemu
  graphics-pack import, gyro, GamePad screen modes), and **renderer CPU engineering with measured
  numbers**. That engineering covers write-protect page tracking for texture and buffer changes, a
  guest buffer cache, asynchronous present and lazy DrawDone, draw batching, a render-thread profiler
  and 15 memo or skip paths.
* **Where we are clearly ahead:** verification (bit-exact OS-call traces, GPU command streams, sound
  and captures against Cemu; diff mode), exact OS behaviour (Cemu's own gx2 and scheduler code,
  forked), our shader pipeline (narrow keys from the start, a shipped shader list, a first start
  without hitches), and true 60 coverage.

---

## 1. Target binary and address compatibility

**Evidence.** `tools/installer/setup.py:68-72`:

```python
# SHA-256 of code/cking.rpx of that version (The Wind Waker HD, USA, 00050000-10143500, v0). ...
SUPPORTED_RPX_SHA256 = "c4f0ab300542e0bfc462696850534e71db2ad02288a7eb55e5a4cd4062f16153"
```

This is the hash in our `orig/README.md`. Setup refuses other versions, including an update merged
into a .wua. The runtime also checks at start that `game/code/cking.rpx` matches the generated code
(README "Windows").

**Loader.** They load the RPX themselves (`runtime/src/core.cpp:177 load_rpx`) and place sections at
their link addresses, as Cemu's RPLLoader does: text at `0x02000000`, data from `0x10000000`. The
dispatch table covers `0x02000000 + 16 MiB` (`core.cpp:219`).

**Function addresses match ours.** Some examples from `tools/recomp/hooks.txt`, compared with ours:

| Rival hook | Their name | Ours (symbols.csv / design doc / overrides) |
|---|---|---|
| `025F172C` | main loop body | m_Do_main's frame body `f_025F172C` (D21) |
| `0203593C` | per-frame function | the game task's calc `f_0203593C` (D21) |
| `0200ECD4`…`0200F8D0` | cLib_addCalc … chaseAngleS | the same addresses (D21, sixty_step.cpp) |
| `027F2FC4`, `027F2BF8` | J3DFrameCtrl::update, checkPass | `f_027F2FC4`, `f_027F2BF8` |
| `025D67A8`, `025D6800` | fopAcM_calcSpeed, posMove | `f_025D67A8`, `f_025D6800` |
| `025E3EC8` | decOldFrameMorfCounter | `f_025E3EC8` |
| `0207A9A0` | cLib_calcTimer<u8> | `f_0207A9A0` |
| `0200E240` | cCcS::Set | `f_0200E240` (dCcS::Set) |
| `0255E854`, `020357CC`, `02614F74`, `0270870C`, `0278FEBC` | per-frame children | the same tick rules (D21) |
| `0282167C` | JPAEmitterManager::calc | (our particles: `f_025A81A0` calc3D, `f_0281F878`) |

Differences that matter if we borrow their lists:

* **Function boundaries.** Their discovery is branch-target analysis plus a fixpoint
  (`tools/recomp/recomp.py` `_fixpoint`). It merges functions that are reached only by tail calls
  into their predecessor; their decomp notes say the matcher has to split 1,932 of them. Ours come from
  Ghidra (`config/US_v0/functions.csv`, about 40,000). A rival "function" address is always one of our
  entries, but not every one of our entries is one of theirs.
* **Wrapper level.** They hook `fpcM_Execute` at `025DF940` and `fpcM_Draw` at `025DF904`, the
  thin callbacks passed to the process iterators. We override `f_025DE58C` and `f_025DE2CC`. Same
  code, a different layer.
* **Data addresses do not match Cemu.** Data imports get runtime storage at `0xC1000000 + i*0x1000`
  (`recomp.py`: `DATA_IMPORT_BASE`). Import stubs are at `0xC0000000`. Heaps and OS objects come
  from their own allocators, not Cemu's SysAllocator layout. Guest pointers, and anything traced, differ
  from the reference's. The *static* .data and .bss addresses that the game links (for example
  `g_dComIfG_gameInfo` 0x1046F0B0, `cM_rnd` state 0x101FF9D4) are the same as ours.

---

## 2. Their recompiler, compared with ours

### 2.1 Translation (`tools/recomp/recomp.py`, 278 lines; `tools/recomp/ppc2c.py`, 581 lines)

* **Output:** plain C11. `code_NNN.c` holds about 30,000 instructions per file, plus `funcs.h`,
  `table.c` (address to function), `imports.c` (weak stubs) and `report.txt`. Each function:

  ```python
  out += ["void %s(Cpu* __restrict c) {" % fname, "    PPC_ENTER(0x%08Xu);" % start]
  ...  # one C statement per instruction, labels L_XXXXXXXX for intra-function targets
  out.append("    MUSTTAIL return %s(c);" % nxt)   # fall through into the next function
  ```
* **Registers:** a `Cpu` struct of their own (`runtime/include/ppc.h`): `r[32]`, `lr`, `ctr`, `cr[32]`
  (one byte per bit, as Cemu and we do), XER bytes, `f[32].{ps0,ps1}` doubles, `fpscr`, `gqr[8]`, the
  reservation, `pc`, `core` and `thread`. It is always memory-resident through `Cpu* __restrict c`, the
  same model as our D2 (where we use Cemu's `PPCInterpreter_t` itself). There are no host locals and no
  liveness analysis.
* **Calls:** a direct `bl` to a known entry becomes `c->lr = addr+4; f_X(c);`. An unconditional `b` to
  another function becomes `MUSTTAIL return f_X(c);`. An unknown target sets
  `c->pc = ...; ppc_dispatch(c);`. `blr` is `return;`. There is no LR check on return: like ours, it
  relies on the GHS ABI.
* **Indirect branches:** `bctr` goes through recovered jump tables as a `switch (c->ctr)` with gotos,
  falling back to `ppc_dispatch` (a flat table indexed by `(addr-0x02000000)>>2`, plus atomic tables
  for import slots and host thunks: `core.cpp:251-275`). `bctrl` and `blrl` always call
  `ppc_dispatch`. We have the same flat table (D5) plus `rt_direct` for CTR calls.
* **Imports:** direct C calls to `imp_<lib>_<name>(c)`. Weak stubs are replaced by strong `HLE(lib, name)`
  definitions in the runtime, so there is no HLE dispatch table, no trampoline, no tracer and no
  300-cycle charge per call. We still go through Cemu's HLE table for checks (D4, D18).
* **Hooks (their overrides):** `hooks.txt` lists entries. The generator emits `f_X` calling
  `hook_X(c)` and keeps the original as `f_X_orig`. `@ADDR` lines add `site_ADDR(c)` *before* the
  instruction (a mid-function hook). That is the same idea as our overrides (D9) and our tick and step
  rules, but their site hook is a free-form C callback, where our rules are typed, checked operations.
* **Memory:** `PPC_MEM_BASE` is a **compile-time constant** (`ppc.h:16-21`):

  ```c
  #if defined(__ANDROID__) || (defined(__linux__) && defined(__aarch64__))
  #define PPC_MEM_BASE ((uint8_t*)0x1000000000ull)   /* 39-bit VA kernels: stay below 512 GiB */
  #else
  #define PPC_MEM_BASE ((uint8_t*)0x200000000000ull)
  #endif
  static inline uint32_t ld32(uint32_t ea) { uint32_t v; memcpy(&v, ppc_ptr(ea), 4); return __builtin_bswap32(v); }
  ```
  A 4 GiB window is reserved at that address (`core.cpp:95-112`: `mach_vm_allocate`, `VirtualAlloc` or
  `mmap` with `MAP_NORESERVE`). Ours uses Cemu's `memory_base`, a global that must be reloaded after
  every call (`tools/recomp/runtime/ppc_ops.h:13`).
* **Endianness:** `bswap` on every access. Floats are stored as `double` and rounded through `(float)`.
* **Floating point and paired singles:** `round25()` on the `frC` operand of single-precision multiplies
  (Espresso's 25-bit multiplicand), `to_single`, `fres` and `frsqrte` from Cemu's tables
  (`runtime/src/espresso_fp.c`, MPL header kept), psq quantisation inlined in `ppc.h`, and
  `-ffp-contract=off`. `fmadds` is `to_single(a*round25(c)+b)` computed in double. That is close to
  Cemu's interpreter, but nothing checks it bit by bit the way our diff mode and fuzzer do.
* **Memory ordering (because threads really run in parallel):** `sync`, `lwsync` and `eieio` become
  real barriers, and `isync` becomes an acquire fence (`ppc2c.py`). `lwarx`/`stwcx.` become a
  host CAS on the big-endian word (`ppc.h` `ppc_stwcx`). We treat all of these as no-ops, which is only
  sound because all guest threads run on one host thread.
* **Time and preemption:** there is no cycle counting at all. `PPC_ENTER` at every function entry:

  ```c
  #define PPC_ENTER(a) do {                                                     \
          if (__builtin_expect(g_ppc_trace, 0)) ppc_trace_enter(a);             \
          if (__builtin_expect(g_core_preempt[c->core], 0)) ppc_preempt(c);     \
      } while (0)
  ```
  Ours charges guest cycles per basic block (with a checked copy of each block for slice ends,
  `RT_FITS`/`RT_CHARGE`). Ours also checks `g_rtJournalOn` on every store (`RT_STORE`), and tick
  and step rules add branches.

### 2.2 Build flags (`CMakeLists.txt:76-82`)

```cmake
add_library(gamecode STATIC ${GEN_SOURCES} ${GEN_DIR}/table.c ${GEN_DIR}/imports.c)
target_compile_options(gamecode PRIVATE -O3 -ffp-contract=off -fno-strict-aliasing -w)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  target_compile_options(gamecode PRIVATE -mcpu=apple-m1)
```

* There is no LTO and no `-march` on x86. On Android, `CPU=oryon-1` (`android/build_native.sh:39`)
  passes `-mcpu`. `-fsigned-char` is forced on arm64 Linux and Android.
* Clang is required for `musttail`. Windows builds use clang (llvm-mingw or Visual Studio's headers),
  never `cl.exe`.
* Release setup compiles the generated C on the user's machine with a pinned compiler that it
  downloads (zig on Linux, 55 MB; llvm-mingw on Windows, 190 MB; Apple's Command Line Tools on Mac). It
  takes **about two minutes**.
* Ours: C++20 at `-O2` with Cemu's flags and ThinLTO bitcode (`src/CMakeLists.txt:102,153`). A full
  recompile of the generated shards takes **about 12 minutes** on the worker (D9).

### 2.3 What makes theirs fast, and how it compares

* **Measured:** "The game's main thread needs about 4 ms of CPU per frame on an M3"
  (`docs/how-it-works.md`). "The recompiled code is about 17% of one core, and the GX2 render thread is
  about 30% busy" on an M4 with interpolation (`docs/performance.md`, "Where the time goes"). Their
  own conclusion: *"the CPU translation isn't the bottleneck on this machine; the renderer work
  matters more on slower CPUs."*
* **Ours:** guest code costs about 3 host cycles per guest instruction (D2). In real time on the
  desktop CPU, the single scheduler thread (all three guest cores) uses 18-33% of a core at 30 fps and
  41-73% at 60 fps. The GPU thread uses 8-18% at 30 and 15-31% at 60 (handoff, "60 fps cost").
* **Per guest instruction, the generated code should cost about the same.** Both use the same
  struct model with `__restrict`. They have no per-block cycle charge and no store-journal check, but
  they pay preemption flags at every function entry. Their real advantages:
  1. three host cores, not one;
  2. a constant memory base;
  3. imports as direct calls;
  4. no virtual-clock machinery.

  Their disadvantages: no determinism, and no exactness check of the translation.

---

## 3. Their OS layer

**Everything is reimplemented natively and is small.** `runtime/src` is about 14,000 lines in all. The
`HLE()` definitions count 122 coreinit functions, 108 gx2, 59 snd_core, 13 padscore, 13 nn_boss,
7 nn_save, and a handful in each of the other libraries. They do not link Cemu's OS libraries.
`docs/how-it-works.md` says "parts of the OS layer are ported from or follow Cemu" (OSThread layout
offsets, AX mixing rules).

* **Threads (`runtime/src/threads.cpp`, 1,652 lines):** each guest thread is a `std::thread` or pthread
  (`struct HostThread { Cpu cpu; std::mutex m; std::condition_variable cv; ... }`). Fibers are never
  used, so they have no context-switch code on any platform. A guest thread runs guest code only while
  it *holds its emulated core* (`CoreSched` × 3: owner, ready deque, mutex, condvar):

  ```cpp
  // Like Cafe OS (and Cemu): strict priority, and equal-priority threads round-robin every time slice
  // (games busy-wait on other threads of the same core and priority). Lower priorities never preempt.
  static constexpr auto kSlice = std::chrono::microseconds(500);
  ```
  A `sched tick` thread wakes every 500 µs and sets `g_core_preempt[core]`. The running thread yields
  at its next function entry (`ppc_preempt` does `core_release` and then `core_acquire`). Their note:
  "Letting same-core threads run truly in parallel corrupted shared data." Synchronisation objects
  (mutexes, events, message queues, alarms) are host objects keyed by guest address. Heaps use
  host-side allocators.
* **Time:** `timebase::now()` comes from `steady_clock`. The guest clock can be offset for save states.
  There is no virtual clock. GPU timestamps return the CPU time base
  (`GX2SampleTopGPUCycle: st64(arg, timebase::guest_now())`).
* **Vsync and flips** (`runtime/src/gx2/gx2_core.cpp:531-580`) are modelled on a 59.94 Hz grid. A flip
  executes on the first vsync at least `swap interval` vsyncs after the previous one, *and* once the
  render thread has finished that frame. `GX2WaitForVsync` sleeps to the next grid point. On macOS it
  sleeps close to the deadline and then spins, with an adaptive window from 0.5 to 2 ms.
* **Audio:** `runtime/src/hle/ax.cpp` (1,069 lines) is its own AX: 96 voices, 3 ms frames, ADPCM and PCM,
  resampling, envelopes, filters, the aux and final-mix callbacks, and both the TV and GamePad devices.
  Output goes through CoreAudio or SDL3.
* **FS:** extracted game files, with case-insensitive resolution on Linux (the game asks for
  `Audiores`). Saves go to a host folder. `FSReadFile` is bracketed with `HostWrite` for page protection.
* **How emulator overhead is avoided:** none of our costs exist there. There is no HLE table hop, no
  trace recorder, no cycle accounting, no fiber switches (the host OS schedules threads) and no global
  scheduler lock: each core has its own mutex. Their one profiled OS hotspot was `OSSendMessage` (about
  26,000 messages a second), which they judged "not worth changing". Our D19 found the task loop's
  switch-to-self spinning (69 million pairs on a route). In their model it presumably costs only host
  CPU on one host core.
* **What it costs them** (README "Status", issues, decomp notes):
  - startup can sit on a black screen for up to a minute (a timing-dependent wait in audio init);
  - an intermittent boot crash (about 1 in 15, every boot on some phones) came from their async
    `GX2CopySurface`. The game's agl code treats the copy as done when the call returns, so a late copy
    zeroed a freed and reused heap block (`docs/decomp-notes.md` "Fixed: intermittent boot crash").
    The fix was to make `GX2CopySurface` wait for the render thread.
  - Their save states can only be taken where every thread is parked at a known HLE wait.

---

## 4. Graphics in depth

### 4.1 The architecture: GX2 calls to their own command words, then a Latte register file, then the renderer

`docs/how-it-works.md`:

> GX2 calls update a register file with the same layout as the Latte GPU (so state is bit-compatible
> with the decompiler) and append compact commands of our own to a queue. Display lists recorded by the
> game use the same encoding. … A render thread turns the commands into Metal work.

`runtime/src/gx2/gx2_cmd.h`:

```cpp
// The encoding is our own (native-endian 32-bit words):
//   word 0: op | (payload word count << 8)
//   word 1..n: payload
enum Op : uint32_t { OP_NOP = 0, OP_SET_REGS, OP_DRAW, OP_DRAW_INDEXED, OP_CLEAR_COLOR, OP_CLEAR_DEPTH,
    OP_CLEAR_BUFFERS, OP_COPY_SURFACE, OP_COPY_TO_SCAN, OP_CALL, OP_SET_CONTEXT, OP_INVALIDATE,
    OP_EXPAND_COLOR, OP_EXPAND_DEPTH, OP_FLUSH, OP_DRAW_DONE, OP_SWAP, OP_SETUP_CONTEXT, OP_FENCE,
    OP_SET_PROJ_REGS, OP_LAYOUT_ROOT, OP_COUNT };
```

The setters build Latte register values with Cemu's register classes (`runtime/src/gx2/gx2_state.cpp`):

```cpp
HLE(gx2, GX2SetBlendControl) {
    LATTE_CB_BLENDN_CONTROL r;
    r.set_COLOR_SRCBLEND((BF)arg(c, 1)); ...
    set_reg(REGADDR::CB_BLEND0_CONTROL + (arg(c, 0) & 7), r.getRawValue());
}
HLE(gx2, GX2DrawEx) { emit(OP_DRAW, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)}); }
```

`emit()` (`gx2_core.cpp:277`) appends to the active guest display list or, outside a list, to a
mutex-protected host vector. A *GX2 render* thread swaps that vector out and executes it
(`render_thread_main`). Executing `OP_SET_REGS` writes the register file (`g_regs[kNumRegs]`).
`OP_DRAW` calls `render::draw(g_regs, prim, count, ...)`. Context states are host copies of the
register file, keyed by the guest `GX2ContextState*`. Struct arguments (color and depth buffers,
surfaces) are copied into the command and written back to scratch guest memory for the renderer
(`unpack_struct`).

**So no, they do not map GX2 calls to Vulkan or Metal directly.** They are a Latte-register-level
renderer, like Cemu's and ours. The only layer removed is PM4: there are no packets to encode or parse
and no command processor. Their render-thread profile shows what that is worth: "register writes 0.9 ms"
of "6.5 ms in ops" per frame (39,000 writes) on an M3 Max at Outset, 60 fps. Most of the time is in the
draw: shader lookup, uniforms, vertex snapshots, textures, pipeline and descriptors.

**What it costs them.** The front half is a reimplementation, so anything the hardware or GX2 does
implicitly has to be modelled. Their own list: "flips waiting for the GPU, aliasing render targets in
the same memory, and depth buffers sampled as textures". Display lists the game records hold *their*
encoding, so `GX2EndDisplayList` sizes differ from real GX2's. For us that alone would break trace
equality (our D12 revision).

**Bugs found by users, not by checks:**
- `GX2CopySurface` timing (the boot crash);
- Picto Box photos black on both renderers, and the camera shot crashing Vulkan (a 3D colour-grading
  texture rendered slice by slice; issue #53, fixed 2026-10-07);
- the decompiler's `strictMul` left off, so the mail-sorting letter card rendered black (issue #28;
  Cemu turns `accurateShaderMul` on by default, and our `src/gpu/vk/draw.cpp:328` follows Cemu's
  profile);
- flags stretched into huge polygons (issue #36, open);
- black shadows after a cold Metal shader cache (issue #47).

### 4.2 Shaders

* Cemu's `LegacyShaderDecompiler` is vendored (`runtime/third_party/cemu/Cafe/HW/Latte/LegacyShaderDecompiler`,
  GLSL *and* MSL emitters, about 12,000 lines), together with the fetch-shader parser and the GS
  copy-shader parser.
* **Vulkan:** microcode is turned into GLSL, then glslang turns that into SPIR-V **at runtime on first
  use**, on the render thread (`gfx/vulkan/shaders.cpp`, "Real Latte microcode -> Cemu GLSL ->
  SPIR-V"). The disk caches are `shadercache/vulkan-shaders/spirv.bin` (it stores the exact GLSL to
  catch collisions) and a `VkPipelineCache` matched by UUID. A snapshot is saved after 120 quiet frames
  on a worker thread.
* **Metal:** microcode becomes MSL, compiled asynchronously by Metal. A draw waits up to 25 ms per
  frame for a pending compile and is then **skipped** (`WWHD_COMPILE_WAIT_MS`). A recipe file
  (`~/Library/Caches/wwhd/shaders.bin`) is replayed at start. An optional "head start"
  (`tools/shaderprep.py`) pre-translates programs from the game's archives using a template of
  register states recorded from the user's own play.
* **Translation counts (v0.2.6, the "narrow keys", `docs/vulkan.md`):**

  | 60 s scripted run, empty cache | before | PR #46 (dedupe identical GLSL) | narrow keys |
  |---|---|---|---|
  | Outset translations / pipelines | 6,127–6,670 / 3,229–3,525 | 6,670 / 339 | **569 / 204** |
  | Windfall translations / pipelines | 4,337–4,981 / 2,323–2,672 | 4,981 / 501 | **829 / 236** |

  The results: 100–115 MB less resident memory, about a third fewer frames over 50 ms, and render CPU
  down 4–12%. Earlier, the Metal cache reached about 260,000 cached shaders for 2,344 programs (about
  111 translations each, 290 MB, 6.2 s replay at boot) because the key included render-target and
  texture state.

  The narrow key is two levels:
  - *linkage:* the program, the fetch shader, the PS input table, the VS semantics, viewport,
    half-Z, points, streamout, and the PS output mask, alpha test and front face;
  - *variant:* the dimension and integer format of each *sampled* unit, the exported parameter
    semantics, and the streamout strides.

  `WWHD_VK_SHADER_KEY_VERIFY=1` re-translates under the wider key and reports `VIOLATION`.
* **Ours already does this:**
  - shader keys from the microcode hash plus the registers the decompiler reads (D20);
  - the save route makes 631 shaders and 430 pipelines;
  - the shipped `shader_list.txt` holds 1,382 shaders, 1,028 programs and 1,000 pipelines, all built
    *before* play from the player's own files (2.3 s cold on the desktop, 0.0 s warm).

  They have nothing like the shipped list. Their pipeline key normalises state the pipeline ignores
  (blend words of attachments that don't blend, stencil words with stencil off, depth bias words with
  bias off), which could shave some of our 430. D20 found 586 distinct shader pairs as the floor of 629,
  so the gain is small.

### 4.3 Textures, surfaces, tiling, render targets and copies

* **Tiling:** Cemu's AddrLib is vendored (`LatteAddrLib.cpp`, `AddrLibFastDecode.h`), along with
  `gx2_surface_calc.cpp` and `gx2_texture_regs.cpp`. Guest surfaces map to `VkImage`s or Metal
  textures keyed by address and descriptor (`gfx/vulkan/surfaces.cpp`: address, mipAddr, size,
  slices, format, dim, tileMode, swizzle and mips). Format mapping is their own (`gfx/vulkan/formats.cpp`).
* **Render targets** stay GPU images (`gpuWritten`). A later texture lookup at the same address uses the
  rendered image. Texture-array targets are supported (shadow cascades). "Feedback images" (a target
  sampled while bound) get a copy per alias.
* **GPU to CPU:** "Nothing writes guest memory from the GPU (no stream-out; render targets stay GPU
  images)". `GX2CopySurface` from a CPU-produced source re-tiles in guest memory on the render thread,
  and now waits for it. Picto photos were black until 2026-10-07.
* **Change detection (2026-10-06): page write-protection** (`runtime/src/write_watch.{h,cpp}`). After a
  full check, a texture's host pages are `mprotect`ed read-only (`VirtualProtect` with a vectored
  exception handler on Windows). The first CPU write faults once, stamps the page and unprotects it. A
  lookup re-hashes the whole texture (xxHash) only if a page carries a newer stamp or the game
  signalled a change. Kernel writes (`fread` into guest memory) are bracketed with `HostWrite`. The
  cost is about 2 faults a frame in steady play, about 3.7 µs each on an M3 Max. They replaced sampled
  hashing because a change "showed up to 63 frames late, and two runs reaching the same scene at
  different frame counts could render it differently (Mirror Shield in the Wind Temple: 668 pixels)".
  **Our real-time path uses exactly that sampled scheme** (`src/gpu/vk/texture.cpp:362-372`:
  `kWholeEvery = 30`, a 4 MB budget); only the virtual clock hashes whole.

### 4.4 Uniforms, attributes and indices: the guest buffer cache

* By default the Vulkan renderer snapshots vertex, index and uniform ranges into a fenced
  upload arena at every draw. Ours does the same (D13).
* **Guest buffer cache** (`gfx/vulkan/buffer_cache_core.h`, 350 lines; default on macOS, opt-in
  elsewhere): persistent GPU copies keyed by guest address. They stay valid while no page in their range
  has a newer write stamp (the same write-watch), plus hint stamps from
  `DCFlushRange`, `DCStoreRange` and `GX2Invalidate(attrib|uniform)`. Converted indices (big-endian,
  fans, quads, restart) are cached too. A range rewritten 3 times within 4 frames goes "dynamic" and
  falls back to the arena, with backoff from 64 to 2048 frames. There is a verify mode, which found 0
  mismatches in 142 million hits.

  | M3 Max, medians | render CPU ms/frame off → on | swaps/s | uploads MiB/frame |
  |---|---|---|---|
  | Outset 30 uncapped | 5.05 → 4.65 | 190.5 → 197.1 | 18.1 → 10.3 |
  | Windfall 30 uncapped | 5.32 → 4.41 | 187.5 → 210.2 | 25.7 → 9.2 |

  The copied guest ranges hold only about 2.2 of 18.6 MiB of unique bytes per frame.
* **Pitfall they hit (issue #44, v0.2.4 to v0.2.5):** comparing new data against the previous copy *in
  mapped upload memory* cost an RX 6700 XT 57 ms a frame, because BAR and write-combined memory is about
  100 times slower to read. One user went from 30 to 12 fps. Their rule now: mapped upload memory is
  written, never read, and shadow copies live in heap memory.

### 4.5 CPU and GPU synchronisation

* GX2 work goes through a queue to the render thread. `render_sync` posts an `OP_FENCE` and waits for
  it, and is used by `GX2DrawDone`, `GX2CopySurface`, flips and save states.
* **Lazy DrawDone** (default since 2026-10-07): `GX2DrawDone` waits only until the render thread has
  *recorded* the work, not for the GPU. That is sound because all guest data is copied into fenced
  slices and nothing is written back. **Async present:** frame N+1 is recorded while the GPU draws
  frame N. Results on an M3 Max: Windfall uncapped went from 103 to 157 swaps/s, and with 7–8 ms of
  added render-thread load, paced 60 fps went from 35–40 swaps/s to 59. Metal keeps the real wait,
  because it binds guest memory directly (`newBufferWithBytesNoCopy`).
* **Draw batching:** every 2,048 draws it submits early, at most 3 times per frame. On Windows, at 3x
  resolution, that took 33 fps to 49–51 fps.
* A **4-slot fenced submission ring** holds per-slot command and descriptor pools and upload arenas.

### 4.6 The Metal backend

The default macOS build has both renderers, chosen at start (`runtime/src/gfx/renderer.h` is a table
of entry points).

* Metal uses the AppKit host: TV and GamePad windows and native menus.
* Shaders come from Cemu's MSL emitter. Pipelines compile asynchronously and draws are skipped while
  a compile is pending.
* Guest memory is wrapped as `MTLBuffer`s without copying (unified memory), so large static meshes are
  read in place.
* Every shader is rewritten to snap 2D texture coordinates to a 1/256-texel grid (`snap_texcoords`),
  and the output is kept byte-stable so that macOS's own shader cache stays valid.

### 4.7 Widescreen (`runtime/src/aspect.cpp`, `tools/recomp/hooks_aspect.txt`)

* **3D:** Hor+ with the vertical FOV kept, or Vert+ for screens narrower than 16:9. Six instruction
  hooks: `@024FFA98` (`camera_execute` stores `view.mAspect` from the 16/9 constant `1004AAF0`),
  `@025020E0` (camera creation), `@024FFD60` (`camera_draw`'s `C_MTXPerspective`), `@024F811C` and
  `@024F8168` (`mDoLib_clipper::setup`, so culling widens and nothing pops at the edges), and
  `@025ADB38` (the file-select scene's perspective).
* **Render targets:** screen-shaped targets (within 3% of 16:9, not the 854x480 GamePad chain) are
  allocated A/(16/9) times wider. Draws keep their guest viewports, which now cover the wider image,
  so every full-screen pass lines up.
* **2D HUD (`nw::lyt`):** hooks on `LoadProjectionMtx` `02874038`, `Pane::CalculateMtx` `028766CC`,
  `Pane::Draw` `02877100` and the `nw::font` text draw `028F8250`.
  - New ops `OP_SET_PROJ_REGS` and `OP_LAYOUT_ROOT` tag the projection upload and each root draw, so
    the render thread knows which screen a layout goes to.
  - Panes more than 300 units from the centre move out to the new edges.
  - Full-screen leaf panes (backgrounds, fades) are stretched.
  - Not handled: bloom and blur kernels sized in guest texels.
* The addresses are ours too. This is a self-contained feature we could port.

### 4.8 Resolution scaling and other renderer enhancements

* **Internal resolution:** 1x, 1.5x, 2x, 3x or up to 4x. Images are scaled and so are viewports and
  scissors, while lookups, aliasing and guest memory use the guest size. The factor is latched at a
  frame boundary. Shadow maps have their own factor (`WWHD_SHADOW_SCALE`).
* **AO:** three modes: the original, a centre-depth filtering fix, and the fix plus corrected noise
  tiling. Full-size AO depth (on by default) comes from a private 1.5x replay of the game's
  depth-downsample pass. Users found this setting halves the worst GPU waits on Adreno, so it is off on
  Android (issue #56).
* **Also:** FXAA at presentation, 16x anisotropic filtering on eligible mipmapped asset textures,
  smooth, sharp or integer scaling, and the GamePad screen as a window, picture-in-picture, an
  automatic overlay or alone.

### 4.9 What still comes from Cemu's Latte code

Vendored in `runtime/third_party/cemu/`, about 23,000 lines including headers and licences:
- the decompiler and analyzer, with the GLSL and MSL emitters and the attribute decoders;
- the register-type tracker;
- `LatteReg.h`, `RegDefines.h` and the ISA headers;
- AddrLib and its fast decode, and the GX2 surface calc and texture-register init;
- the fetch-shader parser (adapted), the GS copy-shader parser and `LatteToMtl` format tables;
- the vertex-format table and shader-parser glue (`latte_support.cpp`).

*Not* vendored: Cemu's command processor, texture cache, texture decoders and Vulkan or Metal
renderers. The renderers are their own: "Vulkan renderer (by OpenAI Codex)", "Guest state
conventions follow Cemu".

### 4.10 How they verify graphics

* Their claim, from the README "Status": "The opening of the game (title, file select, intro, Outset Island) is tested
  and matches Cemu side by side". This is a **manual** visual comparison. No tool in the repository
  compares against Cemu: no capture diff and no command-stream diff.
* What they do have:
  - `--renderer-smoke`, a synthetic GPU test with no game data (uploads, mips and layers, clears,
    blits, depth and stencil, a triangle, presentation, ring wrap);
  - Khronos validation runs of 1,800–2,400 frames from save states;
  - frame dumps that must be byte-identical between their own configurations (old against new path:
    "byte-identical … except a 20-pixel band at the sea horizon that also differs between two runs");
  - verify modes for the buffer cache and shader keys;
  - CTest unit tests;
  - `tools/bench/run_bench.py` for performance;
  - a private "game-test regression" mentioned in issue #28.
* For the game logic, `tools/verify/` runs hand-written decompiled C++ against the recompiled original
  on generated and recorded inputs, function by function (like our diff mode, aimed at a decompilation).
  True 60 is gated by private pair scripts that compare Link and camera dumps, sounds, random numbers
  and save info between 30 and true-60 runs.

---

## 5. 60 fps: interpolation and "true 60"

### 5.1 Interpolation (the default "60 fps" mode; `runtime/src/interp.cpp`, `interp_fx.cpp`)

```
// logic pass: logic advances N -> N+1; everything is drawn with the camera halfway (N, N+1)
// hold pass:  no logic (no execute/create/delete, scene management, counters, audio);
//             everything is drawn again with the camera at N+1
// The painter at the start of each pass renders the previous pass's draw lists, so the screen shows
// halfway(N,N+1), N+1, halfway(N+1,N+2), N+2, ...
```

* `GX2SetSwapInterval(2)` is halved (`effective_swap_interval`). Hooks on the main loop body
  `025F172C`, the per-frame function `0203593C` and its 15 children, `fpcEx_Handler`, `fpcDt_Handler`,
  `fpcPi_Handler`, `fpcCt_Handler`, `fapGm_After` and `cCt_Counter` hold logic back on hold passes.
  Sound starts are suppressed on in-between frames, and the audio frame callback `020315CC` runs once
  per logic step.
* **Camera:** `camera_draw` `024FFC40` inputs (eye, center, up, fovy, bank) are blended. Orbits blend
  direction and distance separately. Cuts and snaps are detected and not blended.
* **Models:** `J3DModel::viewCalc` `027F55FC` and the UBO update `027F5018` (WWHD's `update_ubo` thread)
  run on halfway world matrices, which are restored afterwards. Models that are new or moved more than
  400 units are not blended.
* **Effects** (`interp_fx.cpp`): particles get halfway position, size, colour and rotation, and ones
  born this step are pulled back half a step. Also blended: the sea's grid heights and scroll, wave
  crests, rain, snow, spores, fog, clouds and stars, btk/brk material animation frames, grass, tree,
  flower and bush sway, and cloth vertex buffers. Per-step work in draw code is held on hold passes.
* **"Keep game speed"** (paced): an in-between frame is drawn only if it fits before the next logic
  step is due. Android pauses 60 fps on thermal headroom (≥ 0.72 off, < 0.60 back on) or when slow.
* **Known deviation:** the random-number stream drifts, because draw code draws `cM_rnd` (point-light
  flicker) and culling at 60 differs. "Interpolation mode … drift … t=0.67 s".

### 5.2 True 60 ("key 7", experimental; `runtime/src/true60.cpp`, `true60_link.cpp`)

From `docs/decomp-notes.md`: "**every full pass is exactly the 30 fps game's step.** A half pass computes a
*preview* … It is drawn … and then **taken back**: at the start of the next full pass Link and the camera
are restored to the state the last full pass left, and the full pass runs the whole 30 Hz step with dt = 1."

* **Converted:** the groups `loco` (WAIT, MOVE, rolls, jumps, LAND, FALL, …), `sword` (cuts, combos,
  parries and the spin attack) and `camera` (the follow camera only). `items` is reserved and off; swim,
  sail and the enemies are not done. Everything else is 30 Hz plus interpolation, and Link drops to
  30 Hz on moving collision.
* **The primitives match ours:** cLib approaches use `1-(1-s)^dt`, chases `× dt`,
  `J3DFrameCtrl::update` uses `rate × dt` and `checkPass` runs on full passes only, and calcSpeed,
  posMove and the morph counter are scaled. `posMoveFromFootPos` has the exact-arc term (instruction
  hooks `@023FCEB0`…`@023FD39C`).
* **Fences during the preview:** Link's procedure call is skipped (`@0240D6D8`/`@0240D6F8`), and so are
  his decision functions (`changeDemoProc`, `changeDamageProc`, …), process creation, emitter creation,
  `cCcS::Set`, events, fades, save-data writes, sound starts and RNG use. Undone word by word: Link's
  0x8284-byte process, what his execute changed in his actor heap, d_a_player statics, play-state
  words, the camera's 0xB28 bytes, and some draw state.
* **Their gates:**
  - Link's position and animation frame differ by 0.00 at every full step in sword, locomotion and
    damage scenarios, with identical saves;
  - the RNG drifts within seconds, which the owner accepted;
  - a 5-minute soak ran with no crash or NaN.
* **Their estimate for the rest:** "About 300 files with real per-step logic: roughly 2–3 months for
  everything", plus a week each for Link and global systems, and a week for cutscenes.

### 5.3 Compared with ours (D21)

* **Our owner explicitly ruled out their model:** "running half steps speculatively from the last state
  and rolling them back … but it draws frames between 30 Hz ticks" (D21). Their true 60 is that.
* **Gameplay effect:** decisions (procedure changes, hits, item use) happen only at 30 Hz, so input
  response is unchanged from 30 fps. Exactness at full ticks comes free, but the half-tick picture is a
  preview, not game state.
* **Ours:** converted processes really step at 60 with h = 0.5, and their stores stand. Everything
  unconverted is exactly the 30 Hz game on whole ticks. We have **401 process types in
  `kConvertedByDefault`** (`src/overrides/sixty.cpp:119-127`) and about 3,000 tick and step rules in
  43 per-area files. Checks use state-probe comparisons with tolerances, plus the step-doubling trial,
  census and tracking tools. That is far beyond their 2 groups plus the camera.
* **Their interpolation is a complete and cheaper fallback** that we don't have: logic stays at 30 Hz
  and only rendering doubles. That matters for weak devices: on Android, true 60 doubles the game CPU
  (our handoff says so).

---

## 6. Their performance work (release notes and docs, with techniques)

| When | Change | Technique | Measured |
|---|---|---|---|
| v0.2.6 | narrow shader keys, identical-GLSL dedupe (PR #46) | key = only registers the translation reads; translations giving identical GLSL share shader and pipelines; verify mode | Outset 6.1–6.7k → 569 translations, 3.2–3.5k → 204 pipelines; −100–115 MB; >50 ms frames −⅓; render CPU −4–12% |
| v0.2.5 | never read mapped upload memory | shadow copies in heap for reuse comparisons | RX 6700 XT: back from 12 fps to normal (57 ms/frame was spent reading BAR memory) |
| v0.2.4 | lazy DrawDone + async present on all platforms | GX2DrawDone waits for recording, not GPU idle; present goes into the submission ring | uncapped +13–53%; loaded paced 60: 35–40 → ~59 fps, 99.7% in-between frames |
| v0.2.4 | 15 "CPU paths" on by default | pipeline lookaside (8 entries), shader-state memo (4 entries per stage), sampler memo, skip redundant descriptor and vertex binds, descriptor ranks, uniform and vertex snapshot reuse after exact byte compare, specialised index conversion, fetch memo, register-class dirty filter | render CPU −8–14% (M3 Max), about −25% (S25 Ultra, 40 → 30 ms) |
| v0.2.4 | guest buffer cache (default on macOS) | page write-watch + DCFlush/GX2Invalidate hints; persistent GPU copies | render CPU −6–17%, uploads −43–64% |
| v0.2.4 | render-thread profiler and "Copy performance report" | per op, per draw phase (sampled 1 in 64), waits, uploads, draw classes, shader-miss histogram, build/OS/GPU header | finds e.g. "55% of Outset draws change only buffer pointers or ALU constants" |
| 2026-10-06 | texture change detection | write-protect pages; exact, about 2 faults a frame | within noise on CPU; fixes stale textures |
| 2026-10-05 | draw batching (2,048 draws, at most 3 per frame) | mid-frame submissions for overlap | Windows 3x AO: 33 → 49–51 fps |
| v0.2.0 | Metal performance pass (PR #15) | cursor-limited pipeline-queue scan, event-driven compile waits, one-pass texcoord snapping (25× faster), shader-key memo, index conversion (5×) | process CPU 94% → 62–67%; renderer samples −16% |
| v0.2.0 | Vulkan vsync wait | adaptive spin window | −15% busy CPU |
| 2.1 / Android | fastest-core pinning, ADPF hint session, thermal-aware 60 fps | `/sys/.../cpuinfo_max_freq`; `APerformanceHint`; `AThermal_getThermalHeadroom` | S25 Ultra: 30–32 fps in the heaviest scenes |

**Absolute numbers:**

* **M3 Max, Outset at 60:** render thread 5.56 ms a frame for 3,230 draws (1.74 µs a draw).
* **Ryzen 7 7735HS with a Radeon 680M (Zen 3+, a CPU stronger than the Steam Deck's), issue #7:**
  about 6,900 draws and 34 MiB of uploads a frame, render thread 11.8 ms a pass at 30 and 17.2 ms with
  interpolation. That meant slow motion, and an average of about 27 logic steps a second with "Keep
  game speed".
* **RK3588 with a Mali-G610, issue #50:** 40 ms a frame for 5,800 draws (6.7 µs a draw), so it locks to
  20 fps; 14.8 ms of that is vertex copies.
* **Galaxy S25 Ultra:** GPU-bound spikes over 4,000 draws (issue #56).
* **Steam Deck:** no data in their repository. By extrapolation from the 680M, heavy scenes at 30 fps
  are plausible and 60 is not, without per-draw render-thread savings.

---

## 7. Platform layer and features

**Hosts:**
- macOS: AppKit host, native menus and controls window, Metal or Vulkan (through MoltenVK).
- Windows, Linux and Android: an SDL3 host for windows, input and audio, Vulkan only.

**The Vulkan loader** is opened at runtime (`VK_NO_PROTOTYPES`). It accepts Vulkan 1.3 or 1.1/1.2
with `VK_KHR_dynamic_rendering`, and shows a clear message box when a device is missing something.

**Context switches:** none. Host threads only, so there is no fiber code per ABI.

**arm64:** the memory base is chosen for 39-bit address spaces, `-fsigned-char` is forced, memory
barriers are real, and `-mcpu` is tunable.

**Windows:**
- `timeBeginPeriod(1)` at start;
- SDL high-resolution sleeps;
- write-watch through `VirtualProtect` and a vectored exception handler;
- toolchains: llvm-mingw, MSYS2 CLANG64, or native LLVM with Visual Studio's SDK.

**Android (by rhemfur):**
- `android/`: an SDL3 `SDLActivity` loads `libmain.so`; built with Gradle and NDK 30; arm64, Android
  13 or newer, Vulkan 1.3.
- The user builds the APK themselves: there is no downloadable APK; CI builds it only with
  placeholder code.
- The extracted game goes into `Android/data/org.wwhdrecomp.wwhd/files/game`, with `env.txt` for
  options, and save import and export through the launcher.
- Performance: ADPF hint sessions, thermal headroom, and pinning to at least two of the fastest
  cores. The pinning came after one-core runs exposed the `GX2CopySurface` race.
- A controller is required; touch is the GamePad screen only.

**Release model:** portable zips with no game code. On first start, setup extracts the game
(.wux/.wud with keys, .wua, or a folder), checks the SHA-256, runs the recompiler (Python) and
compiles with a downloaded, SHA-pinned compiler in about 2 minutes. Everything stays in the release
folder.

**Features:**
- **Save states:** 5 slots of about 270 MB each, LZ4. Every guest thread must be parked at a known HLE
  wait, or at a guest function entry for threads that poll; the main thread parks at the top of the
  per-frame function `0203593C`. A state is restored only if each thread parks at the same SP, LR and
  back chain.
- **Crash Recovery:** automatic states every 2 minutes plus recorded input, replayed with
  `WWHD_REPLAY=n`. Crash logs include guest and host backtraces and the faulting module, and list
  the Vulkan layers.
- **ImGui settings overlay** (F1, Home, or holding Select).
- **Mod manager:**
  - content mods through a VFS overlay;
  - Cemu graphics-pack `rules.txt` import (presets, resolution rules, shader replacements on Vulkan;
    no code patches);
  - native mods with a C ABI (`runtime/include/wwhd_mod.h`), after a confirmation;
  - built-in mods: wall climbing, direct and mouse camera, first person, quick doors and fast scene
    changes (extra logic steps per frame during door events).
- **Also:** cheats, gyro aiming (SDL sensors, Cemuhook DSU or mouse), rumble, a GameCube save
  converter (`tools/savegame/gc2hd.py`), and an on-screen keyboard with font checks.
- **Tooling:**
  - `tools/decomp/`: a statistical matcher against zeldaret/tww that names about 14,000 functions.
    Held-out precision is near-certain for asserts, profiles and vtables, 97% for call-graph matches
    and 99.7% for translation-unit windows.
  - `tools/verify/`: a differential harness for decompiled code.
  - `tools/true60/`: actor survey and site generators.

---

## 8. Licence implications

* **Their whole repository is MPL-2.0** (`LICENSE`). Third-party parts keep their licences: Cemu
  MPL-2.0, metal-cpp Apache-2.0, {fmt} MIT, Dear ImGui MIT, xxHash BSD, zstd BSD at build time.
* **MPL-2.0 is file-level.** We may copy their files, or parts of them, into ours under these conditions:
  1. a copied or modified file stays under MPL-2.0, and its source must be available when we
     distribute binaries;
  2. we keep their notices. Their own files carry **no per-file headers** (only `espresso_fp.c` and the
     wudextract files have notices), so we should add the MPL Exhibit A notice and an origin line, for
     example "derived from ZeldaWWHDRecomp `<path>` @ ffb7a12, MPL-2.0";
  3. our other files are not affected (a "Larger Work" is allowed).

  This is the same regime as the Cemu-derived files we already have (D11, D18), and it is compatible with
  any licence for our own code.
* **Copying ideas, facts and addresses** (hook addresses, structure offsets, their findings) has no
  licence consequence. Under our CLAUDE.md, each rename still needs our own recorded evidence. Their
  matcher's output can be a *lead*. Names generated from it are "derived from the game", and they keep
  them out of git too.
* **Caveats:**
  - Some code is described as written by AI agents ("by OpenAI Codex", PR bodies "Generated with Claude
    Code"). That does not change the MPL grant, but provenance is diffuse.
  - Nothing in their repository is game data, which suits our "never commit game data" rule.
  - Their generated code, `names.tsv` and shader caches are local-only, as ours are.
* **Practical point:** most of what is worth taking is *techniques*: write-watch, the buffer cache's
  validation rules, pacing, aspect hooks. We can reimplement those cleanly. Copy a file verbatim only
  where it is self-contained, such as `write_watch.cpp`, `perf_hint.cpp` or parts of `aspect.cpp`.

---

## 9. Assessment and recommendations

### 9.1 Where theirs is better

1. **A shipped, polished product on 4 OSes and 2 architectures**, with an installer, features and a user
   community feeding back measured bug reports.
2. **Renderer CPU engineering.** Every renderer change is measured with a profiler, A/B benchmarks
   and a verify mode: write-watch, the buffer cache, lazy sync, batching and memo paths.
3. **Parallel execution.** Three host cores run guest code, and imports are direct calls.
4. **Enhancements:** widescreen with HUD anchoring, resolution scaling, AO fixes, interpolation
   (including effects), save states and crash recovery.
5. **Function naming at scale** (about 14,000 names with measured precision) and decomp-backed notes.

### 9.2 Where ours is better

1. **Verification.** Bit-exact traces, command streams, sound and captures against Cemu, plus diff
   mode. Their divergence bugs (copy timing, strictMul, Picto photos, 3D-texture slices) were found by
   players; our capture and stream checks catch that class.
2. **Exact OS and GX2 front half by construction** (Cemu's code, forked and verified). Theirs is a
   reimplementation of modelled behaviour.
3. **Shaders.** Narrow keys from the start, a shipped shader and pipeline list, and everything prepared
   before play from the player's files, with no hitches on a first start. They reached narrow keys
   only on 2026-10-07 and still translate on first use.
4. **True 60.** About 401 process types really step at 60 under a verified mixed rate, against their
   Link (two groups) and camera preview with rollback.
5. **Determinism.** Reproducible runs on the virtual clock; their runs are nondeterministic (the sea
   horizon differs run to run).

### 9.3 Prioritised list: what to adopt or do better

| # | Item | What, concretely | Effort | Main risk | How we verify it |
|---|---|---|---|---|---|
| 1 | **Write-watch change detection** for textures (real time) | Replace `texture.cpp`'s sampled hash (`kWholeEvery = 30`) with page protection plus stamps as in `write_watch.{h,cpp}`; bracket Cemu `fsc` reads into guest memory (FS client `FSReadFile` path) with a host-write scope; keep whole hashing on the virtual clock | M | Interplay with Cemu's own SIGSEGV handling and fibers; `vm.max_map_count` on Linux with many protected ranges; host-side writes (memcpy in our OS layer, the GPU thread) faulting | Virtual clock unchanged (it still hashes whole); real time: a verify mode that hashes whole and asserts no missed change; captures identical on lavapipe with the watch forced on |
| 2 | **Render-thread profiler and per-draw CPU work** | Port the shape of `render_prof` (ops, sampled draw phases, waits, uploads, draw classes); then the measured wins: pipeline lookaside, shader-state memo, sampler memo, skip redundant binds, specialised index conversion, uniform and vertex snapshot reuse with heap shadows (never read mapped memory) | M | Few; each path is an exact cache | Lavapipe captures byte-identical with each path on and off (stream_check unaffected: the renderer never writes guest memory) |
| 3 | **Guest buffer cache** (vertex, index and uniform blocks) | Build on item 1's watch; use `DCFlushRange` and `GX2Invalidate` as hints. We already hold `LatteBufferCache_notifyDCFlush` (D12) | M | Stale data from unannounced writes if the watch misses a writer (the GPU thread, OS-layer memcpy); memory budget on phones | Verify mode comparing every hit with fresh guest bytes (they saw 0 in 142 million); captures identical |
| 4 | **Async present, lazy DrawDone, draw batching** in real time | Our renderer already never writes guest memory (D13). Let GX2DrawDone return after recording and keep a present ring; submit every N draws | S-M | Pictograph and GPU-to-CPU copies if they are ever added (they need a real wait); pacing interplay with our host-timed vsync | Virtual-clock runs keep today's waits; measure real time with `WWHD_FRAME_LOG` |
| 5 | **A multi-host-thread real-time scheduler** (Steam Deck, Android) | The open D19 item. Either their model (a host thread per guest thread with a per-core ownership lock and preemption at function entry) or Cemu's three-host-thread fiber mode with per-object locks. Needs real barriers for `sync`/`lwsync`/`eieio`/`isync` and a CAS `stwcx.` in the generator in this mode | L | Races in forked Cemu OS code built for one global lock; arm64 memory ordering; fast paths (the task loop) to re-check; tick rules and the store journal assume one thread | The deterministic single-thread mode stays the check; multi-thread runs scripted routes to the same end scene; ThreadSanitizer on the worker |
| 6 | **"Native GX2" (skip PM4) — low priority** | The gain is small (their register writes are about 14% of render-thread op time; parsing is cheaper still). If wanted: real time only, a direct path from our gx2 front half to the renderer for immediate-mode packets, with display lists still PM4 (the game sees their sizes) | L | Two paths to keep exact | A dual-consumer check: in a verification build both paths run and the register file and draw parameters must be equal at every draw, plus captures |
| 7 | **Widescreen (Hor+) and HUD anchoring** | Port `aspect.cpp`'s six instruction hooks (`@024FFA98` …) as typed overrides or rules, the `nw::lyt` hooks and the wider screen targets in our surfaces | M | HUD edge cases (bloom kernels, panes at actor positions); 60 fps interplay | Off by default, so 16:9 captures stay identical; on, the owner checks visually |
| 8 | **Internal resolution, AO fix, AF, FXAA** | Scale surfaces, viewports and scissors in the renderer; guest sizes stay for aliasing and copies | M | Surface aliasing and copies at scale; feedback images | 1x captures identical; 2x reviewed visually |
| 9 | **Constant guest memory base** | Make `memory_base` a constexpr address in `ppc_ops.h` (reserve Cemu's 4 GiB at a fixed hint, as they do: `0x200000000000` on x86-64, `0x1000000000` on arm64) | S | Cemu's reservation code needs a patch; ASLR collisions | Diff mode and route traces identical; measure with timing.sh |
| 10 | **Mobile and Deck operation** | ADPF hint sessions, thermal-headroom fallback to 30 (or to our step with whole ticks only), fastest-core pinning, `timeBeginPeriod(1)` on Windows | S-M | Platform-specific only | Device runs |
| 11 | **Interpolation as a low-power mode** | Logic at 30, half frames drawn from blended camera and model matrices (their hooks are at our addresses). For phones and the Deck, where our true 60 doubles game CPU | M-L | Overlaps our 60 fps machinery; two modes to maintain | Whole ticks must equal the 30 fps run exactly (logic unchanged); half frames reviewed visually |
| 12 | **Function names from the tww matcher** | Run their `tools/decomp/match.py` (MPL) on the worker in isolation, outside this repo, with `-I`; import only high-precision tiers (assert, profile, vtable, strings) into `symbols.csv` with evidence "tww-matcher:<tier>", and the rest as hints | M | Lower tiers 3–15% wrong; needs a tww build (a GameCube dump) | Each imported name keeps its tier as evidence; spot checks against our own Ghidra decompiles |
| 13 | **Save states and crash recovery** | Ours is easier: guest stacks are on fibers and the scheduler is ours. Park every fiber at an HLE wait or the frame top, then snapshot guest memory, the forked OS state and the GPU register file | L | Cemu OS objects outside guest memory (host-side state in forks); renderer state rebuild | Save, then load, then route trace and captures equal to an uninterrupted run (virtual clock) |
| 14 | **Pipeline-key normalisation** | Zero ignored blend, stencil and depth-bias words in `PipelineDesc` | S | None | Capture-identical; count pipelines (D20 suggests a small gain) |
| 15 | **Faster user-machine build** | Their generated C compiles in about 2 minutes. Look at our shard sizes, the per-block checked copies and C++ overheads, for a future installer | M | Changing the generator touches every check | Diff mode and traces identical |

**Steam Deck and Android viability, in short:**

* **The binding constraint is per-draw renderer CPU.** They see 1.7 µs a draw on an M3 Max, 2–2.5 µs on
  a 7735HS and 6.7 µs on an RK3588, with 3,000–7,000 draws a frame. Items 2–4 attack that first.
* **The second constraint is game CPU on one host thread at 60** (our 41–73% of a 13700K core).
  Item 5, or item 11 as a low-power mode, addresses it.
* **The third is Android packaging**, which for us is still blocked on D18's remaining Cemu
  dependencies: loader, memory map, the fibers' arm64 context switch and fsc.

**How to verify a native graphics path** (they have no automated method; ours must keep its
guarantees):

1. Keep PM4 and the Cemu-derived front half as the *reference path* inside our own binary.
2. Let any new path run *alongside* it in a verification build and compare at every draw: the
   decoded register file, draw parameters, bound resources and copy commands.
3. Lavapipe captures must stay byte-identical against Cemu's for the whole route.
4. Renderer-internal caches (texture watch, buffer cache, memos) each get a verify mode, as theirs
   have, that does the uncached work too and asserts equality. CI runs them on the worker.
