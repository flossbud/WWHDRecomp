// What overrides use (docs/recompiler-design.md D9): a generated function f_X listed in
// config/US_v0/overrides.txt is ours, defined in this directory; the generated body stays callable as
// orig_f_X. Overrides are written like generated code: they work on the guest state (`ctx`, GPR(n),
// rd32/wr32 from ppc_ops.h) and call other functions the way generated code does.
#pragma once
#include "funcs.h"      // generated (build/recomp): every f_X, and orig_f_X for the listed ones
#include "../os/os.h"

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

	// for the real-time log: a thread slept through an idle round, woken by a message or not
	void CountIdleWait(bool byMessage);
}

// The store census (src/overrides/sixty.cpp): while set, with g_rtJournalOn, every store generated
// code makes is handed to it with the host address it was made from (src/runtime/diff.cpp)
extern bool g_rtJournalOn;
extern void (*g_rtStoreCensus)(uint32 ea, uint32 size, void* from);
