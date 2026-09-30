// Host-side waits on TCL (src/os/tcl/TCL.cpp): the GPU thread waits for commands and the CPU for a
// submission to retire, instead of spinning (docs/recompiler-design.md, "Profile of the native build").
// Neither changes what the guest sees: they only replace yield loops.
#pragma once
#include <chrono>

namespace TCL
{
	// the GPU thread: until the ring has a word or `timeout` has passed
	void TCLGPUWaitForCommands(std::chrono::microseconds timeout);
	// the CPU: until the GPU has retired `timestamp` or `deadline` has passed; false if it hasn't
	bool TCLWaitRetiredHost(uint64 timestamp, std::chrono::steady_clock::time_point deadline);
}
