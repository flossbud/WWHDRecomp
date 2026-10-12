// The pause menu's drawing: ui_draw.h.
#include "ui_draw.h"
#include <cmath>
#include <cstdio>
#include <zlib.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

namespace wwhd::ui
{
	namespace
	{
		uint32 Pack(int r, int g, int b, int a) { return (uint32)r | (uint32)g << 8 | (uint32)b << 16 | (uint32)a << 24; }
		int R(uint32 p) { return p & 0xFF; }
		int G(uint32 p) { return p >> 8 & 0xFF; }
		int B(uint32 p) { return p >> 16 & 0xFF; }
		int A(uint32 p) { return p >> 24; }
		uint32 Be32(const uint8* p) { return (uint32)p[0] << 24 | (uint32)p[1] << 16 | (uint32)p[2] << 8 | p[3]; }
	}

	bool LoadPng(const std::string& path, Image& out)
	{
		FILE* f = fopen(path.c_str(), "rb");
		if (!f)
			return false;
		std::vector<uint8> file;
		uint8 buf[65536];
		for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;)
			file.insert(file.end(), buf, buf + n);
		fclose(f);
		static const uint8 kSig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		if (file.size() < 8 || memcmp(file.data(), kSig, 8) != 0)
			return false;
		uint32 w = 0, h = 0;
		std::vector<uint8> idat;
		for (size_t at = 8; at + 12 <= file.size();)
		{
			const uint32 len = Be32(&file[at]);
			const char* type = (const char*)&file[at + 4];
			if (at + 12 + len > file.size())
				return false;
			const uint8* data = &file[at + 8];
			if (memcmp(type, "IHDR", 4) == 0)
			{
				w = Be32(data), h = Be32(data + 4);
				if (data[8] != 8 || data[9] != 6 || data[12] != 0)
					return false;                       // 8-bit RGBA, not interlaced only
			}
			else if (memcmp(type, "IDAT", 4) == 0)
				idat.insert(idat.end(), data, data + len);
			else if (memcmp(type, "IEND", 4) == 0)
				break;
			at += 12 + len;
		}
		if (!w || !h || w > 16384 || h > 16384)
			return false;
		const size_t stride = (size_t)w * 4;
		std::vector<uint8> raw((stride + 1) * h);
		uLongf rawLen = (uLongf)raw.size();
		if (uncompress(raw.data(), &rawLen, idat.data(), (uLong)idat.size()) != Z_OK || rawLen != raw.size())
			return false;
		std::vector<uint8> pixels(stride * h);
		for (uint32 y = 0; y < h; y++)
		{
			const uint8 filter = raw[y * (stride + 1)];
			const uint8* src = &raw[y * (stride + 1) + 1];
			uint8* row = &pixels[y * stride];
			const uint8* up = y ? &pixels[(y - 1) * stride] : nullptr;
			for (size_t i = 0; i < stride; i++)
			{
				const int a = i >= 4 ? row[i - 4] : 0, b = up ? up[i] : 0, c = up && i >= 4 ? up[i - 4] : 0;
				int v = src[i];
				switch (filter)
				{
				case 1: v += a; break;
				case 2: v += b; break;
				case 3: v += (a + b) / 2; break;
				case 4:
				{
					const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
					v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
					break;
				}
				default: break;
				}
				row[i] = (uint8)v;
			}
		}
		out.w = (int)w, out.h = (int)h;
		out.px.resize((size_t)w * h);
		memcpy(out.px.data(), pixels.data(), pixels.size());   // bytes R, G, B, A: the canvas's order
		return true;
	}

	// premultiplied while averaging, so transparent pixels' colours don't bleed into the edges
	Image Scaled(const Image& src, int w, int h)
	{
		Image out;
		if (src.Empty() || w <= 0 || h <= 0)
			return out;
		if (w == src.w && h == src.h)
			return src;
		out.w = w, out.h = h;
		out.px.resize((size_t)w * h);
		const double sx = (double)src.w / w, sy = (double)src.h / h;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
			{
				double r = 0, g = 0, b = 0, a = 0, wsum = 0;
				if (sx > 1 || sy > 1)
				{
					const double x0 = x * sx, x1 = x0 + sx, y0 = y * sy, y1 = y0 + sy;
					for (int yy = (int)y0; yy < std::min((int)std::ceil(y1), src.h); yy++)
					{
						const double wy = std::min(y1, yy + 1.0) - std::max(y0, (double)yy);
						for (int xx = (int)x0; xx < std::min((int)std::ceil(x1), src.w); xx++)
						{
							const double wx = std::min(x1, xx + 1.0) - std::max(x0, (double)xx), k = wx * wy;
							const uint32 p = src.px[(size_t)yy * src.w + xx];
							const double pa = A(p) / 255.0;
							r += R(p) * pa * k, g += G(p) * pa * k, b += B(p) * pa * k, a += pa * k, wsum += k;
						}
					}
				}
				else
				{
					const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, src.w - 1.0), fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, src.h - 1.0);
					const int ix = (int)fx, iy = (int)fy, jx = std::min(ix + 1, src.w - 1), jy = std::min(iy + 1, src.h - 1);
					const double tx = fx - ix, ty = fy - iy;
					const auto take = [&](int px, int py, double k) {
						const uint32 p = src.px[(size_t)py * src.w + px];
						const double pa = A(p) / 255.0;
						r += R(p) * pa * k, g += G(p) * pa * k, b += B(p) * pa * k, a += pa * k, wsum += k;
					};
					take(ix, iy, (1 - tx) * (1 - ty)), take(jx, iy, tx * (1 - ty)), take(ix, jy, (1 - tx) * ty), take(jx, jy, tx * ty);
				}
				if (a <= 0 || wsum <= 0)
				{
					out.px[(size_t)y * w + x] = 0;
					continue;
				}
				out.px[(size_t)y * w + x] = Pack((int)std::lround(r / a), (int)std::lround(g / a), (int)std::lround(b / a),
					(int)std::lround(std::min(1.0, a / wsum) * 255));
			}
		return out;
	}

	std::vector<uint32> Utf8(const std::string& s)
	{
		std::vector<uint32> out;
		for (size_t i = 0; i < s.size();)
		{
			const uint8 c = (uint8)s[i];
			uint32 cp = c;
			int n = 0;
			if (c >= 0xF0) cp = c & 7, n = 3;
			else if (c >= 0xE0) cp = c & 15, n = 2;
			else if (c >= 0xC0) cp = c & 31, n = 1;
			i++;
			for (; n > 0 && i < s.size(); n--, i++)
				cp = cp << 6 | ((uint8)s[i] & 63);
			out.push_back(cp);
		}
		return out;
	}

	struct Font::Info { stbtt_fontinfo info; };
	Font::Font() = default;
	Font::~Font() = default;

	bool Font::Load(const std::string& path)
	{
		FILE* f = fopen(path.c_str(), "rb");
		if (!f)
			return false;
		std::vector<uint8> data;
		uint8 buf[65536];
		for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;)
			data.insert(data.end(), buf, buf + n);
		fclose(f);
		auto info = std::make_unique<Info>();
		if (!stbtt_InitFont(&info->info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0)))
			return false;
		m_data = std::move(data);
		info->info.data = m_data.data();                 // it keeps a pointer to the bytes: the moved vector's
		m_info = std::move(info);
		return true;
	}

	float Font::Descent(float px)
	{
		int asc = 0, desc = 0, gap = 0;
		stbtt_GetFontVMetrics(&m_info->info, &asc, &desc, &gap);
		return desc * stbtt_ScaleForMappingEmToPixels(&m_info->info, px);
	}

	float Font::Ascent(float px)
	{
		int asc = 0, desc = 0, gap = 0;
		stbtt_GetFontVMetrics(&m_info->info, &asc, &desc, &gap);
		return asc * stbtt_ScaleForMappingEmToPixels(&m_info->info, px);
	}

	const Font::Glyph& Font::Get(uint32 cp, float px)
	{
		const uint64 key = (uint64)cp << 32 | (uint32)std::lround(px * 16);
		auto it = m_cache.find(key);
		if (it != m_cache.end())
			return it->second;
		Glyph g;
		const float scale = stbtt_ScaleForMappingEmToPixels(&m_info->info, px);
		int adv = 0, lsb = 0;
		stbtt_GetCodepointHMetrics(&m_info->info, (int)cp, &adv, &lsb);
		g.advance = adv * scale;
		int x0, y0, x1, y1;
		stbtt_GetCodepointBitmapBox(&m_info->info, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
		g.w = x1 - x0, g.h = y1 - y0, g.x = x0, g.y = y0;
		if (g.w > 0 && g.h > 0)
		{
			g.alpha.resize((size_t)g.w * g.h);
			stbtt_MakeCodepointBitmap(&m_info->info, g.alpha.data(), g.w, g.h, g.w, scale, scale, (int)cp);
		}
		return m_cache.emplace(key, std::move(g)).first->second;
	}

	float Font::Kern(uint32 a, uint32 b, float px)
	{
		return stbtt_GetCodepointKernAdvance(&m_info->info, (int)a, (int)b) * stbtt_ScaleForMappingEmToPixels(&m_info->info, px);
	}

	float Font::Width(const std::string& utf8, float px)
	{
		if (!Ok())
			return 0;
		float w = 0;
		const std::vector<uint32> cps = Utf8(utf8);
		for (size_t i = 0; i < cps.size(); i++)
			w += Get(cps[i], px).advance + (i + 1 < cps.size() ? Kern(cps[i], cps[i + 1], px) : 0);
		return w;
	}

	void Canvas::Blend(int x, int y, Colour c, float a)
	{
		if (x < m_cx0 || y < m_cy0 || x >= std::min(m_cx1, m_w) || y >= std::min(m_cy1, m_h) || a <= 0)
			return;
		uint32& d = m_px[(size_t)y * m_w + x];
		a = std::min(a, 1.0f);
		if (A(d) == 255)                                  // over the opaque backdrop (nearly always): integer
		{
			const int k = (int)(a * 256 + 0.5f), j = 256 - k;
			d = Pack((c.r * k + R(d) * j) >> 8, (c.g * k + G(d) * j) >> 8, (c.b * k + B(d) * j) >> 8, 255);
			return;
		}
		const float da = A(d) / 255.0f, oa = a + da * (1 - a);
		if (oa <= 0)
			return;
		const auto mix = [&](int s, int dc) { return (int)std::lround((s * a + dc * da * (1 - a)) / oa); };
		d = Pack(mix(c.r, R(d)), mix(c.g, G(d)), mix(c.b, B(d)), (int)std::lround(oa * 255));
	}

	void Canvas::Copy(const Image& full)
	{
		if (full.w == m_w && full.h == m_h)
			m_px = full.px;
	}

	void Canvas::Fill(float x, float y, float w, float h, Colour c, float opacity)
	{
		const int x0 = (int)std::lround(x), y0 = (int)std::lround(y), x1 = (int)std::lround(x + w), y1 = (int)std::lround(y + h);
		const float a = c.a / 255.0f * opacity;
		for (int yy = std::max(y0, 0); yy < std::min(y1, m_h); yy++)
			for (int xx = std::max(x0, 0); xx < std::min(x1, m_w); xx++)
				Blend(xx, yy, c, a);
	}

	void Canvas::Draw(const Image& img, float x, float y, float opacity, bool flipX, bool flipY)
	{
		const int x0 = (int)std::lround(x), y0 = (int)std::lround(y);
		const int k = (int)(std::clamp(opacity, 0.0f, 1.0f) * 256 + 0.5f);
		const int xa = std::max({ x0, m_cx0, 0 }), xb = std::min({ x0 + img.w, m_cx1, m_w });
		for (int j = 0; j < img.h; j++)
		{
			const int yy = y0 + j;
			if (yy < std::max(m_cy0, 0) || yy >= std::min(m_cy1, m_h))
				continue;
			const uint32* src = &img.px[(size_t)(flipY ? img.h - 1 - j : j) * img.w];
			uint32* dst = &m_px[(size_t)yy * m_w];
			for (int xx = xa; xx < xb; xx++)
			{
				const uint32 p = src[flipX ? img.w - 1 - (xx - x0) : xx - x0];
				const int a = A(p) * k >> 8;
				if (!a)
					continue;
				uint32& d = dst[xx];
				if (A(d) != 255)
				{
					Blend(xx, yy, { (uint8)R(p), (uint8)G(p), (uint8)B(p) }, a / 255.0f);
					continue;
				}
				if (a >= 255)
				{
					d = p | 0xFF000000u;
					continue;
				}
				// over the opaque picture (nearly always): integer, a out of 255
				const int ia = 255 - a;
				d = Pack((R(p) * a + R(d) * ia) / 255, (G(p) * a + G(d) * ia) / 255, (B(p) * a + B(d) * ia) / 255, 255);
			}
		}
	}

	void Canvas::Tint(Colour c, float opacity)
	{
		const int a = (int)(c.a * std::clamp(opacity, 0.0f, 1.0f) + 0.5f), ia = 255 - a;
		if (!a)
			return;
		const int cr = c.r * a, cg = c.g * a, cb = c.b * a;
		for (uint32& d : m_px)
			d = Pack((cr + R(d) * ia) / 255, (cg + G(d) * ia) / 255, (cb + B(d) * ia) / 255, A(d));
	}

	// rows of an opaque picture straight in (a layer drawn earlier): x0..x1 of its columns, at (x, y)
	void Canvas::Rows(const Image& img, int x, int y, int x0, int x1)
	{
		x0 = std::max({ x0, 0, m_cx0 - x }), x1 = std::min({ x1, img.w, m_w - x, m_cx1 - x });
		if (x1 <= x0)
			return;
		for (int j = 0; j < img.h; j++)
		{
			const int yy = y + j;
			if (yy < 0 || yy >= m_h)
				continue;
			memcpy(&m_px[(size_t)yy * m_w + x + x0], &img.px[(size_t)j * img.w + x0], (size_t)(x1 - x0) * 4);
		}
	}

	void Canvas::Silhouette(const Image& img, float x, float y, Colour c, float opacity, bool flipX, bool flipY)
	{
		const int x0 = (int)std::lround(x), y0 = (int)std::lround(y);
		const float k = c.a / 255.0f * opacity / 255.0f;
		for (int j = 0; j < img.h; j++)
			for (int i = 0; i < img.w; i++)
				if (const int a = A(img.px[(size_t)(flipY ? img.h - 1 - j : j) * img.w + (flipX ? img.w - 1 - i : i)]))
					Blend(x0 + i, y0 + j, c, a * k);
	}

	void Canvas::DrawRotated(const Image& img, float cx, float cy, float deg, float opacity)
	{
		const float r = deg * 3.14159265f / 180, cs = std::cos(r), sn = std::sin(r);
		const float half = 0.5f * std::sqrt((float)img.w * img.w + (float)img.h * img.h);
		for (int y = (int)(cy - half); y <= (int)(cy + half); y++)
			for (int x = (int)(cx - half); x <= (int)(cx + half); x++)
			{
				// the destination pixel's centre back into the image (the inverse turn), sampled bilinearly
				const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
				const float sx = cs * dx + sn * dy + img.w / 2.0f - 0.5f, sy = -sn * dx + cs * dy + img.h / 2.0f - 0.5f;
				if (sx < -1 || sy < -1 || sx > img.w || sy > img.h)
					continue;
				const int ix = (int)std::floor(sx), iy = (int)std::floor(sy);
				const float tx = sx - ix, ty = sy - iy;
				float rr = 0, gg = 0, bb = 0, aa = 0;
				for (int k = 0; k < 4; k++)
				{
					const int px = ix + (k & 1), py = iy + (k >> 1);
					if (px < 0 || py < 0 || px >= img.w || py >= img.h)
						continue;
					const float wgt = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
					const uint32 p = img.px[(size_t)py * img.w + px];
					const float pa = A(p) / 255.0f * wgt;
					rr += R(p) * pa, gg += G(p) * pa, bb += B(p) * pa, aa += pa;
				}
				if (aa > 0.002f)
					Blend(x, y, { (uint8)std::lround(rr / aa), (uint8)std::lround(gg / aa), (uint8)std::lround(bb / aa) }, aa * opacity);
			}
	}

	void Canvas::Draw3Slice(const Image& img, float x, float y, float w, int cap, float opacity)
	{
		const int iw = (int)std::lround(w), x0 = (int)std::lround(x), y0 = (int)std::lround(y);
		cap = std::min(cap, img.w / 2);
		for (int i = 0; i < iw; i++)
		{
			// the ends as they are, the middle column stretched over the rest
			const int si = i < cap ? i : i >= iw - cap ? img.w - (iw - i) : cap + (int)((float)(i - cap) / std::max(1, iw - 2 * cap) * (img.w - 2 * cap));
			for (int j = 0; j < img.h; j++)
			{
				const uint32 p = img.px[(size_t)j * img.w + std::clamp(si, 0, img.w - 1)];
				if (A(p))
					Blend(x0 + i, y0 + j, { (uint8)R(p), (uint8)G(p), (uint8)B(p) }, A(p) / 255.0f * opacity);
			}
		}
	}

	void Canvas::Tile(const Image& img, float x, float y, float w, float h, float ox, float oy, bool flipX, bool flipY, float opacity)
	{
		if (img.Empty())
			return;
		const int x0 = (int)std::lround(x), y0 = (int)std::lround(y), x1 = (int)std::lround(x + w), y1 = (int)std::lround(y + h);
		const int iox = (int)std::lround(ox), ioy = (int)std::lround(oy);
		for (int yy = std::max(y0, 0); yy < std::min(y1, m_h); yy++)
		{
			int sj = ((yy - y0 - ioy) % img.h + img.h) % img.h;
			if (flipY)
				sj = img.h - 1 - sj;
			for (int xx = std::max(x0, 0); xx < std::min(x1, m_w); xx++)
			{
				int si = ((xx - x0 - iox) % img.w + img.w) % img.w;
				if (flipX)
					si = img.w - 1 - si;
				const uint32 p = img.px[(size_t)sj * img.w + si];
				if (A(p))
					Blend(xx, yy, { (uint8)R(p), (uint8)G(p), (uint8)B(p) }, A(p) / 255.0f * opacity);
			}
		}
	}

	float Canvas::Text(Font& font, float px, float x, float baseline, const std::string& utf8, Colour c, float opacity, Colour outline,
		float outlineWidth)
	{
		if (!font.Ok())
			return 0;
		const std::vector<uint32> cps = Utf8(utf8);
		const auto pass = [&](float dx, float dy, Colour col) {
			float pen = x;
			const float a = col.a / 255.0f * opacity;
			for (size_t i = 0; i < cps.size(); i++)
			{
				const Font::Glyph& g = font.Get(cps[i], px);
				const int gx = (int)std::lround(pen + dx) + g.x, gy = (int)std::lround(baseline + dy) + g.y;
				for (int j = 0; j < g.h; j++)
					for (int k = 0; k < g.w; k++)
						if (const uint8 v = g.alpha[(size_t)j * g.w + k])
							Blend(gx + k, gy + j, col, v / 255.0f * a);
				pen += g.advance + (i + 1 < cps.size() ? font.Kern(cps[i], cps[i + 1], px) : 0);
			}
			return pen - x;
		};
		if (outline.a && outlineWidth > 0)
			for (int d = 0; d < 8; d++)
			{
				const float ang = d * 3.14159265f / 4;
				pass(std::cos(ang) * outlineWidth, std::sin(ang) * outlineWidth, outline);
			}
		return pass(0, 0, c);
	}

	Image Blurred(const Image& src, float sigma, float saturate, float brightness)
	{
		Image out = src;
		if (src.Empty())
			return out;
		// three box passes of radius r approximate a Gaussian of sigma ~ r * 1.0 (each pass's variance (2r+1)^2-1)/12)
		const int r = std::max(1, (int)std::lround(std::sqrt(12.0 * sigma * sigma / 3 + 1) / 2));
		std::vector<float> ch[3];
		for (int k = 0; k < 3; k++)
			ch[k].resize(out.px.size());
		for (size_t i = 0; i < out.px.size(); i++)
			ch[0][i] = (float)R(out.px[i]), ch[1][i] = (float)G(out.px[i]), ch[2][i] = (float)B(out.px[i]);
		std::vector<float> tmp(out.px.size());
		const int w = out.w, h = out.h;
		for (auto& c : ch)
			for (int pass = 0; pass < 3; pass++)
			{
				for (int y = 0; y < h; y++)                 // across
				{
					float sum = 0;
					for (int i = -r; i <= r; i++)
						sum += c[(size_t)y * w + std::clamp(i, 0, w - 1)];
					for (int x = 0; x < w; x++)
					{
						tmp[(size_t)y * w + x] = sum / (2 * r + 1);
						sum += c[(size_t)y * w + std::min(x + r + 1, w - 1)] - c[(size_t)y * w + std::max(x - r, 0)];
					}
				}
				for (int x = 0; x < w; x++)                 // down
				{
					float sum = 0;
					for (int i = -r; i <= r; i++)
						sum += tmp[(size_t)std::clamp(i, 0, h - 1) * w + x];
					for (int y = 0; y < h; y++)
					{
						c[(size_t)y * w + x] = sum / (2 * r + 1);
						sum += tmp[(size_t)std::min(y + r + 1, h - 1) * w + x] - tmp[(size_t)std::max(y - r, 0) * w + x];
					}
				}
			}
		for (size_t i = 0; i < out.px.size(); i++)
		{
			const float l = 0.2126f * ch[0][i] + 0.7152f * ch[1][i] + 0.0722f * ch[2][i];
			const auto f = [&](float v) { return (int)std::clamp(std::lround((l + (v - l) * saturate) * brightness), 0L, 255L); };
			out.px[i] = Pack(f(ch[0][i]), f(ch[1][i]), f(ch[2][i]), 255);
		}
		return out;
	}
}
