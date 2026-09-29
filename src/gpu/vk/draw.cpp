// Draws (docs/recompiler-design.md D13; milestone G2b): the null GPU's register file at each
// IT_DRAW_INDEX_2 / IT_DRAW_INDEX_AUTO becomes a Vulkan draw.
//
// Shaders are Cemu's decompiler output (GLSL for Vulkan, compiled with glslang), so everything a
// draw binds follows the contract Cemu's Vulkan renderer keeps with that GLSL
// (Renderer/Vulkan/VulkanRendererCore.cpp and VulkanPipelineCompiler.cpp):
// * a descriptor set per stage (0 vertex, 1 pixel): combined image samplers at the decompiler's
//   texture binding points, a dynamic uniform buffer for the uniform variables (remapped
//   constants or the uniform register file, plus alpha-test reference, point size, clip-space
//   transform and frag-coord scale at the decompiler's offsets), and a dynamic uniform buffer per
//   uniform block the shader reads;
// * vertex buffers as raw integer formats at the locations the decompiler assigned per semantic,
//   decoded (endian swap, normalisation) in the shader;
// * pipeline state from the registers (primitive type, raster, blend, depth/stencil), viewport,
//   scissor, blend constants and depth bias as dynamic state.
// Uniform, vertex and index data are copied from guest memory into the per-frame ring at every
// draw: slow and simple. Render targets are the surfaces of renderer.cpp, bound with dynamic
// rendering (Vulkan 1.3); textures come from texture.cpp.
//
// Derived in part from Cemu (the files above, Core/LatteIndices.cpp, Core/LatteBufferData.cpp,
// Core/LatteRenderTarget.cpp, Core/LatteShader.cpp); Mozilla Public License 2.0.
#include "renderer_internal.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "util/helpers/StringBuf.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace wwhd::gpu
{
	void InstallNoopRenderer();                                    // latte_glue.cpp

	namespace
	{
		uint64 Mix(uint64 h, uint64 v) { return (h ^ v) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull; }

		uint64 Fnv(const uint8* p, size_t n)
		{
			uint64 h = 0xCBF29CE484222325ull;
			for (size_t i = 0; i < n; i++)
				h = (h ^ p[i]) * 0x100000001B3ull;
			return h;
		}

		template<typename F>
		void LogOnce(const std::string& what, F&& msg)
		{
			static std::set<std::string> seen;
			if (seen.insert(what).second)
				Log(msg());
		}

		// ---- shaders -------------------------------------------------------------------------------
		struct Shader
		{
			LatteDecompilerShader* dec = nullptr;
			LatteDecompilerOutputUniformOffsets uniforms;
			LatteDecompilerShaderResourceMapping mapping;
			VkShaderModule module = VK_NULL_HANDLE;
			VkDescriptorSetLayout layout = VK_NULL_HANDLE;
			std::vector<uint8> uniformBuffers;                       // uniform block indices, in binding order
		};
		std::unordered_map<uint64, Shader*> s_shaders;
		std::unordered_map<uint64, LatteFetchShader*> s_fetchShaders;
		std::unordered_map<uint64, VkPipelineLayout> s_layouts;
		std::unordered_map<uint64, VkPipeline> s_pipelines;

		bool CompileSpirv(const std::string& glsl, EShLanguage stage, std::vector<uint32>& spirv, std::string& log)
		{
			glslang::TShader shader(stage);
			const char* src = glsl.c_str();
			shader.setStrings(&src, 1);
			shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
			shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetClientVersion::EShTargetVulkan_1_1);
			shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetLanguageVersion::EShTargetSpv_1_3);
			EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
			glslang::TShader::ForbidIncluder includer;
			std::string pre;
			if (!shader.preprocess(GetDefaultResources(), 450, ENoProfile, false, false, messages, &pre, includer))
			{
				log = shader.getInfoLog();
				return false;
			}
			const char* p = pre.c_str();
			shader.setStrings(&p, 1);
			if (!shader.parse(GetDefaultResources(), 100, false, messages))
			{
				log = shader.getInfoLog();
				return false;
			}
			glslang::TProgram program;
			program.addShader(&shader);
			if (!program.link(messages) || !program.mapIO())
			{
				log = program.getInfoLog();
				return false;
			}
			glslang::SpvOptions opt;
			opt.disableOptimizer = false;
			opt.optimizeSize = true;
			spv::SpvBuildLogger logger;
			glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &logger, &opt);
			return !spirv.empty();
		}

		// PipelineCompiler::CreateDescriptorSetLayout
		VkDescriptorSetLayout CreateLayout(Shader& sh, VkShaderStageFlags stage)
		{
			std::vector<VkDescriptorSetLayoutBinding> b;
			sint32 texBase = sh.mapping.getTextureBaseBindingPoint();
			if (texBase >= 0)
				for (sint32 i = 0; i < sh.mapping.getTextureCount(); i++)
					b.push_back({ (uint32)(texBase + i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stage, nullptr });
			if (sh.mapping.uniformVarsBufferBindingPoint >= 0)
				b.push_back({ (uint32)sh.mapping.uniformVarsBufferBindingPoint, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stage, nullptr });
			for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
				if (sh.mapping.uniformBuffersBindingPoint[i] >= 0)
				{
					b.push_back({ (uint32)sh.mapping.uniformBuffersBindingPoint[i], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, stage, nullptr });
					sh.uniformBuffers.push_back((uint8)i);
				}
			VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
			ci.bindingCount = (uint32)b.size();
			ci.pBindings = b.data();
			VkDescriptorSetLayout l;
			Check(vkCreateDescriptorSetLayout(s.device, &ci, nullptr, &l), "vkCreateDescriptorSetLayout");
			return l;
		}

		Shader* GetShader(bool vertex, uint64 key, const uint8* code, uint32 size, LatteFetchShader* fetch)
		{
			auto it = s_shaders.find(key);
			if (it != s_shaders.end())
				return it->second;
			LatteDecompilerOptions opt;
			opt.usesGeometryShader = false;
			opt.useTFViaSSBO = false;
			opt.spirvInstrinsics.hasRoundingModeRTEFloat32 = false;
			opt.strictMul = g_current_game_profile->GetAccurateShaderMul() != AccurateShaderMulOption::False;
			LatteDecompilerOutput_t out{};
			if (vertex)
				LatteDecompiler_DecompileVertexShader(key, LatteGPUState.contextRegister, (uint8*)code, size, fetch, opt, &out);
			else
				LatteDecompiler_DecompilePixelShader(key, LatteGPUState.contextRegister, (uint8*)code, size, opt, &out);
			Shader* sh = new Shader;
			s_shaders[key] = sh;
			sh->dec = out.shader;
			sh->uniforms = out.uniformOffsetsVK;
			sh->mapping = out.resourceMappingVK;
			if (!sh->dec || sh->dec->hasError || !sh->dec->strBuf_shaderSource)
			{
				LogOnce(fmt::format("dec{:x}", key), [&] { return fmt::format("{} shader {:016x}: decompiler error, its draws are skipped",
					vertex ? "vertex" : "pixel", key); });
				return sh;
			}
			std::string glsl = sh->dec->strBuf_shaderSource->c_str();
			std::vector<uint32> spirv;
			std::string log;
			if (!CompileSpirv(glsl, vertex ? EShLangVertex : EShLangFragment, spirv, log))
			{
				LogOnce(fmt::format("glsl{:x}", key), [&] { return fmt::format("{} shader {:016x}: GLSL doesn't compile ({}), its draws are skipped",
					vertex ? "vertex" : "pixel", key, log.substr(0, 200)); });
				return sh;
			}
			VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			mi.codeSize = spirv.size() * 4;
			mi.pCode = spirv.data();
			Check(vkCreateShaderModule(s.device, &mi, nullptr, &sh->module), "vkCreateShaderModule");
			sh->layout = CreateLayout(*sh, vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT);
			return sh;
		}

		// ---- render targets (dynamic rendering) --------------------------------------------------------
		struct Targets
		{
			Image* color[8]{};
			Image* depth = nullptr;
			bool operator==(const Targets& o) const
			{
				return depth == o.depth && std::equal(std::begin(color), std::end(color), std::begin(o.color));
			}
		};
		Targets s_current;
		bool s_rendering = false;

		void BeginRendering(const Targets& t)
		{
			for (Image* c : t.color)
				if (c)
					c->written = ++s.writes;
			if (t.depth && LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_Z_WRITE_ENABLE())
				t.depth->written = ++s.writes;
			if (s_rendering && t == s_current)
				return;
			EndRendering();
			uint32 w = UINT32_MAX, h = UINT32_MAX;
			VkRenderingAttachmentInfo colors[8]{};
			uint32 count = 0;
			for (uint32 i = 0; i < 8; i++)
			{
				colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				if (!t.color[i])
					continue;
				Transition(*t.color[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
				colors[i].imageView = t.color[i]->view;
				colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
				colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				count = i + 1;
				w = std::min(w, t.color[i]->width);
				h = std::min(h, t.color[i]->height);
			}
			VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			if (t.depth)
			{
				Transition(*t.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
				depth.imageView = t.depth->view;
				depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
				depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				w = std::min(w, t.depth->width);
				h = std::min(h, t.depth->height);
			}
			VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
			ri.renderArea = { { 0, 0 }, { w, h } };
			ri.layerCount = 1;
			ri.colorAttachmentCount = count;
			ri.pColorAttachments = colors;
			ri.pDepthAttachment = t.depth ? &depth : nullptr;
			ri.pStencilAttachment = t.depth && (t.depth->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &depth : nullptr;
			vkCmdBeginRendering(s.cmd, &ri);
			s_current = t;
			s_rendering = true;
		}

		// ---- pipelines (PipelineCompiler) -------------------------------------------------------------
		VkFormat VertexFormat(Latte::E_HWFMT f)
		{
			using F = Latte::E_HWFMT;
			switch (f)
			{
			case F::HWFMT_32_32_32_32_FLOAT: case F::HWFMT_32_32_32_32: return VK_FORMAT_R32G32B32A32_UINT;
			case F::HWFMT_32_32_32_FLOAT: case F::HWFMT_32_32_32: return VK_FORMAT_R32G32B32_UINT;
			case F::HWFMT_32_32_FLOAT: case F::HWFMT_32_32: return VK_FORMAT_R32G32_UINT;
			case F::HWFMT_32_FLOAT: case F::HWFMT_32: case F::HWFMT_2_10_10_10: return VK_FORMAT_R32_UINT;
			case F::HWFMT_8_8_8_8: return VK_FORMAT_R8G8B8A8_UINT;
			case F::HWFMT_8_8_8: return VK_FORMAT_R8G8B8_UINT;
			case F::HWFMT_8_8: return VK_FORMAT_R8G8_UINT;
			case F::HWFMT_8: return VK_FORMAT_R8_UINT;
			case F::HWFMT_16_16_16_16: case F::HWFMT_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_UINT;
			case F::HWFMT_16_16_16: case F::HWFMT_16_16_16_FLOAT: return VK_FORMAT_R16G16B16_UINT;
			case F::HWFMT_16_16: case F::HWFMT_16_16_FLOAT: return VK_FORMAT_R16G16_UINT;
			case F::HWFMT_16: case F::HWFMT_16_FLOAT: return VK_FORMAT_R16_UINT;
			default: break;
			}
			LogOnce(fmt::format("vfmt{}", (uint32)f), [&] { return fmt::format("vertex format {:#x} not mapped", (uint32)f); });
			return VK_FORMAT_R32_UINT;
		}

		bool IsIntegerFormat(VkFormat f)
		{
			switch (f)
			{
			case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
			case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
			case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
			case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
			case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
				return true;
			default:
				return false;
			}
		}

		const VkCompareOp kCompare[8] = { VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
			VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS };
		const VkStencilOp kStencilOp[8] = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
			VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP };

		VkBlendFactor BlendFactor(uint32 f)
		{
			static const VkBlendFactor t[] = { VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
				VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
				VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ZERO,
				VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_SRC1_COLOR,
				VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
				VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA };
			return f < std::size(t) ? t[f] : VK_BLEND_FACTOR_ONE;
		}

		VkBlendOp BlendOp(uint32 f)
		{
			switch (f)
			{
			case 1: return VK_BLEND_OP_SUBTRACT;          // SRC_MINUS_DST
			case 2: return VK_BLEND_OP_MIN;
			case 3: return VK_BLEND_OP_MAX;
			case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;  // DST_MINUS_SRC
			default: return VK_BLEND_OP_ADD;              // DST_PLUS_SRC
			}
		}

		VkPipelineLayout PipelineLayout(Shader* vs, Shader* ps)
		{
			uint64 key = Mix((uint64)vs->layout, (uint64)ps->layout);
			auto it = s_layouts.find(key);
			if (it != s_layouts.end())
				return it->second;
			VkDescriptorSetLayout sets[2] = { vs->layout, ps->layout };
			VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			ci.setLayoutCount = 2;
			ci.pSetLayouts = sets;
			VkPipelineLayout l;
			Check(vkCreatePipelineLayout(s.device, &ci, nullptr, &l), "vkCreatePipelineLayout");
			return s_layouts[key] = l;
		}

		VkPipeline GetPipeline(Shader* vs, Shader* ps, LatteFetchShader* fetch, VkPipelineLayout layout, const Targets& t,
			Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE prim)
		{
			const auto& r = LatteGPUState.contextNew;
			const uint32* raw = LatteGPUState.contextRegister;
			// everything the pipeline is built from
			uint64 key = Mix((uint64)vs->module, (uint64)ps->module);
			key = Mix(key, fetch->key);
			for (auto& g : fetch->bufferGroups)
				key = Mix(key, (raw[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >> 11) & 0xFFFF);
			key = Mix(key, (uint64)prim);
			for (uint32 reg : { (uint32)Latte::REGADDR::PA_SU_SC_MODE_CNTL, (uint32)Latte::REGADDR::PA_CL_CLIP_CNTL, (uint32)Latte::REGADDR::CB_COLOR_CONTROL,
				(uint32)Latte::REGADDR::CB_TARGET_MASK, (uint32)Latte::REGADDR::DB_DEPTH_CONTROL, (uint32)Latte::REGADDR::DB_STENCILREFMASK,
				(uint32)Latte::REGADDR::DB_STENCILREFMASK_BF, (uint32)Latte::REGADDR::PA_CL_VTE_CNTL })
				key = Mix(key, raw[reg]);
			for (uint32 i = 0; i < 8; i++)
				key = Mix(Mix(key, raw[Latte::REGADDR::CB_BLEND0_CONTROL + i]), t.color[i] ? (uint64)t.color[i]->format : 0);
			key = Mix(key, t.depth ? (uint64)t.depth->format + 1 : 0);
			auto it = s_pipelines.find(key);
			if (it != s_pipelines.end())
				return it->second;

			// vertex input
			std::vector<VkVertexInputAttributeDescription> attrs;
			std::vector<VkVertexInputBindingDescription> bindings;
			for (auto& g : fetch->bufferGroups)
			{
				std::optional<LatteConst::VertexFetchType2> fetchType;
				for (sint32 j = 0; j < g.attribCount; j++)
				{
					auto& a = g.attrib[j];
					uint32 location = vs->mapping.attributeMapping[a.semanticId];
					if (location == (uint32)-1)
						continue;
					attrs.push_back({ location, a.attributeBufferIndex, VertexFormat(a.format), a.offset });
					if (!fetchType)
						fetchType = a.fetchType;
				}
				uint32 stride = (raw[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >> 11) & 0xFFFF;
				bindings.push_back({ g.attributeBufferIndex, stride,
					fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX });
			}
			VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			vin.vertexBindingDescriptionCount = (uint32)bindings.size();
			vin.pVertexBindingDescriptions = bindings.data();
			vin.vertexAttributeDescriptionCount = (uint32)attrs.size();
			vin.pVertexAttributeDescriptions = attrs.data();

			// input assembly (quads and quad strips arrive as triangle lists, see DecodeIndices)
			using P = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
			VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			ia.primitiveRestartEnable = VK_TRUE;
			switch (prim)
			{
			case P::POINTS: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; ia.primitiveRestartEnable = VK_FALSE; break;
			case P::LINES: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; ia.primitiveRestartEnable = VK_FALSE; break;
			case P::LINE_STRIP: case P::LINE_LOOP: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
			case P::TRIANGLES: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; ia.primitiveRestartEnable = VK_FALSE; break;
			case P::TRIANGLE_FAN: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
			case P::TRIANGLE_STRIP: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
			default: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; ia.primitiveRestartEnable = VK_FALSE; break;
			}

			VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			vp.viewportCount = vp.scissorCount = 1;

			// rasterizer
			const auto& pm = r.PA_SU_SC_MODE_CNTL;
			VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			rs.rasterizerDiscardEnable = r.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL();
			if (!r.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())             // GX2SetSpecialState(0, true) workaround
				rs.rasterizerDiscardEnable = VK_FALSE;
			rs.polygonMode = VK_POLYGON_MODE_FILL;
			rs.depthClampEnable = VK_TRUE;
			rs.lineWidth = 1.0f;
			rs.depthBiasEnable = pm.get_OFFSET_FRONT_ENABLED() ? VK_TRUE : VK_FALSE;
			uint32 cullFront = pm.get_CULL_FRONT(), cullBack = pm.get_CULL_BACK();
			rs.cullMode = (cullFront && cullBack) ? VK_CULL_MODE_FRONT_AND_BACK : cullFront ? VK_CULL_MODE_FRONT_BIT
				: cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
			rs.frontFace = pm.get_FRONT_FACE() == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
			VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT };
			clip.depthClipEnable = !r.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE();
			if (s.depthClip)
				rs.pNext = &clip;
			VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			// blend
			uint32 blendMask = r.CB_COLOR_CONTROL.get_BLEND_MASK(), targetMask = r.CB_TARGET_MASK.get_MASK();
			VkPipelineColorBlendAttachmentState blends[8]{};
			VkFormat colorFormats[8]{};
			uint32 colorCount = 0;
			for (uint32 i = 0; i < 8; i++)
			{
				colorFormats[i] = t.color[i] ? t.color[i]->format : VK_FORMAT_UNDEFINED;
				if (t.color[i])
					colorCount = i + 1;
				auto& b = blends[i];
				const auto& bc = r.CB_BLENDN_CONTROL[i];
				b.blendEnable = (blendMask & (1 << i)) && !IsIntegerFormat(colorFormats[i]);
				b.colorWriteMask = (targetMask >> (i * 4)) & 0xF;
				b.colorBlendOp = BlendOp((uint32)bc.get_COLOR_COMB_FCN());
				b.srcColorBlendFactor = BlendFactor((uint32)bc.get_COLOR_SRCBLEND());
				b.dstColorBlendFactor = BlendFactor((uint32)bc.get_COLOR_DSTBLEND());
				if (bc.get_SEPARATE_ALPHA_BLEND())
				{
					b.alphaBlendOp = BlendOp((uint32)bc.get_ALPHA_COMB_FCN());
					b.srcAlphaBlendFactor = BlendFactor((uint32)bc.get_ALPHA_SRCBLEND());
					b.dstAlphaBlendFactor = BlendFactor((uint32)bc.get_ALPHA_DSTBLEND());
				}
				else
				{
					b.alphaBlendOp = b.colorBlendOp;
					b.srcAlphaBlendFactor = b.srcColorBlendFactor;
					b.dstAlphaBlendFactor = b.dstColorBlendFactor;
				}
			}
			VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			auto rop = r.CB_COLOR_CONTROL.get_ROP();
			cb.logicOpEnable = rop != Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY;
			cb.logicOp = rop == Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::SET ? VK_LOGIC_OP_SET
				: rop == Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::CLEAR ? VK_LOGIC_OP_CLEAR
				: rop == Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::OR ? VK_LOGIC_OP_OR : VK_LOGIC_OP_COPY;
			cb.attachmentCount = colorCount;
			cb.pAttachments = blends;

			// depth and stencil
			const auto& dc = r.DB_DEPTH_CONTROL;
			VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			ds.depthTestEnable = dc.get_Z_ENABLE();
			ds.depthWriteEnable = dc.get_Z_WRITE_ENABLE();
			ds.depthCompareOp = kCompare[(uint32)dc.get_Z_FUNC()];
			ds.maxDepthBounds = 1.0f;
			ds.stencilTestEnable = dc.get_STENCIL_ENABLE();
			auto face = [&](bool back) {
				VkStencilOpState o{};
				const auto& f = r.DB_STENCILREFMASK;
				const auto& b = r.DB_STENCILREFMASK_BF;
				bool useBack = back && dc.get_BACK_STENCIL_ENABLE();
				o.reference = useBack ? b.get_STENCILREF_B() : f.get_STENCILREF_F();
				o.compareMask = useBack ? b.get_STENCILMASK_B() : f.get_STENCILMASK_F();
				o.writeMask = useBack ? b.get_STENCILWRITEMASK_B() : f.get_STENCILWRITEMASK_F();
				o.compareOp = kCompare[(uint32)(useBack ? dc.get_STENCIL_FUNC_B() : dc.get_STENCIL_FUNC_F())];
				o.depthFailOp = kStencilOp[(uint32)(useBack ? dc.get_STENCIL_ZFAIL_B() : dc.get_STENCIL_ZFAIL_F())];
				o.failOp = kStencilOp[(uint32)(useBack ? dc.get_STENCIL_FAIL_B() : dc.get_STENCIL_FAIL_F())];
				o.passOp = kStencilOp[(uint32)(useBack ? dc.get_STENCIL_ZPASS_B() : dc.get_STENCIL_ZPASS_F())];
				return o;
			};
			ds.front = face(false);
			ds.back = face(true);

			VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BIAS };
			VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
			dy.dynamicStateCount = 4;
			dy.pDynamicStates = dyn;

			VkPipelineRenderingCreateInfo rinfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
			rinfo.colorAttachmentCount = colorCount;
			rinfo.pColorAttachmentFormats = colorFormats;
			rinfo.depthAttachmentFormat = t.depth ? t.depth->format : VK_FORMAT_UNDEFINED;
			rinfo.stencilAttachmentFormat = t.depth && (t.depth->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? t.depth->format : VK_FORMAT_UNDEFINED;

			VkPipelineShaderStageCreateInfo stages[2]{};
			stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs->module, "main", nullptr };
			stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps->module, "main", nullptr };
			VkGraphicsPipelineCreateInfo pi{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
			pi.pNext = &rinfo;
			pi.stageCount = 2;
			pi.pStages = stages;
			pi.pVertexInputState = &vin;
			pi.pInputAssemblyState = &ia;
			pi.pViewportState = &vp;
			pi.pRasterizationState = &rs;
			pi.pMultisampleState = &ms;
			pi.pDepthStencilState = &ds;
			pi.pColorBlendState = &cb;
			pi.pDynamicState = &dy;
			pi.layout = layout;
			VkPipeline pipeline;
			Check(vkCreateGraphicsPipelines(s.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline), "vkCreateGraphicsPipelines");
			return s_pipelines[key] = pipeline;
		}

		// ---- guest data ----------------------------------------------------------------------------------
		// indices (LatteIndices_decode): swapped to host order, quads unpacked to triangles
		struct Indices { VkIndexType type = VK_INDEX_TYPE_NONE_KHR; uint32 count = 0, max = 0; VkDeviceSize offset = 0; };

		template<typename T>
		void Decode(const uint8* src, bool bigEndian, uint32 count, bool quads, bool quadStrip, T* dst, uint32& outCount, uint32& max)
		{
			auto get = [&](uint32 i) -> uint32 {
				T v;
				memcpy(&v, src + i * sizeof(T), sizeof(T));
				if (bigEndian)
					v = sizeof(T) == 2 ? (T)_swapEndianU16((uint16)v) : (T)_swapEndianU32((uint32)v);
				return v;
			};
			outCount = 0;
			if (quads)
			{
				for (uint32 q = 0; q + 3 < count; q += 4)
				{
					uint32 i0 = get(q), i1 = get(q + 1), i2 = get(q + 2), i3 = get(q + 3);
					for (uint32 v : { i0, i1, i2, i0, i2, i3 })
						dst[outCount++] = (T)v;
					max = std::max({ max, i0, i1, i2, i3 });
				}
			}
			else if (quadStrip)
			{
				for (uint32 q = 0; count >= 4 && q + 3 < count; q += 2)
				{
					uint32 i0 = get(q), i1 = get(q + 1), i2 = get(q + 2), i3 = get(q + 3);
					for (uint32 v : { i0, i1, i2, i2, i1, i3 })
						dst[outCount++] = (T)v;
					max = std::max({ max, i0, i1, i2, i3 });
				}
			}
			else
			{
				uint32 restart = LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();
				for (uint32 i = 0; i < count; i++)
				{
					uint32 v = get(i);
					dst[outCount++] = (T)v;
					if (v != restart)
						max = std::max(max, v);
				}
			}
		}

		Indices DecodeIndices(MPTR physIndices, uint32 count, Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE prim)
		{
			using P = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
			using I = Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE;
			Indices out;
			bool quads = prim == P::QUADS, strip = prim == P::QUAD_STRIP;
			I type = physIndices ? LatteGPUState.contextNew.VGT_DMA_INDEX_TYPE.get_INDEX_TYPE() : I::AUTO;
			if (type == I::AUTO && !quads && !strip)
			{
				out.count = count;
				out.max = std::max(count, 1u) - 1;
				return out;
			}
			bool u32 = type == I::U32_BE || type == I::U32_LE || (type == I::AUTO && count > 0xFFFF);
			uint32 outMax = quads ? count / 4 * 6 : strip ? (count >= 2 ? (count - 2) / 2 * 6 : 0) : count;
			uint32 elem = u32 ? 4 : 2;
			out.offset = RingAlloc((VkDeviceSize)outMax * elem + 4, 4);
			uint8* dst = s.ring.data + out.offset;
			if (type == I::AUTO)                                        // auto-generated quads
			{
				std::vector<uint32> seq(count);
				for (uint32 i = 0; i < count; i++)
					seq[i] = i;
				if (u32)
					Decode<uint32>((const uint8*)seq.data(), false, count, quads, strip, (uint32*)dst, out.count, out.max);
				else
				{
					std::vector<uint16> seq16(seq.begin(), seq.end());
					Decode<uint16>((const uint8*)seq16.data(), false, count, quads, strip, (uint16*)dst, out.count, out.max);
				}
			}
			else
			{
				// Cemu reads the index data at the packet's address as a virtual one
				const uint8* src = memory_getPointerFromVirtualOffset(physIndices);
				bool be = type == I::U16_BE || type == I::U32_BE;
				if (u32)
					Decode<uint32>(src, be, count, quads, strip, (uint32*)dst, out.count, out.max);
				else
					Decode<uint16>(src, be, count, quads, strip, (uint16*)dst, out.count, out.max);
			}
			out.type = u32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
			return out;
		}

		// uniform variables (uniformData_updateUniformVars, LatteBufferCache_LoadRemappedUniforms)
		VkDeviceSize UniformVars(Shader& sh, bool vertex, float viewportW, float viewportH)
		{
			uint32 size = sh.uniforms.offset_endOfBlock;
			std::vector<uint8> data(size, 0);
			auto at = [&](sint32 offset) { return data.data() + offset; };
			const auto& r = LatteGPUState.contextNew;
			uint32* regs = LatteGPUState.contextRegister;
			for (sint32 t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++)
				if (sh.uniforms.offset_texScale[t] >= 0)
				{
					float one[2] = { 1.0f, 1.0f };                     // textures are never resized here
					memcpy(at(sh.uniforms.offset_texScale[t]), one, 8);
				}
			if (sh.uniforms.offset_alphaTestRef >= 0)
			{
				float ref = r.SX_ALPHA_REF.get_ALPHA_TEST_REF();
				memcpy(at(sh.uniforms.offset_alphaTestRef), &ref, 4);
			}
			if (sh.uniforms.offset_pointSize >= 0)
			{
				float w = (float)r.PA_SU_POINT_SIZE.get_WIDTH() / 8.0f;
				if (w == 0.0f)
					w = 1.0f / 8.0f;
				memcpy(at(sh.uniforms.offset_pointSize), &w, 4);
			}
			uint32 aluConst = vertex ? 0x400 : 0;
			if (sh.uniforms.offset_remapped >= 0)
			{
				uint8* dst = at(sh.uniforms.offset_remapped);
				if (r.SQ_CONFIG.get_DX9_CONSTS())
				{
					uint32* base = regs + mmSQ_ALU_CONSTANT0_0 + aluConst;
					for (auto& e : sh.dec->list_remappedUniformEntries_register)
						memcpy(dst + e.mappedIndexOffset, (uint8*)(base + e.indexOffset / 4), 16);
				}
				else
				{
					uint32 blockRegs = vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
					for (auto& g : sh.dec->list_remappedUniformEntries_bufferGroups)
					{
						MPTR phys = regs[blockRegs + g.kcacheBankIdOffset / 4];
						for (auto& e : g.entries)
						{
							if (phys)
								memcpy(dst + e.mappedIndexOffset, memory_base + phys + e.indexOffset, 16);
							else
								memset(dst + e.mappedIndexOffset, 0, 16);
						}
					}
				}
			}
			if (sh.uniforms.offset_uniformRegister >= 0)
				memcpy(at(sh.uniforms.offset_uniformRegister), regs + mmSQ_ALU_CONSTANT0_0 + aluConst, sh.uniforms.count_uniformRegister * 16);
			if (sh.uniforms.offset_windowSpaceToClipSpaceTransform >= 0)
			{
				float v[2] = { 2.0f / (float)(sint32)viewportW, 2.0f / (float)(sint32)viewportH };  // the guest viewport, as ints
				memcpy(at(sh.uniforms.offset_windowSpaceToClipSpaceTransform), v, 8);
			}
			if (sh.uniforms.offset_fragCoordScale >= 0)
			{
				float v[4] = { 1.0f, 1.0f, (float)(sint32)viewportW, (float)(sint32)viewportH };
				memcpy(at(sh.uniforms.offset_fragCoordScale), v, 16);
			}
			VkDeviceSize off = RingAlloc(std::max<uint32>(size, 16), s.props.limits.minUniformBufferOffsetAlignment);
			memcpy(s.ring.data + off, data.data(), size);
			return off;
		}

		// uniform blocks (LatteBufferCache_syncGPUUniformBuffers): the range the shader reads
		VkDeviceSize UniformBlock(Shader& sh, bool vertex, uint32 index)
		{
			uint32 blockRegs = vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
			MPTR phys = LatteGPUState.contextRegister[blockRegs + index * 7 + 0];
			uint32 size = LatteGPUState.contextRegister[blockRegs + index * 7 + 1] + 1;
			for (auto& q : sh.dec->list_quickBufferList)
				if (q.index == index)
					size = std::min<uint32>(size, q.size);
			VkDeviceSize off = RingAlloc(65536, s.props.limits.minUniformBufferOffsetAlignment);
			memset(s.ring.data + off, 0, 65536);
			if (phys)
				memcpy(s.ring.data + off, memory_getPointerFromPhysicalOffset(phys), std::min<uint32>(size, 65536));
			return off;
		}

		// the stage's textures, before anything else of the draw is allocated (they may upload)
		std::vector<VkDescriptorImageInfo> Textures(Shader& sh, bool vertex, std::span<Image* const> attachments)
		{
			std::vector<VkDescriptorImageInfo> images(sh.mapping.getTextureCount());
			for (sint32 i = 0; i < sh.mapping.getTextureCount(); i++)
			{
				Sampled t = SampleTexture(sh.dec, vertex, sh.mapping.getRelativeTextureUnitFromRelativeBindingPoint(i), attachments);
				images[i] = { t.sampler, t.view, t.layout };
			}
			return images;
		}

		uint32 s_sets = 0, s_imageDescriptors = 0, s_bufferDescriptors = 0;  // allocated from the pool since the last submit

		VkDescriptorSet Descriptors(Shader& sh, bool vertex, const std::vector<VkDescriptorImageInfo>& images,
			std::vector<uint32>& dynamicOffsets, float vpW, float vpH)
		{
			VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			ai.descriptorPool = s.descriptors;
			ai.descriptorSetCount = 1;
			ai.pSetLayouts = &sh.layout;
			VkDescriptorSet set;
			Check(vkAllocateDescriptorSets(s.device, &ai, &set), "vkAllocateDescriptorSets");  // Reserve() made room
			s_sets++;
			s_imageDescriptors += (uint32)images.size();
			s_bufferDescriptors += (uint32)sh.uniformBuffers.size() + 1;
			std::vector<VkWriteDescriptorSet> writes;
			for (sint32 i = 0; i < sh.mapping.getTextureCount(); i++)
			{
				VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
				w.dstSet = set;
				w.dstBinding = sh.mapping.getTextureBaseBindingPoint() + i;
				w.descriptorCount = 1;
				w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				w.pImageInfo = &images[i];
				writes.push_back(w);
			}
			VkDescriptorBufferInfo vars{ s.ring.buffer, 0, sh.uniforms.offset_endOfBlock > 0 ? (VkDeviceSize)sh.uniforms.offset_endOfBlock : 16 };
			VkDescriptorBufferInfo blocks{ s.ring.buffer, 0, 65536 };
			if (sh.mapping.uniformVarsBufferBindingPoint >= 0)
			{
				VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
				w.dstSet = set;
				w.dstBinding = sh.mapping.uniformVarsBufferBindingPoint;
				w.descriptorCount = 1;
				w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
				w.pBufferInfo = &vars;
				writes.push_back(w);
				dynamicOffsets.push_back((uint32)UniformVars(sh, vertex, vpW, vpH));
			}
			for (uint8 i : sh.uniformBuffers)
			{
				VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
				w.dstSet = set;
				w.dstBinding = sh.mapping.uniformBuffersBindingPoint[i];
				w.descriptorCount = 1;
				w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
				w.pBufferInfo = &blocks;
				writes.push_back(w);
				dynamicOffsets.push_back((uint32)UniformBlock(sh, vertex, i));
			}
			vkUpdateDescriptorSets(s.device, (uint32)writes.size(), writes.data(), 0, nullptr);
			return set;
		}

		uint64 s_draws = 0, s_skipped = 0;
		uint32 s_statsEvery = 0;

		// room for one draw's per-draw data (a submit in the middle of a draw would free what it already allocated)
		void Reserve()
		{
			if (s.ring.used + (64ull << 20) > s.ring.size || s_sets + 2 > (1u << 16) || s_imageDescriptors + 64 > (1u << 18) ||
				s_bufferDescriptors + 64 > (1u << 17))
				SubmitAndWait();
		}
	}

	void DrawInit()
	{
		InstallNoopRenderer();
		glslang::InitializeProcess();
		TextureInit();
		if (const char* e = getenv("WWHD_RENDER_STATS"))
			s_statsEvery = (uint32)atoi(e);
	}

	void EndRendering()
	{
		if (s_rendering)
		{
			vkCmdEndRendering(s.cmd);
			s_rendering = false;
		}
	}

	void OnSubmitted()
	{
		s_sets = s_imageDescriptors = s_bufferDescriptors = 0;
	}

	// WWHD_RENDER_STATS=N: a line every N frames
	void DrawStats(uint32 frame)
	{
		if (s_statsEvery && frame % s_statsEvery == 0)
			Log(fmt::format("frame {}: {} draws, {} skipped; {} shaders, {} pipelines", frame, s_draws, s_skipped, s_shaders.size(),
				s_pipelines.size()));
	}

	// IT_DRAW_INDEX_2 (body: ?, index address, ?, count, ?) and IT_DRAW_INDEX_AUTO (body: count, ?),
	// as LatteCP_itDrawIndex2/Auto and DrawPassContext::executeDraw read them
	void RendererDraw(uint32 op, const uint32be* body, uint32 nWords)
	{
		auto skip = [&](const std::string& why) {
			s_skipped++;
			LogOnce(why, [&] { return "draw skipped: " + why; });
		};
		if (op == IT_DRAW_INDEX_IMMD || nWords < (op == IT_DRAW_INDEX_AUTO ? 1u : 4u))
			return skip("immediate-index draws");
		const bool autoIndex = op == IT_DRAW_INDEX_AUTO;
		const uint32 count = autoIndex ? (uint32)body[0] : (uint32)body[3];
		const MPTR physIndices = autoIndex ? MPTR_NULL : (MPTR)(uint32)body[1];
		if (count == 0)
			return;
		const auto& r = LatteGPUState.contextNew;
		uint32* regs = LatteGPUState.contextRegister;
		if (r.GetSpecialStateValues()[8] != 0 || r.GetSpecialStateValues()[5] != 0)
			return skip("GX2 special state 5/8 (clear as depth, surface copy draws)");
		auto prim = r.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
		if (prim == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS)
			return skip("rect primitives (need a geometry shader emulation)");
		if (r.VGT_GS_MODE.get_MODE() != Latte::LATTE_VGT_GS_MODE::E_MODE::OFF || regs[mmVGT_STRMOUT_EN])
			return skip("geometry shaders or stream-out");
		const auto& sq = r.SQ_CONFIG;
		if (autoIndex && sq.get_PS_PRIO() == 0 && sq.get_VS_PRIO() == 1 && sq.get_GS_PRIO() == 2 && sq.get_ES_PRIO() == 3)
			return skip("compute draws (Cemu doesn't run them either)");
		if (r.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() && r.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())
			return;                                                 // no-ops, as in Cemu

		// shaders: fetch shader, then vertex and pixel with the keys Cemu's shader cache would use
		LatteShader_UpdatePSInputs(regs);
		uint64 psInputs = LatteSHRC_GetPSInputTable()->key;
		auto program = [&](uint32 startReg, const uint8*& code, uint32& size) {
			uint32 addr = regs[startReg] << 8;
			size = regs[startReg + 1] << 3;
			code = addr ? memory_getPointerFromPhysicalOffset(addr) : nullptr;
			return code && size;
		};
		const uint8 *fsCode, *vsCode, *psCode;
		uint32 fsSize, vsSize, psSize;
		if (!program(mmSQ_PGM_START_FS, fsCode, fsSize) || !program(mmSQ_PGM_START_VS, vsCode, vsSize) ||
			!program(mmSQ_PGM_START_PS, psCode, psSize))
			return skip("a draw without fetch, vertex or pixel shader");
		uint64 fsHash = Fnv(fsCode, fsSize);
		LatteFetchShader*& fetch = s_fetchShaders[fsHash];
		if (!fetch)
			fetch = LatteShaderRecompiler_createFetchShader(LatteFetchShader::CalculateCacheHash((void*)fsCode, fsSize), regs, (uint32*)fsCode, fsSize);
		uint64 vsKey = Mix(Mix(Mix(Fnv(vsCode, vsSize), fetch->key), psInputs), regs[Latte::REGADDR::PA_CL_VTE_CNTL] ^ 0x43F);
		vsKey = Mix(Mix(Mix(vsKey, (uint64)prim == (uint64)Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::POINTS),
			r.PA_CL_CLIP_CNTL.get_DX_CLIP_SPACE_DEF()), 1);
		uint64 psKey = Mix(Mix(Mix(Fnv(psCode, psSize), psInputs), regs[mmCB_SHADER_MASK]), regs[Latte::REGADDR::SX_ALPHA_TEST_CONTROL] & 0xF);
		for (uint32 u = 0; u < LATTE_NUM_MAX_TEX_UNITS; u++)
		{
			vsKey = Mix(vsKey, regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS + u * 7 + 4] & 0x300);
			psKey = Mix(Mix(psKey, regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + u * 7] & 7),
				regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + u * 7 + 4] & 0x300);
		}
		Shader* vs = GetShader(true, vsKey, vsCode, vsSize, fetch);
		Shader* ps = GetShader(false, psKey | 1, psCode, psSize, nullptr);
		if (!vs->module || !ps->module)
			return skip("a shader that failed to translate");

		// render targets (LatteMRT::UpdateCurrentFBO)
		Targets t;
		uint8 colorMask = LatteMRT::GetActiveColorBufferMask(ps->dec, r);
		for (uint32 i = 0; i < 8; i++)
		{
			if (!(colorMask & (1 << i)))
				continue;
			uint32 base = regs[mmCB_COLOR0_BASE + i] & 0xFFFFFF00, size = regs[mmCB_COLOR0_SIZE + i], info = regs[mmCB_COLOR0_INFO + i];
			if (Latte::TM_IsMacroTiled((Latte::E_HWTILEMODE)((info >> 8) & 0xF)))
				base &= ~0x700u;
			uint32 pitch = ((size & 0x3FF) + 1) << 3, height = ((((size >> 10) & 0xFFFFF) + 1) << 6) / pitch;
			t.color[i] = &Surface(base, (uint32)LatteMRT::GetColorBufferFormat(i, r), false, pitch, height);
		}
		uint32 scissorX = r.PA_SC_GENERIC_SCISSOR_TL.get_TL_X(), scissorY = r.PA_SC_GENERIC_SCISSOR_TL.get_TL_Y();
		uint32 scissorR = r.PA_SC_GENERIC_SCISSOR_BR.get_BR_X(), scissorB = r.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y();
		if (LatteMRT::GetActiveDepthBufferMask(r))
		{
			uint32 base = regs[mmDB_HTILE_DATA_BASE] << 8, size = regs[mmDB_DEPTH_SIZE];
			uint32 pitch = (size & 0x3FF) + 1, height = ((((size >> 10) & 0xFFFFF) + 1) / pitch) << 3;
			pitch <<= 3;
			if (base && scissorR <= pitch && scissorB <= height)
				t.depth = &Surface(base, (uint32)LatteMRT::GetDepthBufferFormat(r), true, std::max(pitch, 2u), std::max(height, 2u));
		}
		if (!colorMask && !t.depth)
			return skip("nothing to draw into");

		// textures first: they may end rendering, upload, or submit
		Image* attachments[9];
		uint32 nAttachments = 0;
		for (Image* c : t.color)
			if (c)
				attachments[nAttachments++] = c;
		if (t.depth)
			attachments[nAttachments++] = t.depth;
		std::vector<VkDescriptorImageInfo> vsImages = Textures(*vs, true, { attachments, nAttachments });
		std::vector<VkDescriptorImageInfo> psImages = Textures(*ps, false, { attachments, nAttachments });
		Reserve();

		Indices idx = DecodeIndices(physIndices, count, prim);
		// vertex buffers: the range the draw can reach (LatteBufferCache_Sync)
		uint32 baseVertex = regs[mmSQ_VTX_BASE_VTX_LOC], baseInstance = regs[mmSQ_VTX_START_INST_LOC];
		uint32 instances = r.VGT_DMA_NUM_INSTANCES.get_NUM_INSTANCES();
		std::vector<std::pair<uint32, VkDeviceSize>> vbufs;
		for (auto& g : fetch->bufferGroups)
		{
			uint32* b = regs + mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7;
			uint32 stride = (b[2] >> 11) & 0xFFFF;
			uint32 maxIndex = std::max(g.hasVtxIndexAccess ? idx.max + baseVertex : 0, g.hasInstanceIndexAccess ? baseInstance + instances - 1 : 0);
			uint32 bytes = ((stride * maxIndex + g.totalAttribRangeSize) + 127) & ~127u;
			VkDeviceSize off = RingAlloc(std::max(bytes, 128u), 16);
			if (b[0])
				memcpy(s.ring.data + off, memory_getPointerFromPhysicalOffset(b[0]), bytes);
			else
				memset(s.ring.data + off, 0, bytes);
			vbufs.push_back({ g.attributeBufferIndex, off });
		}

		// viewport (LatteRenderTarget_updateViewport, VulkanRenderer::renderTarget_setViewport)
		float vpW = r.PA_CL_VPORT_XSCALE.get_SCALE() / 0.5f, vpX = r.PA_CL_VPORT_XOFFSET.get_OFFSET() - r.PA_CL_VPORT_XSCALE.get_SCALE();
		float vpH = r.PA_CL_VPORT_YSCALE.get_SCALE() / -0.5f, vpY = r.PA_CL_VPORT_YOFFSET.get_OFFSET() + r.PA_CL_VPORT_YSCALE.get_SCALE();
		float zs = r.PA_CL_VPORT_ZSCALE.get_SCALE(), zb = r.PA_CL_VPORT_ZOFFSET.get_OFFSET();
		bool halfZ = r.PA_CL_CLIP_CNTL.get_DX_CLIP_SPACE_DEF();
		float farZ = zs + zb, nearZ = halfZ ? zb : zb - zs;

		std::vector<uint32> dynamicOffsets;
		VkDescriptorSet sets[2] = { Descriptors(*vs, true, vsImages, dynamicOffsets, vpW, vpH), VK_NULL_HANDLE };
		sets[1] = Descriptors(*ps, false, psImages, dynamicOffsets, vpW, vpH);
		VkPipelineLayout layout = PipelineLayout(vs, ps);
		VkPipeline pipeline = GetPipeline(vs, ps, fetch, layout, t, prim);

		BeginRendering(t);
		vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, (uint32)dynamicOffsets.size(), dynamicOffsets.data());
		for (auto& [binding, off] : vbufs)
			vkCmdBindVertexBuffers(s.cmd, binding, 1, &s.ring.buffer, &off);
		VkViewport viewport{ vpX, vpY + vpH, vpW, -vpH, std::clamp(nearZ, 0.0f, 1.0f), std::clamp(farZ, 0.0f, 1.0f) };
		vkCmdSetViewport(s.cmd, 0, 1, &viewport);
		VkRect2D scissor{ { (sint32)scissorX, (sint32)scissorY }, { scissorR - std::min(scissorX, scissorR), scissorB - std::min(scissorY, scissorB) } };
		vkCmdSetScissor(s.cmd, 0, 1, &scissor);
		vkCmdSetBlendConstants(s.cmd, (const float*)(regs + Latte::REGADDR::CB_BLEND_RED));
		vkCmdSetDepthBias(s.cmd, r.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET(), r.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP(),
			r.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE() / 16.0f);
		if (idx.type != VK_INDEX_TYPE_NONE_KHR)
		{
			vkCmdBindIndexBuffer(s.cmd, s.ring.buffer, idx.offset, idx.type);
			vkCmdDrawIndexed(s.cmd, idx.count, instances, 0, (sint32)baseVertex, baseInstance);
		}
		else
			vkCmdDraw(s.cmd, idx.count, instances, baseVertex, baseInstance);
		s_draws++;
	}
}
