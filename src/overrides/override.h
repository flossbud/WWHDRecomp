// What overrides use (docs/recompiler-design.md D9): a generated function f_X listed in
// config/US_v0/overrides.txt is ours, defined in this directory; the generated body stays callable as
// orig_f_X. Overrides are written like generated code: they work on the guest state (`ctx`, GPR(n),
// rd32/wr32 from ppc_ops.h) and call other functions the way generated code does.
#pragma once
#include "funcs.h"      // generated (build/recomp): every f_X, and orig_f_X for the listed ones
#include "../os/os.h"

namespace wwhd::pacing
{
	// Frame dips without slowdown (src/overrides/pacing.cpp; docs/research/gpu-plan.md): a half tick's frame that would
	// make the next tick late runs its logic but not its draws, render jobs or present (g_dropFrame, set by FrameStart
	// at each frame's start, on the frame thread)
	extern bool g_dropFrame;
	// a dropped frame's actor draws too (WWHD_60FPS_DROPDRAWS=1; off by default: half-tick draws take numbers from the
	// game's random stream, so leaving them out moves every later draw of it: a seagull's flight, then the sea)
	bool DropDraws();
	void FrameStart(uint32 swap, uint32 from, bool half);
	// at 30 fps a whole frame more than a tick behind is dropped too (the frame skip, pacing.cpp)
	void FrameStart30(uint32 swap);
	void FrameShown();                               // a frame was presented (the frame skip's floor)
	void SceneRequested(uint32 proc);                // a scene change asked for (fopScnM_ChangeReq): the skip is play's only
}
namespace wwhd::rt
{
	// The real-time fast paths (D19) may run: the recompiled program on one host thread without the
	// virtual clock. Never in checks (virtual clock), diff mode or Cemu's three host threads, and not
	// with WWHD_FAST_PATHS=0. An override that isn't the game's code step for step asks this first.
	bool FastPaths();

	// 60 fps (D21, M6): WWHD_60FPS=1 with the recompiled program (WWHD_NATIVE=on), in real time or
	// with the virtual clock. From swap WWHD_60FPS_FROM on (default 0) the game presents every vsync
	// (swap interval 1) and its frame runs 60 times a second (src/overrides/sixty.cpp has what runs at
	// which rate); before it, as at 30 fps. SixtyFrom is that swap (never, when 60 fps is off), and
	// GameFrame(swap) the game's own frame a swap belongs to: the swap itself before SixtyFrom, then
	// one game frame every two swaps. Input scripts and the state probe count game frames.
	bool SixtyFps();
	uint32 SixtyFrom();
	uint32 GameFrame(uint32 swap);

	// The quiet watch: whether one guest call by the calling thread changed anything another thread,
	// or the caller's next call, could see. Begin it right before the call, with [low, high) the stack
	// that is dead once the call returns (from the thread's stack end up to the caller's frame); end it
	// right after. QuietEnd gives the number of OS calls the call made, or -1 if it did anything else
	// that could be seen: a store outside [low, high), interpreted code, leaving the core (a timeslice
	// ending, a wait), or another thread running meanwhile.
	uint64 QuietBegin(PPCInterpreter_t* ctx, uint32 low, uint32 high);
	sint32 QuietEnd(uint64 token);
	bool QuietWatching();                  // a watch is on (the 60 fps tools keep the journal on for it)

	// for the real-time log: a thread slept through an idle round, woken by a message or not
	void CountIdleWait(bool byMessage);
}

// The 60 fps tools (src/overrides/sixty.cpp): while set, with g_rtJournalOn, every store generated
// code makes is handed to it before it is made, with the guest instruction making it (0 from
// hand-written code; src/runtime/diff.cpp)
extern thread_local bool g_rtJournalOn;
extern thread_local void (*g_rtStoreCensus)(uint32 ea, uint32 size, uint32 pc);
// With it, a table of the 4 KB pages it wants (an entry per page: 0 for none; g_rtStorePages, ppc_ops.h):
// stores into other pages don't reach it (unless a fast path's watch runs); g_rtStoresPassed counts
// those that called rt_journal_store directly (per host thread: the frame thread's, read by its frame log)
extern thread_local uint64 g_rtStoresPassed;
// With them, per page of the table (its entry's low 30 bits - 1) 64 words, a bit per byte: a store into bytes all
// set returns at once (saved already this frame, or not kept), unless *g_rtStoreHold or a fast path's watch
extern const uint64* g_rtStoreBits;
extern const int* g_rtStoreHold;
