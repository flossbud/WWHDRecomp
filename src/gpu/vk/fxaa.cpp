// fxaa.cpp: FXAA on the TV's scene (b-gfxopts, session bottom). WWHD_FXAA=1. Off: nothing here runs.
//
// Where: the HUD split (renderer.cpp HudBegins), so the scene is smoothed and the HUD drawn after it stays sharp; at
// the scan copy (HUD included) on a frame with no split. Before FSR 1 (EASU wants its input without jaggies, as AMD's
// notes say), not with FSR 3 when it upscales (its own temporal anti-aliasing does that job better).
// How: Timothy Lottes' FXAA 3.11 "quality" in its usual form: a luma contrast test, the edge's direction from the 3x3
// neighbourhood, a search along the edge for its ends (12 steps, growing), the pixel moved across the edge by where
// it lies between them, and the sub-pixel blend. The game's targets hold display-encoded colour, so luma is the
// colour's own (Rec. 601 weights). It draws into a scratch image of the target's size and format, copied back.
#include "renderer_internal.h"
#include <cstdlib>
#include <cstring>

namespace wwhd::gpu::fxaa
{
	namespace
	{
		const char* const kShader = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Constants { uvec4 c0, c1, c2, c3; } p;
layout(location = 0) out vec4 outColor;

float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float LumaAt(vec2 uv) { return Luma(textureLod(src, uv, 0.0).rgb); }
#define LumaOff(uv, o) Luma(textureLodOffset(src, uv, 0.0, o).rgb)   // the offset must be a constant

const float kQuality[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

void main()
{
	vec2 inv = uintBitsToFloat(p.c0.xy);
	vec2 uv = gl_FragCoord.xy * inv;
	vec4 centre = textureLod(src, uv, 0.0);
	float lC = Luma(centre.rgb);
	float lD = LumaOff(uv, ivec2(0, -1)), lU = LumaOff(uv, ivec2(0, 1));
	float lL = LumaOff(uv, ivec2(-1, 0)), lR = LumaOff(uv, ivec2(1, 0));
	float lMin = min(lC, min(min(lD, lU), min(lL, lR)));
	float lMax = max(lC, max(max(lD, lU), max(lL, lR)));
	float range = lMax - lMin;
	if (range < max(0.0312, lMax * 0.125))
	{
		outColor = centre;
		return;
	}
	float lDL = LumaOff(uv, ivec2(-1, -1)), lUR = LumaOff(uv, ivec2(1, 1));
	float lUL = LumaOff(uv, ivec2(-1, 1)), lDR = LumaOff(uv, ivec2(1, -1));
	float lDU = lD + lU, lLR = lL + lR;
	float leftCorners = lDL + lUL, downCorners = lDL + lDR, rightCorners = lDR + lUR, upCorners = lUR + lUL;
	float edgeH = abs(-2.0 * lL + leftCorners) + abs(-2.0 * lC + lDU) * 2.0 + abs(-2.0 * lR + rightCorners);
	float edgeV = abs(-2.0 * lU + upCorners) + abs(-2.0 * lC + lLR) * 2.0 + abs(-2.0 * lD + downCorners);
	bool horizontal = edgeH >= edgeV;

	// the side of the edge with the steeper gradient; luma1 is the one at the negative offset
	float luma1 = horizontal ? lD : lL, luma2 = horizontal ? lU : lR;
	float g1 = luma1 - lC, g2 = luma2 - lC;
	bool steep1 = abs(g1) >= abs(g2);
	float gradientScaled = 0.25 * max(abs(g1), abs(g2));
	float stepLength = horizontal ? inv.y : inv.x;
	float localAverage;
	if (steep1)
	{
		stepLength = -stepLength;
		localAverage = 0.5 * (luma1 + lC);
	}
	else
		localAverage = 0.5 * (luma2 + lC);
	vec2 cur = uv;
	if (horizontal)
		cur.y += stepLength * 0.5;
	else
		cur.x += stepLength * 0.5;

	// along the edge both ways to its ends
	vec2 offset = horizontal ? vec2(inv.x, 0.0) : vec2(0.0, inv.y);
	vec2 uv1 = cur - offset, uv2 = cur + offset;
	float end1 = LumaAt(uv1) - localAverage, end2 = LumaAt(uv2) - localAverage;
	bool reached1 = abs(end1) >= gradientScaled, reached2 = abs(end2) >= gradientScaled;
	if (!reached1)
		uv1 -= offset;
	if (!reached2)
		uv2 += offset;
	for (int i = 2; i < 12 && !(reached1 && reached2); i++)
	{
		if (!reached1)
			end1 = LumaAt(uv1) - localAverage;
		if (!reached2)
			end2 = LumaAt(uv2) - localAverage;
		reached1 = abs(end1) >= gradientScaled;
		reached2 = abs(end2) >= gradientScaled;
		if (!reached1)
			uv1 -= offset * kQuality[i];
		if (!reached2)
			uv2 += offset * kQuality[i];
	}

	// how far across the edge, by where the pixel lies between its ends
	float d1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y;
	float d2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
	bool nearer1 = d1 < d2;
	float pixelOffset = -min(d1, d2) / (d1 + d2) + 0.5;
	bool centreSmaller = lC < localAverage;
	bool rightWay = ((nearer1 ? end1 : end2) < 0.0) != centreSmaller;
	float finalOffset = rightWay ? pixelOffset : 0.0;

	// the sub-pixel blend (aliasing smaller than a pixel)
	float average = (1.0 / 12.0) * (2.0 * (lDU + lLR) + leftCorners + rightCorners);
	float sub1 = clamp(abs(average - lC) / range, 0.0, 1.0);
	float sub2 = (-2.0 * sub1 + 3.0) * sub1 * sub1;
	finalOffset = max(finalOffset, sub2 * sub2 * 0.75);

	vec2 at = uv;
	if (horizontal)
		at.y += finalOffset * stepLength;
	else
		at.x += finalOffset * stepLength;
	outColor = vec4(textureLod(src, at, 0.0).rgb, centre.a);
}
)";

