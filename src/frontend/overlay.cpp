// The system's own screens in the window (docs/recompiler-design.md D18): the software keyboard, error
// dialogs and the "HOME Menu can't be used now" sign, which on the console the system draws over the
// game. Our swkbd and erreula (src/os) say what is up; this draws it with SDL's software renderer and
// its built-in 8x8 font (ASCII) into a panel the renderer lays over the TV image (never into
// captures). The event loop calls Update often; it redraws only when something changed.
#include "boot.h"
#include "../gpu/vk/renderer.h"
#include "../os/swkbd.h"
#include "../os/erreula.h"
#include "../os/debug_menu.h"
#include <SDL3/SDL.h>

namespace
{
	struct Colour { Uint8 r, g, b; };
	constexpr Colour kPanel{ 24, 28, 44 }, kFrame{ 110, 130, 190 }, kText{ 235, 238, 245 }, kHint{ 150, 160, 185 },
		kField{ 245, 245, 245 }, kFieldText{ 20, 20, 30 };

	class Canvas
	{
	public:
		Canvas(int w, int h) : m_w(w), m_h(h)
		{
			m_surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
			m_r = m_surface ? SDL_CreateSoftwareRenderer(m_surface) : nullptr;
			Fill(0, 0, w, h, kFrame);
			Fill(4, 4, w - 8, h - 8, kPanel);
		}
		~Canvas()
		{
			if (m_r) SDL_DestroyRenderer(m_r);
			if (m_surface) SDL_DestroySurface(m_surface);
		}
		bool ok() const { return m_r; }

		void Fill(int x, int y, int w, int h, Colour c)
		{
			SDL_SetRenderDrawColor(m_r, c.r, c.g, c.b, 255);
			SDL_FRect rect{ (float)x, (float)y, (float)w, (float)h };
			SDL_RenderFillRect(m_r, &rect);
		}
		// text at (x, y) with glyphs `scale` times 8 pixels
		void Text(int x, int y, int scale, const std::string& text, Colour c)
		{
			SDL_SetRenderScale(m_r, (float)scale, (float)scale);
			SDL_SetRenderDrawColor(m_r, c.r, c.g, c.b, 255);
			SDL_RenderDebugText(m_r, (float)x / scale, (float)y / scale, text.c_str());
			SDL_SetRenderScale(m_r, 1, 1);
		}
		std::vector<uint32> Pixels()
		{
			SDL_FlushRenderer(m_r);
			std::vector<uint32> out((size_t)m_w * m_h);
			for (int y = 0; y < m_h; y++)
				memcpy(out.data() + (size_t)y * m_w, (uint8*)m_surface->pixels + (size_t)y * m_surface->pitch, (size_t)m_w * 4);
			return out;
		}

	private:
		int m_w, m_h;
		SDL_Surface* m_surface = nullptr;
		SDL_Renderer* m_r = nullptr;
	};

	std::string Ascii(const std::u16string& s)
	{
		std::string out;
		for (char16_t c : s)
			out.push_back(c >= 0x20 && c < 0x7F ? (char)c : c == '\n' ? '\n' : '?');
		return out;
	}

	// words into lines of at most `width` characters (and at the text's own line breaks)
	std::vector<std::string> Wrap(const std::string& text, size_t width)
	{
		std::vector<std::string> lines;
		std::istringstream paragraphs(text);
		for (std::string paragraph; std::getline(paragraphs, paragraph);)
		{
			std::istringstream words(paragraph);
			std::string line;
			for (std::string word; words >> word;)
			{
				if (!line.empty() && line.size() + 1 + word.size() > width)
				{
					lines.push_back(line);
					line.clear();
				}
				line += (line.empty() ? "" : " ") + word.substr(0, width);
			}
			lines.push_back(line);
		}
		return lines;
	}

	void Show(sint32 x, sint32 y, Canvas& c, int w, int h)
	{
		if (c.ok())
			wwhd::gpu::SetOverlay(x, y, (uint32)w, (uint32)h, c.Pixels());
	}

