// The pause menu's drawing (t-pausemenu): a software canvas in the window's pixels, RGBA8 (bytes R, G, B, A: the
// overlay panels' format), with straight-alpha images (the design's PNGs, assets/pause_menu), filled rectangles and
// text in the design's TrueType fonts (stb_truetype, as Cemu's imgui bundles it) at their size (em, as CSS) times the scale,
// never scaled as bitmaps. ui_draw.cpp.
#pragma once
#include "Common/precompiled.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wwhd::ui
{
	struct Colour
	{
		uint8 r = 0, g = 0, b = 0, a = 255;
		static constexpr Colour Hex(uint32 rgb, uint8 a = 255) { return { (uint8)(rgb >> 16), (uint8)(rgb >> 8), (uint8)rgb, a }; }
	};

	// an image, straight alpha, one uint32 a pixel in the canvas's byte order
	struct Image
	{
		int w = 0, h = 0;
		std::vector<uint32> px;
		bool Empty() const { return w <= 0 || h <= 0; }
	};
	bool LoadPng(const std::string& path, Image& out);      // 8-bit RGBA, not interlaced (the design's PNGs)
	Image Scaled(const Image& src, int w, int h);           // an area average down, bilinear up

	class Font
	{
	public:
		Font();
		~Font();
		bool Load(const std::string& path);
		bool Ok() const { return !m_data.empty(); }
		float Width(const std::string& utf8, float px);      // the advance of a line at that pixel size
		float Ascent(float px);
		float Descent(float px);                             // negative: below the baseline
		struct Glyph { int w = 0, h = 0, x = 0, y = 0; float advance = 0; std::vector<uint8> alpha; };
		const Glyph& Get(uint32 codepoint, float px);        // its bitmap at that size (cached), x/y from the pen and baseline
		float Kern(uint32 a, uint32 b, float px);

	private:
		std::vector<uint8> m_data;
		struct Info;
		std::unique_ptr<Info> m_info;
		std::unordered_map<uint64, Glyph> m_cache;
	};
	std::vector<uint32> Utf8(const std::string& s);

	class Canvas
	{
	public:
		Canvas(int w, int h) : m_w(w), m_h(h), m_px((size_t)w * h, 0), m_cx1(w), m_cy1(h) {}
		int W() const { return m_w; }
		int H() const { return m_h; }
		std::vector<uint32>& Pixels() { return m_px; }
		void Copy(const Image& full);                        // a whole-canvas picture (the backdrop)
		void Fill(float x, float y, float w, float h, Colour c, float opacity = 1.0f);
		// an image at (x, y) w x h (scaled when the size differs; cached by the caller), with an opacity, mirrored
		void Draw(const Image& img, float x, float y, float opacity = 1.0f, bool flipX = false, bool flipY = false);
		// the whole canvas tinted towards a colour by its alpha times the opacity (a scrim)
		void Tint(Colour c, float opacity);
		// an opaque image's columns x0..x1 copied in at (x, y) (a layer drawn earlier; the clip's left and right apply)
		void Rows(const Image& img, int x, int y, int x0, int x1);
		// a canvas's pixels as an image (a layer to draw again)
		Image Take() { Image i; i.w = m_w, i.h = m_h, i.px = std::move(m_px); return i; }
		// the image's shape in one colour (a drop shadow: offset it), its alpha times the colour's times the opacity
		void Silhouette(const Image& img, float x, float y, Colour c, float opacity = 1.0f, bool flipX = false, bool flipY = false);
		// the image turned by deg degrees (clockwise) about its centre, which lands at (cx, cy)
		void DrawRotated(const Image& img, float cx, float cy, float deg, float opacity = 1.0f);
		// the image stretched to w wide, its left and right `cap` pixels kept (a key prompt's rounded ends)
		void Draw3Slice(const Image& img, float x, float y, float w, int cap, float opacity = 1.0f);
		// an image repeated over a rectangle, its pattern starting at (ox, oy) from the rectangle's corner
		void Tile(const Image& img, float x, float y, float w, float h, float ox = 0, float oy = 0, bool flipX = false, bool flipY = false,
			float opacity = 1.0f);
		// text on a baseline; a 2-pixel (times the scale) outline in eight directions under it when outline.a > 0
		float Text(Font& font, float px, float x, float baseline, const std::string& utf8, Colour c, float opacity = 1.0f,
			Colour outline = { 0, 0, 0, 0 }, float outlineWidth = 0);
		void Clip(int x0, int y0, int x1, int y1) { m_cx0 = std::max(0, x0), m_cy0 = std::max(0, y0), m_cx1 = std::min(m_w, x1), m_cy1 = std::min(m_h, y1); }
		void NoClip() { m_cx0 = 0, m_cy0 = 0, m_cx1 = m_w, m_cy1 = m_h; }

	private:
		void Blend(int x, int y, Colour c, float a);
		int m_w, m_h;
		std::vector<uint32> m_px;
		int m_cx1, m_cy1, m_cx0 = 0, m_cy0 = 0;
	};

	// the backdrop: an RGBA picture blurred (a Gaussian of about sigma pixels, three box passes), its saturation and
	// brightness scaled (the design's blur(6px) saturate(.95) brightness(.92))
	Image Blurred(const Image& src, float sigma, float saturate, float brightness);
}
