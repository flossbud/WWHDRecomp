// The wwhd runtime: runs the recompiled program inside Cemu's OS (docs/recompiler-design.md D1).
// Install() sets Cemu's execution seam (g_ppcExecuteHook, cemu-patches/0011), which replaces
// Cemu's interpreter loop. By default the hook still interprets every instruction exactly as Cemu
// would; WWHD_NATIVE=diff turns on diff mode for pure functions (D8.2, milestone M3).
#pragma once

namespace wwhd::rt
{
	void Install();

	// WWHD_PROFILE=path samples where host CPU time goes (profile.cpp); call before anything starts.
	void StartProfiler();
}
