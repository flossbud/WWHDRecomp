// coreinit: per-thread and per-core state that needs no scheduling (os.h, D18): thread-specific
// slots, the GHS C library's errno, and the interrupt mask. Threads are the game's OSThread
// structures in guest memory; which one runs on a core is still the scheduler's (CurrentThread).
#include "os.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"

namespace
{
	using namespace wwhd::os;

	// OSThread fields (the guest's layout)
	constexpr uint32 kThreadErrno = 0x300;      // context.ghs_errno
	constexpr uint32 kThreadSpecific = 0x57C;   // 16 pointers for OSGetThreadSpecific
	constexpr uint32 kThreadSpecificCount = 16;
	static_assert(offsetof(OSThread_t, context) + offsetof(OSContext_t, ghs_errno) == kThreadErrno);
	static_assert(offsetof(OSThread_t, specificArray) == kThreadSpecific);
	static_assert(sizeof(OSThread_t::specificArray) == kThreadSpecificCount * 4);

	// Interrupts off stop the scheduler preempting this core: the cycle budget is pushed out of
	// reach while the mask is 0 and pulled back when it is restored.
	constexpr sint32 kNoPreemption = 0x40000000;

	uint32 SetInterruptMask(PPCInterpreter_t* ctx, uint32 mask)
	{
		uint32 previous = ctx->coreInterruptMask;
		if (previous == 0 && mask != 0)
			ctx->remainingCycles -= kNoPreemption;
		ctx->coreInterruptMask = mask;
		return previous;
	}
}

// void OSSetThreadSpecific(uint32 index, void* value)
WWHD_OS_FUNCTION(coreinit, OSSetThreadSpecific)
{
	uint32 index = Arg(ctx, 0);
	if (index < kThreadSpecificCount)
		Write32(CurrentThread() + kThreadSpecific + index * 4, Arg(ctx, 1));
	Return(ctx);
}

// void* OSGetThreadSpecific(uint32 index)
WWHD_OS_FUNCTION(coreinit, OSGetThreadSpecific)
{
	uint32 index = Arg(ctx, 0);
	Return(ctx, index < kThreadSpecificCount ? Read32(CurrentThread() + kThreadSpecific + index * 4) : 0);
}

// int* __gh_errno_ptr(void), void __gh_set_errno(int), int __gh_get_errno(void)
WWHD_OS_FUNCTION(coreinit, __gh_errno_ptr) { Return(ctx, CurrentThread() + kThreadErrno); }
WWHD_OS_FUNCTION(coreinit, __gh_set_errno)
{
	Write32(CurrentThread() + kThreadErrno, Arg(ctx, 0));
	Return(ctx);
}
WWHD_OS_FUNCTION(coreinit, __gh_get_errno) { Return(ctx, Read32(CurrentThread() + kThreadErrno)); }

// uint32 OSDisableInterrupts(void): returns the previous mask
WWHD_OS_FUNCTION(coreinit, OSDisableInterrupts)
{
	uint32 previous = ctx->coreInterruptMask;
	if (previous != 0)
		ctx->remainingCycles += kNoPreemption;
	ctx->coreInterruptMask = 0;
	Return(ctx, previous);
}

// uint32 OSRestoreInterrupts(uint32 mask): returns the previous mask
WWHD_OS_FUNCTION(coreinit, OSRestoreInterrupts) { Return(ctx, SetInterruptMask(ctx, Arg(ctx, 0))); }

// uint32 OSEnableInterrupts(void): returns the previous mask
WWHD_OS_FUNCTION(coreinit, OSEnableInterrupts) { Return(ctx, SetInterruptMask(ctx, 1)); }
