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
	void SkipSwap();            // a dropped frame's swap: counted, nothing presented (src/overrides/pacing.cpp)
	void SleepTicks(uint64 ticks);                  // the calling guest thread sleeps (the scheduler's)
	uint32 AllocSystemArea(uint32 size, uint32 alignment); // guest memory for the OS's own data (Cemu's system area)
	// run the guest function `fn` with r3, r4 on the OS's callback thread (Cemu's async callbacks)
	void QueueGuestCallback(uint32 fn, uint32 r3, uint32 r4);
	uint32 CurrentThreadStackEnd();                 // the lowest address of the calling guest thread's stack

	// For the game's task loop in real time (src/overrides/task_loop.cpp, D19), in the scheduler's
	// message queues: the queue at guest address `queue` (an OSMessageQueue), read under the scheduler
	// lock: how many messages it holds, the newest one's first word, and whether every first word is
	// `word`
	struct QueueView { uint32 used, last; bool allSame; };
	QueueView ViewQueue(uint32 queue, uint32 word);
	// the calling thread waits, taking no message, until the queue holds a number other than `used`
	// (a message arrived) or timeoutNs has passed; true if the number changed
	bool WaitQueueChange(uint32 queue, uint32 used, uint64 timeoutNs);
}

// Typed functions: WWHD_OS_EXPORT(gx2, GX2InitSampler, ns::GX2InitSampler) registers a plain C++
// function, its arguments taken from the registers as Cemu's cafeExportRegister takes them (integers,
// enums and bools from r3.. then the stack, 64-bit ones from an aligned register pair, pointers as
// host pointers with 0 as null, floats from f1..), its result set likewise (pointers as guest
// addresses, 64-bit in r3:r4, bool as 0/1; void leaves r3).
namespace wwhd::os::detail
{
	struct ArgReader
	{
		PPCInterpreter_t* ctx;
		int gpr = 0, fpr = 0;

		uint32 Word()
		{
			uint32 v = gpr >= 8 ? Read32(ctx->gpr[1] + 8 + (gpr - 8) * 4) : ctx->gpr[3 + gpr];
			gpr++;
			return v;
		}

		template<typename T>
		T Get()
		{
			if constexpr (std::is_pointer_v<T>)
			{
				uint32 addr = Word();
				return addr ? (T)Guest(addr) : nullptr;
			}
			else if constexpr (std::is_base_of_v<MEMPTRBase, T>)
				return T(Word());
			else if constexpr (std::is_enum_v<T>)
				return (T)Get<std::underlying_type_t<T>>();
			else if constexpr (std::is_integral_v<T> && sizeof(T) == 8)
			{
				gpr = (gpr + 1) & ~1;
				uint64 hi = Word(), lo = Word();
				return (T)(hi << 32 | lo);
			}
			else if constexpr (std::is_integral_v<T>)
				return (T)Word();
			else if constexpr (std::is_floating_point_v<T>)
				return (T)ctx->fpr[1 + fpr++].fpr;
			else
				static_assert(sizeof(T) == 0, "unsupported argument type");
		}
	};

	template<typename R>
	void SetResult(PPCInterpreter_t* ctx, R r)
	{
		if constexpr (std::is_pointer_v<R>)
			ctx->gpr[3] = r ? (uint32)((uint8*)r - memory_base) : 0;
		else if constexpr (std::is_enum_v<R>)
			SetResult(ctx, (std::underlying_type_t<R>)r);
		else if constexpr (std::is_integral_v<R> && sizeof(R) == 8)
		{
			ctx->gpr[3] = (uint32)((uint64)r >> 32);
			ctx->gpr[4] = (uint32)r;
		}
		else if constexpr (std::is_integral_v<R>)
			ctx->gpr[3] = (uint32)r;
		else
			static_assert(sizeof(R) == 0, "unsupported result type");
	}

	template<typename R, typename... A>
	void Invoke(PPCInterpreter_t* ctx, R (*fn)(A...))
	{
		ArgReader reader{ ctx };
		std::tuple<A...> args{ reader.Get<A>()... };   // braces: evaluated left to right
		if constexpr (std::is_void_v<R>)
			std::apply(fn, args);
		else
			SetResult(ctx, std::apply(fn, args));
		ctx->instructionPointer = ctx->spr.LR;
	}

	template<auto Fn>
	void Typed(PPCInterpreter_t* ctx) { Invoke(ctx, Fn); }
}
#define WWHD_OS_EXPORT(lib, name, fn) \
	static ::wwhd::os::Registration wwhd_os_reg_##lib##_##name(#lib, #name, ::wwhd::os::detail::Typed<&fn>)

// WWHD_OS_FUNCTION(coreinit, memcpy) { ... } defines and registers coreinit.memcpy; the body sees `ctx`.
// Names that aren't identifiers (C++-mangled exports) use WWHD_OS_FUNCTION_NAMED with their own.
#define WWHD_OS_FUNCTION_NAMED(lib, name, ident)                                                  \
	static void wwhd_os_##ident(PPCInterpreter_t* ctx);                                           \
	static ::wwhd::os::Registration wwhd_os_reg_##ident(#lib, name, wwhd_os_##ident);            \
	static void wwhd_os_##ident([[maybe_unused]] PPCInterpreter_t* ctx)
#define WWHD_OS_FUNCTION(lib, name) WWHD_OS_FUNCTION_NAMED(lib, #name, lib##_##name)
