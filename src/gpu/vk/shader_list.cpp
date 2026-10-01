// The shader list (docs/recompiler-design.md D20, "a first start without hitches"): what the player's
// machine needs to translate, before the game starts, every shader that playthroughs have seen, and
// to know every pipeline they built. It holds hashes, register values and pipeline recipes, no game
// content, so it ships with the port (config/US_v0/shader_list.txt); each shader's program comes
// from the player's own game files.
//
// Lines (text, one record each; '#' starts a comment):
//   program HASH SIZE FILE   a program (FNV-1a 64 of its SIZE bytes) and the content file it is in
//   fetch HASH CODE REGS     a fetch shader (GX2 builds these from the vertex layout at runtime, so
//                            they are state, not game files): its code in hex, and the registers
//                            Cemu's parser read when the run first met it
//   vs KEY PROGRAM FETCH REGS / ps KEY PROGRAM REGS
//                            a shader: its key (draw.cpp's), its program and fetch shader, and the
//                            registers translation read (Registers below) when the run first met it
//   pipeline RECIPE          a pipeline recipe (draw.cpp's PipelineDesc) in hex
// WWHD_SHADER_SOURCES=path makes a run append these lines for everything it translates or builds
// for the first time, with no FILE on the program lines; tools/shaders/shader_list.py merges such
// files into the list and adds where each program is.
#include "renderer_internal.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/Filesystem/fsc.h"
#include "config/ActiveSettings.h"
#include <fstream>
#include <sstream>

namespace wwhd::gpu::shaderlist
{
	namespace
	{
		// The registers translation reads: Cemu's decompiler, its fetch-shader parser and the PS input
		// table (LatteShader_UpdatePSInputs). What else is in a register file can't change a shader.
		// A run that captures checks it for every shader it translates (draw.cpp, Capture).
		constexpr std::pair<uint32, uint32> kRanges[] = {
			{ mmVGT_PRIMITIVE_TYPE, mmVGT_PRIMITIVE_TYPE + 1 },
			{ 0xA000, 0xA300 },        // context: targets, semantics, SPI, clip, alpha test, programs, GS, stream-out
			{ Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + 18 * 7 },
			{ Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS + 18 * 7 },
			{ Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS + 18 * 7 },
			{ mmSQ_VTX_ATTRIBUTE_BLOCK_START, mmSQ_VTX_ATTRIBUTE_BLOCK_END },
		};

		FILE* s_sources = nullptr;

		uint64 Fnv(const uint8* p, size_t n)
		{
			uint64 h = 0xCBF29CE484222325ull;
			for (size_t i = 0; i < n; i++)
				h = (h ^ p[i]) * 0x100000001B3ull;
			return h;
		}

		bool Unhex(std::string_view s, std::vector<uint8>& out)
		{
			if (s.size() % 2)
				return false;
			out.resize(s.size() / 2);
			for (size_t i = 0; i < out.size(); i++)
			{
				auto [p, ec] = std::from_chars(s.data() + 2 * i, s.data() + 2 * i + 2, out[i], 16);
				if (ec != std::errc() || p != s.data() + 2 * i + 2)
					return false;
			}
			return true;
		}

		template<typename T>
		bool Number(std::string_view s, T& v, int base = 16)
		{
			auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
			return ec == std::errc() && p == s.data() + s.size();
		}

		// ---- programs in the game's files (the G1 extractor's walk, tools/shaders/corpus.py) --------
		struct Reader
		{
			std::span<const uint8> d;
			bool little = false;
			uint32 U32(size_t at) const
			{
				if (at + 4 > d.size())
					return 0;
				uint32 v;
				memcpy(&v, d.data() + at, 4);
				return little ? v : _swapEndianU32(v);
			}
			uint16 U16(size_t at) const
			{
				if (at + 2 > d.size())
					return 0;
				uint16 v;
				memcpy(&v, d.data() + at, 2);
				return little ? v : _swapEndianU16(v);
			}
		};

		std::vector<uint8> Yaz0(std::span<const uint8> src)
		{
			Reader r{ src };
			std::vector<uint8> out(r.U32(4));
			size_t in = 16, o = 0;
			while (o < out.size() && in < src.size())
			{
				uint8 group = src[in++];
				for (int bit = 7; bit >= 0 && o < out.size() && in < src.size(); bit--)
				{
					if (group & (1 << bit))
						out[o++] = src[in++];
					else
					{
						if (in + 2 > src.size())
							return out;
						uint32 b = src[in] << 8 | src[in + 1];
						in += 2;
						size_t dist = (b & 0xFFF) + 1, n = b >> 12;
						if (n == 0)
						{
							if (in >= src.size())
								return out;
							n = src[in++] + 0x12;
						}
						else
							n += 2;
						if (dist > o)
							return out;
						for (; n && o < out.size(); n--, o++)
							out[o] = out[o - dist];
					}
				}
			}
			return out;
		}

