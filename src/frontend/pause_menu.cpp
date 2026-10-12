// The pause menu (pause_menu.h). Laid out in the design's 1920x1080 space, drawn at s = min(W/1920, H/1080) of the
// window (the README): the side panel at the left edge, the prompts and toasts at the right, dialogs centred; the
// design's PNGs at 1x (s <= 1) or 2x, scaled to s; text from the TTFs at its size times s. M1 (this): the menu, its
// cursor, prompts, captions and motion, Resume, and Quit to the desktop; Enhancements, Settings and Save come with
// M2-M4 (a toast says so meanwhile).
#include "pause_menu.h"
#include "ui_draw.h"
#include "../gpu/vk/renderer.h"
#include "../os/savestate.h"
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <thread>

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
		};
		Layers& L()
		{
			static Layers l;
			return l;
		}

		enum class Phase { Closed, Waiting, Opening, Open, Closing };
		enum class Dialog { None, Quit, QuitDesktop };

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
			Image backdrop, blurred;                    // the window's picture when it opened, and blurred
			int bw = 0, bh = 0;
			bool dirty = true;
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

		// the prompts at the top right (right 44, top 34): each a glyph with its label under it, overlapping it by 12, centred
		// on each other, 26 apart
		void DrawPrompts(Frame& f, const std::vector<std::pair<std::string, std::string>>& prompts, bool pad, float op)
		{
			Assets& a = A();
			const float s = f.s, top = 34 * s;
			float right = f.c.W() - 44 * s;
			for (auto it = prompts.rbegin(); it != prompts.rend(); ++it)
			{
				const std::string& glyphText = it->first;
				const std::string& label = it->second;
				const float lw = a.rocknroll.Width(label, 24 * s);
				const Image& g = a.Get(pad ? "prompt_pad-button_blank" : "prompt_key_blank");
				const float gw = pad ? (float)g.w : std::max((float)g.w, a.rocknroll.Width(glyphText, 17 * s) + 20 * s);
				const float colW = std::max(gw, lw), cx = right - colW / 2;
				const float gx = cx - gw / 2;
				if (pad)
					f.c.Draw(g, gx, top, op);
				else
					f.c.Draw3Slice(g, gx, top, gw, (int)(12 * s), op);
				TextCentred(f, a.rocknroll, pad ? 21.0f : 17.0f, cx / s, (top + g.h / 2.0f) / s, glyphText, kPromptText, op);
				// the label under it, overlapping it by 12: white, a 2 px outline, its shadow (0 3px 4px rgba(0,30,90,.45))
				const float base = top + g.h - 12 * s + a.rocknroll.Ascent(24 * s) * 0.92f;
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
		DialogSpec Spec(Dialog d)
		{
			const std::string ago = Ago(File1());
			const std::string since = ago.empty() ? "isn't saved yet." : "(" + ago + ") isn't saved yet.";
			switch (d)
			{
			case Dialog::Quit:
				return { "Quit to\xE2\x80\xA6", "Your progress since the last save " + since, { "Title Screen", "Desktop", "Cancel" }, { true, true, true } };
			case Dialog::QuitDesktop:
				return { "Quit to desktop?", "Progress since your last save" + (ago.empty() ? std::string() : " (" + ago + ")") + " will be lost.",
					{ "Save & Quit", "Don't Save", "Cancel" }, { false, true, true } };
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
			const DialogSpec d = Spec(st.dialog);
			Layers& l = L();
			if (l.dialogKind != (int)st.dialog)
			{
				l.dialog = DialogLayer(s, d), l.dialogKind = (int)st.dialog, l.under = {};
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
			if (st.dialog == Dialog::None || l.dialogKind != (int)st.dialog || l.under.w != c.W() || l.under.h != c.H())
				return false;
			c.Copy(l.under);
			return true;
		}

		void BuildLayers(Layers& l, int w, int h, float s)
		{
			Assets& a = A();
			l.w = w, l.h = h, l.dialogKind = -1;
			const int pw = (int)std::lround(kPanelW * s);
			{
				ui::Canvas c(pw, h);
				c.Fill(0, 0, (float)pw, (float)h, kFrame);
				c.Tile(a.Get("texture_frame-felt_tile"), 0, 0, (float)pw, (float)h);
				c.Fill(pw - 9 * s, 0, 3 * s, (float)h, kRimDark);
				c.Fill(pw - 6 * s, 0, 6 * s, (float)h, kRimLight);
				const Image& fl = a.Get("frame_corner-flourish");
				c.Draw(fl, 14 * s, 14 * s), c.Draw(fl, pw - 22 * s - fl.w, 14 * s, 1, true);
				c.Draw(fl, 14 * s, h - 14 * s - fl.h, 1, false, true), c.Draw(fl, pw - 22 * s - fl.w, h - 14 * s - fl.h, 1, true, true);
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
			if (!closing && UnderDialog(st, c))
			{
				DrawDialog(f, st, now, ms);
				DrawToast(f, st, now);
				return;
			}
			// the backdrop: sharp to blurred over 500 ms (back when closing)
			const float blur = closing ? 1 - Linear(ms, 0, 500) : Linear(ms, 0, 500);
			if (blur >= 1)
				c.Copy(st.blurred);
			else
			{
				c.Copy(st.backdrop);
				if (blur > 0)
					c.Draw(st.blurred, 0, 0, blur);
			}
			// the panel wipes in from the left over 560 ms (out, reversed, without delays)
			const float wipe = closing ? 1 - Bezier(.3f, .8f, .25f, 1, Linear(ms, 0, 560)) : Bezier(.3f, .8f, .25f, 1, Linear(ms, 0, 560));
			const float pw = (float)l.panel.w;
			// its shadow (drop-shadow(10px 0 22px rgba(16,56,24,.4))) beyond its right edge
			for (int i = 0; i < (int)(32 * s); i++)
			{
				const float k = 1 - i / (32 * s);
				c.Fill(pw * wipe + i, 0, 1, (float)c.H(), { 16, 56, 24, 255 }, 0.4f * k * k * (wipe > 0.99f ? 1 : wipe));
			}
			const int shown = (int)std::ceil(pw * wipe);
			c.Clip(0, 0, shown, c.H());
			c.Rows(l.panel, 0, 0, 0, shown);
			// the header: drops from -24, after 140 ms
			const float hp = closing ? 1 - Linear(ms, 0, 260) : Linear(ms, 140, 260);
			const float hy = closing ? 0 : -24 * (1 - Bezier(.3f, 1.5f, .5f, 1, Linear(ms, 140, 460)));
			if (hp > 0)
			{
				c.Draw(l.header, 60 * s, (92 + hy) * s, hp);
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
					TextCentred(f, a.rocknroll, 26, 340, top + (i + 0.5f) * 26 * 1.4f, lines[i], kInk, hp);
			}
			// the buttons: pop up from +20, each 45 ms after the one before, from 220 ms; the focus cross-fades over 140
			for (int i = 0; i < 5; i++)
			{
				const float d = 220 + 45.0f * i;
				const float bp = closing ? 1 - Linear(ms, 0, 260) : Linear(ms, d, 260);
				if (bp <= 0)
					continue;
				const float by = closing ? 0 : 20 * (1 - Bezier(.3f, 1.5f, .5f, 1, Linear(ms, d, 460)));
				const float fms = st.focusSince.size() > (size_t)i ? std::chrono::duration<float, std::milli>(now - st.focusSince[i]).count() : 1e9f;
				float focus = Linear(fms, 0, 140);
				if (!(st.focused.size() > (size_t)i && st.focused[i]))
					focus = 1 - focus;
				const float bx = kButtonX * s, byy = (kButtonY + kButtonTops[i] + by) * s;
				c.Draw(l.button[i][0], bx, byy, bp);
				if (focus > 0)
					c.Draw(l.button[i][1], bx, byy, bp * focus);
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
			if (cp > 0 && st.dialog == Dialog::None)
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
			if (pp > 0)
				DrawPrompts(f, { { st.pad ? "A" : "Enter", "Select" }, { st.pad ? "B" : "Esc", "Back" } }, st.pad, pp);
			DrawDialog(f, st, now, ms);
			DrawToast(f, st, now);
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

		void QuitToDesktop()
		{
			cemuLog_log(LogType::Force, "wwhd: quit to the desktop from the pause menu");
			gpu::SaveShaderCache();
			_exit(0);
		}

		enum class Input { Up, Down, Left, Right, Confirm, Back, Toggle };

		void Act(State& st, Input in)
		{
			if (in == Input::Toggle)
			{
				if (st.phase == Phase::Closed)
				{
					if (Clock::now() - st.closedAt < std::chrono::milliseconds(400))
						return;
					st.phase = Phase::Waiting, st.since = Clock::now(), st.dialog = Dialog::None;
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
			if (st.dialog != Dialog::None)
			{
				const DialogSpec d = Spec(st.dialog);
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
						st.dialog = Dialog::QuitDesktop, st.dialogItem = 0, st.dialogSince = Clock::now();
					else if (b == "Don't Save")
						QuitToDesktop();
					else if (b == "Title Screen")
						ShowToast(st, "Returning to the title screen comes in a later update.");
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
				else
					ShowToast(st, std::string(kItems[st.item]) + " comes in a later update.");
				break;
			default: break;
			}
			st.dirty = true;
		}

		// the menu's thread: the backdrop once the TV picture comes, then a frame each 16 ms while it moves
		void Run()
		{
			State& st = S();
			std::unique_lock lock(st.lock);
			for (;;)
			{
				st.wake.wait_for(lock, std::chrono::milliseconds(st.phase == Phase::Closed ? 1000 : 16));
				const Clock::time_point now = Clock::now();
				uint32 ww = 0, wh = 0;
				if (st.phase == Phase::Closed || !gpu::WindowExtent(ww, wh) || !ww || !wh)
					continue;
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
					// blurred at half size (blur(6px) times the scale), then back up
					const float s = std::min(ww / 1920.0f, wh / 1080.0f);
					Image half = ui::Scaled(screen, std::max(1, (int)ww / 2), std::max(1, (int)wh / 2));
					st.blurred = ui::Scaled(ui::Blurred(half, 3 * s, 0.95f, 0.92f), (int)ww, (int)wh);
					st.backdrop = std::move(screen);
					st.bw = (int)ww, st.bh = (int)wh;
					st.phase = Phase::Opening, st.since = Clock::now();
				}
				if ((int)ww != st.bw || (int)wh != st.bh)       // resized while up: the backdrop to the new size
				{
					st.backdrop = ui::Scaled(st.backdrop, (int)ww, (int)wh), st.blurred = ui::Scaled(st.blurred, (int)ww, (int)wh);
					st.bw = (int)ww, st.bh = (int)wh;
				}
				const float ms = std::chrono::duration<float, std::milli>(Clock::now() - st.since).count();
				if (st.phase == Phase::Opening && ms > 1000)
					st.phase = Phase::Open;
				if (st.phase == Phase::Closing && ms > 560)
				{
					st.phase = Phase::Closed, st.closedAt = Clock::now();
					gpu::SetMenuPanel(0, 0, {});
					wwhd::state::RequestHold(false);
					continue;
				}
				const Clock::time_point t0 = Clock::now();
				ui::Canvas c((int)ww, (int)wh);
				Draw(st, c, t0);
				gpu::SetMenuPanel(ww, wh, std::move(c.Pixels()));
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
			return true;
		}();
		return ok;
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
			return false;
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
		if (st.dialog != Dialog::None)
		{
			const DialogSpec d = Spec(st.dialog);
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

	// what the game may see of the pad: nothing while the menu is up, nor a button held since it closed
	uint32 Swallowed()
	{
		State& st = S();
		std::lock_guard lock(st.lock);
		return st.phase != Phase::Closed ? ~0u : st.swallow;
	}
}
