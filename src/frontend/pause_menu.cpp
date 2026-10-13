// The pause menu (pause_menu.h). Laid out in the design's 1920x1080 space, drawn at s = min(W/1920, H/1080) of the
// window (the README): the side panel at the left edge, the prompts and toasts at the right, dialogs centred; the
// design's PNGs at 1x (s <= 1) or 2x, scaled to s; text from the TTFs at its size times s. M1: the menu, its cursor,
// prompts, captions and motion, Resume, and Quit to the desktop; M2 Settings; M3 Save; M4 Enhancements (none yet), Quit
// to the title screen (the game's own soft reset) and the "Pause" prompt while closed. The start screen (t-startscreen,
// the owner's Start Screen design) is drawn here too, on the same layers: S1 Launch over the title's first frame, held.
#include "pause_menu.h"
#include "game_files.h"
#include "ui_draw.h"
#include "../gpu/vk/renderer.h"
#include "../os/savestate.h"
#include "../os/settings.h"
#include <rapidjson/document.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace wwhd::sixty                                // overrides/sixty.cpp: the game's soft reset
{
	bool TitleAvailable();
	uint32 TitleRequests();
	uint32 PlayStarts();
	bool ReturnToTitle();
}

namespace wwhd::pause_menu
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using ui::Colour;
		using ui::Image;
		namespace B = os::input;

		// ---- the design's tokens (README "Design tokens") ----
		constexpr Colour kFrame = Colour::Hex(0x8cc063), kRimLight = Colour::Hex(0xb3da86), kRimDark = Colour::Hex(0x7fb456),
			kSheet = Colour::Hex(0xdef0a8), kInk = Colour::Hex(0x24461b), kInkMuted = Colour::Hex(0x46702c), kDot = Colour::Hex(0x93c36a),
			kPromptText = Colour::Hex(0x3b4d66), kPromptOutline = Colour::Hex(0x2f78d2), kWhite = Colour::Hex(0xffffff),
			kScrim = Colour::Hex(0x14321e, 89), kButtonRim = Colour::Hex(0xf2f9dc);

		// a CSS cubic-bezier(x1, y1, x2, y2) at progress x (0..1): its y
		float Bezier(float x1, float y1, float x2, float y2, float x)
		{
			x = std::clamp(x, 0.0f, 1.0f);
			float lo = 0, hi = 1, t = x;
			for (int i = 0; i < 24; i++)
			{
				t = (lo + hi) / 2;
				const float bx = 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t;
				(bx < x ? lo : hi) = t;
			}
			return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t;
		}
		float Linear(float ms, float delay, float dur) { return std::clamp((ms - delay) / dur, 0.0f, 1.0f); }

		std::string ExeDir()
		{
			if (const char* home = getenv("WWHD_HOME"); home && *home)   // the Android app (cemu_boot.cpp's SetupPaths)
				return home;
			std::error_code ec;
			return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().string();
		}

		// the design's assets: WWHD_UI_ASSETS, else next to the program (a deploy), else the checkout's (build/wwhd/..)
		std::string AssetDir()
		{
			if (const char* e = getenv("WWHD_UI_ASSETS"); e && *e)
				return e;
			const std::string exe = ExeDir();
			for (const std::string& d : { exe + "/assets/pause_menu", exe + "/../../assets/pause_menu" })
				if (std::filesystem::exists(d + "/fonts/RocknRollOne-Regular.ttf"))
					return d;
			return "";
		}

		// "{N minutes ago}" for a file's time (the game's own save: File 1), or "" when it isn't there
		std::string Ago(const std::filesystem::path& p)
		{
			std::error_code ec;
			const auto t = std::filesystem::last_write_time(p, ec);
			if (ec)
				return "";
			const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - t).count();
			if (secs < 60)
				return "just now";
			if (secs < 3600)
				return fmt::format("{} minute{} ago", secs / 60, secs / 60 == 1 ? "" : "s");
			if (secs < 86400)
				return fmt::format("{} hour{} ago", secs / 3600, secs / 3600 == 1 ? "" : "s");
			return fmt::format("{} day{} ago", secs / 86400, secs / 86400 == 1 ? "" : "s");
		}
		std::filesystem::path File1()
		{
			return ExeDir() + "/portable/mlc01/usr/save/00050000/10143500/user/80000001/cking.sav";
		}

		// ---- assets at the current scale ----
		struct Assets
		{
			std::string dir;
			ui::Font reggae, rocknroll;
			float s = 0;
			std::unordered_map<std::string, Image> cache;   // name -> at s

			// the asset at its design size times s, from the 1x PNG (s <= 1) or the 2x one
			const Image& Get(const std::string& name)
			{
				auto it = cache.find(name);
				if (it != cache.end())
					return it->second;
				Image base;
				const bool big = s > 1.0f;
				if (!ui::LoadPng(dir + (big ? "/png/4k/" + name + "-2x.png" : "/png/1080p/" + name + ".png"), base) && big)
					ui::LoadPng(dir + "/png/1080p/" + name + ".png", base), (void)0;
				const float k = big && base.w ? s / 2.0f : s;
				Image& out = cache[name];
				if (!base.Empty())
					out = ui::Scaled(base, std::max(1, (int)std::lround(base.w * k)), std::max(1, (int)std::lround(base.h * k)));
				return out;
			}
			void Rescale(float scale)
			{
				if (scale != s)
					s = scale, cache.clear();
			}
		};

		// ---- Settings (M2): the design's rows (assets/pause_menu/menu-and-settings.json: its copy and options) on the
		// program's switches (os/settings.h): each row's option; a choice row's choices name the switch's value each
		// ({"label", "value"}), in the menu's order, paired by value (CheckRows logs what doesn't pair) ----
		struct Row
		{
			std::string id, label, type, description, env;  // type: choice, toggle, info
			std::vector<std::string> options, values;       // a choice row's choices: their labels and the switch's values
			std::string key;                                // the switch ("" none: an info row)
		};
		struct Category { std::string name; std::vector<Row> rows; };
		std::vector<Category>& Categories()
		{
			static std::vector<Category> c;
			return c;
		}
		// the design's row ids and the switches they set
		const std::map<std::string, std::string> kRowKeys = { { "preset", "WWHD_PRESET" }, { "fps", "WWHD_FRAMERATE" },
			{ "renderScale", "WWHD_RENDER_SCALE" }, { "upscaler", "WWHD_UPSCALER" }, { "frameGen", "WWHD_FRAMEGEN" },
			{ "aniso", "WWHD_ANISO" }, { "fxaa", "WWHD_FXAA" }, { "ao", "WWHD_AO" }, { "display", "WWHD_FULLSCREEN" },
			{ "resolution", "WWHD_WINDOW_SIZE" }, { "aspect", "WWHD_ASPECT" }, { "vsync", "WWHD_VSYNC" }, { "vrr", "WWHD_VRR" },
			{ "fpsOverlay", "WWHD_FPS_OVERLAY" }, { "cpuThreads", "WWHD_CORES" }, { "lazyDD", "WWHD_LAZY_DRAWDONE" },
			{ "writeWatch", "WWHD_WRITE_WATCH" }, { "vtxDedupe", "WWHD_VTX_DEDUPE" }, { "surfaceFit", "WWHD_SURFACE_FIT" },
			{ "barriers", "WWHD_BARRIERS" }, { "clearLoadop", "WWHD_CLEAR_LOADOP" } };

		void CheckRows();
		bool LoadCategories(const std::string& path)
		{
			FILE* f = fopen(path.c_str(), "rb");
			if (!f)
				return false;
			std::string text;
			char buf[65536];
			for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;)
				text.append(buf, n);
			fclose(f);
			rapidjson::Document d;
			d.Parse(text.c_str());
			if (d.HasParseError() || !d.HasMember("settingsCategories") || !d["settingsCategories"].IsArray())
				return false;
			auto str = [](const rapidjson::Value& v, const char* k) {
				return v.HasMember(k) && v[k].IsString() ? std::string(v[k].GetString()) : std::string();
			};
			for (const auto& c : d["settingsCategories"].GetArray())
			{
				Category cat{ str(c, "name"), {} };
				if (cat.name == "Enhancements" || !c.HasMember("rows"))
					continue;                               // its rows are the design's placeholders (M4)
				for (const auto& r : c["rows"].GetArray())
				{
					Row row{ str(r, "id"), str(r, "label"), str(r, "type"), str(r, "description"), str(r, "envVar"), {}, {} };
					if (r.HasMember("options") && r["options"].IsArray())
						for (const auto& o : r["options"].GetArray())
							if (o.IsObject() && !o.HasMember("planned"))
								row.options.push_back(str(o, "label")), row.values.push_back(str(o, "value"));
					if (auto k = kRowKeys.find(row.id); k != kRowKeys.end())
						row.key = k->second;
					cat.rows.push_back(std::move(row));
				}
				Categories().push_back(std::move(cat));
			}
			CheckRows();
			return !Categories().empty();
		}

		// a row as it shows now: its value, note (warn: in the warning ink), markers, and whether it can change
		struct Look
		{
			int option = -1;                                // settings.h's number (-1: none)
			os::settings::OptionView v;
			std::string value, note, lockWhy;
			bool warn = false, changed = false, nextStart = false, locked = false, dimmed = false, on = false;
			int index = 0;
		};
		std::string Setting(const std::string& key)          // a switch's value as set (the file's, else the default)
		{
			const int i = os::settings::Find(key);
			if (i < 0)
				return "";
			const os::settings::OptionView v = os::settings::View(i);
			return v.current < (int)v.values.size() ? v.values[v.current] : "";
		}
		// a switch value's place among a row's choices (-1: not one of them)
		int ChoiceOf(const Row& r, const os::settings::OptionView& v, int j)
		{
			if (j < 0 || j >= (int)v.values.size())
				return -1;
			const auto it = std::find(r.values.begin(), r.values.end(), v.values[j]);
			return it == r.values.end() ? -1 : (int)(it - r.values.begin());
		}

		// a switch value's label: its choice's, else the switch's own less its note in brackets (logged by CheckRows)
		std::string ChoiceLabel(const Row& r, const os::settings::OptionView& v, int j)
		{
			if (const int c = ChoiceOf(r, v, j); c >= 0)
				return r.options[c];
			if (j < 0 || j >= (int)v.shown.size())
				return "";
			std::string t = v.shown[j].substr(0, v.shown[j].find(" ("));
			if (!t.empty())
				t[0] = (char)toupper((unsigned char)t[0]);
			return t;
		}

		// each choice row against its switch (os/settings.cpp's options): a choice whose value the switch hasn't got is
		// dropped, a value of the switch's no choice names is logged (it shows by the switch's own label when set)
		void CheckRows()
		{
			for (Category& cat : Categories())
				for (Row& r : cat.rows)
				{
					if (r.type != "choice" || r.key.empty())
						continue;
					const int i = os::settings::Find(r.key);
					if (i < 0)
					{
						cemuLog_log(LogType::Force, "wwhd: pause menu: the row {} names {}, no switch of the program's", r.id, r.key);
						continue;
					}
					const os::settings::OptionView v = os::settings::View(i);
					for (size_t c = 0; c < r.values.size();)
						if (std::find(v.values.begin(), v.values.end(), r.values[c]) == v.values.end())
						{
							cemuLog_log(LogType::Force, "wwhd: pause menu: the row {}'s choice \"{}\" ({}={}) isn't one of the switch's: left out",
								r.id, r.options[c], r.key, r.values[c]);
							r.options.erase(r.options.begin() + c), r.values.erase(r.values.begin() + c);
						}
						else
							c++;
					for (const std::string& val : v.values)
						if (std::find(r.values.begin(), r.values.end(), val) == r.values.end())
							cemuLog_log(LogType::Force, "wwhd: pause menu: the row {} offers no choice for {}={}", r.id, r.key, val);
				}
		}

		Look LookAt(const Row& r)
		{
			Look k;
			if (r.id == "keepSpeed")
			{
				k.on = true, k.value = "On", k.locked = true, k.lockWhy = "The game always keeps full speed.";
				return k;
			}
			if (r.key.empty())
			{
				k.value = Setting("WWHD_UPSCALER") == "fsr3" ? "On \xC2\xB7 FSR 3" : "Off";   // motion vectors: FSR 3's
				return k;
			}
			k.option = os::settings::Find(r.key);
			if (k.option < 0)
				return k;
			k.v = os::settings::View(k.option);
			const auto& v = k.v;
			k.index = v.current;
			k.value = ChoiceLabel(r, v, k.index);
			if (r.type == "toggle")
			{
				k.on = k.index < (int)v.shown.size() && v.shown[k.index] == "on";
				k.value = k.on ? "On" : "Off";
			}
			k.locked = v.fromEnv || !v.active;
			if (v.fromEnv)
				k.lockWhy = "Set by the launcher (" + r.key + ").";
			else if (!v.active)
				k.lockWhy = "Settings are saved only when the game runs in a window.";
			if (k.option == 0)                                // the preset: what Auto resolved to, or Custom
			{
				const std::string res = v.resolved == "performance" ? "Performance" : "Quality";
				if (v.custom)
					k.value = "Custom", k.note = "You changed an option the preset sets";
				else if (k.index == 0)
					k.value = "Auto \xC2\xB7 " + res, k.note = "Picked " + res + " for this PC";
				k.changed = k.index != 0 || v.custom;
				k.nextStart = !v.fromEnv && v.resolved != v.atStartValue;
				return k;
			}
			k.changed = k.index != v.def;
			k.nextStart = !v.live && !v.fromEnv && k.index != v.atStart;
			const std::string val = k.index < (int)v.values.size() ? v.values[k.index] : "";
			if (r.id == "fps" && (val == "40" || val == "50"))
				k.note = "Smoother at 30 or 60";
			else if (r.id == "renderScale" && (val == "1.5" || val == "2"))
				k.note = "Supersampling \xE2\x80\x94 sharper, much heavier";
			else if (r.id == "renderScale" && val == "auto")
				k.note = "Dynamic resolution that holds 60 fps";
			else if (r.id == "vrr" && k.on && Setting("WWHD_FULLSCREEN") != "1")
				k.note = "Usually needs fullscreen", k.warn = true;
			if (!v.note.empty())
				k.note = v.note, k.warn = true;               // the part that reads it says why it's off
			if (k.locked && k.lockWhy.size() > 1)
				k.note = k.lockWhy.substr(0, k.lockWhy.size() - 1), k.warn = false;
			if (r.id == "upscaler")
			{
				const std::string sc = Setting("WWHD_RENDER_SCALE");
				k.dimmed = sc == "1" || sc == "1.5" || sc == "2";
			}
			return k;
		}

		// what doesn't move, drawn once for the window's size: the panel (opaque), the header sheet with its title, each
		// button idle (with the group's shadow) and focused, and the open dialog's frame and message
		struct Layers
		{
			int w = 0, h = 0;
			Image panel, header, button[5][2];
			int dialogKind = -1;
			Image dialog;
			std::vector<std::array<Image, 2>> dialogButtons;   // each idle and focused
			Image under;                                       // the menu under the open dialog, scrim included (once still)
			Image settings;                                    // the panel with Settings drawn on it, once still (settingsKey)
			std::string settingsKey;
		};
		Layers& L()
		{
			static Layers l;
			return l;
		}

		enum class Phase { Closed, Waiting, Opening, Open, Closing };
		enum class Dialog { None, Quit, QuitDesktop, QuitTitle, Overwrite, Load, SaveFile, Saving, Saved };

		struct Toast { std::string text; Clock::time_point at; };

		struct State
		{
			std::mutex lock;
			std::condition_variable wake;
			std::thread thread;
			bool started = false;
			Phase phase = Phase::Closed;
			Clock::time_point since;                    // the phase's start
			Clock::time_point closedAt;                 // when it last closed (a second Esc then doesn't open it again at once)
			int item = 0;                               // the focused menu item, 0..4
			Dialog dialog = Dialog::None;
			int dialogItem = 0;
			Clock::time_point dialogSince;
			std::vector<Toast> toasts;
			bool pad = false;                           // the last device used: the pad (else the keyboard)
			B::Pad lastPad;
			uint32 held = 0;                            // directions and buttons held (the pad's), for the repeat
			Clock::time_point repeatAt;
			bool repeating = false;
			uint32 swallow = 0;                         // buttons held at closing: the game doesn't see them until released
			// the cursor's glide: from where it was to the target over 170 ms
			float curFrom[4] = {}, curTo[4] = {};
			Clock::time_point curSince;
			float compassFrom = 0, compassTo = 0;
			Clock::time_point compassSince;
			std::vector<Clock::time_point> focusSince;  // per menu item: when its focus last changed (the cross-fade)
			std::vector<bool> focused;
			// the screen: the menu, or Settings over it (M2), and since when (the panel's widening, the layers' fades)
			bool settings = false;
			Clock::time_point screenSince;
			int cat = 0, row = 0;
			Clock::time_point rowSince;                 // the focused row's highlight cross-fades in over 140 ms
			int prevRow = -1;
			int sub = 0;                                // the sub-screen up (with settings set): 0 Settings, 1 Save (M3), 2 Enhancements (M4)
			int saveRow = 0, saveAct = 0;               // Save: the row (File 1, slots 1-4), Save (0) or Load (1)
			int dialogSlot = 0;                         // the slot an Overwrite or Load dialog is about
			Dialog quitAfterSave = Dialog::None;        // Save & Quit: to the desktop or the title once File 1 is saved
			Image thumbs[5];                            // the slots' thumbnails (1-4), as loaded when Save opens
			int busySlot = 0;                           // a slot's save (> 0) or load (< 0) asked for, waiting for its outcome
			std::string blackout;                       // the blackout's line while a slot loads ("" none)
			bool toTitle = false;                       // Quit to the title screen: the game let go under the blackout
			uint32 titleSeen = 0;                       // the title's requests when it was asked (sixty TitleRequests)
			Clock::time_point titleAt{}, titleAsked{};  // when it was asked; when the game asked for the title (0: not yet)
			Clock::time_point hintAt{};                 // the "Pause" prompt's show from (0: none)
			uint32 playSeen = 0;                        // sixty PlayStarts when last looked
			std::string hintKey;                        // the prompt's picture as drawn (size and device)
			std::string toggled;                        // the toggle last flipped, and when (its knob slides over 220 ms)
			Clock::time_point toggledSince;
			Image backdrop, blurred;                    // the window's picture when it opened, and blurred
			std::vector<uint32> spare;                  // the pixels of the frame before last (the panel's, handed back): drawn in again
			int bw = 0, bh = 0;
			bool dirty = true;
			// the start screen (t-startscreen S1, Launch): up over the title's first frame, held, until Start Game
			bool start = false;                         // the Launch screen is up (the menu's own items are not)
			bool startDone = false;                     // shown once a run: after Start Game the menu is the pause menu
			// the title's first frame: the game's requests for the title scene (its logo's fade, ~30 frames) end, its stage
			// loads under black, then its sky: the TV picture sampled every 150 ms until it's bright and settled
			uint32 titleReqs = 0;                       // sixty TitleRequests when last looked
			Clock::time_point titleQuietSince{};        // when they last went up
			Clock::time_point probeAt{};                // the sample asked for (0: none out)
			Clock::time_point nextProbe{};              // and when the next may be
			float lastLuma = -1;
			int startView = 0;                          // 0 its menu, 1 Game Files
			int startItem = 0;                          // the focused button (menu 0-3, Game Files 0-1)
			Clock::time_point startFocusSince[4];       // each button's focus change (the cross-fade)
			int startPrev = -1;
			Image launchBase;                           // the frozen frame with the logo and the footer, as drawn (launchKey)
			std::string launchKey;
			game_files::Copy copy;                      // the game files this run started from, and their check
			bool checked = false;                       // their check is known (from game.txt or a check run here)
			struct CheckJob { std::atomic<float> progress{ 0 }; std::atomic<bool> done{ false }, cancel{ false }; game_files::Copy result; };
			std::shared_ptr<CheckJob> job;              // a check running (Re-verify, or the first at start), on its own thread
			// the start screen's Setup (S2): before the game when no copy is registered, or Change Files from Launch
			bool setup = false;                         // Setup is up
			bool setupPre = false;                      // before the game (nothing running: a sky behind it, no hold)
			int sstate = 0;                             // kIdle, kChecking, kBad, kReady
			bool help = false;                          // Help's text in place of the body (idle, bad)
			bool dragging = false;                      // files dragged over the window
			Clock::time_point dragSeen;                 // the last sign of them (no leave on X11: gone after 400 ms)
			std::string setupFile;                      // the files chosen (being checked, or checked)
			game_files::Copy setupCopy;                 // their check's outcome
			std::shared_ptr<CheckJob> setupJob;
			bool browseWanted = false;                  // Browse chosen: the window's loop shows the picker
			std::string setupResult;                    // Start Game from Setup before the game: the copy's path
			Clock::time_point blackoutAt{};             // Start Game: the blackout's fade (600 ms), then the game
			// the menu's own file browser (the pad's, for the Deck's Game Mode, where the system's picker can't be used)
			struct Entry { std::string name, path; int kind; };   // kind: 0 up, 1 a folder, 2 a game folder, 3 a game file
			bool browsing = false;
			std::string browseDir;
			std::vector<Entry> entries;
			int browseRow = 0, browseTop = 0;
		};
		State& S()
		{
			static State s;
			return s;
		}
		Assets& A()
		{
			static Assets a;
			return a;
		}

		const char* const kItems[5] = { "Resume", "Enhancements", "Settings", "Save", "Quit" };
		constexpr float kButtonTops[5] = { 0, 90, 180, 292, 404 };
		constexpr float kWidePanelW = 1380;
		constexpr float kPanelW = 680, kButtonX = 110, kButtonY = 396, kButtonW = 460, kButtonH = 72;

		std::string Caption(int item)
		{
			switch (item)
			{
			case 0: return "Return to your voyage.";
			case 1: return "Quality-of-life and gameplay tweaks.";
			case 2: return "Graphics, display and advanced options.";
			case 3:
			{
				const std::string ago = Ago(File1());
				return ago.empty() ? "Not saved yet." : "Last saved " + ago + ".";
			}
			default: return "Return to the title screen or close the game.";
			}
		}

		// the cursor's box for menu item i (design units): the button plus 13 on every side
		void CursorFor(int i, float out[4])
		{
			out[0] = kButtonX - 13, out[1] = kButtonY + kButtonTops[i] - 13, out[2] = kButtonW + 26, out[3] = kButtonH + 26;
		}
		// Settings' row i: the row (26, 26 + i 74 in the sheet at 84,212, 808 x 70) plus 13 on every side
		void RowCursorFor(int i, float out[4])
		{
			out[0] = 84 + 26 - 13, out[1] = 212 + 26 + i * 74 - 13, out[2] = 808 + 26, out[3] = 70 + 26;
		}
		void DialogCursorFor(int n, int i, float fx, float fy, float out[4])
		{
			// the button zone's left at (952 - (n 280 + (n - 1) 36)) / 2 + i 316 inside the frame's padding, y 19
			out[0] = fx + 44 + (952 - (n * 280 + (n - 1) * 36)) / 2.0f + i * 316 - 13, out[1] = fy + 42 + 260 + 24 + 19 - 13;
			out[2] = 280 + 26, out[3] = 72 + 26;
		}

		void ShowToast(State& st, std::string text)
		{
			st.toasts = { { std::move(text), Clock::now() } };
		}

		// ---- the drawing ----
		struct Frame
		{
			ui::Canvas& c;
			float s;
			float X(float v) const { return v * s; }
		};

		// text centred on (cx, cy), in design units (at the scale's pixels)
		void TextCentred(Frame& f, ui::Font& font, float px, float cx, float cy, const std::string& t, Colour col, float op = 1.0f)
		{
			const float size = px * f.s, w = font.Width(t, size);
			const float base = cy * f.s + (font.Ascent(size) + font.Descent(size)) / 2;
			f.c.Text(font, size, cx * f.s - w / 2, base, t, col, op);
		}

		void DrawSheet(Frame& f, float x, float y, float w, float h, float offs[4], float op)
		{
			Assets& a = A();
			const float s = f.s;
			f.c.Fill(x * s, y * s, w * s, h * s, kSheet, op);
			f.c.Tile(a.Get("texture_sheet-paper_tile"), x * s, y * s, w * s, h * s, 0, 0, false, false, op);
			// the inner shade: inset 0 0 18px rgba(120,165,60,.45), a falloff from each edge
			for (int i = 0; i < (int)(18 * s); i++)
			{
				const float k = 0.45f * (1 - i / (18 * s)) * (1 - i / (18 * s));
				const Colour sh{ 120, 165, 60, 255 };
				f.c.Fill(x * s + i, y * s + i, w * s - 2 * i, 1, sh, k * op / 3);
				f.c.Fill(x * s + i, (y + h) * s - i - 1, w * s - 2 * i, 1, sh, k * op / 3);
				f.c.Fill(x * s + i, y * s + i + 1, 1, h * s - 2 * i - 2, sh, k * op / 3);
				f.c.Fill((x + w) * s - i - 1, y * s + i + 1, 1, h * s - 2 * i - 2, sh, k * op / 3);
			}
			// the torn edges: 18 px strips of the frame's notches, offset per edge so they never mirror
			const Image& hz = a.Get("sheet_torn-notch_horizontal");
			const Image& vt = a.Get("sheet_torn-notch_vertical");
			const float e = 18 * s;
			f.c.Tile(hz, x * s, y * s, w * s, e, offs[0] * s, 0, false, false, op);
			f.c.Tile(hz, x * s, (y + h) * s - e, w * s, e, offs[1] * s, 0, false, true, op);
			f.c.Tile(vt, x * s, y * s, e, h * s, 0, offs[2] * s, false, false, op);
			f.c.Tile(vt, (x + w) * s - e, y * s, e, h * s, 0, offs[3] * s, true, false, op);
		}

		void DrawCursor(Frame& f, const float box[4], float ms, float op)
		{
			const Image& corner = A().Get("cursor_bracket-corner_top-left");
			const float s = f.s, pulse = 5 * s * (1 - std::cos(ms / 900.0f * 2 * 3.14159265f)) / 2;
			const float x0 = box[0] * s - pulse, y0 = box[1] * s - pulse, x1 = (box[0] + box[2]) * s - corner.w + pulse,
				y1 = (box[1] + box[3]) * s - corner.h + pulse;
			const Colour shadow{ 40, 30, 10, 89 };
			const struct { float x, y; bool fx, fy; } corners[4] = { { x0, y0, false, false }, { x1, y0, true, false },
				{ x0, y1, false, true }, { x1, y1, true, true } };
			for (const auto& k : corners)
				f.c.Silhouette(corner, k.x + s, k.y + 2 * s, shadow, op, k.fx, k.fy);
			for (const auto& k : corners)
				f.c.Draw(corner, k.x, k.y, op, k.fx, k.fy);
		}

		// a button as a layer: (focused) its focused image and label, else its idle image over the group's shadow, the label
		Image ButtonLayer(float s, const char* kind, float w, float h, float labelPx, const std::string& label, bool focused)
		{
			ui::Canvas c((int)std::ceil(w * s) + 2, (int)std::ceil((h + 4) * s) + 2);
			Frame f{ c, s };
			Assets& a = A();
			const Image& img = a.Get(std::string("button_") + kind + (focused ? "_focused" : "_idle"));
			if (!focused)
				c.Silhouette(img, 0, 4 * s, { 58, 104, 32, 89 });
			c.Draw(img, 0, 0);
			TextCentred(f, a.rocknroll, labelPx, w / 2, h / 2, label, kInk);
			return c.Take();
		}

		// the glossy button: its image, the group's shadow under it, the label
		void DrawButton(Frame& f, const char* kind, float x, float y, float w, float h, float labelPx, const std::string& label, float focus,
			float op)
		{
			Assets& a = A();
			const Image& idle = a.Get(std::string("button_") + kind + "_idle");
			const Image& on = a.Get(std::string("button_") + kind + "_focused");
			const float s = f.s;
			f.c.Silhouette(idle, x * s, (y + 4) * s, { 58, 104, 32, 89 }, op);
			if (focus < 1)
				f.c.Draw(idle, x * s, y * s, op);
			if (focus > 0)
				f.c.Draw(on, x * s, y * s, op * focus);
			TextCentred(f, a.rocknroll, labelPx, x + w / 2, y + h / 2, label, kInk, op);
		}

		// a prompt's glyph (a pad button, a bumper's pill, or a key), its width
		float GlyphWidth(const std::string& t, bool pad)
		{
			Assets& a = A();
			const float s = a.s;
			if (pad)
				return (float)a.Get(t == "LB" || t == "RB" ? "prompt_pad-bumper_blank" : "prompt_pad-button_blank").w;
			return std::max((float)a.Get("prompt_key_blank").w, a.rocknroll.Width(t, 17 * s) + 20 * s);
		}
		void DrawGlyph(Frame& f, const std::string& t, bool pad, float x, float top, float op)
		{
			Assets& a = A();
			const float s = f.s;
			const bool bumper = pad && (t == "LB" || t == "RB");
			const Image& g = a.Get(pad ? (bumper ? "prompt_pad-bumper_blank" : "prompt_pad-button_blank") : "prompt_key_blank");
			const float gw = GlyphWidth(t, pad);
			if (pad)
				f.c.Draw(g, x, top, op);
			else
				f.c.Draw3Slice(g, x, top, gw, (int)(12 * s), op);
			TextCentred(f, a.rocknroll, pad ? 21.0f : 17.0f, (x + gw / 2) / s, (top + g.h / 2.0f) / s, t, kPromptText, op);
		}

		// the prompts at the top right (right 44, top 34): each its glyphs (4 apart when paired) with its label under them,
		// overlapping them by 12, centred on each other, 26 apart
		void DrawPrompts(Frame& f, const std::vector<std::pair<std::string, std::string>>& prompts, bool pad, float op)
		{
			Assets& a = A();
			const float s = f.s, top = 34 * s;
			const float gh = (float)a.Get(pad ? "prompt_pad-button_blank" : "prompt_key_blank").h;
			float right = f.c.W() - 44 * s;
			for (auto it = prompts.rbegin(); it != prompts.rend(); ++it)
			{
				std::vector<std::string> glyphs;
				std::istringstream words(it->first);
				for (std::string g; words >> g;)
					glyphs.push_back(g);
				const std::string& label = it->second;
				const float lw = a.rocknroll.Width(label, 24 * s);
				float gw = 0;
				for (size_t i = 0; i < glyphs.size(); i++)
					gw += GlyphWidth(glyphs[i], pad) + (i ? 4 * s : 0);
				const float colW = std::max(gw, lw), cx = right - colW / 2;
				float gx = cx - gw / 2;
				for (const std::string& g : glyphs)
				{
					DrawGlyph(f, g, pad, gx, top + (gh - (float)a.Get(pad && (g == "LB" || g == "RB") ? "prompt_pad-bumper_blank" :
						pad ? "prompt_pad-button_blank" : "prompt_key_blank").h) / 2, op);
					gx += GlyphWidth(g, pad) + 4 * s;
				}
				// the label under them, overlapping by 12: white, a 2 px outline, its shadow (0 3px 4px rgba(0,30,90,.45))
				const float base = top + gh - 12 * s + a.rocknroll.Ascent(24 * s) * 0.92f;
				f.c.Text(a.rocknroll, 24 * s, cx - lw / 2, base + 3 * s, label, { 0, 30, 90, 115 }, op, { 0, 30, 90, 60 }, 3 * s);
				f.c.Text(a.rocknroll, 24 * s, cx - lw / 2, base, label, kWhite, op, kPromptOutline, 2 * s);
				right -= colW + 26 * s;
			}
		}

		void DrawToast(Frame& f, State& st, Clock::time_point now)
		{
			if (st.toasts.empty())
				return;
			const Toast& t = st.toasts.back();
			const float ms = std::chrono::duration<float, std::milli>(now - t.at).count();
			if (ms > 2200 + 260)
			{
				st.toasts.clear();
				return;
			}
			Assets& a = A();
			const float s = f.s;
			const float op = ms < 2200 ? Linear(ms, 0, 260) : 1 - Linear(ms, 2200, 260);
			const float dy = 24 * (1 - Bezier(.3f, 1.5f, .5f, 1, Linear(ms, 0, 320)));
			const float tw = a.rocknroll.Width(t.text, 24 * s) / s;
			const float w = tw + 60 + 20, h = 24 * 1.4f + 28 + 20;
			const float x = f.c.W() / s - 56 - w, y = f.c.H() / s - 56 - h + dy;
			f.c.Fill((x + 4) * s, (y + 10) * s, w * s, h * s, { 10, 40, 20, 255 }, 0.2f * op);
			f.c.Fill(x * s, y * s, w * s, h * s, kFrame, op);
			f.c.Fill(x * s, y * s, w * s, 4 * s, kRimLight, op), f.c.Fill(x * s, (y + h - 4) * s, w * s, 4 * s, kRimLight, op);
			f.c.Fill(x * s, y * s, 4 * s, h * s, kRimLight, op), f.c.Fill((x + w - 4) * s, y * s, 4 * s, h * s, kRimLight, op);
			f.c.Fill((x + 10) * s, (y + 10) * s, (w - 20) * s, (h - 20) * s, kSheet, op);
			TextCentred(f, a.rocknroll, 24, x + w / 2, y + h / 2, t.text, kInk, op);
		}

		struct DialogSpec { std::string title, body; std::vector<std::string> buttons; std::vector<bool> enabled; };
		// File 1 counts as saved for 20 s after its last write (the design prototype's rule: the game's own "unsaved" flag
		// isn't known)
		bool JustSaved()
		{
			std::error_code ec;
			const auto t = std::filesystem::last_write_time(File1(), ec);
			return !ec && std::filesystem::file_time_type::clock::now() - t < std::chrono::seconds(20);
		}

		// a save state's age, as the slots show it ("" when empty)
		std::string SlotAgo(int slot)
		{
			const std::string f = wwhd::state::SlotFile(slot);
			return f.empty() ? "" : Ago(f);
		}

		DialogSpec Spec(Dialog d, int slot)
		{
			const std::string ago = Ago(File1());
			const std::string since = ago.empty() ? "isn't saved yet." : "(" + ago + ") isn\xE2\x80\x99t saved yet.";
			switch (d)
			{
			case Dialog::Quit:
				return { "Quit to\xE2\x80\xA6", JustSaved() ? "All progress is saved." : "Your progress since the last save " + since,
					{ "Title Screen", "Desktop", "Cancel" }, { wwhd::sixty::TitleAvailable(), true, true } };
			case Dialog::QuitTitle:
				return { "Return to the title screen?", "Progress since your last save" + (ago.empty() ? std::string() : " (" + ago + ")") + " will be lost.",
					{ "Save & Quit", "Don\xE2\x80\x99t Save", "Cancel" }, { true, true, true } };
			case Dialog::QuitDesktop:
				return { "Quit to desktop?", "Progress since your last save" + (ago.empty() ? std::string() : " (" + ago + ")") + " will be lost.",
					{ "Save & Quit", "Don\xE2\x80\x99t Save", "Cancel" }, { true, true, true } };
			case Dialog::SaveFile:
				return { "Save your progress?", "This overwrites File 1." + (ago.empty() ? std::string() : " Last saved " + ago + "."), { "Save", "Cancel" }, { true, true } };
			case Dialog::Saving:
				return { "Saving\xE2\x80\xA6", "Please don\xE2\x80\x99t close the game.", {}, {} };
			case Dialog::Saved:
				return { "Save data has been updated.", "Your progress is safe in File 1.", {}, {} };
			case Dialog::Overwrite:
				return { fmt::format("Overwrite Slot {}?", slot), "It holds a state saved " + SlotAgo(slot) + ".", { "Overwrite", "Cancel" }, { true, true } };
			case Dialog::Load:
				return { fmt::format("Load Slot {}?", slot), "You\xE2\x80\x99ll go back to the moment saved " + SlotAgo(slot) +
					". Anything since then is lost unless it\xE2\x80\x99s saved.", { "Load", "Cancel" }, { true, true } };
			default:
				return {};
			}
		}

		constexpr float kDialogW = 1040, kDialogH = 42 + 260 + 24 + 110 + 30;

		// a dialog's frame and message as a layer (its buttons and cursor are drawn over it each frame)
		Image DialogLayer(float s, const DialogSpec& d)
		{
			Assets& a = A();
			const float fw = kDialogW, fh = kDialogH;
			ui::Canvas c((int)std::ceil(fw * s), (int)std::ceil(fh * s));
			Frame f{ c, s };
			c.Fill(0, 0, fw * s, fh * s, kFrame);
			c.Tile(a.Get("texture_frame-felt_tile"), 0, 0, fw * s, fh * s);
			for (auto [inset, col] : { std::pair{ 0.0f, kRimLight }, std::pair{ 5.0f, kRimDark } })
			{
				const float t = inset == 0 ? 5 : 2;
				c.Fill(inset * s, inset * s, (fw - 2 * inset) * s, t * s, col);
				c.Fill(inset * s, (fh - inset - t) * s, (fw - 2 * inset) * s, t * s, col);
				c.Fill(inset * s, inset * s, t * s, (fh - 2 * inset) * s, col);
				c.Fill((fw - inset - t) * s, inset * s, t * s, (fh - 2 * inset) * s, col);
			}
			const Image& fl = a.Get("frame_corner-flourish");
			c.Draw(fl, 10 * s, 10 * s), c.Draw(fl, (fw - 10) * s - fl.w, 10 * s, 1, true);
			c.Draw(fl, 10 * s, (fh - 10) * s - fl.h, 1, false, true), c.Draw(fl, (fw - 10) * s - fl.w, (fh - 10) * s - fl.h, 1, true, true);
			float offs[4] = { -40, -110, 30, -50 };
			const float sx = 44, sy = 42, sw = fw - 88;
			DrawSheet(f, sx, sy, sw, 260, offs, 1);
			TextCentred(f, a.reggae, 32, sx + sw / 2, sy + 80, d.title, kInk);
			// the body: 24 px, at most 640 wide, wrapped
			std::vector<std::string> lines;
			{
				std::string line, word;
				std::istringstream words(d.body);
				while (words >> word)
				{
					const std::string next = line.empty() ? word : line + " " + word;
					if (!line.empty() && a.rocknroll.Width(next, 24 * s) > 640 * s)
						lines.push_back(line), line = word;
					else
						line = next;
				}
				if (!line.empty())
					lines.push_back(line);
			}
			for (size_t i = 0; i < lines.size(); i++)
				TextCentred(f, a.rocknroll, 24, sx + sw / 2, sy + 140 + i * 24 * 1.45f, lines[i], kInk);
			return c.Take();
		}

		void DrawDialog(Frame& f, State& st, Clock::time_point now, float ms)
		{
			if (st.dialog == Dialog::None)
				return;
			const float s = f.s;
			const DialogSpec d = Spec(st.dialog, st.dialogSlot);
			Layers& l = L();
			if (l.dialogKind != (int)st.dialog * 8 + st.dialogSlot)
			{
				l.dialog = DialogLayer(s, d), l.dialogKind = (int)st.dialog * 8 + st.dialogSlot, l.under = {};
				l.dialogButtons.clear();
				for (const std::string& b : d.buttons)
					l.dialogButtons.push_back({ ButtonLayer(s, "dialog_280x72", 280, 72, 27, b, false),
						ButtonLayer(s, "dialog_280x72", 280, 72, 27, b, true) });
			}
			const float dms = std::chrono::duration<float, std::milli>(now - st.dialogSince).count();
			const float p = Bezier(.3f, 1.3f, .5f, 1, Linear(dms, 0, 300)), op = Linear(dms, 0, 300);
			if (op >= 1 && l.under.Empty())
			{
				f.c.Tint(kScrim, 1);
				l.under.w = f.c.W(), l.under.h = f.c.H(), l.under.px = f.c.Pixels();   // the menu under it no longer moves
			}
			else if (op < 1)
				f.c.Tint(kScrim, op);
			const float fx = (f.c.W() / s - kDialogW) / 2, fy = (f.c.H() / s - kDialogH) / 2 + 40 * (1 - p);
			f.c.Fill(fx * s, (fy + 20) * s, kDialogW * s, kDialogH * s, { 10, 40, 20, 255 }, 0.2f * op);
			f.c.Draw(l.dialog, fx * s, fy * s, op);
			const int n = (int)d.buttons.size();
			if (n == 0)
			{
				// Saving: the compass needle spins (1.2 s a turn); Saved: the check
				const float cx = fx + kDialogW / 2, cy = fy + 42 + 260 + 24 + 55;
				if (st.dialog == Dialog::Saving)
				{
					const float t = std::fmod(dms, 1200.0f) / 1200.0f;
					f.c.DrawRotated(A().Get("spinner_compass-needle"), cx * s, cy * s, 360 * Bezier(.45f, 0, .55f, 1, t), op);
				}
				else
				{
					const Image& ck = A().Get("icon_saved-check");
					f.c.Draw(ck, cx * s - ck.w / 2.0f, cy * s - ck.h / 2.0f, op);
				}
				return;
			}
			for (int i = 0; i < n; i++)
			{
				float box[4];
				DialogCursorFor(n, i, fx, fy, box);
				f.c.Draw(l.dialogButtons[i][i == st.dialogItem ? 1 : 0], (box[0] + 13) * s, (box[1] + 13) * s, op * (d.enabled[i] ? 1.0f : 0.45f));
			}
			float box[4];
			DialogCursorFor(n, st.dialogItem, fx, fy, box);
			DrawCursor(f, box, ms, op);
		}

		// the frame under a dialog that has come in: the cached menu and scrim (the dialog drawn over it)
		bool UnderDialog(State& st, ui::Canvas& c)
		{
			Layers& l = L();
			if (st.dialog == Dialog::None || l.dialogKind != (int)st.dialog * 8 + st.dialogSlot || l.under.w != c.W() || l.under.h != c.H())
				return false;
			c.Copy(l.under);
			return true;
		}

		// words into lines no wider than w (pixels) at that size
		std::vector<std::string> WrapText(ui::Font& font, float px, float w, const std::string& text)
		{
			std::vector<std::string> lines;
			std::string line, word;
			std::istringstream words(text);
			while (words >> word)
			{
				const std::string next = line.empty() ? word : line + " " + word;
				if (!line.empty() && font.Width(next, px) > w)
					lines.push_back(line), line = word;
				else
					line = next;
			}
			if (!line.empty())
				lines.push_back(line);
			return lines;
		}

		constexpr Colour kTabIdle = Colour::Hex(0xe3f2b6), kTabRim = Colour::Hex(0xa3cd78), kRule = Colour::Hex(0xa9cf7f),
			kWarnInk = Colour::Hex(0x8a4b14), kControl = Colour::Hex(0x5d8f3a), kToggleOn = Colour::Hex(0x7cc23a),
			kToggleOff = Colour::Hex(0xc3dc9f), kCursorBrown = Colour::Hex(0x5b3b22), kOrange = Colour::Hex(0xf0a53a),
			kOrangeText = Colour::Hex(0x3a240f), kBanner = Colour::Hex(0xf7d58a), kBannerRim = Colour::Hex(0xd79a3c);

		// a rounded pill (a badge, a toggle), in design units
		void Pill(Frame& f, float x, float y, float w, float h, Colour fill, Colour ring, float ringW, float op)
		{
			f.c.RoundRect(x * f.s, y * f.s, w * f.s, h * f.s, fill, ring, ringW * f.s, op);
		}

		// Settings (M2): the title, the tab bar, the rows sheet, the info pane and the next-start banner, dx to the right
		void DrawSettings(Frame& f, State& st, Clock::time_point now, float dx, float op)
		{
			Assets& a = A();
			const float s = f.s;
			const auto& cats = Categories();
			if (cats.empty())
				return;
			const int ci = std::clamp(st.cat, 0, (int)cats.size() - 1);
			const Category& cat = cats[ci];
			// the title: Reggae 46 at 90,50
			f.c.Text(a.reggae, 46 * s, (90 + dx) * s, (50 * s) + a.reggae.Ascent(46 * s), "Settings", kInk, op);
			// the tab bar, 84..1290 at 128, 60 tall: the arrows and the bumpers' glyphs at its ends, the tabs between
			{
				const float y = 128, h = 60, cy = y + h / 2;
				const Image& arrow = a.Get("arrow_tab_left");
				f.c.Draw(arrow, (84 + dx) * s, cy * s - arrow.h / 2.0f, op);
				f.c.Draw(arrow, (1290 + dx) * s - arrow.w, cy * s - arrow.h / 2.0f, op, true);
				const std::string lg = st.pad ? "LB" : "Q", rg = st.pad ? "RB" : "E";
				const float gl = GlyphWidth(lg, st.pad), gr = GlyphWidth(rg, st.pad);
				const float gh = (float)a.Get(st.pad ? "prompt_pad-bumper_blank" : "prompt_key_blank").h;
				DrawGlyph(f, lg, st.pad, (84 + dx) * s + arrow.w + 6 * s, cy * s - gh / 2, op);
				DrawGlyph(f, rg, st.pad, (1290 + dx) * s - arrow.w - 6 * s - gr, cy * s - gh / 2, op);
				float x = 84 + dx + (arrow.w + gl) / s + 20;
				for (int i = 0; i < (int)cats.size(); i++)
				{
					const float tw = a.rocknroll.Width(cats[i].name, 20 * s) / s + 36;
					const bool active = i == ci;
					const float ty = cy - 22;
					if (active)
					{
						f.c.Fill(x * s, ty * s, tw * s, 44 * s, kButtonRim, op);
						f.c.Fill((x + 3) * s, (ty + 3) * s, (tw - 6) * s, 38 * s, Colour::Hex(0xa3dc48), op);
						f.c.Fill((x + 3) * s, (ty + 3) * s, (tw - 6) * s, 12 * s, Colour::Hex(0xc0ea5e), op * 0.8f);
					}
					else
					{
						f.c.Fill(x * s, ty * s, tw * s, 44 * s, kTabRim, op);
						f.c.Fill((x + 3) * s, (ty + 3) * s, (tw - 6) * s, 38 * s, kTabIdle, op);
					}
					TextCentred(f, a.rocknroll, 20, x + tw / 2, cy, cats[i].name, kInk, op);
					x += tw + 8;
				}
			}
			// the rows sheet at 84,212, 860 wide, 26 padding, rows 70 tall 4 apart
			const int n = (int)cat.rows.size();
			const float sx = 84 + dx, sy = 212, sw = 860, sh = 52 + n * 74 - 4;
			float offs[4] = { -60, -20, 40, -90 };
			DrawSheet(f, sx, sy, sw, sh, offs, op);
			int pending = 0;
			for (const Category& c2 : cats)
				for (const Row& r2 : c2.rows)
					if (LookAt(r2).nextStart)
						pending++;
			const float rms = std::chrono::duration<float, std::milli>(now - st.rowSince).count();
			std::vector<Look> looks;
			for (const Row& r : cat.rows)
				looks.push_back(LookAt(r));
			for (int i = 0; i < n; i++)
			{
				const Row& r = cat.rows[i];
				const Look& k = looks[i];
				const float rx = sx + 26, ry = sy + 26 + i * 74, rw = 808, rh = 70;
				const bool focus = i == st.row;
				float hl = focus ? Linear(rms, 0, 140) : (i == st.prevRow ? 1 - Linear(rms, 0, 140) : 0);
				if (hl > 0)
					f.c.Draw(a.Get("row_settings_808x70_focused"), rx * s, ry * s, op * hl);
				for (float x = rx + 4; x < rx + rw - 4; x += 10)          // the dotted rule under it
					f.c.Fill(x * s, (ry + rh - 2) * s, 3 * s, 2 * s, kRule, op);
				const float dim = k.dimmed || k.locked ? 0.5f : 1.0f;   // locked: as a dimmed row, its reason as its note
				const Colour ink = kInk;
				// the label, its markers, the note under it
				const float lx = rx + 26;
				const bool note = !k.note.empty();
				const float lbase = note ? ry + 34 : ry + rh / 2 + (a.rocknroll.Ascent(26 * s) + a.rocknroll.Descent(26 * s)) / 2 / s;
				const float lw = f.c.Text(a.rocknroll, 26 * s, lx * s, lbase * s, r.label, ink, op * dim) / s;
				float mx = lx + lw + 12;
				if (k.changed)
				{
					const Image& dot = a.Get("indicator_changed-dot");
					f.c.Draw(dot, mx * s, (lbase - 13) * s - dot.h / 2.0f, op * dim);
					mx += 14 + 12;
				}
				if (k.nextStart)
				{
					const float bw = a.rocknroll.Width("Next start", 14 * s) / s + 20;
					Pill(f, mx, lbase - 22, bw, 22, kOrange, kOrange, 0, op);
					TextCentred(f, a.rocknroll, 14, mx + bw / 2, lbase - 11, "Next start", kOrangeText, op);
				}
				if (note)
				{
					const float maxw = 808 - 26 - 20 - 330 - 20;
					std::string t = k.note;
					while (t.size() > 1 && a.rocknroll.Width(t, 17 * s) > maxw * s)
						t = t.substr(0, t.size() - 4) + "\xE2\x80\xA6";
					f.c.Text(a.rocknroll, 17 * s, lx * s, (ry + 58) * s, t, k.warn ? kWarnInk : focus ? kInk : kInkMuted, op * dim);
				}
				// the value column: 330 wide, right-aligned, 20 from the row's right
				const float vr = rx + rw - 20, vcy = ry + rh / 2;
				const float vop = op * (k.locked || k.dimmed ? 0.5f : 1.0f);
				if (r.type == "toggle")
				{
					const float tx = vr - 78, ty = vcy - 19;
					float pos = k.on ? 1.0f : 0.0f;
					if (st.toggled == r.id)
					{
						const float t = Bezier(.3f, 1.4f, .5f, 1, Linear(std::chrono::duration<float, std::milli>(now - st.toggledSince).count(), 0, 220));
						pos = k.on ? t : 1 - t;
					}
					Pill(f, tx, ty, 78, 38, k.on ? kToggleOn : kToggleOff, kControl, 3, vop);
					const float kx = tx + 3 + 40 * pos;
					Pill(f, kx + 3, ty + 6, 26, 26, kWhite, kControl, 3, vop);
					const std::string t = k.on ? "On" : "Off";
					f.c.Text(a.rocknroll, 23 * s, (tx - 12) * s - a.rocknroll.Width(t, 23 * s),
						vcy * s + (a.rocknroll.Ascent(23 * s) + a.rocknroll.Descent(23 * s)) / 2, t, ink, vop);
				}
				else if (r.type == "info" || k.locked)
					f.c.Text(a.rocknroll, 23 * s, vr * s - a.rocknroll.Width(k.value, 23 * s),
						vcy * s + (a.rocknroll.Ascent(23 * s) + a.rocknroll.Descent(23 * s)) / 2, k.value, ink, vop);
				else
				{
					// < value >: the arrows in 36 boxes either side of a 226 box
					const Image& arr = a.Get("arrow_choice_right");
					const float bx = vr - 36 - 226 - 36;
					f.c.Draw(arr, (bx + 18) * s - arr.w / 2.0f, vcy * s - arr.h / 2.0f, vop, true);
					f.c.Draw(arr, (vr - 18) * s - arr.w / 2.0f, vcy * s - arr.h / 2.0f, vop);
					float vpx = 24;                              // a long value (the preset's "Auto · Performance") shrinks to fit
					while (vpx > 14 && a.rocknroll.Width(k.value, vpx * s) > 214 * s)
						vpx -= 1;
					TextCentred(f, a.rocknroll, vpx, bx + 36 + 113, vcy, k.value, ink, vop);
				}
			}
			// the next-start banner under the sheet (margin 16), when any next-start change waits
			if (pending > 0)
			{
				const float by = sy + sh + 16, bh = 54;
				f.c.Fill(sx * s, by * s, sw * s, bh * s, kBannerRim, op);
				f.c.Fill((sx + 3) * s, (by + 3) * s, (sw - 6) * s, (bh - 6) * s, kBanner, op);
				Pill(f, sx + 20, by + 12, 30, 30, kOrange, kCursorBrown, 3, op);
				TextCentred(f, a.rocknroll, 18, sx + 35, by + 27, std::to_string(pending), kOrangeText, op);
				f.c.Text(a.rocknroll, 21 * s, (sx + 62) * s, (by + 27) * s + (a.rocknroll.Ascent(21 * s) + a.rocknroll.Descent(21 * s)) / 2,
					pending == 1 ? "This change takes effect next time you start WindSong." :
						"These changes take effect next time you start WindSong.", kOrangeText, op);
			}
			// the info pane at 966,212, 310 wide: the focused row's title, description and facts
			if (st.row < n)
			{
				const Row& r = cat.rows[st.row];
				const Look& k = looks[st.row];
				const float px = 966 + dx, py = 212, pw = 310, inner = pw - 64;
				std::vector<std::string> desc = WrapText(a.rocknroll, 21 * s, inner * s, r.description);
				std::vector<std::pair<std::string, std::string>> facts;
				if (k.option >= 0 && !k.v.values.empty())
				{
					const int d = std::clamp(k.v.def, 0, (int)k.v.values.size() - 1);
					facts.push_back({ "Default", r.type == "toggle" ? (k.v.shown[d] == "on" ? "On" : "Off") : ChoiceLabel(r, k.v, d) });
					facts.push_back({ "Applies", k.locked ? "Not now" : k.option == 0 || !k.v.live ? "Next time you start the game" : "Right away" });
					if (!k.v.performance.empty())
					{
						int pi = 0;
						for (int j = 0; j < (int)k.v.values.size(); j++)
							if (k.v.values[j] == k.v.performance)
								pi = j;
						facts.push_back({ "Performance preset", r.type == "toggle" ? (k.v.shown[pi] == "on" ? "On" : "Off") : ChoiceLabel(r, k.v, pi) });
					}
				}
				if (!r.env.empty())
					facts.push_back({ "Environment variable", r.env });
				else if (!r.key.empty() && r.key != "WWHD_PRESET")
					facts.push_back({ "Environment variable", r.key });
				if (k.locked && !k.lockWhy.empty())
					facts.push_back({ "Locked", k.lockWhy });
				float h = 36 + 40 + 14 + 6 + desc.size() * 21 * 1.55f + 14 + 32;
				for (const auto& fc : facts)
					h += 16 + 4 + 20 * 1.3f * WrapText(a.rocknroll, 20 * s, inner * s, fc.second).size() + 12;
				float offs2[4] = { 20, -70, -30, 50 };
				DrawSheet(f, px, py, pw, h, offs2, op);
				float y = py + 36;
				f.c.Text(a.reggae, 28 * s, (px + 32) * s, y * s + a.reggae.Ascent(28 * s), r.label, kInk, op);
				y += 40 + 14;
				for (int i = 0; i < 10; i++)
					f.c.Fill((px + 32 + i * 12) * s, y * s, 4 * s, 4 * s, kDot, op);
				y += 6 + 14;
				for (const std::string& line : desc)
				{
					f.c.Text(a.rocknroll, 21 * s, (px + 32) * s, (y + 21) * s, line, kInk, op);
					y += 21 * 1.55f;
				}
				y += 14;
				for (const auto& fc : facts)
				{
					f.c.Text(a.rocknroll, 16 * s, (px + 32) * s, (y + 16) * s, fc.first, kInkMuted, op);
					y += 16 + 4;
					for (const std::string& line : WrapText(a.rocknroll, 20 * s, inner * s, fc.second))
					{
						f.c.Text(a.rocknroll, 20 * s, (px + 32) * s, (y + 20) * s, line, kInk, op);
						y += 20 * 1.3f;
					}
					y += 12;
				}
			}
		}

		// ---- Save (M3): the slots' thumbnails, 320x180 RGBA beside each state (slotN.thumb: "WWTH", w, h, pixels; a
		// picture of the game: never in git) ----
		std::string ThumbPath(int slot)
		{
			std::string f = wwhd::state::SlotFile(slot);
			return f.empty() ? f : f.substr(0, f.size() - 6) + ".thumb";
		}
		void WriteThumb(int slot, const Image& picture)
		{
			const std::string p = ThumbPath(slot);
			if (p.empty() || picture.Empty())
				return;
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
			const Image t = ui::Scaled(picture, 320, 180);
			if (FILE* f = fopen(p.c_str(), "wb"))
			{
				const uint32 head[3] = { 0x48545757u, (uint32)t.w, (uint32)t.h };   // "WWTH"
				fwrite(head, 4, 3, f);
				fwrite(t.px.data(), 4, t.px.size(), f);
				fclose(f);
			}
		}
		Image ReadThumb(int slot)
		{
			Image t;
			const std::string p = ThumbPath(slot);
			if (p.empty() || SlotAgo(slot).empty())
				return t;
			if (FILE* f = fopen(p.c_str(), "rb"))
			{
				uint32 head[3] = {};
				if (fread(head, 4, 3, f) == 3 && head[0] == 0x48545757u && head[1] <= 1024 && head[2] <= 1024)
				{
					t.w = (int)head[1], t.h = (int)head[2];
					t.px.resize((size_t)t.w * t.h);
					if (fread(t.px.data(), 4, t.px.size(), f) != t.px.size())
						t = {};
				}
				fclose(f);
			}
			return t;
		}

		// Save's buttons, in the sheet at 84,150: File 1's Save at 538,102, slot k's Save and Load at 538 and 680, 296 + 134 (k - 1)
		void SaveButtonAt(int row, int act, float out[2])
		{
			out[0] = 84 + (act ? 680 : 538), out[1] = 150 + (row == 0 ? 102 : 296 + 134 * (row - 1));
		}
		void SaveCursorFor(int row, int act, float out[4])
		{
			float b[2];
			SaveButtonAt(row, row == 0 ? 0 : act, b);
			out[0] = b[0] - 13, out[1] = b[1] - 13, out[2] = 130 + 26, out[3] = 60 + 26;
		}

		void DrawSaves(Frame& f, State& st, Clock::time_point now, float dx, float op)
		{
			Assets& a = A();
			const float s = f.s;
			f.c.Text(a.reggae, 46 * s, (90 + dx) * s, (50 * s) + a.reggae.Ascent(46 * s), "Save", kInk, op);
			const float sx = 84 + dx, sy = 150, sw = 860, sh = 816;
			float offs[4] = { -30, -80, 10, -40 };
			DrawSheet(f, sx, sy, sw, sh, offs, op);
			const auto label = [&](float y, const std::string& l, const std::string& r) {
				f.c.Text(a.reggae, 24 * s, (sx + 26) * s, (y + 26) * s, l, kInk, op);
				f.c.Text(a.rocknroll, 18 * s, (sx + sw - 26) * s - a.rocknroll.Width(r, 18 * s), (y + 26) * s, r, kInkMuted, op);
			};
			label(sy + 26, "Game File", "Your real progress");
			label(sy + 220, "Save States", "Quick snapshots \xC2\xB7 separate from File 1");
			const float rms = std::chrono::duration<float, std::milli>(now - st.rowSince).count();
			for (int row = 0; row < 5; row++)
			{
				const float ry = sy + (row == 0 ? 70 : 264 + 134 * (row - 1)), rx = sx + 26;
				const bool focus = row == st.saveRow;
				const float hl = focus ? Linear(rms, 0, 140) : (row == st.prevRow ? 1 - Linear(rms, 0, 140) : 0);
				if (hl > 0)
					f.c.Draw(a.Get("row_save_808x124_focused"), rx * s, ry * s, op * hl);
				const std::string ago = row == 0 ? Ago(File1()) : SlotAgo(row);
				// the tile: File 1's "1", a slot's thumbnail, or an empty slot's hatching
				const float tx = rx + 20, ty = ry + 17;
				if (row == 0 || ago.empty())
				{
					f.c.Fill(tx * s, (ty + 3) * s, 160 * s, 90 * s, { 58, 104, 32, 255 }, 0.35f * op);
					f.c.Fill(tx * s, ty * s, 160 * s, 90 * s, row == 0 ? kFrame : kTabIdle, op);
					if (row == 0)
						TextCentred(f, a.reggae, 48, tx + 80, ty + 45, "1", kButtonRim, op);
					else
					{
						for (float d = -90; d < 160; d += 12)       // the hatching
							for (float t = 0; t < 90; t += 1)
								if (tx + d + t >= tx && tx + d + t < tx + 160)
									f.c.Fill((tx + d + t) * s, (ty + t) * s, 2 * s, 1 * s, kTabRim, op * 0.6f);
						TextCentred(f, a.rocknroll, 20, tx + 80, ty + 45, "Empty", kInkMuted, op);
					}
				}
				else
				{
					const Image& th = st.thumbs[row];
					if (!th.Empty())
					{
						const Image sc = ui::Scaled(th, (int)std::lround(160 * s), (int)std::lround(90 * s));
						f.c.Draw(sc, tx * s, ty * s, op);
					}
					else
						f.c.Fill(tx * s, ty * s, 160 * s, 90 * s, kFrame, op);
				}
				if (row == 0 || !ago.empty())
				{
					const float b = 4;                            // its 4 px border
					f.c.Fill(tx * s, ty * s, 160 * s, b * s, kButtonRim, op), f.c.Fill(tx * s, (ty + 90 - b) * s, 160 * s, b * s, kButtonRim, op);
					f.c.Fill(tx * s, ty * s, b * s, 90 * s, kButtonRim, op), f.c.Fill((tx + 160 - b) * s, ty * s, b * s, 90 * s, kButtonRim, op);
				}
				const std::string title = row == 0 ? "File 1" : fmt::format("Slot {}", row);
				const std::string sub = row == 0 ? (ago.empty() ? "Not saved yet" : "Saved " + ago) : ago.empty() ? "No state saved" : "Saved " + ago;
				f.c.Text(a.reggae, 28 * s, (tx + 184) * s, (ry + 56) * s, title, kInk, op);
				f.c.Text(a.rocknroll, 21 * s, (tx + 184) * s, (ry + 90) * s, sub, focus ? kInk : kInkMuted, op);
				// the buttons
				for (int act = 0; act < (row == 0 ? 1 : 2); act++)
				{
					float b[2];
					SaveButtonAt(row, act, b);
					const bool on = focus && (row == 0 || st.saveAct == act);
					const float bop = op * (act == 1 && ago.empty() ? 0.45f : 1.0f);
					f.c.Silhouette(a.Get("button_slot_130x60_idle"), (b[0] + dx) * s, (b[1] + 4) * s, { 58, 104, 32, 89 }, bop);
					f.c.Draw(a.Get(on ? "button_slot_130x60_focused" : "button_slot_130x60_idle"), (b[0] + dx) * s, b[1] * s, bop);
					TextCentred(f, a.rocknroll, 25, b[0] + dx + 65, b[1] + 30, act ? "Load" : "Save", kInk, bop);
				}
			}
			// the info pane at 966,150
			{
				const int row = st.saveRow;
				const std::string ago = row == 0 ? Ago(File1()) : SlotAgo(row);
				std::string title, desc, note;
				if (row == 0)
					title = "File 1", desc = "Your real progress, the same save the game makes at a save point. Last saved " +
						(ago.empty() ? std::string("never") : ago) + ".", note = "Saving overwrites File 1.";
				else
				{
					title = fmt::format("Slot {}", row);
					desc = ago.empty() ? "Empty. Save here to capture this exact moment." : "Saved " + ago + ". Load it to jump straight back to that moment.";
					note = "Save states don\xE2\x80\x99t touch File 1.";
				}
				const float px = 966 + dx, py = 150, pw = 310, inner = pw - 64;
				const std::vector<std::string> d = WrapText(a.rocknroll, 21 * s, inner * s, desc), n = WrapText(a.rocknroll, 17 * s, inner * s, note);
				const float h = 36 + 40 + 14 + 6 + 14 + d.size() * 21 * 1.55f + 12 + n.size() * 17 * 1.4f + 32;
				float offs2[4] = { 20, -70, -30, 50 };
				DrawSheet(f, px, py, pw, h, offs2, op);
				float y = py + 36;
				f.c.Text(a.reggae, 28 * s, (px + 32) * s, y * s + a.reggae.Ascent(28 * s), title, kInk, op);
				y += 40 + 14;
				for (int i = 0; i < 10; i++)
					f.c.Fill((px + 32 + i * 12) * s, y * s, 4 * s, 4 * s, kDot, op);
				y += 6 + 14;
				for (const std::string& line : d)
					f.c.Text(a.rocknroll, 21 * s, (px + 32) * s, (y + 21) * s, line, kInk, op), y += 21 * 1.55f;
				y += 12;
				for (const std::string& line : n)
					f.c.Text(a.rocknroll, 17 * s, (px + 32) * s, (y + 17) * s, line, kInkMuted, op), y += 17 * 1.4f;
			}
		}

		// Enhancements (M4): the screen with its sheet and a note while there are none yet (the owner's choice: no
		// placeholder rows)
		void DrawEnhancements(Frame& f, float dx, float op)
		{
			Assets& a = A();
			const float s = f.s;
			f.c.Text(a.reggae, 46 * s, (90 + dx) * s, (50 * s) + a.reggae.Ascent(46 * s), "Enhancements", kInk, op);
			const float sx = 84 + dx, sy = 150, sw = 860, sh = 816;
			float offs[4] = { -30, -80, 10, -40 };
			DrawSheet(f, sx, sy, sw, sh, offs, op);
			TextCentred(f, a.reggae, 34, sx + sw / 2, sy + sh / 2 - 26, "No enhancements yet", kInk, op);
			TextCentred(f, a.rocknroll, 21, sx + sw / 2, sy + sh / 2 + 24, "Quality-of-life and gameplay tweaks will appear here.", kInkMuted, op);
		}

		void BuildLayers(Layers& l, int w, int h, float s)
		{
			Assets& a = A();
			l.w = w, l.h = h, l.dialogKind = -1;
			// the panel's felt at its widest (Settings' 1380) and its left corners; the right rim and corners follow its width
			const int pw = (int)std::lround(kWidePanelW * s);
			{
				ui::Canvas c(pw, h);
				c.Fill(0, 0, (float)pw, (float)h, kFrame);
				c.Tile(a.Get("texture_frame-felt_tile"), 0, 0, (float)pw, (float)h);
				const Image& fl = a.Get("frame_corner-flourish");
				c.Draw(fl, 14 * s, 14 * s), c.Draw(fl, 14 * s, h - 14 * s - fl.h, 1, false, true);
				l.panel = c.Take();
			}
			{
				ui::Canvas c((int)std::ceil(560 * s) + 1, (int)std::ceil(250 * s) + 1);
				Frame f{ c, s };
				float offs[4] = { -40, -110, 30, -50 };
				DrawSheet(f, 0, 0, 560, 250, offs, 1);
				TextCentred(f, a.reggae, 52, 280, 48 + 34, "Paused", kInk);
				for (int i = 0; i < 16; i++)                  // the dotted divider: 180 wide, a dot each 12
					c.Fill((280 - 90 + i * 12 + 3) * s, 132 * s, 4 * s, 4 * s, kDot);
				l.header = c.Take();
			}
			for (int i = 0; i < 5; i++)
				for (int k = 0; k < 2; k++)
					l.button[i][k] = ButtonLayer(s, "menu_460x72", kButtonW, kButtonH, 32, kItems[i], k == 1);
		}

		// ---- the start screen (t-startscreen S1: Launch; the owner's Start Screen design, README "2. Launch: desktop"):
		// over the title's first frame, held; the logo, the board with its menu or Game Files, the footer ----
		const char* const kLaunchItems[4] = { "Start Game", "Settings", "Game Files", "Quit" };
		const char* const kFilesItems[2] = { "Re-verify", "Change Files" };
		constexpr float kBoardY = 400, kBoardW = 680, kSheetY = 416, kSheetW = 648;
		// the board's left (design units): 620 at 16:9, centred on a wider window (README "Desktop at non-16:9")
		float LaunchOX()
		{
			uint32 w = 0, h = 0;
			if (!gpu::WindowExtent(w, h) || !w || !h)
				return 0;
			const float s = std::min(w / 1920.0f, h / 1080.0f);
			return std::max(0.0f, (w / s - 1920) / 2);
		}
		float BoardX() { return 620 + LaunchOX(); }
		float SheetX() { return 636 + LaunchOX(); }
		constexpr Colour kWarn = Colour::Hex(0x8a4b14), kKey = Colour::Hex(0x4f7a32);
		// a TV picture's mean brightness (0..1, a pixel in 16 sampled)
		float Luma(const std::vector<uint32>& px)
		{
			uint64 sum = 0, n = 0;
			for (size_t i = 0; i < px.size(); i += 16, n++)
				sum += (px[i] & 0xFF) * 2 + ((px[i] >> 8) & 0xFF) * 5 + ((px[i] >> 16) & 0xFF);
			return n ? (float)sum / (n * 8 * 255.0f) : 0;
		}

		float LaunchSheetH(int view) { return view == 0 ? 430 : 516; }
		// a button's top (design units): the menu's four 90 apart from the sheet's padding; Game Files' two under its rows
		float LaunchButtonY(int view, int i) { return view == 0 ? kSheetY + 44 + i * 90 : kSheetY + 310 + i * 90; }
		void LaunchCursorFor(int view, int i, float out[4])
		{
			out[0] = SheetX() + (kSheetW - kButtonW) / 2 - 13, out[1] = LaunchButtonY(view, i) - 13, out[2] = kButtonW + 26, out[3] = kButtonH + 26;
		}
		int LaunchCount(int view) { return view == 0 ? 4 : 2; }

		std::string RegionOf(const State& st) { return st.copy.region.empty() ? "USA" : st.copy.region; }
		bool Checking(const State& st) { return st.job && !st.job->done.load(); }
		std::string StatusLine(const State& st, Colour& col)
		{
			col = kInk;
			if (Checking(st))
				return fmt::format("Checking\xE2\x80\xA6 {}%", (int)std::lround(st.job->progress.load() * 100));
			if (st.checked && st.copy.ok)
				return "Verified";
			if (st.checked)
				return col = kWarn, "Couldn\xE2\x80\x99t verify";
			return "Not checked";
		}

		// text cut to a width with an ellipsis at its start (a path: its end matters most)
		std::string FitStart(ui::Font& font, float px, float w, const std::string& t)
		{
			if (font.Width(t, px) <= w)
				return t;
			const std::vector<uint32> cps = ui::Utf8(t);
			for (size_t cut = 1; cut < cps.size(); cut++)
			{
				std::string tail = "\xE2\x80\xA6";
				for (size_t i = cut; i < cps.size(); i++)
				{
					const uint32 c = cps[i];
					if (c < 0x80) tail += (char)c;
					else if (c < 0x800) tail += (char)(0xC0 | (c >> 6)), tail += (char)(0x80 | (c & 0x3F));
					else if (c < 0x10000) tail += (char)(0xE0 | (c >> 12)), tail += (char)(0x80 | ((c >> 6) & 0x3F)), tail += (char)(0x80 | (c & 0x3F));
					else tail += (char)(0xF0 | (c >> 18)), tail += (char)(0x80 | ((c >> 12) & 0x3F)), tail += (char)(0x80 | ((c >> 6) & 0x3F)), tail += (char)(0x80 | (c & 0x3F));
				}
				if (font.Width(tail, px) <= w)
					return tail;
			}
			return "\xE2\x80\xA6";
		}

		// the frozen frame, the logo (its shadows: 0 6px 0 #1f5fb0, 0 12px 14px rgba(0,30,90,.35)) and the footer, drawn once
		void LaunchBase(State& st, Frame& f)
		{
			Assets& a = A();
			ui::Canvas& c = f.c;
			const float s = f.s;
			c.Copy(st.backdrop);
			const Image& logo = a.Get("logo_launch_desktop_h264");
			const float lx = std::round((c.W() - logo.w) / 2.0f), ly = std::round(64 * s);
			for (int i = 0; i < 4; i++)                      // the soft one: four offsets spread about 12 px down
				c.Silhouette(logo, lx + (i % 2 ? 3 : -3) * s, ly + (10 + i) * s, { 0, 30, 90, 255 }, 0.09f);
			c.Silhouette(logo, lx, ly + 6 * s, Colour::Hex(0x1f5fb0));
			c.Draw(logo, lx, ly);
			// the footer: the game and its check at the bottom left, the build at the bottom right (21 px, outlined)
			Colour col;
			const std::string status = StatusLine(st, col);
			const std::string left = "The Wind Waker HD \xC2\xB7 " + RegionOf(st) + (st.checked && st.copy.ok ? " \xC2\xB7 Verified" : "");
			const std::string right = game_files::Version();
			const float px = 21 * s, base = c.H() - 34 * s + a.rocknroll.Descent(px);
			c.Text(a.rocknroll, px, 44 * s, base, left, kWhite, 1, kPromptOutline, 2 * s);
			c.Text(a.rocknroll, px, c.W() - 44 * s - a.rocknroll.Width(right, px), base, right, kWhite, 1, kPromptOutline, 2 * s);
		}

		// the board as a layer (drawn once a view, size and status): its shadow (0 20px 50px rgba(10,40,20,.35), blurred), the
		// felt frame (16 padding, its rims and flourishes), the sheet, and Game Files' title, divider and rows; kShadowM around it
		constexpr float kShadowM = 70;
		// (view 2: Setup's board, 1200 wide, its sheet 649 tall, nothing on it: its content is a layer of its own)
		constexpr float kSetupBoardW = 1200, kSetupSheetH = 649;
		Image BoardLayer(State& st, float s, int view, float extra = 0)
		{
			Assets& a = A();
			const float fw = view == 2 ? kSetupBoardW : kBoardW, fh = (view == 2 ? kSetupSheetH + extra : LaunchSheetH(view)) + 32, M = kShadowM;
			ui::Canvas c((int)std::ceil((fw + 2 * M) * s), (int)std::ceil((fh + 2 * M) * s));
			{
				Image mask;                                 // the board's shape, white on black, blurred: its red is the shadow's alpha
				mask.w = c.W(), mask.h = c.H();
				mask.px.assign((size_t)mask.w * mask.h, 0xFF000000u);
				const int x0 = (int)(M * s), y0 = (int)((M + 20) * s), x1 = (int)((M + fw) * s), y1 = std::min(mask.h, (int)((M + 20 + fh) * s));
				for (int y = y0; y < y1; y++)
					std::fill(mask.px.begin() + (size_t)y * mask.w + x0, mask.px.begin() + (size_t)y * mask.w + x1, 0xFFFFFFFFu);
				const Image soft = ui::Blurred(mask, 25 * s, 1, 1);
				for (size_t i = 0; i < soft.px.size(); i++)
					c.Pixels()[i] = 10u | 40u << 8 | 20u << 16 | (uint32)((soft.px[i] & 0xFF) * 0.35f) << 24;
			}
			Frame f{ c, s };
			const float x = M, y = M;
			c.Fill(x * s, y * s, fw * s, fh * s, kFrame);
			c.Tile(a.Get("texture_frame-felt_tile"), x * s, y * s, fw * s, fh * s);
			for (auto [inset, col] : { std::pair{ 0.0f, kRimLight }, std::pair{ 5.0f, kRimDark } })
			{
				const float t = inset == 0 ? 5 : 2;
				c.Fill((x + inset) * s, (y + inset) * s, (fw - 2 * inset) * s, t * s, col);
				c.Fill((x + inset) * s, (y + fh - inset - t) * s, (fw - 2 * inset) * s, t * s, col);
				c.Fill((x + inset) * s, (y + inset) * s, t * s, (fh - 2 * inset) * s, col);
				c.Fill((x + fw - inset - t) * s, (y + inset) * s, t * s, (fh - 2 * inset) * s, col);
			}
			const float sx = x + 16, sy = y + 16, sh = fh - 32;
			float offs[4] = { 0, -64, 12, -80 };
			DrawSheet(f, sx, sy, fw - 32, sh, offs, 1);
			const Image& fl = a.Get("frame_corner-flourish");
			c.Draw(fl, (x + 6) * s, (y + 6) * s), c.Draw(fl, (x + fw - 6) * s - fl.w, (y + 6) * s, 1, true);
			c.Draw(fl, (x + 6) * s, (y + fh - 6) * s - fl.h, 1, false, true), c.Draw(fl, (x + fw - 6) * s - fl.w, (y + fh - 6) * s - fl.h, 1, true, true);
			if (view == 1)
			{
				// Game Files: its title (42 px), the divider (160), its rows (key 120 wide at 21 px, the value at 23)
				TextCentred(f, a.reggae, 42, sx + kSheetW / 2, sy + 44 + 23, "Game Files", kInk);
				for (int i = 0; i < 13; i++)
					c.Fill((sx + kSheetW / 2 - 80 + i * 12 + 4) * s, (sy + 106) * s, 4 * s, 4 * s, kDot);
				Colour statusCol;
				const std::string status = StatusLine(st, statusCol);
				const std::string game = "The Wind Waker HD \xC2\xB7 " + RegionOf(st) + " \xC2\xB7 v" + std::to_string(std::max(0, st.copy.version));
				const std::string rows[3][2] = { { "Game", game }, { "Location", st.copy.path }, { "Status", status } };
				const float rx = sx + 60, rw = kSheetW - 120;
				const Image& rule = a.Get("rule_dotted_tile_20x4");
				for (int i = 0; i < 3; i++)
				{
					const float ry = sy + 136 + i * 50;
					const float base = (ry + 25) * s + (a.rocknroll.Ascent(23 * s) + a.rocknroll.Descent(23 * s)) / 2;
					c.Text(a.rocknroll, 21 * s, (rx + 6) * s, base, rows[i][0], kKey);
					c.Text(a.rocknroll, 23 * s, (rx + 146) * s, base, FitStart(a.rocknroll, 23 * s, (rw - 140) * s, rows[i][1]), i == 2 ? statusCol : kInk);
					c.Tile(rule, rx * s, (ry + 48) * s, rw * s, (float)rule.h);
				}
			}
			return c.Take();
		}

		struct LaunchLayers
		{
			std::string key;                            // the size, view and what Game Files shows
			Image board;
			int size = 0;                               // the buttons' size (canvas width)
			Image buttons[2][4][2];                     // each view's buttons, idle and focused
		};
		LaunchLayers& LL()
		{
			static LaunchLayers l;
			return l;
		}

		void DrawLaunch(State& st, Frame& f, Clock::time_point now, float op)
		{
			ui::Canvas& c = f.c;
			const float s = f.s;
			const std::string baseKey = fmt::format("{}x{} {} {}", c.W(), c.H(), RegionOf(st), st.checked && st.copy.ok ? 1 : 0);
			if (baseKey != st.launchKey || st.launchBase.w != c.W() || st.launchBase.h != c.H())
			{
				ui::Canvas b(c.W(), c.H());
				Frame bf{ b, s };
				LaunchBase(st, bf);
				st.launchBase = b.Take(), st.launchKey = baseKey;
			}
			c.Copy(st.launchBase);
			if (op <= 0)
				return;
			const int view = st.startView;
			LaunchLayers& l = LL();
			Colour col;
			const std::string boardKey = fmt::format("{}x{} {} {}|{}", c.W(), c.H(), view, view == 1 ? StatusLine(st, col) : "", st.copy.path);
			if (boardKey != l.key)
				l.board = BoardLayer(st, s, view), l.key = boardKey;
			if (l.size != c.W())
			{
				for (int v = 0; v < 2; v++)
					for (int i = 0; i < LaunchCount(v); i++)
						for (int k = 0; k < 2; k++)
							l.buttons[v][i][k] = ButtonLayer(s, "menu_460x72", kButtonW, kButtonH, 29, v == 0 ? kLaunchItems[i] : kFilesItems[i], k == 1);
				l.size = c.W();
			}
			c.Draw(l.board, std::round((BoardX() - kShadowM) * s), std::round((kBoardY - kShadowM) * s), op);
			const float bx = SheetX() + (kSheetW - kButtonW) / 2;
			for (int i = 0; i < LaunchCount(view); i++)
			{
				const float fms = std::chrono::duration<float, std::milli>(now - st.startFocusSince[i]).count();
				float focus = Linear(fms, 0, 140);
				if (i != st.startItem)
					focus = st.startPrev == i ? 1 - focus : 0;
				const bool dim = view == 0 && i == 0 && st.checked && !st.copy.ok;    // Start Game, when the copy didn't verify
				const float bop = op * (dim ? 0.5f : 1), px = std::round(bx * s), py = std::round(LaunchButtonY(view, i) * s);
				c.Draw(l.buttons[view][i][0], px, py, bop);
				if (focus > 0)
					c.Draw(l.buttons[view][i][1], px, py, bop * focus);
			}
		}

		// ---- the start screen's Setup (t-startscreen S2; README "1. Setup: desktop"): the logo (184 tall), the board (1200
		// wide at 360,236), its title, body, drop zone and buttons by its state; before the game over a sky, or over the held
		// title frame from Launch's Change Files ----
		constexpr int kIdle = 0, kChecking = 1, kBad = 2, kReady = 3;
		constexpr Colour kZoneText = Colour::Hex(0x3f6a26), kSky = Colour::Hex(0x0bd8f6);
		const char* const kFormats = "WUA \xC2\xB7 WUX \xC2\xB7 WUD \xC2\xB7 or an extracted game folder";
		float SetupX() { return 360 + LaunchOX(); }
		float ZoneX() { return 570 + LaunchOX(); }
		constexpr float kZoneY = 499, kZoneW = 780, kZoneH = 250, kSetupButtonsY = 779, kSetupButtonW = 340;
		// the menu's own file browser's rows (in the sheet, under its title and folder)
		constexpr int kBrowseRows = 9;
		constexpr float kBrowseX = 440, kBrowseW = 1040, kBrowseY = 440, kBrowseRowH = 46;
		void BrowseCursorFor(int row, float out[4])
		{
			out[0] = kBrowseX + LaunchOX() - 13, out[1] = kBrowseY + row * kBrowseRowH - 13, out[2] = kBrowseW + 26, out[3] = kBrowseRowH + 26;
		}

		std::vector<std::string> SetupButtons(const State& st)
		{
			if (st.browsing)
				return {};
			switch (st.sstate)
			{
			case kChecking: return { "Cancel" };
			case kBad: return { "Try Another File", st.help ? "Got It" : "Help" };
			case kReady: return { "Start Game", "Change Files" };
			default: return { "Browse Files", st.help ? "Got It" : "Help" };
			}
		}

		// text wrapped to a width, two lines balanced (the design's text-wrap: balance)
		std::vector<std::string> WrapBalanced(ui::Font& font, float px, float w, const std::string& text)
		{
			std::vector<std::string> lines = WrapText(font, px, w, text);
			if (lines.size() < 2)
				return lines;
			float lo = 0, hi = w;                           // the narrowest width that wraps to as many lines
			for (int i = 0; i < 14; i++)
			{
				const float mid = (lo + hi) / 2;
				(WrapText(font, px, mid, text).size() == lines.size() ? hi : lo) = mid;
			}
			return WrapText(font, px, hi, text);
		}

		// the sky, the logo and the version (once a size): before the game the design's sky colour, from Launch the frozen frame
		void SetupBase(State& st, Frame& f)
		{
			Assets& a = A();
			ui::Canvas& c = f.c;
			const float s = f.s;
			if (st.setupPre || st.backdrop.Empty())
				c.Fill(0, 0, (float)c.W(), (float)c.H(), kSky);   // no title frame yet: the design's sky fallback
			else
				c.Copy(st.backdrop);
			const Image& logo = a.Get("logo_setup_desktop_h184");
			const float lx = std::round((c.W() - logo.w) / 2.0f), ly = std::round(34 * s);
			for (int i = 0; i < 4; i++)
				c.Silhouette(logo, lx + (i % 2 ? 3 : -3) * s, ly + (10 + i) * s, { 0, 30, 90, 255 }, 0.09f);
			c.Silhouette(logo, lx, ly + 6 * s, Colour::Hex(0x1f5fb0));
			c.Draw(logo, lx, ly);
			const std::string right = game_files::Version();
			const float px = 21 * s, base = c.H() - 34 * s + a.rocknroll.Descent(px);
			c.Text(a.rocknroll, px, c.W() - 44 * s - a.rocknroll.Width(right, px), base, right, kWhite, 1, kPromptOutline, 2 * s);
		}

		// Setup's title and body by its state (README "Copy by state")
		std::pair<std::string, std::string> SetupCopy(const State& st)
		{
			std::string title, body;
			switch (st.sstate)
			{
			case kChecking:
				title = "Checking your game files\xE2\x80\xA6", body = "This only happens once and takes a few seconds."; break;
			case kBad:
				title = "That doesn\xE2\x80\x99t look right";
				body = st.setupCopy.why.empty() ? "This isn\xE2\x80\x99t a supported copy of The Wind Waker HD. Make sure it\xE2\x80\x99s a full Wii U dump and try again."
					: st.setupCopy.why;
				break;
			case kReady:
				title = "Ready to set sail!", body = "Your game files check out. You won\xE2\x80\x99t be asked for them again."; break;
			default:
				title = st.dragging ? "Let go to load" : "Let\xE2\x80\x99s find your game";
				body = st.dragging ? "We\xE2\x80\x99ll check the files before anything is copied."
					: "This port runs on your own copy of The Wind Waker HD. Drop your Wii U game files here, or browse for them.";
			}
			if (st.help && (st.sstate == kIdle || st.sstate == kBad) && !st.dragging)
				body = "Dump the game from your own Wii U with a homebrew dumping tool, then choose the output here. Nothing is uploaded; "
					"your files stay on this PC.";
			return { title, body };
		}
		// the body's lines (25 px, 820 wide, balanced; at most three) and the room a third takes: two are reserved (min-height
		// 70), a third moves the zone and the buttons down a line and the sheet grows by it (the design's help)
		std::vector<std::string> SetupBody(const State& st, float s)
		{
			static std::map<std::string, std::vector<std::string>> s_wrapped;   // by size and text (the cursor asks each frame)
			const std::string text = SetupCopy(st).second, key = fmt::format("{} {}", s, text);
			if (auto it = s_wrapped.find(key); it != s_wrapped.end())
				return it->second;
			std::vector<std::string> lines = WrapBalanced(A().rocknroll, 25 * s, 820 * s, text);
			if (lines.size() > 3)
				lines.resize(3);
			if (s_wrapped.size() > 64)
				s_wrapped.clear();
			return s_wrapped[key] = lines;
		}
		constexpr float kBodyLineH = 36.25f;
		float SetupExtra(const State& st)
		{
			return st.browsing ? 0 : std::max(0, (int)SetupBody(st, 1).size() - 2) * kBodyLineH;
		}
		void SetupCursorFor(const State& st, int i, float out[4])
		{
			const int n = (int)SetupButtons(st).size();
			const float x0 = 960 + LaunchOX() - (n * kSetupButtonW + (n - 1) * 40) / 2;
			out[0] = x0 + i * (kSetupButtonW + 40) - 13, out[1] = kSetupButtonsY + SetupExtra(st) - 13, out[2] = kSetupButtonW + 26, out[3] = kButtonH + 26;
		}

		std::string FileName(const std::string& path)
		{
			std::string p = path;
			while (p.size() > 1 && p.back() == '/')
				p.pop_back();
			const size_t slash = p.rfind('/');
			return slash == std::string::npos ? p : p.substr(slash + 1);
		}

		// the sheet's content by the state (a layer over the board, drawn when the state changes): the title (46 px), the
		// divider, the body (25 px, two lines reserved), the drop zone and what's in it (not the progress: drawn each frame)
		Image SetupContent(State& st, float s, int w, int h)
		{
			Assets& a = A();
			ui::Canvas c(w, h);
			Frame f{ c, s };
			const float cx = 960 + LaunchOX();
			if (st.browsing)
			{
				// the browser: its title, the folder (its end kept), the rows (a game folder or file named as such)
				TextCentred(f, a.reggae, 46, cx, 331, "Choose your game files", kInk);
				for (int i = 0; i < 15; i++)
					c.Fill((cx - 90 + i * 12 + 4) * s, 373 * s, 4 * s, 4 * s, kDot);
				TextCentred(f, a.rocknroll, 21, cx, 412, FitStart(a.rocknroll, 21 * s, 1000 * s, st.browseDir), kZoneText);
				const float x = kBrowseX + LaunchOX();
				const Image& rule = a.Get("rule_dotted_tile_20x4");
				if (st.entries.empty())
					TextCentred(f, a.rocknroll, 23, cx, kBrowseY + 60, "No folders or game files here.", kKey);
				for (int i = 0; i < kBrowseRows && st.browseTop + i < (int)st.entries.size(); i++)
				{
					const State::Entry& e = st.entries[st.browseTop + i];
					const float y = kBrowseY + i * kBrowseRowH;
					if (st.browseTop + i == st.browseRow)
						c.Fill(x * s, y * s, kBrowseW * s, kBrowseRowH * s, Colour::Hex(0xb6da78), 0.85f);
					const float base = (y + kBrowseRowH / 2) * s + (a.rocknroll.Ascent(23 * s) + a.rocknroll.Descent(23 * s)) / 2;
					const char* kind = e.kind == 0 ? "Up a folder" : e.kind == 1 ? "Folder" : e.kind == 2 ? "Game folder" : "Game file";
					const float kw = a.rocknroll.Width(kind, 20 * s);
					c.Text(a.rocknroll, 23 * s, (x + 16) * s, base, FitStart(a.rocknroll, 23 * s, (kBrowseW - 48) * s - kw, e.kind == 0 ? ".." : e.name),
						e.kind >= 2 ? kInk : kKey);
					c.Text(a.rocknroll, 20 * s, (x + kBrowseW - 16) * s - kw, base, kind, e.kind >= 2 ? kInk : kZoneText);
					c.Tile(rule, x * s, (y + kBrowseRowH - 2) * s, kBrowseW * s, (float)rule.h);
				}
				if ((int)st.entries.size() > kBrowseRows)
					TextCentred(f, a.rocknroll, 20, cx, kBrowseY + kBrowseRows * kBrowseRowH + 22,
						fmt::format("{} of {}", st.browseRow + 1, st.entries.size()), kZoneText);
				return c.Take();
			}
			TextCentred(f, a.reggae, 46, cx, 331, SetupCopy(st).first, kInk);
			for (int i = 0; i < 15; i++)
				c.Fill((cx - 90 + i * 12 + 4) * s, 373 * s, 4 * s, 4 * s, kDot);
			const std::vector<std::string> lines = SetupBody(st, s);
			for (size_t i = 0; i < lines.size(); i++)
				TextCentred(f, a.rocknroll, 25, cx, 421 + i * kBodyLineH + (lines.size() == 1 ? 18 : 0), lines[i], kInk);
			// the zone (a third line of the body moves it and what's in it down): its gloss while files are over it
			const float e = SetupExtra(st);
			c.Draw(a.Get(st.dragging && st.sstate != kChecking && st.sstate != kReady ? "dropzone_780x250_drag" : "dropzone_780x250_idle"),
				ZoneX() * s, (kZoneY + e) * s);
			const std::string name = FitStart(a.rocknroll, 21 * s, 620 * s, FileName(st.setupFile));
			switch (st.sstate)
			{
			case kChecking:
				TextCentred(f, a.rocknroll, 21, cx, 581 + e, name, kZoneText);
				break;
			case kBad:
				c.Draw(a.Get("icon_warning_76"), (cx - 38) * s, (536 + e) * s);
				TextCentred(f, a.rocknroll, 28, cx, 648 + e, "Couldn\xE2\x80\x99t recognise this file", kInk);
				TextCentred(f, a.rocknroll, 20, cx, 698 + e, FitStart(a.rocknroll, 20 * s, 700 * s, FileName(st.setupFile)), kZoneText);
				break;
			case kReady:
				c.Draw(a.Get("icon_ready-check_70"), (cx - 35) * s, (538 + e) * s);
				TextCentred(f, a.rocknroll, 30, cx, 645 + e, "The Wind Waker HD", kInk);
				TextCentred(f, a.rocknroll, 20, cx, 696 + e, (st.setupCopy.region.empty() ? "USA" : st.setupCopy.region) + " \xC2\xB7 Version " +
					std::to_string(std::max(0, st.setupCopy.version)) + " \xC2\xB7 Verified", kZoneText);
				break;
			default:
				c.Draw(a.Get("icon_drop-target_96"), (cx - 48) * s, (525 + e) * s);
				TextCentred(f, a.rocknroll, 30, cx, 658 + e, st.dragging ? "Let go to load" : "Drop game files here", kInk);
				TextCentred(f, a.rocknroll, 20, cx, 708 + e, kFormats, kZoneText);
			}
			return c.Take();
		}

		struct SetupLayers
		{
			std::string baseKey, boardKey, contentKey;
			Image base, board, content;
			std::map<std::string, std::array<Image, 2>> buttons;   // by label: idle and focused
			int size = 0;
		};
		SetupLayers& SL()
		{
			static SetupLayers l;
			return l;
		}

		// the checking's progress (each frame): the bar (540 x 24, its fill eased ~100 ms), the percentage, the three steps
		void DrawProgress(State& st, Frame& f, float op)
		{
			Assets& a = A();
			ui::Canvas& c = f.c;
			const float s = f.s, cx = 960 + LaunchOX();
			static float shown = 0;
			const float p = st.setupJob ? std::clamp(st.setupJob->progress.load(), 0.0f, 1.0f) : 0;
			shown += (p - shown) * 0.3f;                    // eased towards the real progress
			if (p < shown)
				shown = p;
			const float e = SetupExtra(st), bx = cx - (540 + 18 + 70) / 2.0f, by = 611.5f + e;
			c.Draw(a.Get("progress_track_540x24"), bx * s, by * s, op);
			const Image& fill = a.Get("progress_fill_534x18");
			c.Clip(0, 0, (int)((bx + 3 + 534 * shown) * s), c.H());
			c.Draw(fill, (bx + 3) * s, (by + 3) * s, op);
			c.NoClip();
			c.Text(a.rocknroll, 23 * s, (bx + 540 + 18) * s, (by + 12) * s + (a.rocknroll.Ascent(23 * s) + a.rocknroll.Descent(23 * s)) / 2,
				fmt::format("{}%", (int)std::lround(p * 100)), kInk, op);
			const char* const names[3] = { "Reading files", "Verifying", "Preparing assets" };
			const float ends[3] = { 0.35f, 0.75f, 1.0f };
			float widths[3], total = 0;
			for (int i = 0; i < 3; i++)
				widths[i] = 30 + 8 + a.rocknroll.Width(names[i], 20 * s) / s, total += widths[i] + (i ? 30 : 0);
			float x = cx - total / 2;
			for (int i = 0; i < 3; i++)
			{
				const bool done = p >= ends[i], current = !done && (i == 0 || p >= ends[i - 1]);
				c.Draw(a.Get(done ? "step_done_30" : current ? "step_current_30" : "step_pending_30"), x * s, (651 + e) * s, op);
				if (!done)
					TextCentred(f, a.rocknroll, 16, x + 15, 666 + e, std::to_string(i + 1), current ? kInk : kKey, op);
				c.Text(a.rocknroll, 20 * s, (x + 38) * s, (666 + e) * s + (a.rocknroll.Ascent(20 * s) + a.rocknroll.Descent(20 * s)) / 2, names[i],
					done || current ? kInk : kKey, op);
				x += widths[i] + 30;
			}
		}

		void DrawSetup(State& st, Frame& f, Clock::time_point now, float op)
		{
			ui::Canvas& c = f.c;
			const float s = f.s;
			SetupLayers& l = SL();
			const std::string baseKey = fmt::format("{}x{} {}", c.W(), c.H(), st.setupPre ? 1 : 0);
			if (baseKey != l.baseKey)
			{
				ui::Canvas b(c.W(), c.H());
				Frame bf{ b, s };
				SetupBase(st, bf);
				l.base = b.Take(), l.baseKey = baseKey;
			}
			const float extra = SetupExtra(st);
			const std::string boardKey = fmt::format("{} {}", baseKey, extra);
			if (boardKey != l.boardKey)
				l.board = BoardLayer(st, s, 2, extra), l.boardKey = boardKey;
			const std::string contentKey = fmt::format("{}x{} {} {} {} {}|{}|{} {} {} {}", c.W(), c.H(), st.sstate, st.help ? 1 : 0, st.dragging ? 1 : 0,
				st.setupFile, st.setupCopy.why, st.browsing ? 1 : 0, st.browseDir, st.browseRow, st.browseTop);
			if (contentKey != l.contentKey)
				l.content = SetupContent(st, s, c.W(), c.H()), l.contentKey = contentKey;
			if (l.size != c.W())
				l.buttons.clear(), l.size = c.W();
			c.Copy(l.base);
			if (op > 0)
			{
				c.Draw(l.board, std::round((SetupX() - kShadowM) * s), std::round((236 - kShadowM) * s), op);
				c.Draw(l.content, 0, 0, op);
				if (st.sstate == kChecking)
					DrawProgress(st, f, op);
				const std::vector<std::string> labels = SetupButtons(st);
				for (int i = 0; i < (int)labels.size(); i++)
				{
					auto it = l.buttons.find(labels[i]);
					if (it == l.buttons.end())
						it = l.buttons.emplace(labels[i], std::array<Image, 2>{ ButtonLayer(s, "setup_340x72", kSetupButtonW, kButtonH, 29, labels[i], false),
							ButtonLayer(s, "setup_340x72", kSetupButtonW, kButtonH, 29, labels[i], true) }).first;
					float box[4];
					SetupCursorFor(st, i, box);
					const float fms = std::chrono::duration<float, std::milli>(now - st.startFocusSince[i]).count();
					float focus = Linear(fms, 0, 140);
					if (i != st.startItem)
						focus = st.startPrev == i ? 1 - focus : 0;
					const float px = std::round((box[0] + 13) * s), py = std::round((box[1] + 13) * s);
					c.Draw(it->second[0], px, py, op);
					if (focus > 0)
						c.Draw(it->second[1], px, py, op * focus);
				}
			}
		}

		void Draw(State& st, ui::Canvas& c, Clock::time_point now)
		{
			Assets& a = A();
			const float s = std::min(c.W() / 1920.0f, c.H() / 1080.0f);
			a.Rescale(s);
			Layers& l = L();
			if (l.w != c.W() || l.h != c.H())
				BuildLayers(l, c.W(), c.H(), s);
			Frame f{ c, s };
			const float ms = std::chrono::duration<float, std::milli>(now - st.since).count();
			const bool closing = st.phase == Phase::Closing;
			// the start screen: Launch over the frozen title frame, in over 400 ms (out the same at Start Game); Settings over
			// it as the pause menu's panel at its widest, wiping in from the left over 480 ms (out over 400)
			if (st.setup)
			{
				// Setup: its screen, the cursor (around the zone while files are over it), its prompts, the blackout at Start
				const float op = st.setupPre ? Linear(ms, 0, 400) : 1.0f;
				DrawSetup(st, f, now, op);
				if (st.blackoutAt == Clock::time_point{})
				{
					float box[4];
					if (st.dragging && st.sstate != kChecking && st.sstate != kReady)
						box[0] = ZoneX() - 13, box[1] = kZoneY + SetupExtra(st) - 13, box[2] = kZoneW + 26, box[3] = kZoneH + 26;
					else
					{
						const float g = Bezier(.25f, .8f, .25f, 1, Linear(std::chrono::duration<float, std::milli>(now - st.curSince).count(), 0, 170));
						for (int k = 0; k < 4; k++)
							box[k] = st.curFrom[k] + (st.curTo[k] - st.curFrom[k]) * g;
					}
					DrawCursor(f, box, ms, op);
					std::vector<std::pair<std::string, std::string>> pr;
					if (st.browsing)
					{
						const bool game = !st.entries.empty() && st.entries[st.browseRow].kind >= 2;
						pr = { { st.pad ? "A" : "Enter", game ? "Choose" : "Open" }, { st.pad ? "B" : "Esc", "Back" }, { st.pad ? "X" : "F", "Close" } };
					}
					else if (st.sstate == kChecking)
						pr = { { st.pad ? "B" : "Esc", "Cancel" } };
					else if (st.sstate == kReady)
						pr = { { st.pad ? "A" : "Enter", "Start" }, { st.pad ? "X" : "F", "Change" } };
					else
						pr = { { st.pad ? "A" : "Enter", "Browse" }, { st.pad ? "Y" : "H", "Help" } };
					if (!st.setupPre && st.sstate != kChecking && !st.browsing)
						pr.push_back({ st.pad ? "B" : "Esc", "Back" });     // Change Files from Launch: back to it
					DrawPrompts(f, pr, st.pad, op);
					DrawToast(f, st, now);
				}
				else
				{
					const float bms = std::chrono::duration<float, std::milli>(now - st.blackoutAt).count();
					c.Fill(0, 0, (float)c.W(), (float)c.H(), { 0, 0, 0, 255 }, Linear(bms, 0, 600));
					TextCentred(f, A().rocknroll, 36, c.W() / s / 2, c.H() / s / 2, "Setting sail\xE2\x80\xA6", kWhite, Linear(bms, 0, 600));
				}
				return;
			}
			const bool launch = st.start;
			float launchWipe = 1;
			if (launch)
			{
				const float sms0 = std::chrono::duration<float, std::milli>(now - st.screenSince).count();
				const float uiOp = closing ? 1 - Linear(ms, 0, 400) : Linear(ms, 0, 400);
				launchWipe = st.settings ? Bezier(.3f, .8f, .25f, 1, Linear(sms0, 0, 480)) : 1 - Bezier(.3f, .8f, .25f, 1, Linear(sms0, 0, 400));
				DrawLaunch(st, f, now, uiOp);
				if (launchWipe <= 0)
				{
					if (!closing && uiOp > 0)
					{
						const float g = Bezier(.25f, .8f, .25f, 1, Linear(std::chrono::duration<float, std::milli>(now - st.curSince).count(), 0, 170));
						float box[4];
						for (int k = 0; k < 4; k++)
							box[k] = st.curFrom[k] + (st.curTo[k] - st.curFrom[k]) * g;
						DrawCursor(f, box, ms, uiOp);
					}
					std::vector<std::pair<std::string, std::string>> pr = { { st.pad ? "A" : "Enter", "Select" } };
					if (st.startView == 1)
						pr.push_back({ st.pad ? "B" : "Esc", "Back" });
					DrawPrompts(f, pr, st.pad, uiOp);
					DrawToast(f, st, now);
					return;
				}
			}
			if (!closing && UnderDialog(st, c))
			{
				DrawDialog(f, st, now, ms);
				DrawToast(f, st, now);
				return;
			}
			// the backdrop: sharp to blurred over 500 ms (back when closing)
			const float blur = closing ? 1 - Linear(ms, 0, 500) : Linear(ms, 0, 500);
			if (launch)
				;                                           // Launch under it, drawn above
			else if (blur >= 1)
				c.Copy(st.blurred);
			else
			{
				c.Copy(st.backdrop);
				if (blur > 0)
					c.Draw(st.blurred, 0, 0, blur);
			}
			// the panel wipes in from the left over 560 ms (out, reversed, without delays)
			const float wipe = launch ? launchWipe : closing ? 1 - Bezier(.3f, .8f, .25f, 1, Linear(ms, 0, 560)) : Bezier(.3f, .8f, .25f, 1, Linear(ms, 0, 560));
			// the panel's width: 680, widening to Settings' 1380 over 480 ms (and back)
			const float sms = std::chrono::duration<float, std::milli>(now - st.screenSince).count();
			const float widen = launch ? 1.0f : st.settings ? Bezier(.6f, 0, .2f, 1, Linear(sms, 0, 480)) : 1 - Bezier(.6f, 0, .2f, 1, Linear(sms, 0, 480));
			const float pw = std::round((kPanelW + (kWidePanelW - kPanelW) * std::clamp(widen, 0.0f, 1.0f)) * s);
			// its shadow (drop-shadow(10px 0 22px rgba(16,56,24,.4))) beyond its right edge
			for (int i = 0; i < (int)(32 * s); i++)
			{
				const float k = 1 - i / (32 * s);
				c.Fill(pw * wipe + i, 0, 1, (float)c.H(), { 16, 56, 24, 255 }, 0.4f * k * k * (wipe > 0.99f ? 1 : wipe));
			}
			const int shown = (int)std::ceil(pw * wipe);
			c.Clip(0, 0, shown, c.H());
			// Settings once still (its widening and fades over, no row's highlight or toggle's knob moving): the panel as
			// last drawn while nothing it shows has changed (its key: the category, row, values, device)
			std::string skey;
			if (st.settings && !closing && sms > 700 && wipe >= 1 &&
				std::chrono::duration<float, std::milli>(now - st.rowSince).count() > 160 &&
				std::chrono::duration<float, std::milli>(now - st.toggledSince).count() > 240)
			{
				skey = fmt::format("{}x{} {} {} {} {} {} {}", c.W(), c.H(), st.sub, st.cat, st.row, st.pad ? 1 : 0, st.saveRow, st.saveAct);
				if (st.sub == 1)
					for (int r = 0; r <= 4; r++)
						skey += "|" + (r ? SlotAgo(r) : Ago(File1())) + (st.thumbs[r].Empty() ? "" : "t");
				else if (st.cat < (int)Categories().size())
					for (const Row& r : Categories()[st.cat].rows)
					{
						const Look k = LookAt(r);
						skey += "|" + k.value + (k.nextStart ? "*" : "") + (k.locked ? "L" : "") + k.note;
					}
				int pending = 0;
				for (const Category& c2 : Categories())
					for (const Row& r2 : c2.rows)
						pending += LookAt(r2).nextStart ? 1 : 0;
				skey += fmt::format("#{}", pending);
			}
			const bool cached = !skey.empty() && skey == l.settingsKey && l.settings.w == (int)pw && l.settings.h == c.H();
			if (cached)
				c.Rows(l.settings, 0, 0, 0, (int)pw);
			else
			{
			c.Rows(l.panel, 0, 0, 0, std::min(shown, (int)pw));
			c.Fill(pw - 9 * s, 0, 3 * s, (float)c.H(), kRimDark);
			c.Fill(pw - 6 * s, 0, 6 * s, (float)c.H(), kRimLight);
			{
				const Image& fl = a.Get("frame_corner-flourish");
				c.Draw(fl, pw - 22 * s - fl.w, 14 * s, 1, true), c.Draw(fl, pw - 22 * s - fl.w, c.H() - 14 * s - fl.h, 1, true, true);
			}
			// the menu's layer: out over 180 ms (30 to the left) as Settings opens, back 200 ms after it closes; Settings in
			// after 260 ms (opacity 300, from 40 to the right over 380), out over 160
			const float menuOp = launch ? 0.0f : st.settings ? 1 - Linear(sms, 0, 180) : Linear(sms, 200, 200);
			const float menuDx = st.settings ? -30 * Linear(sms, 0, 180) : -30 * (1 - Linear(sms, 200, 200));
			const float setOp = (st.settings ? Linear(sms, 260, 300) : 1 - Linear(sms, 0, 160)) * (closing ? 1 - Linear(ms, 0, 260) : 1);
			const float setDx = st.settings ? 40 * (1 - Bezier(.2f, .8f, .2f, 1, Linear(sms, 260, 380))) : 0;
			// the header: drops from -24, after 140 ms
			const float hp = (closing ? 1 - Linear(ms, 0, 260) : Linear(ms, 140, 260)) * menuOp;
			const float hy = closing ? 0 : -24 * (1 - Bezier(.3f, 1.5f, .5f, 1, Linear(ms, 140, 460)));
			if (hp > 0)
			{
				c.Draw(l.header, (60 + menuDx) * s, (92 + hy) * s, hp);
				// the caption, 26 px, in a box 74 tall: wrapped to the sheet
				const std::string cap = Caption(st.item);
				std::vector<std::string> lines;
				std::string line, word;
				std::istringstream words(cap);
				while (words >> word)
				{
					const std::string next = line.empty() ? word : line + " " + word;
					if (!line.empty() && a.rocknroll.Width(next, 26 * s) > 500 * s)
						lines.push_back(line), line = word;
					else
						line = next;
				}
				if (!line.empty())
					lines.push_back(line);
				const float top = 92 + hy + 148 + (74 - std::min<size_t>(lines.size(), 2) * 26 * 1.4f) / 2;
				for (size_t i = 0; i < lines.size() && i < 2; i++)
					TextCentred(f, a.rocknroll, 26, 340 + menuDx, top + (i + 0.5f) * 26 * 1.4f, lines[i], kInk, hp);
			}
			// the buttons: pop up from +20, each 45 ms after the one before, from 220 ms; the focus cross-fades over 140
			for (int i = 0; i < 5; i++)
			{
				const float d = 220 + 45.0f * i;
				const float bp = (closing ? 1 - Linear(ms, 0, 260) : Linear(ms, d, 260)) * menuOp;
				if (bp <= 0)
					continue;
				const float by = closing ? 0 : 20 * (1 - Bezier(.3f, 1.5f, .5f, 1, Linear(ms, d, 460)));
				const float fms = st.focusSince.size() > (size_t)i ? std::chrono::duration<float, std::milli>(now - st.focusSince[i]).count() : 1e9f;
				float focus = Linear(fms, 0, 140);
				if (!(st.focused.size() > (size_t)i && st.focused[i]))
					focus = 1 - focus;
				const float bx = (kButtonX + menuDx) * s, byy = (kButtonY + kButtonTops[i] + by) * s;
				c.Draw(l.button[i][0], bx, byy, bp);
				if (focus > 0)
					c.Draw(l.button[i][1], bx, byy, bp * focus);
			}
			if (setOp > 0)
				st.sub == 1 ? DrawSaves(f, st, now, setDx, setOp) : st.sub == 2 ? DrawEnhancements(f, setDx, setOp) : DrawSettings(f, st, now, setDx, setOp);
			if (!skey.empty())                            // drawn still: kept for the frames after
			{
				l.settings.w = (int)pw, l.settings.h = c.H();
				l.settings.px.resize((size_t)l.settings.w * l.settings.h);
				for (int y = 0; y < c.H(); y++)
					memcpy(&l.settings.px[(size_t)y * l.settings.w], &c.Pixels()[(size_t)y * c.W()], (size_t)l.settings.w * 4);
				l.settingsKey = skey;
			}
			}
			// the compass emblem at the panel's bottom right (right 84, bottom 22), turned by the item
			{
				const float cms = std::chrono::duration<float, std::milli>(now - st.compassSince).count();
				const float deg = st.compassFrom + (st.compassTo - st.compassFrom) * Bezier(.3f, 1.35f, .5f, 1, Linear(cms, 0, 800));
				const Image& em = a.Get("emblem_compass");
				c.DrawRotated(em, pw - 84 * s - em.w / 2.0f, c.H() - 22 * s - em.h / 2.0f, deg);
			}
			c.NoClip();
			// the cursor: glides over 170 ms, fades in from 520 ms
			const float cp = closing ? 1 - Linear(ms, 0, 200) : Linear(ms, 520, 200);
			if (cp > 0 && st.dialog == Dialog::None && !(st.settings && st.sub == 2))
			{
				const float g = Bezier(.25f, .8f, .25f, 1, Linear(std::chrono::duration<float, std::milli>(now - st.curSince).count(), 0, 170));
				float box[4];
				for (int k = 0; k < 4; k++)
					box[k] = st.curFrom[k] + (st.curTo[k] - st.curFrom[k]) * g;
				c.Clip(0, 0, shown + (int)(40 * s), c.H());
				DrawCursor(f, box, ms, cp);
				c.NoClip();
			}
			// the prompts over the game frame
			const float pp = closing ? 1 - Linear(ms, 0, 200) : Linear(ms, 300, 260);
			if (pp > 0 && !st.settings)
				DrawPrompts(f, { { st.pad ? "A" : "Enter", "Select" }, { st.pad ? "B" : "Esc", "Back" } }, st.pad, pp);
			else if (pp > 0)
			{
				// Settings: Change (Toggle on a toggle; none on a locked or an info row), the categories, the defaults, back
				std::vector<std::pair<std::string, std::string>> pr;
				const auto& cats = Categories();
				if (st.sub == 1)
				{
					pr.push_back({ st.pad ? "A" : "Enter", st.saveRow > 0 && st.saveAct == 1 ? "Load" : "Save" });
					pr.push_back({ st.pad ? "B" : "Esc", "Back" });
				}
				else if (st.sub == 2)
					pr.push_back({ st.pad ? "B" : "Esc", "Back" });
				else if (st.cat < (int)cats.size() && st.row < (int)cats[st.cat].rows.size())
				{
					const Row& r = cats[st.cat].rows[st.row];
					const Look k = LookAt(r);
					if (!k.locked && r.type != "info")
						pr.push_back({ st.pad ? "A" : "Enter", r.type == "toggle" ? "Toggle" : "Change" });
				}
				if (st.sub == 0)
				{
					pr.push_back({ st.pad ? "LB RB" : "Q E", "Category" });
					pr.push_back({ st.pad ? "Y" : "R", "Defaults" });
					pr.push_back({ st.pad ? "B" : "Esc", "Back" });
				}
				DrawPrompts(f, pr, st.pad, pp);
			}
			DrawDialog(f, st, now, ms);
			DrawToast(f, st, now);
			if (!st.blackout.empty())                       // a slot loading: black, its line (a placeholder in the design)
			{
				c.Fill(0, 0, (float)c.W(), (float)c.H(), { 0, 0, 0, 255 });
				TextCentred(f, a.rocknroll, 36, c.W() / s / 2, c.H() / s / 2, st.blackout, kWhite);
			}
		}

		// ---- the actions ----
		void Close(State& st)
		{
			L().dialogKind = -1;
			if (st.phase == Phase::Open || st.phase == Phase::Opening)
			{
				st.phase = Phase::Closing, st.since = Clock::now(), st.dialog = Dialog::None;
				st.swallow = st.lastPad.buttons;
			}
		}

		void Focus(State& st, int item)
		{
			const Clock::time_point now = Clock::now();
			if (st.focusSince.size() < 5)
				st.focusSince.assign(5, now - std::chrono::seconds(10)), st.focused.assign(5, false), st.focused[0] = true;
			if (item != st.item)
			{
				st.focused[st.item] = false, st.focusSince[st.item] = now;
				st.focused[item] = true, st.focusSince[item] = now;
				st.compassFrom = st.compassFrom + (st.compassTo - st.compassFrom) *
					Bezier(.3f, 1.35f, .5f, 1, Linear(std::chrono::duration<float, std::milli>(Clock::now() - st.compassSince).count(), 0, 800));
				st.compassTo = item * -52.0f, st.compassSince = Clock::now();
			}
			const float g = Bezier(.25f, .8f, .25f, 1, Linear(std::chrono::duration<float, std::milli>(Clock::now() - st.curSince).count(), 0, 170));
			for (int k = 0; k < 4; k++)
				st.curFrom[k] = st.curFrom[k] + (st.curTo[k] - st.curFrom[k]) * g;
			st.item = item;
			CursorFor(item, st.curTo);
			st.curSince = Clock::now();
		}

		enum class Input { Up, Down, Left, Right, Confirm, Back, Toggle, CatPrev, CatNext, Reset, Help, Alt };

		// the cursor glides to a new box from wherever it is now
		void GlideTo(State& st, const float box[4])
		{
			const float g = Bezier(.25f, .8f, .25f, 1, Linear(std::chrono::duration<float, std::milli>(Clock::now() - st.curSince).count(), 0, 170));
			for (int k = 0; k < 4; k++)
				st.curFrom[k] = st.curFrom[k] + (st.curTo[k] - st.curFrom[k]) * g, st.curTo[k] = box[k];
			st.curSince = Clock::now();
		}
		void TurnCompass(State& st, float deg)
		{
			st.compassFrom = st.compassFrom + (st.compassTo - st.compassFrom) *
				Bezier(.3f, 1.35f, .5f, 1, Linear(std::chrono::duration<float, std::milli>(Clock::now() - st.compassSince).count(), 0, 800));
			st.compassTo = deg, st.compassSince = Clock::now();
		}
		void FocusRow(State& st, int row)
		{
			if (row != st.row)
				st.prevRow = st.row, st.rowSince = Clock::now();
			st.row = row;
			float box[4];
			RowCursorFor(row, box);
			GlideTo(st, box);
		}

		void FocusSave(State& st, int row, int act)
		{
			if (row != st.saveRow)
				st.prevRow = st.saveRow, st.rowSince = Clock::now();
			st.saveRow = row, st.saveAct = row == 0 ? 0 : act;
			float box[4];
			SaveCursorFor(row, st.saveAct, box);
			GlideTo(st, box);
			TurnCompass(st, 45 + row * 72.0f);
		}

		// a slot saved now: the state from the hold, its thumbnail the picture behind the menu
		void SaveSlot(State& st, int slot)
		{
			WriteThumb(slot, st.backdrop);
			st.thumbs[slot] = ReadThumb(slot);
			wwhd::state::RequestSave(slot);
			st.busySlot = slot;
		}

		// Save's input: File 1 and the four slots, Save or Load, their dialogs
		void ActSaves(State& st, Input in)
		{
			switch (in)
			{
			case Input::Up: FocusSave(st, (st.saveRow + 4) % 5, st.saveAct); return;
			case Input::Down: FocusSave(st, (st.saveRow + 1) % 5, st.saveAct); return;
			case Input::Left: case Input::Right: if (st.saveRow > 0) FocusSave(st, st.saveRow, 1 - st.saveAct); return;
			case Input::Back:
			{
				st.settings = false, st.screenSince = Clock::now();
				float box[4];
				CursorFor(st.item, box);
				GlideTo(st, box);
				TurnCompass(st, st.item * -52.0f);
				return;
			}
			case Input::Confirm:
			{
				if (st.busySlot)
					return;
				const int slot = st.saveRow;
				if (slot == 0)
				{
					st.dialog = Dialog::SaveFile, st.dialogItem = 0, st.dialogSince = Clock::now();
					return;
				}
				const bool filled = !SlotAgo(slot).empty();
				if (st.saveAct == 0)
				{
					if (!filled)
						SaveSlot(st, slot);
					else
						st.dialog = Dialog::Overwrite, st.dialogSlot = slot, st.dialogItem = 0, st.dialogSince = Clock::now();
				}
				else if (!filled)
					ShowToast(st, fmt::format("Slot {} is empty.", slot));
				else
					st.dialog = Dialog::Load, st.dialogSlot = slot, st.dialogItem = 0, st.dialogSince = Clock::now();
				return;
			}
			default: return;
			}
		}

		// Settings' input: rows, values, categories, the category's defaults, back to the menu
		void ActSettings(State& st, Input in)
		{
			const auto& cats = Categories();
			if (cats.empty())
				return;
			st.cat = std::clamp(st.cat, 0, (int)cats.size() - 1);
			const Category& cat = cats[st.cat];
			const int n = (int)cat.rows.size();
			switch (in)
			{
			case Input::Up: FocusRow(st, (st.row + n - 1) % n); return;
			case Input::Down: FocusRow(st, (st.row + 1) % n); return;
			case Input::CatPrev: case Input::CatNext:
				st.cat = (st.cat + (in == Input::CatNext ? 1 : (int)cats.size() - 1)) % (int)cats.size();
				st.prevRow = -1, st.row = 0, st.rowSince = Clock::now();
				FocusRow(st, 0);
				TurnCompass(st, st.cat * 45.0f);
				return;
			case Input::Reset:
			{
				std::vector<std::string> keys;
				for (const Row& r : cat.rows)
					if (!r.key.empty())
						keys.push_back(r.key);
				os::settings::ResetToDefaults(keys);
				ShowToast(st, cat.name + " reset to defaults.");
				return;
			}
			case Input::Back:
			{
				st.settings = false, st.screenSince = Clock::now();
				float box[4];
				CursorFor(st.item, box);
				GlideTo(st, box);
				TurnCompass(st, st.item * -52.0f);
				return;
			}
			case Input::Left: case Input::Right: case Input::Confirm:
			{
				if (st.row >= n)
					return;
				const Row& r = cat.rows[st.row];
				const Look k = LookAt(r);
				if (k.locked)
				{
					if (in == Input::Confirm && !k.lockWhy.empty())
						ShowToast(st, k.lockWhy);
					return;
				}
				if (r.type == "info" || k.option < 0)
					return;
				if (r.type == "toggle")
				{
					// to the other of on and off
					for (int j = 0; j < (int)k.v.shown.size(); j++)
						if ((k.v.shown[j] == "on") != k.on)
						{
							os::settings::Set(k.option, j);
							st.toggled = r.id, st.toggledSince = Clock::now();
							break;
						}
					return;
				}
				// the next choice in the menu's order (from a value no choice names: the first), set by its value
				const int count = (int)r.values.size();
				if (count == 0)
					return;
				const int at = ChoiceOf(r, k.v, k.index);
				const int next = at < 0 ? 0 : ((at + (in == Input::Left ? -1 : 1)) % count + count) % count;
				const auto it = std::find(k.v.values.begin(), k.v.values.end(), r.values[next]);
				if (it != k.v.values.end())
					os::settings::Set(k.option, (int)(it - k.v.values.begin()));
				return;
			}
			default: return;
			}
		}

		// the game's own soft reset (overrides/sixty.cpp): the menu goes and the game fades to the title once let go
		// (the design's blackout, "Returning to title...", until the game has faded to black under it and asked for the title)
		void QuitToTitle(State& st)
		{
			st.dialog = Dialog::None, L().dialogKind = -1;
			const uint32 seen = wwhd::sixty::TitleRequests();
			if (!wwhd::sixty::ReturnToTitle())
			{
				ShowToast(st, "Can\xE2\x80\x99t return to the title screen here.");
				return;
			}
			st.blackout = "Returning to title\xE2\x80\xA6";
			st.toTitle = true, st.titleSeen = seen, st.titleAt = Clock::now(), st.titleAsked = {};
			wwhd::state::RequestHold(false);
		}

		void QuitToDesktop()
		{
			cemuLog_log(LogType::Force, "wwhd: quit to the desktop from the pause menu");
			gpu::SaveShaderCache();
			_exit(0);
		}


		// the start screen's focus: a button of the view (the cursor glides, the fills cross-fade)
		void FocusLaunch(State& st, int i, bool instant = false)
		{
			const Clock::time_point now = Clock::now();
			st.startPrev = instant || i == st.startItem ? -1 : st.startItem;
			st.startItem = i;
			st.startFocusSince[i] = instant ? now - std::chrono::seconds(10) : now;
			if (st.startPrev >= 0)
				st.startFocusSince[st.startPrev] = now;
			float box[4];
			LaunchCursorFor(st.startView, i, box);
			GlideTo(st, box);
		}

		// the game files' check on its own thread (Re-verify, or the first at start when game.txt has none for them)
		void StartCheck(State& st)
		{
			if (Checking(st) || st.copy.path.empty())
				return;
			auto job = std::make_shared<State::CheckJob>();
			st.job = job;
			std::thread([job, path = st.copy.path] {
				job->result = game_files::Check(path, &job->progress);
				job->done.store(true);
			}).detach();
		}

		// ---- Setup's actions (S2) ----
		void FocusSetup(State& st, int i, bool instant = false)
		{
			const Clock::time_point now = Clock::now();
			if (SetupButtons(st).empty())
				return;
			i = std::clamp(i, 0, (int)SetupButtons(st).size() - 1);
			st.startPrev = instant || i == st.startItem ? -1 : st.startItem;
			st.startItem = i;
			st.startFocusSince[i] = instant ? now - std::chrono::seconds(10) : now;
			if (st.startPrev >= 0)
				st.startFocusSince[st.startPrev] = now;
			float box[4];
			SetupCursorFor(st, i, box);
			GlideTo(st, box);
			if (instant)
				for (int k = 0; k < 4; k++)
					st.curFrom[k] = st.curTo[k];
		}

		// what the player chose, as the copy's root: an extracted folder's code/cking.rpx or code/ means the folder
		std::string GameRoot(std::string path)
		{
			while (path.size() > 1 && path.back() == '/')
				path.pop_back();
			std::error_code ec;
			fs::path p = _utf8ToPath(path);
			if (p.extension() == ".rpx" && p.parent_path().filename() == "code")
				p = p.parent_path().parent_path();
			else if (p.filename() == "code" && fs::is_directory(p.parent_path() / "content", ec))
				p = p.parent_path();
			return _pathToUtf8(p);
		}

		void SetupChoose(State& st, const std::string& chosen)
		{
			st.setupFile = GameRoot(chosen), st.sstate = kChecking, st.help = false, st.dragging = false;
			st.setupCopy = game_files::Copy{};
			auto job = std::make_shared<State::CheckJob>();
			st.setupJob = job;
			cemuLog_log(LogType::Force, "wwhd: start screen: Setup checks {}", st.setupFile);
			std::thread([job, path = st.setupFile] {
				job->result = game_files::Check(path, &job->progress, &job->cancel);
				job->done.store(true);
			}).detach();
			FocusSetup(st, 0, true);
		}

		void SetupCancel(State& st)
		{
			if (st.setupJob)
				st.setupJob->cancel.store(true), st.setupJob.reset();
			st.sstate = kIdle;
			FocusSetup(st, 0, true);
		}

		// ---- the menu's own file browser (S2: the pad's, for the Deck's Game Mode): a folder's folders and game files, a game
		// folder (code/ content/ meta/) chosen as one; A opens or chooses, B goes up, X closes ----
		bool GameFolder(const fs::path& p)
		{
			std::error_code ec;
			return fs::is_directory(p / "code", ec) && fs::is_directory(p / "content", ec) && fs::is_directory(p / "meta", ec);
		}
		void FocusBrowse(State& st, int row)
		{
			const int n = (int)st.entries.size();
			st.browseRow = std::clamp(row, 0, std::max(0, n - 1));
			if (st.browseRow < st.browseTop)
				st.browseTop = st.browseRow;
			if (st.browseRow >= st.browseTop + kBrowseRows)
				st.browseTop = st.browseRow - kBrowseRows + 1;
			float box[4];
			BrowseCursorFor(st.browseRow - st.browseTop, box);
			GlideTo(st, box);
		}
		void ReadDir(State& st, const std::string& dir, const std::string& focusName = "")
		{
			std::error_code ec;
			const fs::path d = _utf8ToPath(dir);
			st.browseDir = dir, st.entries.clear();
			if (d != d.root_path() && d.has_parent_path())
				st.entries.push_back({ "..", _pathToUtf8(d.parent_path()), 0 });
			std::vector<State::Entry> dirs, files;
			for (auto it = fs::directory_iterator(d, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator();
				it.increment(ec))
			{
				const std::string name = _pathToUtf8(it->path().filename());
				if (name.empty() || name[0] == '.')
					continue;
				std::error_code ec2;
				if (it->is_directory(ec2))
					dirs.push_back({ name, _pathToUtf8(it->path()), GameFolder(it->path()) ? 2 : 1 });
				else
				{
					std::string ext = _pathToUtf8(it->path().extension());
					for (char& ch : ext)
						ch = (char)tolower((unsigned char)ch);
					if (ext == ".wua" || ext == ".wux" || ext == ".wud")
						files.push_back({ name, _pathToUtf8(it->path()), 3 });
				}
				if (dirs.size() + files.size() > 4000)
					break;
			}
			auto byName = [](const State::Entry& a, const State::Entry& b) {
				return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
					[](char x, char y) { return tolower((unsigned char)x) < tolower((unsigned char)y); });
			};
			std::sort(dirs.begin(), dirs.end(), byName), std::sort(files.begin(), files.end(), byName);
			st.entries.insert(st.entries.end(), dirs.begin(), dirs.end());
			st.entries.insert(st.entries.end(), files.begin(), files.end());
			int row = 0;
			for (int i = 0; i < (int)st.entries.size(); i++)
				if (!focusName.empty() && st.entries[i].name == focusName)
					row = i;
			st.browseTop = 0;
			FocusBrowse(st, row);
		}
		// where browsing starts: the folder of the copy last chosen or registered, else home
		std::string BrowseStart(const State& st)
		{
			std::error_code ec;
			for (const std::string& p : { st.setupFile, game_files::RegisteredPath() })
				if (!p.empty())
				{
					const fs::path parent = _utf8ToPath(p).parent_path();
					if (fs::is_directory(parent, ec))
						return _pathToUtf8(parent);
				}
			const char* home = getenv("HOME");
			return home && *home ? home : "/";
		}
		// the menu's own browser in place of the system's picker: with the pad, under gamescope (the Deck's Game Mode), or
		// when there's no picker (WWHD_OWN_BROWSER=1 always)
		bool OwnBrowser(const State& st)
		{
			static const bool gamescope = getenv("GAMESCOPE_WAYLAND_DISPLAY") || getenv("WWHD_OWN_BROWSER");
			return st.pad || gamescope;
		}
		void OpenBrowser(State& st)
		{
			st.browsing = true, st.help = false;
			ReadDir(st, BrowseStart(st));
			cemuLog_log(LogType::Force, "wwhd: start screen: the file browser at {}", st.browseDir);
		}
		void ActBrowse(State& st, Input in)
		{
			const int n = (int)st.entries.size();
			switch (in)
			{
			case Input::Up: FocusBrowse(st, st.browseRow - 1); return;
			case Input::Down: FocusBrowse(st, st.browseRow + 1); return;
			case Input::Left: FocusBrowse(st, st.browseRow - kBrowseRows); return;
			case Input::Right: FocusBrowse(st, st.browseRow + kBrowseRows); return;
			case Input::Alt: case Input::Help: st.browsing = false; FocusSetup(st, 0, true); return;
			case Input::Back:
			{
				const fs::path d = _utf8ToPath(st.browseDir);
				if (d != d.root_path() && d.has_parent_path())
					ReadDir(st, _pathToUtf8(d.parent_path()), _pathToUtf8(d.filename()));
				else
					st.browsing = false, FocusSetup(st, 0, true);
				return;
			}
			case Input::Confirm:
				if (n == 0)
					return;
				else
				{
					const State::Entry e = st.entries[st.browseRow];
					if (e.kind == 0)
						ReadDir(st, e.path, _pathToUtf8(_utf8ToPath(st.browseDir).filename()));
					else if (e.kind == 1)
						ReadDir(st, e.path);
					else
						st.browsing = false, SetupChoose(st, e.path);
				}
				return;
			default: return;
			}
		}

		// Change Files from Launch, once the new copy verified: the program again on it (the hold's game can't swap its files)
		void Restart(const std::string& path)
		{
			std::ifstream f("/proc/self/cmdline", std::ios::binary);
			std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			std::vector<std::string> args;
			for (size_t at = 0; at < all.size();)
			{
				const size_t end = all.find('\0', at);
				args.push_back(all.substr(at, end == std::string::npos ? std::string::npos : end - at));
				at = end == std::string::npos ? all.size() : end + 1;
			}
			for (size_t i = 0; i + 1 < args.size(); i++)
				if (args[i] == "-g" || args[i] == "--game")
					args[i + 1] = path;             // without -g the registered copy is read, which is this one now
			std::vector<char*> argv;
			for (std::string& a : args)
				argv.push_back(a.data());
			argv.push_back(nullptr);
			cemuLog_log(LogType::Force, "wwhd: start screen: the game files changed to {}: the program starts again", path);
			gpu::SaveShaderCache();
			// every file open closes at the exec (the window's connection, the GPU's and the sound's devices, the log)
			if (DIR* d = opendir("/proc/self/fd"))
			{
				while (const dirent* e = readdir(d))
					if (const int fd = atoi(e->d_name); fd > 2 && fd != dirfd(d))
						fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
				closedir(d);
			}
			execv("/proc/self/exe", argv.data());
			cemuLog_log(LogType::Force, "wwhd: start screen: couldn't start again ({})", strerror(errno));
		}

		void ActSetup(State& st, Input in)
		{
			if (st.blackoutAt != Clock::time_point{})
				return;                                     // setting sail
			if (st.browsing)
			{
				ActBrowse(st, in);
				return;
			}
			const std::vector<std::string> labels = SetupButtons(st);
			const int n = (int)labels.size();
			const bool canHelp = st.sstate == kIdle || st.sstate == kBad;
			switch (in)
			{
			case Input::Up: case Input::Left: FocusSetup(st, (st.startItem + n - 1) % n); return;
			case Input::Down: case Input::Right: FocusSetup(st, (st.startItem + 1) % n); return;
			case Input::Help: case Input::Reset:
				if (canHelp)
					st.help = !st.help, FocusSetup(st, st.startItem, true);   // a third line moves the buttons
				return;
			case Input::Alt:
				if (st.sstate == kReady)
					OwnBrowser(st) ? OpenBrowser(st) : (void)(st.browseWanted = true);   // Change
				return;
			case Input::Back:
				if (st.sstate == kChecking)
					SetupCancel(st);
				else if (st.help)
					st.help = false;
				else if (!st.setupPre)                      // Change Files from Launch: back to its Game Files
				{
					if (st.setupJob)
						st.setupJob->cancel.store(true), st.setupJob.reset();
					st.setup = false, st.startView = 1;
					FocusLaunch(st, 1, true);
				}
				return;
			case Input::Confirm: break;
			default: return;
			}
			const std::string& b = labels[std::clamp(st.startItem, 0, n - 1)];
			if (b == "Browse Files" || b == "Try Another File" || b == "Change Files")
				OwnBrowser(st) ? OpenBrowser(st) : (void)(st.browseWanted = true);
			else if (b == "Help" || b == "Got It")
				st.help = !st.help, FocusSetup(st, st.startItem, true);
			else if (b == "Cancel")
				SetupCancel(st);
			else if (b == "Start Game")
				st.blackoutAt = Clock::now();
		}

		void ActLaunch(State& st, Input in)
		{
			const int n = LaunchCount(st.startView);
			switch (in)
			{
			case Input::Up: case Input::Left: FocusLaunch(st, (st.startItem + n - 1) % n); return;
			case Input::Down: case Input::Right: FocusLaunch(st, (st.startItem + 1) % n); return;
			case Input::Back:
				if (st.startView == 1)                      // Game Files: back to the menu, on Game Files
					st.startView = 0, FocusLaunch(st, 2, true);
				return;
			case Input::Confirm: break;
			default: return;
			}
			if (st.startView == 1)
			{
				if (st.startItem == 0)
					StartCheck(st);                         // Re-verify: ignored while a check runs
				else
				{
					// Change Files: Setup over the held title; the copy registered stays so until a new one verifies
					st.setup = true, st.setupPre = false, st.sstate = kIdle, st.help = false, st.dragging = false;
					st.setupFile.clear(), st.setupCopy = game_files::Copy{};
					FocusSetup(st, 0, true);
				}
				return;
			}
			switch (st.startItem)
			{
			case 0:
				if (st.checked && !st.copy.ok)
					ShowToast(st, "These game files didn\xE2\x80\x99t verify: see Game Files.");
				else
					Close(st);                              // Start Game: the hold let go, into the live title
				return;
			case 1:
				if (Categories().empty())
					return;
				st.settings = true, st.sub = 0, st.screenSince = Clock::now();
				st.cat = 0, st.prevRow = -1, st.row = 0, st.rowSince = Clock::now();
				FocusRow(st, 0);
				TurnCompass(st, 0);
				return;
			case 2: st.startView = 1, FocusLaunch(st, 0, true); return;
			default: QuitToDesktop();
			}
		}

		void Act(State& st, Input in)
		{
			if (in == Input::Toggle && (st.start || st.setup))
				return;                                     // F2 on the start screen: nothing (Esc is its Back)
			if (in == Input::Toggle)
			{
				if (st.phase == Phase::Closed)
				{
					if (Clock::now() - st.closedAt < std::chrono::milliseconds(400))
						return;
					st.phase = Phase::Waiting, st.since = Clock::now(), st.dialog = Dialog::None;
					st.settings = false, st.screenSince = Clock::now() - std::chrono::seconds(10);
					st.item = 0, st.focusSince.clear();
					Focus(st, 0);
					for (int k = 0; k < 4; k++)
						st.curFrom[k] = st.curTo[k];
					st.compassFrom = st.compassTo = 0;
					wwhd::state::RequestHold(true);
					gpu::RequestTvPicture();
				}
				else
					Close(st);
				return;
			}
			if (st.phase != Phase::Open && st.phase != Phase::Opening)
				return;
			if (!st.blackout.empty())
				return;                                     // a slot loading: nothing until it's done
			if (st.settings && st.dialog == Dialog::None && st.start && st.sub == 0 && in == Input::Back)
			{
				st.settings = false, st.screenSince = Clock::now();   // Settings from the start screen: back to Launch
				FocusLaunch(st, 1, true);
				st.dirty = true;
				return;
			}
			if (st.setup)
			{
				ActSetup(st, in);
				st.dirty = true;
				return;
			}
			if (st.start && !st.settings)
			{
				ActLaunch(st, in);
				st.dirty = true;
				return;
			}
			if (st.settings && st.dialog == Dialog::None)
			{
				if (st.sub == 2)
				{
					if (in == Input::Back)                  // Enhancements: back to the menu
					{
						st.settings = false, st.screenSince = Clock::now();
						float box[4];
						CursorFor(st.item, box);
						GlideTo(st, box);
						TurnCompass(st, st.item * -52.0f);
					}
				}
				else
					st.sub == 1 ? ActSaves(st, in) : ActSettings(st, in);
				st.dirty = true;
				return;
			}
			if (st.dialog == Dialog::Saving || st.dialog == Dialog::Saved)
				return;                                     // File 1 being saved: nothing until it's done
			if (st.dialog != Dialog::None)
			{
				const DialogSpec d = Spec(st.dialog, st.dialogSlot);
				const int n = (int)d.buttons.size();
				if (in == Input::Left || in == Input::Up)
					st.dialogItem = (st.dialogItem + n - 1) % n;
				else if (in == Input::Right || in == Input::Down)
					st.dialogItem = (st.dialogItem + 1) % n;
				else if (in == Input::Back)
					st.dialog = Dialog::None;
				else if (in == Input::Confirm && d.enabled[st.dialogItem])
				{
					const std::string& b = d.buttons[st.dialogItem];
					if (b == "Cancel")
						st.dialog = Dialog::None;
					else if (b == "Desktop")
					{
						if (JustSaved())
							QuitToDesktop();                    // nothing unsaved: straight away
						st.dialog = Dialog::QuitDesktop, st.dialogItem = 0, st.dialogSince = Clock::now();
					}
					else if (b == "Don\xE2\x80\x99t Save" && st.dialog == Dialog::QuitTitle)
						QuitToTitle(st);
					else if (b == "Don\xE2\x80\x99t Save")
						QuitToDesktop();
					else if (b == "Save & Quit" || b == "Save")
					{
						st.quitAfterSave = b == "Save & Quit" ? st.dialog : Dialog::None;
						st.dialog = Dialog::Saving, st.dialogItem = 0, st.dialogSince = Clock::now();
						wwhd::state::RequestGameSave();
					}
					else if (b == "Title Screen")
					{
						if (JustSaved())
							QuitToTitle(st);                    // nothing unsaved: straight away
						else
							st.dialog = Dialog::QuitTitle, st.dialogItem = 0, st.dialogSince = Clock::now();
					}
					else if (b == "Overwrite")
					{
						st.dialog = Dialog::None;
						SaveSlot(st, st.dialogSlot);
					}
					else if (b == "Load")
					{
						st.dialog = Dialog::None;
						st.blackout = fmt::format("Loading Slot {}\xE2\x80\xA6", st.dialogSlot);
						st.busySlot = -st.dialogSlot;
						wwhd::state::RequestLoad(st.dialogSlot);
					}
				}
				if (st.dialog == Dialog::None)
					L().dialogKind = -1;                    // the next one's layers and the frame under it anew
				st.dirty = true;
				return;
			}
			switch (in)
			{
			case Input::Up: Focus(st, (st.item + 4) % 5); break;
			case Input::Down: Focus(st, (st.item + 1) % 5); break;
			case Input::Back: Close(st); break;
			case Input::Confirm:
				if (st.item == 0)
					Close(st);
				else if (st.item == 4)
				{
					st.dialog = Dialog::Quit, st.dialogItem = 0, st.dialogSince = Clock::now();
					// the Save & Quit choice needs File 1's save from the menu (M3): Desktop opens the unsaved dialog
				}
				else if (st.item == 3)
				{
					st.settings = true, st.sub = 1, st.screenSince = Clock::now();
					for (int r = 1; r <= 4; r++)
						st.thumbs[r] = ReadThumb(r);
					st.prevRow = -1, st.saveRow = -1, st.rowSince = Clock::now();
					FocusSave(st, 0, 0);
				}
				else if (st.item == 2 && !Categories().empty())
				{
					st.settings = true, st.sub = 0, st.screenSince = Clock::now();
					st.cat = 0, st.prevRow = -1, st.row = 0, st.rowSince = Clock::now();
					FocusRow(st, 0);
					TurnCompass(st, 0);
				}
				else if (st.item == 1)
					st.settings = true, st.sub = 2, st.screenSince = Clock::now();
				else
					ShowToast(st, std::string(kItems[st.item]) + " comes in a later update.");
				break;
			default: break;
			}
			st.dirty = true;
		}

		// the menu's thread: the backdrop once the TV picture comes, then a frame each 16 ms while it moves
		// the "Pause" prompt over the game while the menu is closed (the design's closed state; the owner's choice: a few
		// seconds at the start of play and after the menu closes): in over 260 ms, 4 s in all, out over 600; the glyph
		// follows the last device. A panel of the renderer's, blended at its opacity (gpu SetHintPanel)
		void Hint(State& st, Clock::time_point now, uint32 ww, uint32 wh)
		{
			const uint32 plays = wwhd::sixty::PlayStarts();
			if (plays != st.playSeen)
			{
				st.playSeen = plays, st.hintAt = now + std::chrono::milliseconds(3000);   // play's first frames: after its load and fade
				cemuLog_log(LogType::Force, "wwhd: pause menu: play starts: the Pause prompt in 3 s");
			}
			const float ms = std::chrono::duration<float, std::milli>(now - st.hintAt).count();
			if (st.hintAt == Clock::time_point{} || st.phase != Phase::Closed || !ww || !wh || ms > 4600)
			{
				if (st.hintAt != Clock::time_point{} && (st.phase != Phase::Closed || ms > 4600))
					st.hintAt = {};
				gpu::SetHintOpacity(0);
				return;
			}
			if (ms < 0)
				return;
			const float s = std::min(ww / 1920.0f, wh / 1080.0f);
			const std::string key = fmt::format("{}x{} {}", ww, wh, st.pad ? 1 : 0);
			if (key != st.hintKey)
			{
				A().Rescale(s);
				// at the top centre: the design's top right is where the game's own button prompts are (ZR, R, X, Y)
				ui::Canvas c((int)std::ceil(300 * s), (int)std::ceil(130 * s));
				Frame f{ c, s };
				DrawPrompts(f, { { st.pad ? "\xE2\x88\x92" : "Esc", "Pause" } }, st.pad, 1);
				const float pw = std::max(GlyphWidth(st.pad ? "\xE2\x88\x92" : "Esc", st.pad), A().rocknroll.Width("Pause", 24 * s));
				const sint32 right = (sint32)std::lround(44 * s);    // DrawPrompts' right edge in the canvas
				gpu::SetHintPanel((sint32)ww / 2 - (c.W() - right - (sint32)(pw / 2)), 0, (uint32)c.W(), (uint32)c.H(), std::move(c.Pixels()));
				st.hintKey = key;
			}
			gpu::SetHintOpacity(ms < 260 ? ms / 260 : ms < 4000 ? 1.0f : 1 - (ms - 4000) / 600);
		}

		std::string& GamePath()
		{
			static std::string path;                    // the game files this run reads (SetGame)
			return path;
		}

		// the start screen at all: a player's run (off under a route's input, CEMU_INPUT_SCRIPT, as the checks run; WWHD_START_SCREEN=0|1)
		bool StartWanted()
		{
			static const bool on = [] {
				if (const char* e = getenv("WWHD_START_SCREEN"))
					return strcmp(e, "0") != 0;
				return getenv("CEMU_INPUT_SCRIPT") == nullptr;
			}();
			return on;
		}

		// Launch over the title: once the game has asked for the title scene and its first frames are shown, the game held
		// there (its picture the backdrop, frozen), the menu on Start Game; the game files' check from game.txt, or run now
		void StartLaunch(State& st, bool window)
		{
			if (st.startDone || st.start || st.phase != Phase::Closed || !window || !StartWanted())
				return;
			if (wwhd::sixty::PlayStarts() != 0)
			{
				st.startDone = true;                        // play already (a state loaded at start): no start screen
				return;
			}
			const uint32 reqs = wwhd::sixty::TitleRequests();
			const Clock::time_point now = Clock::now();
			if (reqs == 0)
				return;
			if (reqs != st.titleReqs)                       // still asking: the logo's fade
			{
				st.titleReqs = reqs, st.titleQuietSince = now, st.probeAt = {};
				return;
			}
			if (now - st.titleQuietSince < std::chrono::milliseconds(300))
				return;
			// the title's stage loading, then its sky: the first bright, settled picture (20 s at most)
			bool ready = now - st.titleQuietSince > std::chrono::seconds(20);
			if (!ready)
			{
				if (st.probeAt == Clock::time_point{})
				{
					if (now >= st.nextProbe)
						gpu::RequestTvPicture(), st.probeAt = now;
					return;
				}
				std::vector<uint32> tv;
				uint32 tw = 0, th = 0;
				if (!gpu::TakeTvPicture(tv, tw, th))
				{
					if (now - st.probeAt > std::chrono::seconds(1))
						st.probeAt = {};                    // none came: ask again
					return;
				}
				const float luma = Luma(tv);
				ready = luma > 0.35f && st.lastLuma > 0 && std::abs(luma - st.lastLuma) < 0.02f;
				st.lastLuma = luma;
				st.probeAt = {};
				if (!ready)
				{
					st.nextProbe = now + std::chrono::milliseconds(150);
					return;
				}
			}
			st.start = true, st.startView = 0, st.phase = Phase::Waiting, st.since = now, st.dialog = Dialog::None;
			st.settings = false, st.screenSince = now - std::chrono::seconds(10);
			FocusLaunch(st, 0, true);
			for (int k = 0; k < 4; k++)
				st.curFrom[k] = st.curTo[k];
			st.compassFrom = st.compassTo = 0;
			st.copy.path = GamePath();
			game_files::Copy reg;
			if (game_files::Registered(reg) && reg.path == st.copy.path && reg.ok)
				st.copy = reg, st.checked = true;
			else
				StartCheck(st);
			cemuLog_log(LogType::Force, "wwhd: start screen: over the title ({} frames shown, its brightness {:.2f}); the game files {}",
				gpu::ShownFrames(), st.lastLuma, st.checked ? "verified before" : "being checked");
			wwhd::state::RequestHold(true);
			gpu::RequestTvPicture();
		}

		void Run()
		{
			State& st = S();
			std::unique_lock lock(st.lock);
			st.playSeen = wwhd::sixty::PlayStarts();
			for (;;)
			{
				st.wake.wait_for(lock, std::chrono::milliseconds(st.phase != Phase::Closed || st.hintAt != Clock::time_point{} ||
					(!st.startDone && StartWanted()) ? 16 : 250));
				const Clock::time_point now = Clock::now();
				uint32 ww = 0, wh = 0;
				const bool window = gpu::WindowExtent(ww, wh) && ww && wh;
				Hint(st, now, window ? ww : 0, window ? wh : 0);
				StartLaunch(st, window);
				if (st.setupJob && st.setupJob->done.load())   // Setup's check done: ready (registered) or wrong
				{
					st.setupCopy = st.setupJob->result, st.setupCopy.path = st.setupFile;
					st.sstate = st.setupCopy.ok ? kReady : kBad;
					if (st.setupCopy.ok)
						game_files::Register(st.setupCopy);
					cemuLog_log(LogType::Force, "wwhd: start screen: Setup: {} {}{}", st.setupFile, st.setupCopy.ok ? "verified" : "didn't verify: ",
						st.setupCopy.why);
					st.setupJob.reset(), st.dirty = true;
					FocusSetup(st, 0, true);
				}
				if (st.dragging && now - st.dragSeen > std::chrono::milliseconds(400))
					st.dragging = false;                    // dragged away (X11 says nothing when files leave)
				if (st.setup && st.blackoutAt != Clock::time_point{} && now - st.blackoutAt > std::chrono::milliseconds(700))
				{
					if (st.setupPre)
					{
						if (st.setupResult.empty())
							st.setupResult = st.setupCopy.path;   // the window's loop takes it (SetupDone): the game is prepared
					}
					else
					{
						Restart(st.setupCopy.path);             // back only when it failed: the copy is registered, the next start reads it
						st.blackoutAt = {}, st.sstate = kBad, st.dirty = true;
						st.setupCopy.why = "WindSong couldn\xE2\x80\x99t restart on these files. Quit and open it again to play them.";
						FocusSetup(st, 0, true);
					}
				}
				if (st.job && st.job->done.load())          // a check of the game files done: its outcome (registered if it passed)
				{
					const std::string path = st.copy.path;
					st.copy = st.job->result, st.copy.path = path, st.checked = true;
					if (st.copy.ok)
						game_files::Register(st.copy);
					cemuLog_log(LogType::Force, "wwhd: start screen: the game files {}{}", st.copy.ok ? "verified" : "didn't verify: ", st.copy.why);
					st.job.reset(), st.dirty = true;
				}
				if (st.phase == Phase::Closed || !window)
				{
					if (st.phase == Phase::Closed && st.spare.capacity())
						st.spare = std::vector<uint32>();       // closed: its pixels not kept for play
					continue;
				}
				// the pad's held direction repeats: first after 380 ms, then each 110
				if (st.held && now >= st.repeatAt)
				{
					const uint32 h = st.held;
					st.repeatAt = now + std::chrono::milliseconds(110);
					if (h & B::UP) Act(st, Input::Up);
					else if (h & B::DOWN) Act(st, Input::Down);
					else if (h & B::LEFT) Act(st, Input::Left);
					else if (h & B::RIGHT) Act(st, Input::Right);
				}
				if (st.phase == Phase::Waiting)
				{
					std::vector<uint32> tv;
					uint32 tw = 0, th = 0;
					const bool got = gpu::TakeTvPicture(tv, tw, th);
					if (!got && now - st.since < std::chrono::milliseconds(500))
						continue;
					// the window's picture: the TV image fitted and centred as the renderer shows it, black around it
					Image screen;
					screen.w = (int)ww, screen.h = (int)wh;
					screen.px.assign((size_t)ww * wh, 0xFF000000u);
					if (got && tw && th)
					{
						Image t{ (int)tw, (int)th, std::move(tv) };
						for (uint32& p : t.px)
							p |= 0xFF000000u;                   // the TV image's alpha is the targets' (0 where the world is): opaque
						const double k = std::min((double)ww / tw, (double)wh / th);
						const int dw = std::max(1, (int)(tw * k)), dh = std::max(1, (int)(th * k));
						Image fit = ui::Scaled(t, dw, dh);
						const int ox = ((int)ww - dw) / 2, oy = ((int)wh - dh) / 2;
						for (int y = 0; y < dh; y++)
							memcpy(&screen.px[(size_t)(y + oy) * ww + ox], &fit.px[(size_t)y * dw], (size_t)dw * 4);
					}
					// blurred at half size (blur(6px) times the scale), then back up (the start screen's frame stays sharp)
					const float s = std::min(ww / 1920.0f, wh / 1080.0f);
					if (!st.start)
					{
						Image half = ui::Scaled(screen, std::max(1, (int)ww / 2), std::max(1, (int)wh / 2));
						st.blurred = ui::Scaled(ui::Blurred(half, 3 * s, 0.95f, 0.92f), (int)ww, (int)wh);
					}
					st.backdrop = std::move(screen);
					st.bw = (int)ww, st.bh = (int)wh;
					st.phase = Phase::Opening, st.since = Clock::now();
				}
				if ((int)ww != st.bw || (int)wh != st.bh)       // resized while up: the backdrop to the new size
				{
					st.backdrop = ui::Scaled(st.backdrop, (int)ww, (int)wh);
					if (!st.blurred.Empty())
						st.blurred = ui::Scaled(st.blurred, (int)ww, (int)wh);
					st.bw = (int)ww, st.bh = (int)wh;
				}
				// File 1's save: "Saving..." until the game's save thread has written it, "Saved" for 850 ms, then the toast
				if (st.dialog == Dialog::Saving)
				{
					const int g = wwhd::state::GameSaveStatus();
					if (g == 2)
						st.dialog = Dialog::Saved, st.dialogSince = Clock::now(), L().dialogKind = -1;
					else if (g == -1)
						st.dialog = Dialog::None, L().dialogKind = -1, st.quitAfterSave = Dialog::None, ShowToast(st, "Couldn\xE2\x80\x99t save File 1.");
				}
				else if (st.dialog == Dialog::Saved && now - st.dialogSince > std::chrono::milliseconds(850))
				{
					st.dialog = Dialog::None, L().dialogKind = -1;
					if (st.quitAfterSave == Dialog::QuitDesktop)
						QuitToDesktop();
					if (st.quitAfterSave == Dialog::QuitTitle)
						st.quitAfterSave = Dialog::None, QuitToTitle(st);
					else
						ShowToast(st, "Progress saved to File 1.");
				}
				// a slot's save or load from the hold: its outcome (savestate's status) when it's done
				if (st.busySlot)
				{
					const std::string status = wwhd::state::Status();
					const int slot = std::abs(st.busySlot);
					if (st.busySlot > 0 && status.rfind("Saved", 0) == 0)
					{
						ShowToast(st, fmt::format("Saved to Slot {}.", slot));
						st.thumbs[slot] = ReadThumb(slot);         // its state is there now
						st.busySlot = 0, st.dirty = true;
					}
					else if (st.busySlot < 0 && status.rfind("Loaded", 0) == 0)
					{
						// the game is the state's now: the menu goes at once (its picture is from before), the game goes on
						st.busySlot = 0, st.blackout.clear();
						st.phase = Phase::Closed, st.closedAt = Clock::now(), st.settings = false, st.hintAt = st.closedAt + std::chrono::milliseconds(300);
						gpu::SetMenuPanel(0, 0, {});
						wwhd::state::RequestHold(false);
						continue;
					}
					else if (status.rfind("Couldn't", 0) == 0)
					{
						ShowToast(st, status + ".");
						if (st.busySlot > 0)
							std::filesystem::remove(ThumbPath(slot)), st.thumbs[slot] = ReadThumb(slot);
						st.busySlot = 0, st.blackout.clear(), st.dirty = true;
					}
				}
				// Quit to the title: the menu goes 1.2 s after the game asked for the title (its 30-frame fade under the
				// blackout), or after 8 s if it never does
				if (st.toTitle)
				{
					const Clock::time_point t = Clock::now();
					if (st.titleAsked == Clock::time_point{} && wwhd::sixty::TitleRequests() != st.titleSeen)
						st.titleAsked = t;
					if ((st.titleAsked != Clock::time_point{} && t - st.titleAsked > std::chrono::milliseconds(1200)) ||
						t - st.titleAt > std::chrono::seconds(8))
					{
						st.toTitle = false, st.blackout.clear();
						st.phase = Phase::Closed, st.closedAt = t, st.settings = false;
						gpu::SetMenuPanel(0, 0, {});
						continue;
					}
				}
				const float ms = std::chrono::duration<float, std::milli>(Clock::now() - st.since).count();
				if (st.phase == Phase::Opening && ms > 1000)
					st.phase = Phase::Open;
				if (st.phase == Phase::Closing && ms > 560)
				{
					st.phase = Phase::Closed, st.closedAt = Clock::now(), st.hintAt = st.closedAt + std::chrono::milliseconds(300);
					if (st.start)                               // Start Game: into the live title (no Pause prompt over it)
					{
						st.start = false, st.startDone = true, st.hintAt = {};
						st.launchBase = Image{}, st.launchKey.clear();
						cemuLog_log(LogType::Force, "wwhd: start screen: Start Game, the title goes on");
					}
					gpu::SetMenuPanel(0, 0, {});
					wwhd::state::RequestHold(false);
					continue;
				}
				const Clock::time_point t0 = Clock::now();
				ui::Canvas c((int)ww, (int)wh, std::move(st.spare));
				Draw(st, c, t0);
				st.spare = gpu::SetMenuPanel(ww, wh, std::move(c.Pixels()));
				// the cost of a frame (the log, each 120 frames drawn): it is all on this thread, the game held meanwhile
				static uint32 s_frames = 0;
				static double s_ms = 0, s_worst = 0;
				const double took = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
				s_ms += took, s_worst = std::max(s_worst, took);
				if (++s_frames % 120 == 0)
				{
					cemuLog_log(LogType::Force, "wwhd: pause menu: {}x{}, {:.1f} ms a frame drawn ({:.1f} the longest)", ww, wh, s_ms / 120, s_worst);
					s_ms = 0, s_worst = 0;
				}
			}
		}

		void Start(State& st)
		{
			if (st.started)
				return;
			st.started = true;
			cemuLog_log(LogType::Force, "wwhd: pause menu: its thread starts");
			st.thread = std::thread(Run);
			st.thread.detach();
		}
	}

	bool Available()
	{
		static const bool ok = [] {
			if (!gpu::RendererOn())
				return false;
			Assets& a = A();
			a.dir = AssetDir();
			if (a.dir.empty() || !a.reggae.Load(a.dir + "/fonts/ReggaeOne-Regular.ttf") || !a.rocknroll.Load(a.dir + "/fonts/RocknRollOne-Regular.ttf"))
			{
				cemuLog_log(LogType::Force, "wwhd: the pause menu's assets weren't found (assets/pause_menu; WWHD_UI_ASSETS): no pause menu");
				return false;
			}
			if (!LoadCategories(a.dir + "/menu-and-settings.json"))
				cemuLog_log(LogType::Force, "wwhd: the pause menu's settings (menu-and-settings.json) didn't load: no Settings screen");
			return true;
		}();
		return ok;
	}

	void Begin()
	{
		static std::atomic<bool> begun{ false };
		if (begun.load(std::memory_order_relaxed) || gpu::ShownFrames() == 0 || !Available())
			return;                                     // (once the renderer has shown a frame: it is up, Available won't start it)
		begun = true;
		State& st = S();
		std::lock_guard lock(st.lock);
		Start(st);
	}

	bool Up()
	{
		State& st = S();
		std::lock_guard lock(st.lock);
		return st.phase != Phase::Closed;
	}

	void Toggle()
	{
		if (!Available())
			return;
		State& st = S();
		std::lock_guard lock(st.lock);
		Start(st);
		Act(st, Input::Toggle);
		st.wake.notify_one();
	}

	bool Key(uint32 key, bool repeat)
	{
		State& st = S();
		std::unique_lock lock(st.lock);
		const bool up = st.phase != Phase::Closed;
		if (key == SDLK_F2 || (key == SDLK_ESCAPE && !up))
		{
			if (repeat)
				return true;
			lock.unlock();
			if (!Available())
				return false;
			Toggle();
			return true;
		}
		if (!up)
		{
			if (!repeat)
				st.pad = false;                         // the closed prompt's glyph: the keyboard's
			return false;
		}
		st.pad = false;
		Input in;
		switch (key)
		{
		case SDLK_UP: case SDLK_W: in = Input::Up; break;
		case SDLK_DOWN: case SDLK_S: in = Input::Down; break;
		case SDLK_LEFT: case SDLK_A: in = Input::Left; break;
		case SDLK_RIGHT: case SDLK_D: in = Input::Right; break;
		case SDLK_RETURN: case SDLK_SPACE: case SDLK_KP_ENTER: in = Input::Confirm; break;
		case SDLK_ESCAPE: case SDLK_BACKSPACE: in = Input::Back; break;
		case SDLK_Q: in = Input::CatPrev; break;
		case SDLK_E: in = Input::CatNext; break;
		case SDLK_R: in = Input::Reset; break;
		case SDLK_H: in = Input::Help; break;
		case SDLK_F: in = Input::Alt; break;
		default: return true;                           // the game doesn't see keys while the menu is up
		}
		if (!repeat || in != Input::Confirm)
			Act(st, in);
		st.wake.notify_one();
		return true;
	}

	void Pad(const os::input::Pad& pad)
	{
		State& st = S();
		std::unique_lock lock(st.lock);
		uint32 now = pad.buttons;
		if (pad.ly > 0.5f) now |= B::UP;
		if (pad.ly < -0.5f) now |= B::DOWN;
		if (pad.lx < -0.5f) now |= B::LEFT;
		if (pad.lx > 0.5f) now |= B::RIGHT;
		const uint32 pressed = now & ~st.lastPad.buttons;
		st.lastPad.buttons = now;
		if (pressed && st.phase == Phase::Closed)
			st.pad = true;                              // the closed prompt's glyph follows the device too
		st.swallow &= now;                              // a button held at closing stays the menu's until released
		if (pressed & B::MINUS)                         // SELECT (the owner's choice: − does nothing useful in-game without the GamePad)
		{
			lock.unlock();
			Toggle();
			return;
		}
		if (st.phase == Phase::Closed)
			return;
		if (pressed)
			st.pad = true;
		const uint32 dirs = B::UP | B::DOWN | B::LEFT | B::RIGHT;
		if (pressed & dirs)
		{
			st.held = now & dirs, st.repeatAt = Clock::now() + std::chrono::milliseconds(380);
			Act(st, (pressed & B::UP) ? Input::Up : (pressed & B::DOWN) ? Input::Down : (pressed & B::LEFT) ? Input::Left : Input::Right);
		}
		else if (!(now & dirs))
			st.held = 0;
		if (pressed & B::A)
			Act(st, Input::Confirm);
		else if (pressed & B::B)
			Act(st, Input::Back);
		else if (pressed & B::L)
			Act(st, Input::CatPrev);
		else if (pressed & B::R)
			Act(st, Input::CatNext);
		else if (pressed & B::Y)
			Act(st, st.setup ? Input::Help : Input::Reset);
		else if (pressed & B::X)
			Act(st, Input::Alt);
		st.wake.notify_one();
	}

	void Mouse(float x, float y, int button)
	{
		State& st = S();
		std::lock_guard lock(st.lock);
		if (st.phase != Phase::Open && st.phase != Phase::Opening)
			return;
		uint32 ww = 0, wh = 0;
		if (!gpu::WindowExtent(ww, wh) || !ww || !wh)
			return;
		st.pad = false;                                 // the mouse: the keyboard's prompts
		const float s = std::min(ww / 1920.0f, wh / 1080.0f), dx = x / s, dy = y / s;
		const auto inside = [&](const float box[4]) {   // a cursor box less its 13 on every side: the button
			return dx >= box[0] + 13 && dy >= box[1] + 13 && dx < box[0] + box[2] - 13 && dy < box[1] + box[3] - 13;
		};
		if (st.setup && st.browsing)
		{
			// the browser: a row under the mouse takes the focus, a click opens or chooses it; a right click goes up
			for (int i = 0; i < kBrowseRows && st.browseTop + i < (int)st.entries.size(); i++)
			{
				float box[4];
				BrowseCursorFor(i, box);
				if (!inside(box))
					continue;
				if (st.browseRow != st.browseTop + i)
					FocusBrowse(st, st.browseTop + i), st.dirty = true;
				if (button == 1)
				{
					Act(st, Input::Confirm);
					break;
				}
			}
			if (button == 2)
				Act(st, Input::Back);
		}
		else if (st.setup)
		{
			// Setup: a button under the mouse takes the focus, a click chooses it; a right click goes back
			const int n = (int)SetupButtons(st).size();
			for (int i = 0; i < n; i++)
			{
				float box[4];
				SetupCursorFor(st, i, box);
				if (!inside(box))
					continue;
				if (st.startItem != i)
					FocusSetup(st, i), st.dirty = true;
				if (button == 1)
					Act(st, Input::Confirm);
			}
			if (button == 2)
				Act(st, Input::Back);
		}
		else if (st.start && !st.settings)
		{
			// the start screen: a button under the mouse takes the focus, a click chooses it; a right click goes back
			for (int i = 0; i < LaunchCount(st.startView); i++)
			{
				float box[4];
				LaunchCursorFor(st.startView, i, box);
				if (!inside(box))
					continue;
				if (st.startItem != i)
					FocusLaunch(st, i), st.dirty = true;
				if (button == 1)
					Act(st, Input::Confirm);
			}
			if (button == 2)
				Act(st, Input::Back);
		}
		else if (st.dialog != Dialog::None)
		{
			const DialogSpec d = Spec(st.dialog, st.dialogSlot);
			const int n = (int)d.buttons.size();
			const float fw = 1040, fh = 42 + 260 + 24 + 110 + 30, fx = (ww / s - fw) / 2, fy = (wh / s - fh) / 2;
			for (int i = 0; i < n; i++)
			{
				float box[4];
				DialogCursorFor(n, i, fx, fy, box);
				if (inside(box))
				{
					if (st.dialogItem != i)
						st.dialogItem = i, st.dirty = true;
					if (button == 1)
						Act(st, Input::Confirm);
				}
			}
			if (button == 2)
				Act(st, Input::Back);
		}
		else if (st.settings && st.sub == 2)
		{
			if (button == 2)                            // Enhancements: nothing to choose yet
				Act(st, Input::Back);
		}
		else if (st.settings && st.sub == 1)
		{
			// Save: a row under the mouse takes the focus, a button the focus and a click chooses it; a right click
			// goes back
			for (int row = 0; row < 5; row++)
			{
				const float rx = 110, ry = 150 + (row == 0 ? 70 : 264 + 134 * (row - 1));
				if (dx < rx || dx >= rx + 808 || dy < ry || dy >= ry + 124)
					continue;
				int act = st.saveRow == row ? st.saveAct : 0;
				for (int k = 0; k < (row == 0 ? 1 : 2); k++)
				{
					float b[2];
					SaveButtonAt(row, k, b);
					if (dx >= b[0] && dx < b[0] + 130 && dy >= b[1] && dy < b[1] + 60)
						act = k;
				}
				if (st.saveRow != row || st.saveAct != act)
					FocusSave(st, row, act), st.dirty = true;
				float b[2];
				SaveButtonAt(row, act, b);
				if (button == 1 && dx >= b[0] && dx < b[0] + 130 && dy >= b[1] && dy < b[1] + 60)
					Act(st, Input::Confirm);
			}
			if (button == 2)
				Act(st, Input::Back);
		}
		else if (st.settings && !Categories().empty())
		{
			// Settings: a row under the mouse takes the focus; a click on its arrows steps its value (elsewhere on it:
			// as A); the tab bar's arrows change the category; a right click goes back
			const Category& cat = Categories()[std::clamp(st.cat, 0, (int)Categories().size() - 1)];
			for (int i = 0; i < (int)cat.rows.size(); i++)
			{
				const float rx = 110, ry = 238 + i * 74.0f;
				if (dx < rx || dx >= rx + 808 || dy < ry || dy >= ry + 70)
					continue;
				if (st.row != i)
					FocusRow(st, i);
				if (button == 1)
				{
					const float vr = rx + 808 - 20, bx = vr - 36 - 226 - 36;
					Act(st, dx >= bx && dx < bx + 36 ? Input::Left : dx >= vr - 36 && dx < vr ? Input::Right : Input::Confirm);
				}
			}
			if (button == 1 && dy >= 128 && dy < 188)
			{
				const float aw = A().Get("arrow_tab_left").w / s;
				if (dx >= 84 && dx < 84 + aw)
					Act(st, Input::CatPrev);
				else if (dx >= 1290 - aw && dx < 1290)
					Act(st, Input::CatNext);
			}
			if (button == 2)
				Act(st, Input::Back);
		}
		else
		{
			for (int i = 0; i < 5; i++)
			{
				float box[4];
				CursorFor(i, box);
				if (inside(box))
				{
					if (st.item != i)
						Focus(st, i);
					if (button == 1)
						Act(st, Input::Confirm);
				}
			}
			if (button == 2)
				Act(st, Input::Back);
		}
		st.wake.notify_one();
	}

	void SetGame(const std::string& path)
	{
		State& st = S();
		std::lock_guard lock(st.lock);
		GamePath() = path;
	}

	void BeginSetup()
	{
		if (!Available())
			return;
		State& st = S();
		std::lock_guard lock(st.lock);
		Start(st);
		const Clock::time_point now = Clock::now();
		st.setup = true, st.setupPre = true, st.startDone = true;   // after Setup's Start Game, no Launch at the title
		st.sstate = kIdle, st.phase = Phase::Opening, st.since = now, st.screenSince = now;
		FocusSetup(st, 0, true);
		cemuLog_log(LogType::Force, "wwhd: start screen: Setup (no game files given or registered)");
		st.wake.notify_one();
	}

	bool SetupDone(std::string& path)
	{
		State& st = S();
		std::lock_guard lock(st.lock);
		if (st.setupResult.empty())
			return false;
		path = st.setupResult;
		st.setupResult.clear(), st.setup = false, st.setupPre = false, st.blackoutAt = {};
		st.phase = Phase::Closed, st.closedAt = Clock::now();
		gpu::SetMenuPanel(0, 0, {});
		return true;
	}

	void Drag(bool over)
	{
		State& st = S();
		std::lock_guard lock(st.lock);
		if (!st.setup || st.sstate == kChecking || st.sstate == kReady || st.blackoutAt != Clock::time_point{})
			return;
		st.dragging = over, st.dragSeen = Clock::now(), st.dirty = true;
		st.wake.notify_one();
	}

	void Drop(const std::string& path)
	{
		State& st = S();
		std::lock_guard lock(st.lock);
		st.dragging = false;
		if (!st.setup || st.sstate == kChecking || st.sstate == kReady || st.blackoutAt != Clock::time_point{} || path.empty())
			return;                                     // dropped where Setup doesn't take files (README: idle, drag, bad)
		SetupChoose(st, path);
		st.wake.notify_one();
	}

	bool TakeBrowse()
	{
		State& st = S();
		std::lock_guard lock(st.lock);
		const bool want = st.browseWanted;
		st.browseWanted = false;
		return want;
	}

	void Chosen(const std::string& path, bool failed)
	{
		State& st = S();
		std::lock_guard lock(st.lock);
		if (!st.setup)
			return;
		if (failed)
			OpenBrowser(st);                            // no system picker: the menu's own
		else if (!path.empty())
			SetupChoose(st, path);
		st.dirty = true;
		st.wake.notify_one();
	}

	// what the game may see of the pad: nothing while the menu is up, nor a button held since it closed
	uint32 Swallowed()
	{
		State& st = S();
		std::lock_guard lock(st.lock);
		return st.phase != Phase::Closed ? ~0u : st.swallow;
	}
}
