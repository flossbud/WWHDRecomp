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
	inline void Return(PPCInterpreter_t* ctx) { ctx->instructionPointer = ctx->spr.LR; }
	inline void Return(PPCInterpreter_t* ctx, uint32 value)
	{
		ctx->gpr[3] = value;
		ctx->instructionPointer = ctx->spr.LR;
	}
}

// WWHD_OS_FUNCTION(coreinit, memcpy) { ... } defines and registers coreinit.memcpy; the body sees `ctx`.
// Names that aren't identifiers (C++-mangled exports) use WWHD_OS_FUNCTION_NAMED with their own.
#define WWHD_OS_FUNCTION_NAMED(lib, name, ident)                                                  \
	static void wwhd_os_##ident(PPCInterpreter_t* ctx);                                           \
	static ::wwhd::os::Registration wwhd_os_reg_##ident(#lib, name, wwhd_os_##ident);            \
	static void wwhd_os_##ident([[maybe_unused]] PPCInterpreter_t* ctx)
#define WWHD_OS_FUNCTION(lib, name) WWHD_OS_FUNCTION_NAMED(lib, #name, lib##_##name)
