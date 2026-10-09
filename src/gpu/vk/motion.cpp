// motion.cpp: motion vectors for the temporal upscalers and frame generation (b-motion, session bottom), from the
// renderer alone: no game-side work per actor. WWHD_MOTION=1 (or =debug: the motion drawn over the TV image at its
// scan copy). Off: nothing here runs, and the checks are unchanged by construction.
//
// How (docs/research/gpu-plan.md, "Motion vectors"):
// - Each draw's vertex shader constants are the ring's copy of its uniform data (the uniform vars and the blocks).
//   They're kept for the frame's scene draws (a depth target the TV's size), and the next presented frame matches
//   each scene draw to one of them: within its group (vertex and pixel shader, vertex buffer addresses), the nearest
//   by the draw's world matrix (its first block's first 12 floats), one to one, closer than kMaxMove, with no other
//   candidate as near that differs. The game's skinning is in its vertex shaders (bone matrices in the blocks), and
//   its uniform data moves around a per-frame ring in guest memory, so neither guest addresses nor draw order are a
//   key; the matrix is (traces of tour3: all 5,075 scene draws with fixed vertex data match).
// - A matched draw runs a variant of its vertex shader: the decompiled body twice, once on this frame's constants
//   and once on the previous frame's (every uniform block declared again at binding + kPrevBinding, its members
//   renamed _prev), writing only the previous clip position; both positions go to a pixel shader variant that
//   writes their difference (in UV units, half the NDC difference) to colour slot kSlot, an RG16F target of the
//   scene's own size. Draws without depth writes keep their shaders and leave the target alone (write mask 0).
// - A draw with no match (new, ambiguous, its vertex data written fresh each frame: particles) gets the camera's
//   motion only: its first block (its own matrix) from this frame, the others from the last frame's draws of the
//   same vertex shader.
#include "renderer_internal.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <regex>
#include <unordered_map>

namespace wwhd::gpu::motion
{
	namespace
	{
		constexpr float kMaxMove = 200.0f;                       // a world matrix's change a frame (world units + rotation)
		struct Binding { uint32 arena = 0, bytes = 0, readable = 0; MPTR phys = 0; };
		struct Kept
		{
			uint64 group = 0, vsKey = 0;
			float id[12]{};
			bool hasId = false;
			std::vector<Binding> bindings;
		};
		struct Frame
		{
			std::vector<uint8> arena;
			std::unordered_map<MPTR, std::pair<uint32, uint32>> shared;   // a block's bytes by guest address, once a frame
			std::vector<Kept> records;
			std::unordered_map<uint64, std::vector<uint32>> groups;       // group -> records
			std::unordered_map<uint64, uint32> lastOfVs;                  // vs key -> its last record (camera-only motion)
			std::vector<bool> claimed;
			void Clear() { arena.clear(); shared.clear(); records.clear(); groups.clear(); lastOfVs.clear(); claimed.clear(); }
		};
		Frame s_frames[2];
		uint32 s_cur = 0;
		// the previous frame's data already in the ring this command buffer, by its place in that frame's arena (a shared
		// block, the camera's or the lights', is copied once, not once per draw); cleared at each submit (OnSubmitted)
		std::unordered_map<uint32, VkDeviceSize> s_inRing;
		Image s_target;                                         // the motion target, the scene's own size
		uint32 s_targetFrame = UINT32_MAX;                      // the frame it was last cleared in
		struct { uint32 draws = 0, matched = 0, camera = 0, plain = 0, noId = 0, noGroup = 0, far = 0, ambiguous = 0, taken = 0; } s_stats;

		uint32 Store(Frame& f, const uint8* p, uint32 n)
		{
			const uint32 at = (uint32)f.arena.size();
			f.arena.insert(f.arena.end(), p, p + n);
			return at;
		}

		float Dist(const float* a, const float* b)
		{
			float d = 0.0f;
			for (int i = 0; i < 12; i++)
				d += (a[i] - b[i]) * (a[i] - b[i]);
			return std::sqrt(d);
		}
	}

	bool On()
	{
		static const bool on = [] { const char* e = getenv("WWHD_MOTION"); return e && (atoi(e) != 0 || strcmp(e, "debug") == 0); }();
		return on;
	}

	bool Debug()
	{
		static const bool on = [] { const char* e = getenv("WWHD_MOTION"); return e && strcmp(e, "debug") == 0; }();
		return on;
	}

