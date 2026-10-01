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
