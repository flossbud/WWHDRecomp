// fsr1.cpp: AMD FidelityFX Super Resolution 1 (EASU, then RCAS sharpening; MIT, src/third_party/fsr1) for the
// render scale (docs/research/gpu-plan.md, b-fsr1; session bottom). WWHD_UPSCALER=fsr1 (or the settings page's
// Upscaler): the TV image drawn at a render scale under 1 is upscaled to the guest's size where the game's HUD begins
// (renderer.cpp, HudBegins), so the HUD is drawn at full resolution over it. Two full-screen fragment passes, the
// way AMD's sample uses them: EASU from the scaled image into an image of the full size and the target's format,
// then RCAS from that into the target (any colour format the game renders to). The headers come in as strings (CMake
// writes fsr1_headers.inc); glslang compiles the shaders at the first use. WWHD_FSR_SHARPNESS (stops, default 0.2,
// AMD's): 0 is the sharpest; off: EASU alone, straight into the target. AMD's fp16 path where the device has
// shaderFloat16 (WWHD_FSR_HALF=0: fp32). The worker's iGPU (en-tn at 75%): EASU 3.2 ms, RCAS 1.1 ms with fp16 (fp32
// 5.6 and 1.4). Off (the default, and with no render scale) nothing here runs.
#include "renderer_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <glslang/Public/ShaderLang.h>
#include "fsr1_headers.inc"

namespace wwhd::gpu::fsr1
{
	namespace
	{
		const char* kVertex = R"(#version 450
void main()
{
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

		// EASU: c0-c3 FsrEasuCon's; c3.zw (unused by EASU) one over the output's size, for the alpha's bilinear sample
		// half: AMD's fp16 path (A_HALF, FsrEasuH and FsrRcasH), with the device's shaderFloat16
		std::string Prologue(bool half)
		{
			return std::string("#version 450\n#define A_GPU 1\n#define A_GLSL 1\n") + (half ? "#define A_HALF 1\n" : "") + kFfxA;
		}

		std::string EasuSource(bool half)
		{
			if (half)
				return Prologue(true) + R"(
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Constants { uvec4 c0, c1, c2, c3; } p;
layout(location = 0) out vec4 outColor;
#define FSR_EASU_H 1
)" + kFfxFsr1 + R"(
AH4 FsrEasuRH(AF2 q) { return AH4(textureGather(src, q, 0)); }
AH4 FsrEasuGH(AF2 q) { return AH4(textureGather(src, q, 1)); }
AH4 FsrEasuBH(AF2 q) { return AH4(textureGather(src, q, 2)); }
void main()
{
	AU2 ip = AU2(gl_FragCoord.xy);
	AH3 c;
	FsrEasuH(c, ip, p.c0, p.c1, p.c2, p.c3);
	float a = texture(src, (vec2(ip) + 0.5) * uintBitsToFloat(p.c3.zw)).a;
	outColor = vec4(vec3(c), a);
}
)";
			return Prologue(false) + R"(
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Constants { uvec4 c0, c1, c2, c3; } p;
layout(location = 0) out vec4 outColor;
#define FSR_EASU_F 1
)" + kFfxFsr1 + R"(
AF4 FsrEasuRF(AF2 q) { return textureGather(src, q, 0); }
AF4 FsrEasuGF(AF2 q) { return textureGather(src, q, 1); }
AF4 FsrEasuBF(AF2 q) { return textureGather(src, q, 2); }
void main()
{
	AU2 ip = AU2(gl_FragCoord.xy);
	AF3 c;
	FsrEasuF(c, ip, p.c0, p.c1, p.c2, p.c3);
	float a = texture(src, (vec2(ip) + 0.5) * uintBitsToFloat(p.c3.zw)).a;
	outColor = vec4(c, a);
}
)";
		}