	// ---- the shader variants ------------------------------------------------------------------------------
	namespace
	{
		// the decompiler's uniform blocks: "UNIFORM_BUFFER_LAYOUT(gl, set, binding) uniform NAME { ... };" and, in its
		// Vulkan branch, "layout(set = S, binding = B) uniform NAME { ... };"
		const std::regex kBlock(R"((UNIFORM_BUFFER_LAYOUT\((\d+), (\d+), (\d+)\)|layout\(set = (\d+), binding = (\d+)\)) uniform (\w+)\s*\{([^}]*)\};)");
		const std::regex kMember(R"((\w+)\s*(\[|;))");
		const std::regex kOut(R"(layout\(location = (\d+)\) out (\w+) (\w+);)");

		std::string RenameWords(std::string text, const std::vector<std::string>& names, const std::string& suffix)
		{
			for (const auto& n : names)
				text = std::regex_replace(text, std::regex("\\b" + n + "\\b"), n + suffix);
			return text;
		}
	}

	// the decompiler's varyings sit at locations under 16 (the game's: under 10); ours at 16 and 17, well inside every
	// device's limit (the last locations of a 128-component limit are refused by some: VUID-RuntimeSpirv-Location-06272)
	uint32 VaryingBase()
	{
		const uint32 comps = std::min(s.props.limits.maxVertexOutputComponents, s.props.limits.maxFragmentInputComponents);
		return comps / 4 >= 24 ? 16 : 0;
	}

	bool VertexVariant(const std::string& in, std::string& out)
	{
		const uint32 loc = VaryingBase();
		const size_t mainAt = in.find("void main()");
		const size_t open = mainAt == std::string::npos ? std::string::npos : in.find('{', mainAt);
		const size_t close = in.rfind('}');
		if (!loc || open == std::string::npos || close == std::string::npos || close < open)
			return false;
		std::string head = in.substr(0, mainAt), body = in.substr(open + 1, close - open - 1);
		// the blocks again at binding + kPrevBinding, members renamed; the outputs' names, to shadow in the copy
		std::vector<std::string> members, outputs;
		std::string extra;
		for (std::sregex_iterator it(head.begin(), head.end(), kBlock), end; it != end; ++it)
		{
			const auto& m = *it;
			const uint32 set = m[3].matched ? (uint32)std::stoul(m[3]) : (uint32)std::stoul(m[5]);
			const uint32 binding = m[4].matched ? (uint32)std::stoul(m[4]) : (uint32)std::stoul(m[6]);
			std::string fields = m[8].str();
			std::vector<std::string> names;
			for (std::sregex_iterator f(fields.begin(), fields.end(), kMember), fe; f != fe; ++f)
				names.push_back((*f)[1].str());
			members.insert(members.end(), names.begin(), names.end());
			extra += fmt::format("layout(set = {}, binding = {}, std140) uniform {}_prev\n{{{}}};\n", set, binding + kPrevBinding, m[7].str(),
				RenameWords(fields, names, "_prev"));
		}
		std::string shadows;
		for (std::sregex_iterator it(head.begin(), head.end(), kOut), end; it != end; ++it)
		{
			if ((uint32)std::stoul((*it)[1]) >= loc)
				return false;
			shadows += fmt::format("{} {};\n", (*it)[2].str(), (*it)[3].str());
		}
		if (members.empty())
			return false;
		std::string prevBody = RenameWords(body, members, "_prev");
		prevBody = std::regex_replace(prevBody, std::regex(R"(\bSET_POSITION\()"), "WWHD_PREV_POSITION(");
		prevBody = std::regex_replace(prevBody, std::regex(R"(\bgl_Position\b)"), "wwhd_prev");
		prevBody = std::regex_replace(prevBody, std::regex(R"(\bgl_PointSize\b)"), "wwhd_pointSize");
		prevBody = std::regex_replace(prevBody, std::regex(R"(\breturn\s*;)"), "return wwhd_prev;");
		out = head + extra + fmt::format("layout(location = {}) out vec4 wwhd_currClip;\nlayout(location = {}) out vec4 wwhd_prevClip;\n", loc, loc + 1) +
			"void wwhd_main()\n{" + body + "}\n" +
			"#define WWHD_PREV_POSITION(_v) wwhd_prev = _v\n"
			"vec4 wwhd_prevPosition()\n{\nvec4 wwhd_prev = vec4(0.0, 0.0, 0.0, 1.0);\nfloat wwhd_pointSize = 1.0;\n" + shadows + prevBody +
			"return wwhd_prev;\n}\n"
			"void main()\n{\nwwhd_main();\nwwhd_currClip = gl_Position;\nwwhd_prevClip = wwhd_prevPosition();\n}\n";
		return true;
	}

