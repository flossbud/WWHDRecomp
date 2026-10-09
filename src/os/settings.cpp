// The settings (settings.h): the options, their file and what applies live.
#include "settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace
{
	struct Value { const char* value; const char* shown; };
	struct Option
	{
		const char* key;                            // the switch
		const char* label;
		std::vector<Value> values;                  // the first is the program's default (no file, no switch)
		bool live;                                  // applied at once (else at the next start)
	};
	const std::vector<Option> kOptions = {
		{ "WWHD_60FPS", "Frame rate", { { "0", "30" }, { "1", "60" } }, false },
		{ "WWHD_VSYNC", "Vsync", { { "0", "off" }, { "1", "on" } }, true },
		{ "WWHD_FULLSCREEN", "Display", { { "0", "window" }, { "1", "fullscreen" } }, true },
		{ "WWHD_WINDOW_SIZE", "Window size", { { "1280x720", "1280x720" }, { "1600x900", "1600x900" },
			{ "1920x1080", "1920x1080" }, { "2560x1440", "2560x1440" } }, true },
		{ "WWHD_LAZY_DRAWDONE", "Lazy DrawDone (faster)", { { "0", "off" }, { "1", "on" } }, false },
		{ "WWHD_CORES", "CPU threads", { { "1", "1" }, { "3", "3" } }, false },
		{ "WWHD_60FPS_KEEPSPEED", "Keep game speed when frames drop", { { "0", "off" }, { "1", "on" } }, false },
	};

	std::mutex s_lock;
	bool s_active = false;
	std::string s_path;
	std::map<std::string, std::string> s_file;      // the file's lines (unknown keys kept as they are)
	std::map<std::string, std::string> s_atStart;   // each option's value when the program started
	std::set<std::string> s_fromEnv;                // options the environment set (a launcher's own: they win)
	bool s_windowChanged = false, s_vsyncChanged = false;

	int IndexOf(const Option& o, const std::string& v)
	{
		for (size_t i = 0; i < o.values.size(); i++)
			if (v == o.values[i].value)
				return (int)i;
		return 0;
	}

	std::string Current(const Option& o)
	{
		auto f = s_file.find(o.key);
		return f != s_file.end() ? f->second : o.values[0].value;
	}

	void Save()
	{
		std::ofstream out(s_path, std::ios::trunc);
		out << "# The Wind Waker HD recompiled: settings (the settings page, F2). KEY=VALUE, the program's own switches;\n"
		       "# a switch set in the environment wins over its line here.\n";
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
		for (const auto& [k, v] : s_file)
		{
			if (getenv(k.c_str()))
				s_fromEnv.insert(k);
			else
				setenv(k.c_str(), v.c_str(), 0);
		}
		for (const Option& o : kOptions)
		{
			const char* e = getenv(o.key);
			s_atStart[o.key] = e ? e : o.values[0].value;
			if (e && !s_file.count(o.key))
				s_fromEnv.insert(o.key);
		}
	}

	bool Active()
	{
		std::lock_guard lock(s_lock);
		return s_active;
	}

	int Count()
	{
		return (int)kOptions.size();
	}

	std::string Line(int option)
	{
		std::lock_guard lock(s_lock);
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
		if (!s_active || option < 0 || option >= (int)kOptions.size())
			return;
		const Option& o = kOptions[option];
		if (s_fromEnv.count(o.key))
			return;                                  // the launcher's own: changing the file would do nothing
		const int n = (int)o.values.size();
		const int i = ((IndexOf(o, Current(o)) + step) % n + n) % n;
		s_file[o.key] = o.values[i].value;
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