		std::string RcasSource(bool half)
		{
			if (half)
				return Prologue(true) + R"(
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Constants { uvec4 c0, c1, c2, c3; } p;
layout(location = 0) out vec4 outColor;
#define FSR_RCAS_H 1
#define FSR_RCAS_PASSTHROUGH_ALPHA 1
)" + kFfxFsr1 + R"(
AH4 FsrRcasLoadH(ASW2 q) { return AH4(texelFetch(src, clamp(ASU2(q), ASU2(0), textureSize(src, 0) - 1), 0)); }
void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b) {}
void main()
{
	AH1 r, g, b, a;
	FsrRcasH(r, g, b, a, AU2(gl_FragCoord.xy), p.c0);
	outColor = vec4(r, g, b, a);
}
)";
			return Prologue(false) + R"(
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Constants { uvec4 c0, c1, c2, c3; } p;
layout(location = 0) out vec4 outColor;
#define FSR_RCAS_F 1
#define FSR_RCAS_PASSTHROUGH_ALPHA 1
)" + kFfxFsr1 + R"(
AF4 FsrRcasLoadF(ASU2 q) { return texelFetch(src, clamp(q, ASU2(0), textureSize(src, 0) - 1), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
void main()
{
	FsrRcasF(outColor.r, outColor.g, outColor.b, outColor.a, AU2(gl_FragCoord.xy), p.c0);
}
)";
		}

		// the full-screen passes' common objects (fullscreen::, also motion.cpp's debug view)
		struct Common
		{
			bool tried = false, ok = false;
			VkShaderModule vs = VK_NULL_HANDLE;
			VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
			VkPipelineLayout layout = VK_NULL_HANDLE;
			VkSampler sampler = VK_NULL_HANDLE;
			std::unordered_map<uint64, VkPipeline> pipelines;    // (fragment module, colour format)
		};
		Common s_common;

		struct State
		{
			bool tried = false, ok = false;
			VkShaderModule easu = VK_NULL_HANDLE, rcas = VK_NULL_HANDLE;
			Image mid;                                           // EASU's output, RCAS's input (the target's format and size)
		};
		State s_fsr;

		VkShaderModule Module(const std::string& glsl, EShLanguage stage, const char* name)
		{
			std::vector<uint32> spirv;
			std::string log;
			if (!CompileGlsl(glsl, (int)stage, spirv, log))
			{
				Log(fmt::format("full-screen pass: the {} shader didn't compile: {}", name, log));
				return VK_NULL_HANDLE;
			}
			VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			ci.codeSize = spirv.size() * 4;
			ci.pCode = spirv.data();
			VkShaderModule m;
			Check(vkCreateShaderModule(s.device, &ci, nullptr, &m), "vkCreateShaderModule");
			return m;
		}

		bool InitCommon()
		{
			if (s_common.tried)
				return s_common.ok;
			s_common.tried = true;
			s_common.vs = Module(kVertex, EShLangVertex, "vertex");
			if (!s_common.vs)
				return false;
			VkDescriptorSetLayoutBinding b{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
			VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
			li.bindingCount = 1;
			li.pBindings = &b;
			Check(vkCreateDescriptorSetLayout(s.device, &li, nullptr, &s_common.setLayout), "vkCreateDescriptorSetLayout");
			VkPushConstantRange pc{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64 };
			VkPipelineLayoutCreateInfo pi{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			pi.setLayoutCount = 1;
			pi.pSetLayouts = &s_common.setLayout;
			pi.pushConstantRangeCount = 1;
			pi.pPushConstantRanges = &pc;
			Check(vkCreatePipelineLayout(s.device, &pi, nullptr, &s_common.layout), "vkCreatePipelineLayout");
			VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
			si.magFilter = si.minFilter = VK_FILTER_LINEAR;
			si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			si.maxLod = 0.0f;
			Check(vkCreateSampler(s.device, &si, nullptr, &s_common.sampler), "vkCreateSampler");
			return s_common.ok = true;
		}

		bool Init()
		{
			if (s_fsr.tried)
				return s_fsr.ok;
			s_fsr.tried = true;
			static const bool half = [] { const char* e = getenv("WWHD_FSR_HALF"); return s.float16 && !(e && atoi(e) == 0); }();
			if (!InitCommon())
				return false;
			s_fsr.easu = Module(EasuSource(half), EShLangFragment, "EASU");
			s_fsr.rcas = Module(RcasSource(half), EShLangFragment, "RCAS");
			if (!s_fsr.easu || !s_fsr.rcas)
				return false;
			s_fsr.ok = true;
			Log(fmt::format("fsr1: AMD FidelityFX FSR 1 (EASU + RCAS, {}) where the HUD begins (WWHD_UPSCALER=fsr1)",
				half ? "fp16" : "fp32"));
			return true;
		}

		VkPipeline Pipeline(VkShaderModule fragment, VkFormat format)
		{
			VkPipeline& p = s_common.pipelines[((uint64)(uintptr_t)fragment << 8) ^ (uint64)format];
			if (p)
				return p;
			VkPipelineShaderStageCreateInfo stages[2]{ { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
				{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
			stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = s_common.vs, stages[0].pName = "main";
			stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fragment, stages[1].pName = "main";
			VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			vp.viewportCount = vp.scissorCount = 1;
			VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			rs.polygonMode = VK_POLYGON_MODE_FILL;
			rs.cullMode = VK_CULL_MODE_NONE;
			rs.lineWidth = 1.0f;
			VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineColorBlendAttachmentState att{};
			att.colorWriteMask = 0xF;
			VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			cb.attachmentCount = 1;
			cb.pAttachments = &att;
			VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
			ds.dynamicStateCount = 2;
			ds.pDynamicStates = dyn;
			VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
			ri.colorAttachmentCount = 1;
			ri.pColorAttachmentFormats = &format;
			VkGraphicsPipelineCreateInfo gi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
			gi.pNext = &ri;
			gi.stageCount = 2;
			gi.pStages = stages;
			gi.pVertexInputState = &vi;
			gi.pInputAssemblyState = &ia;
			gi.pViewportState = &vp;
			gi.pRasterizationState = &rs;
			gi.pMultisampleState = &ms;
			gi.pColorBlendState = &cb;
			gi.pDynamicState = &ds;
			gi.layout = s_common.layout;
			Check(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &gi, nullptr, &p), "vkCreateGraphicsPipelines");
			return p;
		}

		// one full-screen pass of `fragment` from `src` (sampled) into `dst` (a colour attachment), with its constants
		void Pass(VkShaderModule fragment, Image& src, Image& dst, const uint32 (&c)[16])
		{
			Transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			Transition(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			ai.descriptorPool = s.descriptors;
			ai.descriptorSetCount = 1;
			ai.pSetLayouts = &s_common.setLayout;
			VkDescriptorSet set;
			Check(vkAllocateDescriptorSets(s.device, &ai, &set), "vkAllocateDescriptorSets");
			VkDescriptorImageInfo ii{ s_common.sampler, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			w.dstSet = set;
			w.descriptorCount = 1;
			w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			w.pImageInfo = &ii;
			vkUpdateDescriptorSets(s.device, 1, &w, 0, nullptr);
			VkRenderingAttachmentInfo a{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			a.imageView = dst.view;
			a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;           // every pixel written
			a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
			ri.renderArea = { { 0, 0 }, { dst.width, dst.height } };
			ri.layerCount = 1;
			ri.colorAttachmentCount = 1;
			ri.pColorAttachments = &a;
			vkCmdBeginRendering(s.cmd, &ri);
			vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipeline(fragment, dst.format));
			vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_common.layout, 0, 1, &set, 0, nullptr);
			vkCmdPushConstants(s.cmd, s_common.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, c);
			VkViewport v{ 0.0f, 0.0f, (float)dst.width, (float)dst.height, 0.0f, 1.0f };
			VkRect2D r{ { 0, 0 }, { dst.width, dst.height } };
			vkCmdSetViewport(s.cmd, 0, 1, &v);
			vkCmdSetScissor(s.cmd, 0, 1, &r);
			vkCmdDraw(s.cmd, 3, 1, 0, 0);
			vkCmdEndRendering(s.cmd);
		}

		// two copies of f as IEEE halves (AU1_AH2_AF2 of FsrRcasCon); f in (0, 1]: normal halves, rounded down
		uint32 PackHalf2(float f)
		{
			uint32 u;
			memcpy(&u, &f, 4);
			const uint32 exp = ((u >> 23) & 0xFF) - 127 + 15, mant = (u >> 13) & 0x3FF;
			const uint32 h = (exp << 10) | mant;
			return h | (h << 16);
		}

		uint32 Bits(float f)
		{
			uint32 u;
			memcpy(&u, &f, 4);
			return u;
		}
	}
}

namespace wwhd::gpu::fullscreen
{
	VkShaderModule Fragment(const std::string& glsl, const char* name)
	{
		return fsr1::InitCommon() ? fsr1::Module(glsl, EShLangFragment, name) : VK_NULL_HANDLE;
	}

	void Draw(VkShaderModule fragment, Image& src, Image& dst, const uint32 (&c)[16])
	{
		EndRendering();
		fsr1::Pass(fragment, src, dst, c);
		s.bound.valid = false;
	}
}

namespace wwhd::gpu::fsr1
{
	bool On()
	{
		static const bool on = [] { const char* e = getenv("WWHD_UPSCALER"); return e && strcmp(e, "fsr1") == 0; }();
		return on;
	}

	bool Upscale(Image& src, Image& dst)
	{
		if (!Init())
			return false;
		EndRendering();
		timing::Scope span(timing::Kind::Upscale);
		// WWHD_FSR_SHARPNESS=off: EASU straight into the target, no RCAS (a pass and an image's traffic fewer)
		static const char* sharpEnv = getenv("WWHD_FSR_SHARPNESS");
		static const bool rcasOn = !(sharpEnv && strcmp(sharpEnv, "off") == 0);
		// EASU's output, RCAS's input: the target's own format (the game's 8-bit targets: half the traffic of RGBA16F)
		if (rcasOn && (!s_fsr.mid.image || s_fsr.mid.width != dst.width || s_fsr.mid.height != dst.height || s_fsr.mid.format != dst.format))
		{
			if (s_fsr.mid.image)
			{
				WaitPending();
				DestroyImage(s_fsr.mid);
			}
			s_fsr.mid = CreateImage(dst.format, VK_IMAGE_ASPECT_COLOR_BIT, dst.width, dst.height,
				VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		}
		// FsrEasuCon: the whole source image (its rows are all the guest's, at the scale) onto the whole target
		const float inW = (float)src.width, inH = (float)src.height, outW = (float)dst.width, outH = (float)dst.height;
		uint32 easu[16] = {
			Bits(inW / outW), Bits(inH / outH), Bits(0.5f * inW / outW - 0.5f), Bits(0.5f * inH / outH - 0.5f),
			Bits(1.0f / inW), Bits(1.0f / inH), Bits(1.0f / inW), Bits(-1.0f / inH),
			Bits(-1.0f / inW), Bits(2.0f / inH), Bits(1.0f / inW), Bits(2.0f / inH),
			Bits(0.0f / inW), Bits(4.0f / inH), Bits(1.0f / outW), Bits(1.0f / outH) };
		timing::Mark(timing::Kind::Upscale, !timing::On() ? std::string() : fmt::format("fsr1 EASU {}x{} -> {}x{}", src.width,
			src.height, dst.width, dst.height));
		Pass(s_fsr.easu, src, rcasOn ? s_fsr.mid : dst, easu);
		if (!rcasOn)
		{
			s.bound.valid = false;
			return true;
		}
		// FsrRcasCon: the sharpness in stops, as a linear factor
		static const float sharpness = sharpEnv ? std::max(0.0f, (float)atof(sharpEnv)) : 0.2f;
		// con[1]: the factor twice as halves (the fp16 path's)
		uint32 rcas[16]{ Bits(std::exp2(-sharpness)), PackHalf2(std::exp2(-sharpness)) };
		timing::Mark(timing::Kind::Upscale, !timing::On() ? std::string() : fmt::format("fsr1 RCAS {}x{}", dst.width, dst.height));
		Pass(s_fsr.rcas, s_fsr.mid, dst, rcas);
		s.bound.valid = false;                                    // the game's next draw binds everything again
		return true;
	}
}
