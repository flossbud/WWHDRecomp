// Our OS layer (docs/recompiler-design.md D18): the game's imports implemented here rather than by
// Cemu, one function at a time.
//
// Each function takes over its import's entry in Cemu's HLE handler table at boot (Install, from the
// runtime's first timeslice), so native code and the interpreter both reach it, and Cemu's dispatch
// (PPCInterpreter_virtualHLE) still traces the call and charges its guest cycles: traces stay
// comparable with the reference's. Functions not implemented here stay Cemu's. WWHD_OS=cemu keeps
// every handler Cemu's, to compare.
//
// A function receives the guest CPU state in the Espresso calling convention: arguments in r3..r10,
// the result in r3, and it returns to LR. It must do exactly what the Cemu handler it replaces did
// to registers and guest memory, which the whole-route trace checks.
#pragma once
#include "Common/precompiled.h"
#include "Cafe/HW/Espresso/PPCState.h"

namespace wwhd::os
{
	using Function = void (*)(PPCInterpreter_t* ctx);

	struct Export
	{
		const char* lib;
		const char* name;
		Function fn;
	};
	std::vector<Export>& Exports();

	struct Registration
	{
		Registration(const char* lib, const char* name, Function fn) { Exports().push_back({ lib, name, fn }); }
	};

	void Install();

	inline uint32 Arg(PPCInterpreter_t* ctx, int i) { return ctx->gpr[3 + i]; }   // integer arguments 0-7
	inline uint8* Guest(uint32 addr) { return memory_base + addr; }
	inline uint32 Read32(uint32 addr) { return _swapEndianU32(*(uint32*)Guest(addr)); }   // guest memory is big-endian
	inline void Write32(uint32 addr, uint32 value) { *(uint32*)Guest(addr) = _swapEndianU32(value); }
	inline void Write16(uint32 addr, uint16 value) { *(uint16*)Guest(addr) = _swapEndianU16(value); }
	inline void Write8(uint32 addr, uint8 value) { *Guest(addr) = value; }
	inline uint16 Read16(uint32 addr) { return _swapEndianU16(*(uint16*)Guest(addr)); }
	inline void WriteFloat(uint32 addr, float value)
	{
		uint32 bits;
		memcpy(&bits, &value, 4);
		Write32(addr, bits);
	}
	inline void Return(PPCInterpreter_t* ctx) { ctx->instructionPointer = ctx->spr.LR; }
	inline void Return(PPCInterpreter_t* ctx, uint32 value)
	{
		ctx->gpr[3] = value;
		ctx->instructionPointer = ctx->spr.LR;
	}
	inline void Return64(PPCInterpreter_t* ctx, uint64 value)    // high word in r3, low in r4
	{
		ctx->gpr[3] = (uint32)(value >> 32);
		ctx->gpr[4] = (uint32)value;
		ctx->instructionPointer = ctx->spr.LR;
	}

	// What our functions still borrow from Cemu, each behind one accessor so it can move to our
	// runtime (the clock), our scheduler (the current thread, sleeping, callbacks), our gx2 (the swap
	// count) or our memory map (the system area) without touching the functions.
	uint64 Timebase();          // the timer clock (core cycles / 20) since boot
	uint64 TimebaseAt2000();    // the timer clock at boot, counted from 1 January 2000
	uint32 CurrentThread();     // the guest OSThread running on the calling core
	uint32 SwapCount();         // GX2SwapScanBuffers calls so far: the frame number input scripts use
	void SleepTicks(uint64 ticks);                  // the calling guest thread sleeps (the scheduler's)
	uint32 AllocSystemArea(uint32 size, uint32 alignment); // guest memory for the OS's own data (Cemu's system area)
	// run the guest function `fn` with r3, r4 on the OS's callback thread (Cemu's async callbacks)
	void QueueGuestCallback(uint32 fn, uint32 r3, uint32 r4);
}

// WWHD_OS_FUNCTION(coreinit, memcpy) { ... } defines and registers coreinit.memcpy; the body sees `ctx`.
// Names that aren't identifiers (C++-mangled exports) use WWHD_OS_FUNCTION_NAMED with their own.
#define WWHD_OS_FUNCTION_NAMED(lib, name, ident)                                                  \
	static void wwhd_os_##ident(PPCInterpreter_t* ctx);                                           \
	static ::wwhd::os::Registration wwhd_os_reg_##ident(#lib, name, wwhd_os_##ident);            \
	static void wwhd_os_##ident([[maybe_unused]] PPCInterpreter_t* ctx)
#define WWHD_OS_FUNCTION(lib, name) WWHD_OS_FUNCTION_NAMED(lib, #name, lib##_##name)