	bool PixelVariant(const std::string& in, std::string& out)
	{
		const uint32 loc = VaryingBase();
		const size_t mainAt = in.find("void main()");
		if (!loc || mainAt == std::string::npos || in.find(fmt::format("layout(location = {}) out", kSlot)) != std::string::npos)
			return false;
		out = in.substr(0, mainAt) + fmt::format("layout(location = {}) in vec4 wwhd_currClip;\nlayout(location = {}) in vec4 wwhd_prevClip;\n"
			"layout(location = {}) out vec4 wwhd_motion;\n", loc, loc + 1, kSlot) +
			"void wwhd_main()" + in.substr(mainAt + 11) +
			"\nvoid main()\n{\nwwhd_main();\n"
			"wwhd_motion = vec4((wwhd_currClip.xy / wwhd_currClip.w - wwhd_prevClip.xy / wwhd_prevClip.w) * 0.5, 0.0, 1.0);\n}\n";
		return true;
	}

	// ---- the history -------------------------------------------------------------------------------------
	void Remember(uint64 group, uint64 vsKey, std::span<const UniformData> data, std::vector<uint32>& prevOffsets, bool want)
	{
		Frame& cur = s_frames[s_cur];
		Frame& prev = s_frames[s_cur ^ 1];
		uint32 idx = (uint32)cur.records.size();
		auto& rec = cur.records.emplace_back();
		rec.group = group, rec.vsKey = vsKey;
		// the identity: the first block's first 12 floats (as the GPU reads them), else the uniform vars'
		const UniformData* idSrc = nullptr;
		for (const auto& d : data)
			if (d.phys && d.bytes >= 48)
			{
				idSrc = &d;
				break;
			}
		if (!idSrc && !data.empty() && data[0].bytes >= 48)
			idSrc = &data[0];
		if (idSrc)
		{
			memcpy(rec.id, s.ring.data + idSrc->offset, 48);
			rec.hasId = std::all_of(rec.id, rec.id + 12, [](float f) { return std::isfinite(f); });
		}
		for (const auto& d : data)
		{
			Binding b{ 0, d.bytes, d.readable, d.phys };
			if (d.phys)
			{
				auto [it, fresh] = cur.shared.try_emplace(d.phys, 0u, 0u);
				if (fresh || it->second.second != d.bytes)
					it->second = { Store(cur, s.ring.data + d.offset, d.bytes), d.bytes };
				b.arena = it->second.first;
			}
			else
				b.arena = Store(cur, s.ring.data + d.offset, d.bytes);
			rec.bindings.push_back(b);
		}
		cur.groups[group].push_back(idx);
		// the match in the last frame
		const Kept* match = nullptr;
		if (!rec.hasId)
			s_stats.noId++;
		else if (prev.groups.find(group) == prev.groups.end())
			s_stats.noGroup++;
		if (rec.hasId)
			if (auto g = prev.groups.find(group); g != prev.groups.end())
			{
				float best = FLT_MAX;
				uint32 bestIdx = UINT32_MAX;
				for (uint32 i : g->second)
					if (!prev.claimed[i] && prev.records[i].hasId)
						if (float d = Dist(rec.id, prev.records[i].id); d < best)
							best = d, bestIdx = i;
				bool ambiguous = false;
				if (bestIdx != UINT32_MAX)
					for (uint32 i : g->second)
						if (i != bestIdx && !prev.claimed[i] && prev.records[i].hasId &&
							Dist(rec.id, prev.records[i].id) < 2.0f * best + 0.05f && Dist(prev.records[i].id, prev.records[bestIdx].id) > 0.05f)
							ambiguous = true;
				if (bestIdx != UINT32_MAX && best < kMaxMove && !ambiguous && prev.records[bestIdx].bindings.size() == data.size())
				{
					prev.claimed[bestIdx] = true;
					match = &prev.records[bestIdx];
				}
				else
					(bestIdx == UINT32_MAX ? s_stats.taken : best >= kMaxMove ? s_stats.far : s_stats.ambiguous)++;
			}
		const Kept* camera = nullptr;
		if (!match)
			if (auto l = prev.lastOfVs.find(vsKey); l != prev.lastOfVs.end() && prev.records[l->second].bindings.size() == data.size())
				camera = &prev.records[l->second];
		cur.lastOfVs[vsKey] = idx;
		s_stats.draws++;
		(match ? s_stats.matched : camera ? s_stats.camera : s_stats.plain)++;
		// the previous constants into the ring, in the bindings' order (their dynamic offsets follow this frame's)
		for (size_t j = 0; want && j < data.size(); j++)
		{
			const auto& d = data[j];
			const Kept* from = match ? match : (camera && idSrc && &d != idSrc) ? camera : nullptr;
			if (!from)
			{
				prevOffsets.push_back((uint32)d.offset);              // this frame's: no motion from this binding
				continue;
			}
			const Binding& b = from->bindings[j];
			if (b.phys)                                           // a block: once a command buffer
				if (auto it = s_inRing.find(b.arena); it != s_inRing.end())
				{
					prevOffsets.push_back((uint32)it->second);
					continue;
				}
			// the range the shader may read, but only the guest's bytes copied: past them nothing the game indexes
			// (the current data's zeros there are for the guest's view; the previous position needs none)
			const VkDeviceSize off = RingAlloc(d.readable, s.props.limits.minUniformBufferOffsetAlignment);
			memcpy(s.ring.data + off, prev.arena.data() + b.arena, std::min(b.bytes, d.readable));
			if (b.phys)
				s_inRing[b.arena] = off;
			prevOffsets.push_back((uint32)off);
		}
	}