		Image s_scratch;
	}

	bool On()
	{
		static const bool on = [] {
			const char* e = getenv("WWHD_FXAA");
			const bool v = e && strcmp(e, "0") != 0;
			if (v)
				Log("fxaa: FXAA 3.11 on the TV's scene (WWHD_FXAA; not where FSR 3 upscales)");
			return v;
		}();
		return on;
	}

	bool Apply(Image& img)
	{
		static VkShaderModule frag = fullscreen::Fragment(kShader, "fxaa");
		if (!frag || img.layers != 1 || (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
			return false;
		if (!s_scratch.image || s_scratch.width != img.width || s_scratch.height != img.height || s_scratch.format != img.format)
		{
			if (s_scratch.image)
			{
				SubmitAndWait();                                      // this frame's commands may use it (a resize)
				DestroyImage(s_scratch);
			}
			s_scratch = CreateImage(img.format, VK_IMAGE_ASPECT_COLOR_BIT, img.width, img.height,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		}
		timing::Scope span(timing::Kind::Upscale);
		timing::Mark(timing::Kind::Upscale, !timing::On() ? std::string() : fmt::format("fxaa {}x{}", img.width, img.height));
		const float inv[2] = { 1.0f / img.width, 1.0f / img.height };
		uint32 c[16]{};
		memcpy(c, inv, 8);
		fullscreen::Draw(frag, img, s_scratch, c);
		Transition(s_scratch, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageCopy r{};
		r.srcSubresource = r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		r.extent = { img.width, img.height, 1 };
		vkCmdCopyImage(s.cmd, s_scratch.image, s_scratch.layout, img.image, img.layout, 1, &r);
		return true;
	}
}
