// The settings (settings.h): the options, the presets, their file and what applies live.
//
// Presets (the owner's ask: like a modern game's, Performance turns on everything that makes it faster, Quality
// keeps the defaults, Auto picks Performance on a low-powered device): WWHD_PRESET=auto|performance|quality. An
// option a preset covers takes its value from, first to last: the environment, the option's own line in the file,
// the preset, the program's default. Changing such an option in the menu writes its own line (the preset then shows
// "Custom"); choosing a preset again drops those lines. Frame rate, vsync and the display are no preset's. The frame
// rate (WWHD_FRAMERATE 30/40/50/60/90/120/150/display, 60 by default) sets WWHD_60FPS, at 40 and 50 WWHD_FPS_CAP
// (overrides/pacing.cpp), at 90, 120 and 150 WWHD_STEPS (3, 4 and 5 frames a tick), and at "display" WWHD_STEPS=display: the
// most frames a tick the display's refresh rate shows (the 60 fps switch reads it, overrides/sixty.cpp: 144 Hz 4, 165 Hz 5;
// for VRR displays).
#include "settings.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace
{
	struct Value { const char* value; const char* shown; };
	struct Option
	{
		const char* key;                            // the switch
		const char* label;
		std::vector<Value> values;                  // in the menu's order
		bool live;                                  // applied at once (else at the next start)
		const char* performance;                    // its value in the Performance preset, or nullptr: no preset's
		int def = 0;                                // which value is the program's default (no file, no switch)
	};
	const std::vector<Option> kOptions = {
		// 40 and 50 run 60's logic with a fixed share of its half frames left out (pacing.cpp, WWHD_FPS_CAP): smoother at 30 or 60
		// 60 by default (the owner's ask: new players start at 60); an ini line or WWHD_FRAMERATE/WWHD_60FPS still wins.
		// 90, 120 and 150 (uncapped U5): 3, 4 and 5 frames a tick (WWHD_STEPS); for a display of that rate or more and a machine that
		// keeps it (keep-speed drops what it can't, as at 60)
		{ "WWHD_FRAMERATE", "Frame rate", { { "30", "30" }, { "40", "40 (smoother at 30 or 60)" }, { "50", "50 (smoother at 30 or 60)" },
			{ "60", "60" }, { "90", "90 (90 Hz+ displays, fast PCs)" }, { "120", "120 (120 Hz+ displays, fast PCs)" },
			{ "150", "150 (150 Hz+ displays, fast PCs)" }, { "display", "The display's (30 x N up to its rate; VRR)" } }, false, nullptr, 3 },
		{ "WWHD_VSYNC", "Vsync", { { "0", "off" }, { "1", "on" } }, true, nullptr },
		{ "WWHD_VRR", "Variable refresh (FreeSync / G-Sync / VRR)", { { "0", "off" }, { "1", "on" } }, false, nullptr },
		{ "WWHD_FULLSCREEN", "Display", { { "0", "window" }, { "1", "fullscreen" } }, true, nullptr },
		// the resolution: the window's size (fullscreen: the display's own); the game renders at it times the render scale
		// (b-displayres: gpu/vk/renderer.cpp DisplayScale), the upscalers and the HUD at it
		{ "WWHD_WINDOW_SIZE", "Resolution (window; fullscreen: the display's)", { { "1920x1080", "1920x1080" },
			{ "1280x720", "1280x720" }, { "1280x800", "1280x800" }, { "1600x900", "1600x900" }, { "2560x1440", "2560x1440" },
			{ "3840x2160", "3840x2160" } }, true, nullptr },
		{ "WWHD_RENDER_SCALE", "Render scale (of the resolution; the effects' buffers stay at the game's size)", { { "1", "100%" },
			{ "0.75", "75%" }, { "0.67", "67%" }, { "0.5", "50%" }, { "1.5", "150% (supersampling)" }, { "2", "200% (supersampling)" },
			{ "auto", "Auto" } }, false, "auto" },
		// the TV picture's shape (os/aspect.h, b-wide): Auto fills the window (more view to the sides on a wider one, above
		// and below on a taller one, the Steam Deck's 16:10), 4:3 a 4:3 picture (more view above and below; bars at the
		// sides on a wide screen, the GameCube's shape); the HUD keeps its proportions, at the picture's corners
		{ "WWHD_ASPECT", "Aspect ratio", { { "16:9", "16:9" }, { "auto", "Auto (fill the window)" }, { "4:3", "4:3" } }, false, nullptr },
		{ "WWHD_UPSCALER", "Upscaler (render scale under 100%)", { { "none", "bilinear" }, { "fsr1", "FSR 1" },
			{ "fsr3", "FSR 3 (desktop GPUs)" } }, false, nullptr },
		// FSR 3.1 frame generation (gpu/vk/fsr3.cpp Interpolate): a frame between each two, shown when the display runs at
		// twice the game's rate (present.cpp FrameGenPresents, which notes on this line why it's off); with the FSR 3
		// upscaler only. Its input lag: each real frame shows one refresh later, after the generated one (8 ms at 120 Hz).
		// Lossless Scaling x2-x4 (gpu/vk/lsfg.cpp, b-lsfg): an installed lsfg-vk layer with the player's own Lossless Scaling
		{ "WWHD_FRAMEGEN", "Frame generation (generated in-between frames, adds input lag; 120 Hz+ displays)",
			{ { "0", "off" }, { "1", "FSR 3" }, { "lsfg2", "Lossless Scaling x2 (lsfg-vk)" },
			{ "lsfg3", "Lossless Scaling x3 (lsfg-vk)" }, { "lsfg4", "Lossless Scaling x4 (lsfg-vk)" } }, false, nullptr },
		// at least this much anisotropy on the game's mipmapped textures (gpu/vk/texture.cpp Sampler; render targets keep
		// the game's own): the ground and the sea sharper at a slant
		{ "WWHD_ANISO", "Anisotropic filtering", { { "0", "the game's" }, { "4", "4x" }, { "8", "8x" }, { "16", "16x" } },
			false, "0" },
		// FXAA on the scene, the HUD left sharp (gpu/vk/fxaa.cpp); with FSR 3 upscaling, FSR 3's own anti-aliasing instead
		{ "WWHD_FXAA", "FXAA (smooths the edges; not where FSR 3 upscales)", { { "0", "off" }, { "1", "on" } }, false, nullptr },
		// the game's ambient occlusion (gpu/vk/draw.cpp AoOff): off, the shading in corners and under things goes (the
		// Performance preset: off, the design's "later AO off": gpu-plan.md, item 4)
		{ "WWHD_AO", "Ambient occlusion", { { "1", "on" }, { "0", "off" } }, false, "0" },
		// frontend/overlay.cpp UpdateFpsOverlay: the frames shown a second (with frame generation, the generated ones
		// too: one number, the owner's choice), in the picture's bottom left
		{ "WWHD_FPS_OVERLAY", "FPS overlay", { { "0", "off" }, { "1", "FPS" }, { "2", "FPS + frame time" } }, true, nullptr },
		{ "WWHD_CORES", "CPU threads", { { "3", "3" }, { "1", "1" } }, false, "3" },
		{ "WWHD_LAZY_DRAWDONE", "Lazy DrawDone", { { "1", "on" }, { "0", "off" } }, false, "1" },
		// diagnostics (the pause menu's Diagnostics tab): the renderer's and the runtime's own speed-ups, each on by default,
		// off only to rule one out when something looks wrong
		{ "WWHD_WRITE_WATCH", "Texture write-watch", { { "1", "on" }, { "0", "off" } }, false, nullptr },
		{ "WWHD_VTX_DEDUPE", "Vertex dedupe", { { "1", "on" }, { "0", "off" } }, false, nullptr },
		{ "WWHD_SURFACE_FIT", "Surface fit", { { "1", "on" }, { "0", "off" } }, false, nullptr },
		{ "WWHD_BARRIERS", "Precise barriers", { { "1", "on" }, { "all", "off" } }, false, nullptr },
		{ "WWHD_CLEAR_LOADOP", "Clears as load ops", { { "1", "on" }, { "0", "off" } }, false, nullptr },
	};
	constexpr const char* kPresetKey = "WWHD_PRESET";
	const char* const kPresets[] = { "auto", "performance", "quality" };

	std::mutex s_lock;
	bool s_active = false;
	std::string s_path;
	std::map<std::string, std::string> s_file;      // the file's lines (unknown keys kept as they are)
	std::map<std::string, std::string> s_atStart;   // each option's value when the program started
	std::set<std::string> s_fromEnv;                // options the environment set (a launcher's own: they win)
	std::string s_presetAtStart;                    // "performance" or "quality", as resolved at start
	bool s_presetFromEnv = false;
	std::string s_autoChoice, s_autoWhy;            // what Auto picks on this machine, and why
	std::string s_startLog;                         // the start's decision, logged once Cemu's log is up
	std::string s_startLog2;                        // and notes on the file's lines
	std::map<std::string, std::string> s_notes;     // SetNote's, by key
	bool s_windowChanged = false, s_vsyncChanged = false;

	int IndexOf(const Option& o, const std::string& v)
	{
		for (size_t i = 0; i < o.values.size(); i++)
			if (v == o.values[i].value)
				return (int)i;
		return 0;
	}

	std::string ReadLine(const std::string& path)
	{
		std::ifstream f(path);
		std::string s;
		std::getline(f, s);
		while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
			s.pop_back();
		return s;
	}

	// a low-powered device (sysfs only: this runs before anything else): the Steam Deck, a battery, only integrated
	// GPUs (Intel, or AMD with at most 2 GiB of VRAM: an APU), or at most 8 logical CPUs
	bool LowPowered(std::string& why)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		const std::string product = ReadLine("/sys/class/dmi/id/product_name");
		if (product == "Jupiter" || product == "Galileo")
			return why = "Steam Deck", true;
		for (const auto& e : fs::directory_iterator("/sys/class/power_supply", ec))
			if (e.path().filename().string().rfind("BAT", 0) == 0)
				return why = "a battery", true;
		int gpus = 0, integrated = 0;
		for (const auto& e : fs::directory_iterator("/sys/class/drm", ec))
		{
			const std::string name = e.path().filename().string();
			if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos)
				continue;
			const std::string vendor = ReadLine(e.path().string() + "/device/vendor");
			if (vendor.empty())
				continue;
			gpus++;
			if (vendor == "0x8086")
				integrated++;
			else if (vendor == "0x1002")
			{
				const std::string vram = ReadLine(e.path().string() + "/device/mem_info_vram_total");
				if (!vram.empty() && strtoull(vram.c_str(), nullptr, 10) <= (2ull << 30))
					integrated++;
			}
		}
		if (gpus > 0 && integrated == gpus)
			return why = "integrated GPU only", true;
		const unsigned cpus = std::thread::hardware_concurrency();
		if (cpus > 0 && cpus <= 8)
			return why = std::to_string(cpus) + " logical CPUs", true;
		why = "none of: Steam Deck, battery, integrated GPU only, 8 or fewer CPUs";
		return false;
	}

	// the preset chosen in the file (auto, performance, quality), and what a choice resolves to
	std::string PresetChosen()
	{
		auto f = s_file.find(kPresetKey);
		const std::string p = f != s_file.end() ? f->second : "auto";
		return (p == "performance" || p == "quality") ? p : "auto";
	}
	std::string Resolve(const std::string& p)
	{
		return p == "auto" ? s_autoChoice : p;
	}

	const char* Default(const Option& o) { return o.values[o.def].value; }

	// an option's value as the file and the preset give it (not the environment)
	std::string Current(const Option& o)
	{
		if (auto f = s_file.find(o.key); f != s_file.end())
			return f->second;
		if (o.performance && Resolve(PresetChosen()) == "performance")
			return o.performance;
		return Default(o);
	}

	// some option the preset covers has a line of its own that differs from the preset
	bool Custom()
	{
		const bool perf = Resolve(PresetChosen()) == "performance";
		for (const Option& o : kOptions)
			if (o.performance)
				if (auto f = s_file.find(o.key); f != s_file.end() && f->second != (perf ? o.performance : Default(o)))
					return true;
		return false;
	}

	void Save()
	{
		std::ofstream out(s_path, std::ios::trunc);
		out << "# The Wind Waker HD recompiled: settings (the settings page, F2). KEY=VALUE, the program's own switches;\n"
		       "# WWHD_PRESET=auto|performance|quality sets those an option's own line doesn't; a switch set in the\n"
		       "# environment wins over its line here.\n";
		for (const auto& [k, v] : s_file)
			out << k << "=" << v << "\n";
	}
}

