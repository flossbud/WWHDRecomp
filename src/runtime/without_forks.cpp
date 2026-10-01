// What wwhd-null takes from our forks of Cemu's sources (src/forks.txt), as weak stand-ins for a build
// without them: WWHD_FORKS=0 src/build.sh build/wwhd-cemu, which records a route's command stream and
// sound with Cemu's own libraries (tools/reference/baseline.sh). With the forks linked, theirs win.
#include "../os/tcl/tcl_host.h"
#include <thread>

namespace coreinit
{
	__attribute__((weak)) uint64 __OSIdleNanoseconds()          // os/coreinit/coreinit_Thread.cpp: never idle
	{
		return 0;
	}
}

namespace TCL
{
	// os/tcl/TCL.cpp: Cemu's TCL wakes no one, so look again soon
	__attribute__((weak)) void TCLGPUWaitForCommands(std::chrono::microseconds timeout)
	{
		std::this_thread::sleep_for(std::min(timeout, std::chrono::microseconds(100)));
	}
}

// runtime/fiber/FiberUnix.cpp: the context switch's bounds, for the profiler (Cemu's fibers have none)
extern "C" __attribute__((weak)) char wwhd_fiber_switch[1] = {}, wwhd_fiber_switch_end[1] = {};
