// The settings (docs/research/gpu-plan.md, item 4): the player's choices of the switches the program reads at start
// (WWHD_60FPS, WWHD_VSYNC...), kept in portable/wwhd.ini as KEY=VALUE lines of the switches' own names. Load applies
// them to the environment before anything reads a switch, unless the environment sets one already (a launcher's or a
// test's own wins), and only in real time with a window: the virtual clock (every check) never reads the file.
// WWHD_SETTINGS=0: the file is ignored. The pause menu's Settings (frontend/pause_menu.cpp) shows and changes them. Presets
// (WWHD_PRESET=auto|performance|quality) set the speed options at once; Auto picks Performance on a low-powered
// device (settings.cpp).
#pragma once
#include <string>
#include <vector>

namespace wwhd::os::settings
{
	void Load(const std::string& portableDir);     // at start (cemu_boot.cpp SetupPaths)
	bool Active();                                 // the file is in use (real time, a window, not WWHD_SETTINGS=0)
	std::string StartLog();                        // the preset and its options as they started (one line), or ""
	void SetDisplayHz(float hz);                   // the window's display's refresh rate (window_system.cpp)
	float DisplayHz();                             // it, or 0 (unknown, or no window)
	void SetFrameGen(int multiplier);              // frames shown a frame drawn: FSR 3 (gpu/vk/present.cpp) 2, lsfg-vk (lsfg.cpp) 2-4
	int FrameGen();                                // it, or 1 (decided before the 60 fps switch)

	int Count();                                   // the settings page's options
	std::string Line(int option);                  // "Frame rate: 60 (at the next start)"
	void Cycle(int option, int step);              // to its next (+1) or previous (-1) value, saved at once
	// a note on an option's line, from the part that reads it ("off: the display runs at 60 Hz"); "" removes it
	void SetNote(const char* key, const std::string& note);

	// The pause menu's Settings (t-pausemenu M2): an option as its rows show it. Option 0 is the preset (values auto,
	// performance, quality); Find gives an option's number by its switch (-1: none)
	struct OptionView
	{
		std::string key, label;
		std::vector<std::string> values, shown;     // the switch's values, and as the menus show them
		int current = 0;                            // the value set (in use, or at the next start)
		int atStart = 0;                            // the value the program started with
		std::string atStartValue;                   // the same as the switch's text (the preset: as resolved)
		int def = 0;                                // the program's default
		bool live = false;                          // applied at once (else at the next start)
		bool fromEnv = false;                       // the launcher's: the menu can't change it
		bool active = false;                        // the file is in use (else nothing is saved)
		std::string performance;                    // its value in the Performance preset ("": no preset's)
		std::string note;                           // SetNote's
		bool custom = false;                        // the preset: an option it covers has a line of its own
		std::string resolved, autoWhy;              // the preset: what it resolves to, and why Auto picked it
	};
	int Find(const std::string& key);
	OptionView View(int option);
	void Set(int option, int value);               // to that value (an index into values), saved at once
	void RescanDrivers();                          // Android: the GPU driver option's values again (after an import)
	void ResetToDefaults(const std::vector<std::string>& keys);   // their lines dropped (the preset's own values back)

	// what applies live: the event loop takes the requests (window_system.cpp): the display (fullscreen, window size;
	// width 0 when no size is set) and vsync (the swapchain rebuilt); true when either changed
	bool TakeWindowRequest(bool& fullscreen, int& width, int& height, bool& vsync);
}