	void DrawKeyboard(const wwhd::os::swkbd::View& v)
	{
		constexpr int w = 1200, h = 290;
		Canvas c(w, h);
		c.Text(40, 30, 3, "Enter a name", kText);
		c.Fill(40, 84, w - 80, 104, kField);
		std::string text = Ascii(v.text);
		if (text.size() < v.maxLength)
			text += "_";
		c.Text(64, 108, 7, text, kFieldText);
		c.Text(40, 214, 2, "Type the name.  Enter or Start: OK.  Backspace: delete.", kHint);
		c.Text(40, 244, 2, "OK with no name enters Link.", kHint);
		Show((1920 - w) / 2, 330, c, w, h);
	}

	void DrawError(const wwhd::os::erreula::View& v)
	{
		constexpr int w = 1200;
		std::vector<std::string> lines;
		if (v.errorCode)
			lines.push_back(fmt::format("Error Code: {:03}-{:04}", v.errorCode / 10000, v.errorCode % 10000));
		for (const std::string& l : Wrap(Ascii(v.text), 46))
			lines.push_back(l);
		int h = 60 + (int)lines.size() * 36 + 90;
		Canvas c(w, h);
		int y = 40;
		for (const std::string& l : lines)
		{
			c.Text(40, y, 3, l, kText);
			y += 36;
		}
		std::string buttons = "[A] " + Ascii(v.left);
		if (!v.right.empty())
			buttons += "        [B] " + Ascii(v.right);
		c.Text(40, h - 70, 3, buttons, kHint);
		Show((1920 - w) / 2, (1080 - h) / 2, c, w, h);
	}

	void DrawPreparing(uint32 done, uint32 total)
	{
		constexpr int w = 1100, h = 200;
		Canvas c(w, h);
		c.Text(40, 34, 3, "Preparing shaders", kText);
		c.Fill(40, 88, w - 80, 36, kFrame);
		c.Fill(44, 92, (int)((w - 88) * (total ? (double)done / total : 1.0)), 28, kField);
		c.Text(40, 146, 2, fmt::format("{} of {}. Once per graphics driver: later starts are quick.", done, total), kHint);
		Show((1920 - w) / 2, (1080 - h) / 2, c, w, h);
	}

	void DrawDebugMenu(const wwhd::os::debug_menu::View& v)
	{
		constexpr int w = 900;
		const int h = 130 + (int)v.items.size() * 40 + 60;
		Canvas c(w, h);
		c.Text(40, 34, 3, v.title, kText);
		int y = 100;
		for (size_t i = 0; i < v.items.size(); i++)
		{
			if ((int)i == v.cursor)
			{
				c.Fill(30, y - 8, w - 60, 38, kFrame);
				c.Text(48, y, 3, "> " + v.items[i], kText);
			}
			else
				c.Text(48, y, 3, "  " + v.items[i], kHint);
			y += 40;
		}
		c.Text(40, h - 50, 2, v.hint, kHint);
		Show((1920 - w) / 2, (1080 - h) / 2, c, w, h);
	}

	void DrawHomeSign()
	{
		constexpr int w = 620, h = 60;
		Canvas c(w, h);
		c.Text(24, 22, 2, "The HOME Menu can't be used now.", kText);
		Show(1920 - w - 40, 40, c, w, h);
	}
}

namespace wwhd
{
	void ShowPreparing(uint32 done, uint32 total)
	{
		DrawPreparing(done, total);
	}

	void UpdateOverlay()
	{
		static uint32 s_keyboard = ~0u, s_error = ~0u, s_menu = ~0u;
		static bool s_home = false, s_shown = false;
		os::swkbd::View kb = os::swkbd::Current();
		os::erreula::View err = os::erreula::Current();
		os::debug_menu::View menu = os::debug_menu::Current();
		if (kb.version == s_keyboard && err.version == s_error && err.homeNixSign == s_home && menu.version == s_menu)
			return;
		s_keyboard = kb.version;
		s_error = err.version;
		s_home = err.homeNixSign;
		s_menu = menu.version;
		if (err.dialog)
			DrawError(err);
		else if (kb.open)
			DrawKeyboard(kb);
		else if (menu.open)
			DrawDebugMenu(menu);
		else if (err.homeNixSign)
			DrawHomeSign();
		else if (s_shown)
			gpu::SetOverlay(0, 0, 0, 0, {});
		s_shown = err.dialog || kb.open || menu.open || err.homeNixSign;
	}
}
