// The settings (docs/research/gpu-plan.md, item 4): the player's choices of the switches the program reads at start
// (WWHD_60FPS, WWHD_VSYNC...), kept in portable/wwhd.ini as KEY=VALUE lines of the switches' own names. Load applies
// them to the environment before anything reads a switch, unless the environment sets one already (a launcher's or a
// test's own wins), and only in real time with a window: the virtual clock (every check) never reads the file.
// WWHD_SETTINGS=0: the file is ignored. The settings page (debug_menu.cpp, F2) shows and changes them.
#pragma once
#include <string>
#include <vector>

namespace wwhd::os::settings
{
	void Load(const std::string& portableDir);     // at start (cemu_boot.cpp SetupPaths)
	bool Active();                                 // the file is in use (real time, a window, not WWHD_SETTINGS=0)

	int Count();                                   // the settings page's options
	std::string Line(int option);                  // "Frame rate: 60 (at the next start)"
	void Cycle(int option, int step);              // to its next (+1) or previous (-1) value, saved at once

	// what applies live: the event loop takes the requests (window_system.cpp): the display (fullscreen, window size;
	// width 0 when no size is set) and vsync (the swapchain rebuilt); true when either changed
	bool TakeWindowRequest(bool& fullscreen, int& width, int& height, bool& vsync);
}