		// a program's microcode: its GX2 struct (Cemu's GX2_Shader.h; size, then offset of the code)
		using Found = std::function<void(std::span<const uint8> code)>;
		void Gx2Program(uint32 stage, Reader blob, std::function<std::span<const uint8>(uint32 ptr, uint32 size)> codeAt, const Found& found)
		{
			static const uint32 kSize[3] = { 0xD0, 0xA4, 0x4C }, kPtr[3] = { 0xD4, 0xA8, 0x50 };
			if (stage < 3)
				found(codeAt(blob.U32(kPtr[stage]), blob.U32(kSize[stage])));
		}

		// agl's shader archive ("BAHS", version 9): one GX2 struct per program, pointers relative to it
		void Sharcfb(std::span<const uint8> d, const Found& found)
		{
			Reader r{ d };
			r.little = !memcmp(d.data() + 12, "\x01\0\0\0", 4);   // version 9 is little-endian (word 3 is 1)
			size_t pos = 24 + r.U32(20);
			uint32 count = r.U32(pos + 4);
			size_t p = pos + 8;
			for (uint32 i = 0; i < count && p + 16 <= d.size(); i++)
			{
				uint32 size = r.U32(p), kind = r.U32(p + 4), blobSize = r.U32(p + 12);
				if (size == 0 || p + 16 + blobSize > d.size())
					return;
				std::span<const uint8> blob = d.subspan(p + 16, blobSize);
				Gx2Program(kind, Reader{ blob, r.little }, [&](uint32 ptr, uint32 n) {
					return ptr <= blob.size() && n <= blob.size() - ptr ? blob.subspan(ptr, n) : std::span<const uint8>();
				}, found);
				p += size;
			}
		}

		// a GX2 shader file ("Gfx2", big-endian): a shader header block, then its program block
		void Gfd(std::span<const uint8> d, const Found& found)
		{
			Reader r{ d };
			size_t p = r.U32(4);
			std::span<const uint8> pending[3];
			while (p + 0x20 <= d.size() && !memcmp(d.data() + p, "BLK{", 4))
			{
				uint32 hsize = r.U32(p + 4), kind = r.U32(p + 16), dsize = r.U32(p + 20);
				if (p + hsize + dsize > d.size())
					return;
				std::span<const uint8> body = d.subspan(p + hsize, dsize);
				if (kind == 3 || kind == 6 || kind == 8)
					pending[kind == 3 ? 0 : kind == 6 ? 1 : 2] = body;
				else if (kind == 5 || kind == 7 || kind == 9)
				{
					uint32 stage = kind == 5 ? 0 : kind == 7 ? 1 : 2;
					Gx2Program(stage, Reader{ pending[stage] }, [&](uint32, uint32 n) { return body.first(std::min<size_t>(n, body.size())); }, found);
				}
				else if (kind == 1)
					return;
				p += hsize + dsize;
			}
		}

		void Walk(std::span<const uint8> d, const Found& found)
		{
			if (d.size() < 16)
				return;
			if (!memcmp(d.data(), "Yaz0", 4))
			{
				std::vector<uint8> plain = Yaz0(d);
				Walk(plain, found);
			}
			else if (!memcmp(d.data(), "SARC", 4))
			{
				Reader r{ d };
				r.little = d[6] == 0xFF && d[7] == 0xFE;
				uint32 dataStart = r.U32(12);
				size_t sfat = r.U16(4);
				uint32 nodes = r.U16(sfat + 6);
				for (uint32 i = 0; i < nodes; i++)
				{
					size_t node = sfat + 12 + i * 16;
					uint32 begin = r.U32(node + 8), end = r.U32(node + 12);
					if (dataStart + (size_t)end <= d.size() && begin <= end)
						Walk(d.subspan(dataStart + begin, end - begin), found);
				}
			}
			else if (!memcmp(d.data(), "BAHS", 4))
				Sharcfb(d, found);
			else if (!memcmp(d.data(), "Gfx2", 4))
				Gfd(d, found);
			else
			{
				// shader files inside other files: BFRES keeps them among its external files
				static const std::pair<std::string_view, std::string_view> kSigs[] = { { "Gfx2", std::string_view("\0\0\0\x20", 4) },
					{ "BAHS", std::string_view("\x09\0\0\0", 4) } };
				for (auto& [sig, next] : kSigs)
				{
					std::string_view all((const char*)d.data(), d.size());
					for (size_t at = all.find(sig); at != std::string_view::npos; at = all.find(sig, at + 4))
						if (all.substr(at + 4, 4) == next)
						{
							if (sig == "Gfx2")
								Gfd(d.subspan(at), found);
							else
								Sharcfb(d.subspan(at), found);
						}
				}
			}
		}
	}

	std::string Registers(const uint32* regs)
	{
		std::string s;
		for (auto [first, end] : kRanges)
			for (uint32 i = first; i < end; i++)
				if (regs[i])
					s += fmt::format("{}{:x}:{:x}", s.empty() ? "" : " ", i, regs[i]);
		return s;
	}

