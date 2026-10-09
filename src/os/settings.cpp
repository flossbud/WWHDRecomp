// The settings (settings.h): the options, the presets, their file and what applies live.
//
// Presets (the owner's ask: like a modern game's, Performance turns on everything that makes it faster, Quality
// keeps the defaults, Auto picks Performance on a low-powered device): WWHD_PRESET=auto|performance|quality. An
// option a preset covers takes its value from, first to last: the environment, the option's own line in the file,
// the preset, the program's default. Changing such an option in the menu writes its own line (the preset then shows
// "Custom"); choosing a preset again drops those lines. Frame rate, vsync and the display are no preset's. The frame
// rate (WWHD_FRAMERATE 30/40/50/60) sets WWHD_60FPS and, at 40 and 50, WWHD_FPS_CAP (overrides/pacing.cpp).
#include "settings.h"
#include <cstdio>
#include <cstdlib>
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
		std::vector<Value> values;                  // the first is the program's default (no file, no switch)
		bool live;                                  // applied at once (else at the next start)
		const char* performance;                    // its value in the Performance preset, or nullptr: no preset's
	};
	const std::vector<Option> kOptions = {
		// 40 and 50 run 60's logic with a fixed share of its half frames left out (pacing.cpp, WWHD_FPS_CAP): smoother at 30 or 60
		{ "WWHD_FRAMERATE", "Frame rate", { { "30", "30" }, { "40", "40 (smoother at 30 or 60)" }, { "50", "50 (smoother at 30 or 60)" },
			{ "60", "60" } }, false, nullptr },
		{ "WWHD_VSYNC", "Vsync", { { "0", "off" }, { "1", "on" } }, true, nullptr },
		{ "WWHD_FULLSCREEN", "Display", { { "0", "window" }, { "1", "fullscreen" } }, true, nullptr },
		{ "WWHD_WINDOW_SIZE", "Window size", { { "1280x720", "1280x720" }, { "1600x900", "1600x900" },
			{ "1920x1080", "1920x1080" }, { "2560x1440", "2560x1440" } }, true, nullptr },
		{ "WWHD_RENDER_SCALE", "Render scale", { { "1", "100%" }, { "0.75", "75%" }, { "0.5", "50%" }, { "auto", "Auto" } }, false, "auto" },
		{ "WWHD_RENDER_SCALE", "Render scale", { { "1", "100%" }, { "0.75", "75%" }, { "0.5", "50%" }, { "auto", "Auto" } }, false, "auto" },
		{ "WWHD_UPSCALER", "Upscaler (render scale under 100%)", { { "none", "bilinear" }, { "fsr1", "FSR 1" } }, false, nullptr },
		{ "WWHD_CORES", "CPU threads", { { "3", "3" }, { "1", "1" } }, false, "3" },
		{ "WWHD_LAZY_DRAWDONE", "Lazy DrawDone", { { "0", "off" }, { "1", "on" } }, false, "1" },
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

	// an option's value as the file and the preset give it (not the environment)
	std::string Current(const Option& o)
	{
		if (auto f = s_file.find(o.key); f != s_file.end())
			return f->second;
		if (o.performance && Resolve(PresetChosen()) == "performance")
			return o.performance;
		return o.values[0].value;
	}

	// some option the preset covers has a line of its own that differs from the preset
	bool Custom()
	{
		const bool perf = Resolve(PresetChosen()) == "performance";
		for (const Option& o : kOptions)
			if (o.performance)
				if (auto f = s_file.find(o.key); f != s_file.end() && f->second != (perf ? o.performance : o.values[0].value))
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
			s_atStart[o.key] = e ? e : o.values[0].value;
		}
		// the frame rate as the program's switches: 60 fps (WWHD_60FPS) from 40 up, and the cap (WWHD_FPS_CAP) at 40 and 50;
		// a launcher's WWHD_60FPS or WWHD_FPS_CAP wins
		if (const char* fr = getenv("WWHD_FRAMERATE"))
		{
			const int v = atoi(fr);
			setenv("WWHD_60FPS", v >= 40 ? "1" : "0", 0);
			if (v == 40 || v == 50)
				setenv("WWHD_FPS_CAP", fr, 0);
		}
		s_startLog = "wwhd settings: preset " + preset + (preset == "auto" ? " -> " + s_autoChoice + " (" + s_autoWhy + ")" : "") +
			(s_presetFromEnv ? " (from the environment)" : "");
		for (const Option& o : kOptions)
			if (o.performance)
				s_startLog += std::string(", ") + o.key + "=" + s_atStart[o.key] + (s_fromEnv.count(o.key) ? " (environment)" : "");
		if (const char* fr = getenv("WWHD_FRAMERATE"))
		{
			const char* sixty = getenv("WWHD_60FPS");
			const char* cap = getenv("WWHD_FPS_CAP");
			s_startLog += std::string("; frame rate ") + fr + " (WWHD_60FPS=" + (sixty ? sixty : "unset") + (cap ? std::string(", WWHD_FPS_CAP=") + cap : "") + ")";
		}
		s_startLog += s_startLog2;
	}

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
		if (!s_active)
			line += " (not saved: no window or a test)";
		else if (s_fromEnv.count(o.key))
			line += " (set by the launcher)";
		else if (!o.live && v != s_atStart[o.key])
			line += " (at the next start)";
		return line;
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
		if (o.performance && s_file[o.key] == (Resolve(PresetChosen()) == "performance" ? o.performance : o.values[0].value))
			s_file.erase(o.key);
		Save();
		if (o.live)
		{
			setenv(o.key, o.values[i].value, 1);
			if (strcmp(o.key, "WWHD_VSYNC") == 0)
				s_vsyncChanged = true;
			else
				s_windowChanged = true;
		}
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
