// coreinit: the clock (os.h, D18). The timer runs at a twentieth of the core clock (the bus clock is a
// fifth of the core's, the timer a quarter of the bus's). System time counts from boot, OS time from
// 1 January 2000.
#include "os.h"

namespace
{
	using namespace wwhd::os;

	uint64 OSTime() { return Timebase() + TimebaseAt2000(); }
}

// OSTime OSGetSystemTime(void)
WWHD_OS_FUNCTION(coreinit, OSGetSystemTime) { Return64(ctx, Timebase()); }

// OSTime OSGetTime(void)
WWHD_OS_FUNCTION(coreinit, OSGetTime) { Return64(ctx, OSTime()); }

// OSTick OSGetSystemTick(void): the low word of the system time
WWHD_OS_FUNCTION(coreinit, OSGetSystemTick) { Return(ctx, (uint32)Timebase()); }

// OSTick OSGetTick(void): the low word of the OS time
WWHD_OS_FUNCTION(coreinit, OSGetTick) { Return(ctx, (uint32)OSTime()); }