	bool SetRegisters(uint32* regs, size_t count, std::string_view text)
	{
		std::fill(regs, regs + count, 0);
		while (!text.empty())
		{
			size_t space = text.find(' ');
			std::string_view item = text.substr(0, space);
			text = space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
			if (item.empty())
				continue;
			size_t colon = item.find(':');
			uint32 index, value;
			if (colon == std::string_view::npos || !Number(item.substr(0, colon), index) || !Number(item.substr(colon + 1), value) || index >= count)
				return false;
			regs[index] = value;
		}
		return true;
	}

	bool Capturing()
	{
		static bool opened = false;
		if (!opened)
		{
			opened = true;
			if (const char* path = getenv("WWHD_SHADER_SOURCES"); path && *path)
			{
				s_sources = fopen(path, "ab");
				if (!s_sources)
					Log(fmt::format("shader list: can't write {}", path));
			}
		}
		return s_sources != nullptr;
	}

	void Capture(const std::string& line)
	{
		if (!Capturing())
			return;
		fputs(line.c_str(), s_sources);
		fputc('\n', s_sources);
		fflush(s_sources);
	}

	std::string Hex(std::span<const uint8> bytes)
	{
		static const char* digits = "0123456789abcdef";
		std::string s(bytes.size() * 2, '0');
		for (size_t i = 0; i < bytes.size(); i++)
		{
			s[2 * i] = digits[bytes[i] >> 4];
			s[2 * i + 1] = digits[bytes[i] & 15];
		}
		return s;
	}

	bool Read(List& list)
	{
		fs::path path = ActiveSettings::GetDataPath("wwhd/shader_list.txt");
		if (const char* e = getenv("WWHD_SHADER_LIST"))
			path = e;
		std::ifstream in(path);
		if (!in)
			return false;
		std::string line;
		uint32 bad = 0, n = 0;
		while (std::getline(in, line))
		{
			n++;
			if (line.empty() || line[0] == '#')
				continue;
			std::istringstream ss(line);
			std::string kind, a, b, c;
			ss >> kind >> a;
			bool ok = false;
			if (kind == "program")
			{
				List::Program p;
				ss >> b >> std::ws;
				std::getline(ss, p.file);
				ok = Number(a, p.hash) && Number(b, p.size, 10) && !p.file.empty() && p.file != "-";
				if (ok)
					list.programs[p.hash] = std::move(p);
			}
			else if (kind == "fetch")
			{
				List::Fetch f;
				ss >> b >> std::ws;
				std::getline(ss, f.registers);
				ok = Number(a, f.hash) && Unhex(b, f.code) && Fnv(f.code.data(), f.code.size()) == f.hash;
				if (ok)
					list.fetches[f.hash] = std::move(f);
			}
			else if (kind == "vs" || kind == "ps")
			{
				List::Shader sh;
				sh.vertex = kind == "vs";
				ss >> b;
				if (sh.vertex)
					ss >> c;
				ss >> std::ws;
				std::getline(ss, sh.registers);
				ok = Number(a, sh.key) && Number(b, sh.program) && (!sh.vertex || Number(c, sh.fetch));
				if (ok)
					list.shaders.push_back(std::move(sh));
			}
			else if (kind == "pipeline")
			{
				std::vector<uint8> recipe;
				ok = Unhex(a, recipe) && !recipe.empty();
				if (ok)
					list.pipelines.push_back(std::move(recipe));
			}
			if (!ok && bad++ < 5)
				Log(fmt::format("shader list: line {} of {} isn't understood", n, path.string()));
		}
		Log(fmt::format("shader list: {} programs, {} fetch shaders, {} shaders, {} pipelines ({}{})", list.programs.size(),
			list.fetches.size(), list.shaders.size(), list.pipelines.size(), path.string(), bad ? fmt::format(", {} lines not understood", bad) : ""));
		return true;
	}

	std::unordered_map<uint64, std::vector<uint8>> FindPrograms(const List& list, const std::set<uint64>& wanted)
	{
		std::map<std::string, std::vector<uint64>> byFile;
		for (uint64 h : wanted)
			if (auto it = list.programs.find(h); it != list.programs.end())
				byFile[it->second.file].push_back(h);
		std::unordered_map<uint64, std::vector<uint8>> out;
		for (auto& [file, hashes] : byFile)
		{
			std::string path = "/vol/content/" + file;
			sint32 status;
			FSCVirtualFile* f = fsc_open(path.c_str(), FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status);
			if (!f)
			{
				Log(fmt::format("shader list: can't open {} in the game's files", path));
				continue;
			}
			std::vector<uint8> data(fsc_getFileSize(f));
			uint32 got = fsc_readFile(f, data.data(), (uint32)data.size());
			fsc_close(f);
			if (got != data.size())
				continue;
			std::set<uint64> here(hashes.begin(), hashes.end());
			Walk(data, [&](std::span<const uint8> code) {
				if (code.empty())
					return;
				uint64 h = Fnv(code.data(), code.size());
				if (here.count(h) && !out.count(h) && list.programs.at(h).size == code.size())
					out[h].assign(code.begin(), code.end());
			});
		}
		return out;
	}
}