	void OnSubmitted()
	{
		s_inRing.clear();
	}

	void FrameEnd(uint32 frame)
	{
		s_inRing.clear();                                       // the arena it points into is about to change
		Frame& cur = s_frames[s_cur];
		if (frame % 600 == 0 && s_stats.draws)
		{
			Log(fmt::format("motion: frames {}-{}: {} scene draws, {} matched to the last frame, {} with the camera's motion only, {} none; "
				"unmatched: {} without a matrix, {} new groups, {} with every candidate taken, {} too far, {} ambiguous",
				frame - 599, frame, s_stats.draws, s_stats.matched, s_stats.camera, s_stats.plain, s_stats.noId, s_stats.noGroup, s_stats.taken,
				s_stats.far, s_stats.ambiguous));
			s_stats = {};
		}
		if (cur.records.empty())                               // a frame drawn without its scene (a dropped half frame)
			return;
		s_cur ^= 1;
		s_frames[s_cur].Clear();
		Frame& prev = s_frames[s_cur ^ 1];
		prev.claimed.assign(prev.records.size(), false);
	}

	// ---- the camera's jitter --------------------------------------------------------------------------
	// WWHD_JITTER=1 (with a temporal upscaler: alone it only makes the picture shimmer): a sub-pixel offset of the
	// scene camera's draws, the Halton (2, 3) sequence over 8 frames, in the target's pixels (-0.5..0.5). Applied to
	// their viewport (draw.cpp), so the clip positions, and with them the motion vectors, don't contain it.
	bool JitterOn()
	{
		static const bool on = [] { const char* e = getenv("WWHD_JITTER"); return On() && e && atoi(e) != 0; }();
		return on;
	}

	void Jitter(uint32 frame, float& x, float& y)
	{
		auto halton = [](uint32 i, uint32 base) {
			float f = 1.0f, r = 0.0f;
			for (; i; i /= base)
				f /= (float)base, r += f * (float)(i % base);
			return r;
		};
		const uint32 i = frame % 8 + 1;
		x = halton(i, 2) - 0.5f;
		y = halton(i, 3) - 0.5f;
	}

	// ---- the target -----------------------------------------------------------------------------------
	Image* Target(const Image& depth, bool& clear)
	{
		if (!s_target.image || s_target.width != depth.width || s_target.height != depth.height)
		{
			if (s_target.image)
			{
				WaitPending();
				DestroyImage(s_target);
			}
			s_target = CreateImage(VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, depth.width, depth.height,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
			s_target.gw = depth.gw, s_target.gh = depth.gh, s_target.scaled = depth.scaled, s_target.scale = depth.scale;
			s_targetFrame = UINT32_MAX;
		}
		clear = s_targetFrame != s.frame;
		s_targetFrame = s.frame;
		return &s_target;
	}

	void DrawDebug(Image& tv)
	{
		if (!s_target.image || s_targetFrame != s.frame)
			return;
		static VkShaderModule frag = fullscreen::Fragment(R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Constants { uvec4 c0, c1, c2, c3; } p;
layout(location = 0) out vec4 outColor;
void main()
{
	vec2 m = texture(src, gl_FragCoord.xy * uintBitsToFloat(p.c0.xy)).rg * 40.0;
	outColor = vec4(0.5 + m.x, 0.5 + m.y, 0.5 - 0.5 * length(m), 1.0);
}
)", "motion debug");
		if (!frag)
			return;
		float inv[2] = { 1.0f / tv.width, 1.0f / tv.height };
		uint32 c[16]{};
		memcpy(c, inv, 8);
		fullscreen::Draw(frag, s_target, tv, c);
	}
}