namespace wwhd::os::settings
{
	void Load(const std::string& portableDir)
	{
		std::lock_guard lock(s_lock);
		const char* use = getenv("WWHD_SETTINGS");
		const char* window = getenv("WWHD_WINDOW");
		s_active = !(use && strcmp(use, "0") == 0) && window && strcmp(window, "1") == 0 && !getenv("CEMU_VIRTUAL_CLOCK");
		if (!s_active)
			return;
		s_path = portableDir + "/wwhd.ini";
		std::ifstream in(s_path);
		for (std::string line; std::getline(in, line);)
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.pop_back();
			const size_t eq = line.find('=');
			if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0)
				continue;
			s_file[line.substr(0, eq)] = line.substr(eq + 1);
		}
		s_autoChoice = LowPowered(s_autoWhy) ? "performance" : "quality";
		// the preset: the environment's, else the file's, else auto
		std::string preset = PresetChosen();
		if (const char* e = getenv(kPresetKey); e && *e)
		{
			s_presetFromEnv = true;
			preset = strcmp(e, "performance") == 0 || strcmp(e, "quality") == 0 ? e : "auto";
		}
		s_presetAtStart = Resolve(preset);
		// the file's lines first (every key, unknown ones too), then the preset's values, never over the environment
		for (const auto& [k, v] : s_file)
		{
			if (k == kPresetKey)
				continue;
			if (k == "WWHD_60FPS_KEEPSPEED")              // an option no more: full speed always (t-fpscap, the owner's choice)
			{
				s_startLog2 += "; wwhd.ini's WWHD_60FPS_KEEPSPEED line ignored: the game keeps full speed always";
				continue;
			}
			if (getenv(k.c_str()))
				s_fromEnv.insert(k);
			else
				setenv(k.c_str(), v.c_str(), 0);
		}
		// a file from before the frame rate's four values has WWHD_60FPS=0|1: it stands for WWHD_FRAMERATE=30|60
		if (!getenv("WWHD_FRAMERATE"))
			if (const char* old = getenv("WWHD_60FPS"))
				setenv("WWHD_FRAMERATE", atoi(old) == 1 ? "60" : "30", 0);
		for (const Option& o : kOptions)
		{
			const char* e = getenv(o.key);
			if (!e && o.performance && s_presetAtStart == "performance")
				setenv(o.key, o.performance, 0);
			else if (e && !s_file.count(o.key))
				s_fromEnv.insert(o.key);
			e = getenv(o.key);
			s_atStart[o.key] = e ? e : Default(o);
		}
		// the frame rate's default (60) is a switch too: without a line or a launcher's, the game would start at 30
		for (const Option& o : kOptions)
			if (o.def != 0 && !getenv(o.key))
				setenv(o.key, Default(o), 0);
		// the frame rate as the program's switches: 60 fps (WWHD_60FPS) from 40 up, and the cap (WWHD_FPS_CAP) at 40 and 50;
		// a launcher's WWHD_60FPS or WWHD_FPS_CAP wins
		if (const char* fr = getenv("WWHD_FRAMERATE"))
		{
			const bool display = strcmp(fr, "display") == 0;
			const int v = display ? 60 : atoi(fr);
			setenv("WWHD_60FPS", v >= 40 ? "1" : "0", 0);
			if (v == 40 || v == 50)
				setenv("WWHD_FPS_CAP", fr, 0);
			if (v == 90 || v == 120 || v == 150)
				setenv("WWHD_STEPS", v == 90 ? "3" : v == 120 ? "4" : "5", 0);
			else if (display)
				setenv("WWHD_STEPS", "display", 0);
		}
		s_startLog = "wwhd settings: preset " + preset + (preset == "auto" ? " -> " + s_autoChoice + " (" + s_autoWhy + ")" : "") +
			(s_presetFromEnv ? " (from the environment)" : "");
		for (const Option& o : kOptions)
			if (o.performance)
				s_startLog += std::string(", ") + o.key + "=" + s_atStart[o.key] + (s_fromEnv.count(o.key) ? " (environment)" : "");
		{
			std::set<std::string> seen;                 // the table lists each option once (a rebase once doubled one)
			for (const Option& o : kOptions)
				if (!seen.insert(o.key).second)
					s_startLog += std::string("; an option listed twice: ") + o.key;
		}
		if (const char* fr = getenv("WWHD_FRAMERATE"))
		{
			const char* sixty = getenv("WWHD_60FPS");
			const char* cap = getenv("WWHD_FPS_CAP");
			const char* steps = getenv("WWHD_STEPS");
			s_startLog += std::string("; frame rate ") + fr + " (WWHD_60FPS=" + (sixty ? sixty : "unset") + (cap ? std::string(", WWHD_FPS_CAP=") + cap : "") +
				(steps ? std::string(", WWHD_STEPS=") + steps : "") + ")";
		}
		s_startLog += s_startLog2;
	}

	// the window's display's refresh rate (frontend/window_system.cpp; 0 unknown or no window)
	static std::atomic<float> s_displayHz{ 0.0f };
	void SetDisplayHz(float hz) { s_displayHz = hz; }
	float DisplayHz() { return s_displayHz.load(); }
	// frame generation's decision (gpu/vk/present.cpp, at the first present with a window, before the 60 fps switch)
	static std::atomic<int> s_frameGen{ 1 };
	void SetFrameGen(int multiplier) { s_frameGen = multiplier < 1 ? 1 : multiplier; }
	int FrameGen() { return s_frameGen.load(); }

	std::string StartLog()
	{
		std::lock_guard lock(s_lock);
		return s_startLog;
	}

	bool Active()
	{
		std::lock_guard lock(s_lock);
		return s_active;
	}

	int Count()
	{
		return 1 + (int)kOptions.size();             // the preset, then the options
	}

	std::string Line(int option)
	{
		std::lock_guard lock(s_lock);
		if (option == 0)
		{
			std::string chosen = PresetChosen();
			if (s_presetFromEnv)
			{
				const char* e = getenv(kPresetKey);
				chosen = e && (strcmp(e, "performance") == 0 || strcmp(e, "quality") == 0) ? e : "auto";
			}
			const std::string resolved = Resolve(chosen);
			std::string line = "Preset: ";
			if (!s_presetFromEnv && Custom())
				line += "Custom";
			else if (chosen == "performance")
				line += "Performance";
			else if (chosen == "quality")
				line += "Quality";
			else
				line += std::string("Auto (") + (resolved == "performance" ? "Performance" : "Quality") + ")";
			if (!s_active)
				line += " (not saved: no window or a test)";
			else if (s_presetFromEnv)
				line += " (set by the launcher)";
			else if (resolved != s_presetAtStart)
				line += " (at the next start)";
			return line;
		}
		option--;
		if (option < 0 || option >= (int)kOptions.size())
			return {};
		const Option& o = kOptions[option];
		const std::string v = s_fromEnv.count(o.key) ? s_atStart[o.key] : Current(o);
		std::string line = std::string(o.label) + ": " + o.values[IndexOf(o, v)].shown;
		if (!v.empty() && o.values[IndexOf(o, v)].value != v)       // a value the list hasn't (the file's, the launcher's)
			line = std::string(o.label) + ": " + v + " (not one of the list)";
		if (auto n = s_notes.find(o.key); n != s_notes.end())
			line += " (" + n->second + ")";
		if (!s_active)
			line += " (not saved: no window or a test)";
		else if (s_fromEnv.count(o.key))
			line += " (set by the launcher)";
		else if (!o.live && v != s_atStart[o.key])
			line += " (at the next start)";
		return line;
	}

	void SetNote(const char* key, const std::string& note)
	{
		std::lock_guard lock(s_lock);
		if (note.empty())
			s_notes.erase(key);
		else
			s_notes[key] = note;
	}

	void Cycle(int option, int step)
	{
		std::lock_guard lock(s_lock);
		if (!s_active)
			return;
		if (option == 0)
		{
			if (s_presetFromEnv)
				return;                              // the launcher's own
			const std::string chosen = PresetChosen();
			int i = 0;
			while (i < 2 && chosen != kPresets[i])
				i++;
			// out of Custom, the first step lands on the chosen preset itself (its options' own lines dropped)
			if (!Custom())
				i = ((i + step) % 3 + 3) % 3;
			s_file[kPresetKey] = kPresets[i];
			for (const Option& o : kOptions)
				if (o.performance)
					s_file.erase(o.key);
			Save();
			return;
		}
		option--;
		if (option < 0 || option >= (int)kOptions.size())
			return;
		const Option& o = kOptions[option];
		if (s_fromEnv.count(o.key))
			return;                                  // the launcher's own: changing the file would do nothing
		const int n = (int)o.values.size();
		const int i = ((IndexOf(o, Current(o)) + step) % n + n) % n;
		s_file[o.key] = o.values[i].value;
		// a preset's option back at the preset's value needs no line of its own
		if (o.performance && s_file[o.key] == (Resolve(PresetChosen()) == "performance" ? o.performance : Default(o)))
			s_file.erase(o.key);
		Save();
		if (o.live)
		{
			setenv(o.key, o.values[i].value, 1);
			if (strcmp(o.key, "WWHD_VSYNC") == 0)
				s_vsyncChanged = true;
			else if (strcmp(o.key, "WWHD_FULLSCREEN") == 0 || strcmp(o.key, "WWHD_WINDOW_SIZE") == 0)
				s_windowChanged = true;                      // the FPS overlay's reads the switch itself
		}
	}

	int Find(const std::string& key)
	{
		if (key == kPresetKey)
			return 0;
		for (size_t i = 0; i < kOptions.size(); i++)
			if (key == kOptions[i].key)
				return (int)i + 1;
		return -1;
	}

	OptionView View(int option)
	{
		std::lock_guard lock(s_lock);
		OptionView v;
		v.active = s_active;
		if (option == 0)
		{
			v.key = kPresetKey, v.label = "Preset";
			v.values = { "auto", "performance", "quality" }, v.shown = { "Auto", "Performance", "Quality" };
			const std::string chosen = PresetChosen();
			v.current = chosen == "performance" ? 1 : chosen == "quality" ? 2 : 0;
			v.custom = !s_presetFromEnv && Custom();
			v.resolved = Resolve(chosen);
			v.atStartValue = s_presetAtStart;
			v.fromEnv = s_presetFromEnv;
			v.autoWhy = s_autoWhy;
			return v;
		}
		if (option < 1 || option > (int)kOptions.size())
			return v;
		const Option& o = kOptions[option - 1];
		v.key = o.key, v.label = o.label, v.live = o.live, v.def = o.def;
		for (const Value& x : o.values)
			v.values.push_back(x.value), v.shown.push_back(x.shown);
		v.fromEnv = s_fromEnv.count(o.key) != 0;
		const std::string cur = v.fromEnv ? s_atStart[o.key] : Current(o);
		v.current = IndexOf(o, cur);
		v.atStartValue = s_atStart[o.key];
		v.atStart = IndexOf(o, v.atStartValue);
		v.performance = o.performance ? o.performance : "";
		if (auto n = s_notes.find(o.key); n != s_notes.end())
			v.note = n->second;
		return v;
	}

	void Set(int option, int value)
	{
		const OptionView v = View(option);
		const int n = (int)v.values.size();
		if (!n || value < 0 || value >= n || value == v.current)
			return;
		if (option == 0)
		{
			// the preset: straight to the chosen one (its options' own lines dropped), from Custom too
			std::lock_guard lock(s_lock);
			if (!s_active || s_presetFromEnv)
				return;
			s_file[kPresetKey] = kPresets[value];
			for (const Option& o : kOptions)
				if (o.performance)
					s_file.erase(o.key);
			Save();
			return;
		}
		Cycle(option, ((value - v.current) % n + n) % n);
	}

	void ResetToDefaults(const std::vector<std::string>& keys)
	{
		for (const std::string& k : keys)
		{
			const int i = Find(k);
			if (i > 0)
			{
				// a preset's option goes back to what the preset gives it (its line dropped), any other to its default
				std::lock_guard lock(s_lock);
				if (!s_active || s_fromEnv.count(k))
					continue;
				const Option& o = kOptions[i - 1];
				const std::string target = o.performance && Resolve(PresetChosen()) == "performance" ? o.performance : Default(o);
				const bool changed = Current(o) != target;
				s_file.erase(k);
				if (changed && o.live)
				{
					setenv(o.key, target.c_str(), 1);
					if (k == "WWHD_VSYNC")
						s_vsyncChanged = true;
					else if (k == "WWHD_FULLSCREEN" || k == "WWHD_WINDOW_SIZE")
						s_windowChanged = true;
				}
			}
			else if (i == 0)
			{
				std::lock_guard lock(s_lock);
				if (!s_active || s_presetFromEnv)
					continue;
				s_file.erase(kPresetKey);
				for (const Option& o : kOptions)
					if (o.performance)
						s_file.erase(o.key);
			}
		}
		std::lock_guard lock(s_lock);
		if (s_active)
			Save();
	}

	bool TakeWindowRequest(bool& fullscreen, int& width, int& height, bool& vsync)
	{
		std::lock_guard lock(s_lock);
		vsync = s_vsyncChanged;
		const bool window = s_windowChanged;
		s_vsyncChanged = s_windowChanged = false;
		const char* f = getenv("WWHD_FULLSCREEN");
		fullscreen = f && strcmp(f, "1") == 0;
		width = height = 0;
		if (const char* s = getenv("WWHD_WINDOW_SIZE"))
			sscanf(s, "%dx%d", &width, &height);
		return window || vsync;
	}
}
