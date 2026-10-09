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
#include <array>
#include <cstring>

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N& texUnitWord1,
	const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N& texUnitWord4);  // latte_glue.cpp

namespace wwhd::gpu
{
	void InstallNoopRenderer();                                    // latte_glue.cpp

	namespace
	{
		uint64 Mix(uint64 h, uint64 v) { return (h ^ v) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull; }

		uint64 HashBytes(const uint8* p, size_t n)
		{
			uint64 h = 0xCBF29CE484222325ull;
			size_t i = 0;
			for (; i + 8 <= n; i += 8)
			{
				uint64 w;
				memcpy(&w, p + i, 8);
				h = (h ^ w) * 0x100000001B3ull;
			}
			for (; i < n; i++)
				h = (h ^ p[i]) * 0x100000001B3ull;
			return h;
		}

		uint64 Fnv(const uint8* p, size_t n)
		{
			uint64 h = 0xCBF29CE484222325ull;
			for (size_t i = 0; i < n; i++)
				h = (h ^ p[i]) * 0x100000001B3ull;
			return h;
		}

		// A shader program's Fnv, once a frame per program: a frame's thousands of draws use a few
		// hundred programs, and hashing every draw's three byte by byte took a quarter of the GPU
		// thread's time facing Outset at 60 fps (D21). A program doesn't change within a frame.
		// Its first use in a frame compares the program with a copy of its bytes from the last hash
		// (memcmp, vectorised) and hashes again only when they differ, so the hash is always the
		// content's, as before; hashing every program byte by byte each frame was still 9% of the
		// render thread (docs/research/perf-baseline.md). The per-draw table is larger too: two
		// programs sharing a slot of the old 1,024 hashed each other out within a frame.
		uint64 ProgramHash(const uint8* code, uint32 size)
		{
			struct Entry { const uint8* code; uint32 size, frame; uint64 hash; };
			static std::array<Entry, 4096> s_memo{};
			Entry& e = s_memo[((uintptr_t)code >> 8) & (s_memo.size() - 1)];   // programs are 256-byte aligned
			if (e.code == code && e.size == size && e.frame == s.frame + 1)
				return e.hash;
			struct Known { std::vector<uint8> bytes; uint64 hash; };
			static std::unordered_map<const uint8*, Known> s_known;
			Known& k = s_known[code];
			if (k.bytes.size() != size || memcmp(k.bytes.data(), code, size) != 0)
			{
				k.bytes.assign(code, code + size);
				k.hash = Fnv(code, size);
			}
			e = { code, size, s.frame + 1, k.hash };
			return e.hash;
		}

		template<typename F>
		void LogOnce(const std::string& what, F&& msg)
		{
			static std::set<std::string> seen;
			if (seen.insert(what).second)
				Log(msg());
		}

		// ---- records for the cache on disk (shader_cache.cpp) -------------------------------------------
		struct Writer
		{
			std::vector<uint8> bytes;
			void Raw(const void* p, size_t n) { bytes.insert(bytes.end(), (const uint8*)p, (const uint8*)p + n); }
			template<typename T> void Pod(const T& v) { static_assert(std::is_trivially_copyable_v<T>); Raw(&v, sizeof(T)); }
			template<typename T> void Vec(const std::vector<T>& v)
			{
				Pod((uint32)v.size());
				if (!v.empty())
					Raw(v.data(), v.size() * sizeof(T));
			}
		};

		struct Reader
		{
			std::span<const uint8> bytes;
			size_t at = 0;
			bool ok = true;
			bool Raw(void* p, size_t n)
			{
				if (!ok || n > bytes.size() - at)
					return ok = false;
				memcpy(p, bytes.data() + at, n);
				at += n;
				return true;
			}
			template<typename T> bool Pod(T& v) { static_assert(std::is_trivially_copyable_v<T>); return Raw(&v, sizeof(T)); }
			template<typename T> bool Vec(std::vector<T>& v)
			{
				uint32 n = 0;
				if (!Pod(n) || n > (bytes.size() - at) / sizeof(T))
					return ok = false;
				v.resize(n);
				return n == 0 || Raw(v.data(), n * sizeof(T));
			}
			bool End() const { return ok && at == bytes.size(); }
		};

		// shaders translated and pipelines built during play (not prepared), for the real-time log
		FirstSights s_sights;
		double MsSince(std::chrono::steady_clock::time_point t)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
		}

		// ---- shaders -------------------------------------------------------------------------------
		struct Shader
		{
			uint64 key = 0;
			LatteDecompilerShader* dec = nullptr;
			LatteDecompilerOutputUniformOffsets uniforms;
			LatteDecompilerShaderResourceMapping mapping;
			VkShaderModule module = VK_NULL_HANDLE;
			VkDescriptorSetLayout layout = VK_NULL_HANDLE;
			std::vector<uint8> uniformBuffers;                       // uniform block indices, in binding order
		};
		std::unordered_map<uint64, Shader*> s_shaders;
		std::unordered_map<uint64, LatteFetchShader*> s_fetchShaders;
		std::map<std::pair<VkDescriptorSetLayout, VkDescriptorSetLayout>, VkPipelineLayout> s_layouts;
		std::unordered_map<uint64, VkPipeline> s_pipelines;        // by the registers they come from, this run
		std::unordered_map<uint64, VkPipeline> s_recipes;          // by recipe (PipelineDesc::Serialize), prepared or built

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

		// A shader as the cache on disk keeps it: its SPIR-V, the decompiler's uniform offsets and
		// resource mapping, and the fields of its analysis the draws read. Keep the list complete: a
		// draw that reads another field of dec must have it written here (and cache::kVersion bumped).
		std::vector<uint8> SerializeShader(const Shader& sh, bool vertex, const std::vector<uint32>& spirv)
		{
			Writer w;
			w.Pod(sh.key);
			w.Pod((uint8)vertex);
			w.Vec(spirv);
			w.Pod(sh.uniforms);
			w.Pod(sh.mapping);
			const LatteDecompilerShader& d = *sh.dec;
			w.Pod(d.pixelColorOutputMask);
			w.Pod(d.textureUnitDim);
			w.Pod(d.textureUnitSamplerAssignment);
			w.Pod(d.textureUsesDepthCompare);
			w.Vec(std::vector<LatteDecompilerShader::QuickBufferEntry>(d.list_quickBufferList.begin(), d.list_quickBufferList.end()));
			w.Vec(d.list_remappedUniformEntries_register);
			w.Pod((uint32)d.list_remappedUniformEntries_bufferGroups.size());
			for (auto& g : d.list_remappedUniformEntries_bufferGroups)
			{
				w.Pod(g.bufferId);
				w.Pod(g.kcacheBankIdOffset);
				w.Vec(g.entries);
			}
			return std::move(w.bytes);
		}

		// the shader again from its record, without the decompiler or glslang; nullptr if it's damaged
		Shader* LoadShader(std::span<const uint8> record)
		{
			Reader r{ record };
			uint64 key = 0;
			uint8 vertex = 0;
			std::vector<uint32> spirv;
			r.Pod(key);
			r.Pod(vertex);
			r.Vec(spirv);
			auto* d = new LatteDecompilerShader(vertex ? LatteConst::ShaderType::Vertex : LatteConst::ShaderType::Pixel);
			Shader* sh = new Shader;
			sh->key = key;
			sh->dec = d;
			r.Pod(sh->uniforms);
			r.Pod(sh->mapping);
			r.Pod(d->pixelColorOutputMask);
			r.Pod(d->textureUnitDim);
			r.Pod(d->textureUnitSamplerAssignment);
			r.Pod(d->textureUsesDepthCompare);
			std::vector<LatteDecompilerShader::QuickBufferEntry> quick;
			if (r.Vec(quick) && quick.size() <= LATTE_NUM_MAX_UNIFORM_BUFFERS)
				for (auto& q : quick)
					d->list_quickBufferList.push_back(q);
			else
				r.ok = false;
			r.Vec(d->list_remappedUniformEntries_register);
			uint32 groups = 0;
			r.Pod(groups);
			for (uint32 i = 0; r.ok && i < groups; i++)
			{
				uint16 id = 0, bank = 0;
				r.Pod(id);
				r.Pod(bank);
				r.Vec(d->list_remappedUniformEntries_bufferGroups.emplace_back(id, bank).entries);
			}
			VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			mi.codeSize = spirv.size() * 4;
			mi.pCode = spirv.data();
			if (!r.End() || spirv.empty() || vkCreateShaderModule(s.device, &mi, nullptr, &sh->module) != VK_SUCCESS)
			{
				delete d;
				delete sh;
				return nullptr;
			}
			sh->layout = CreateLayout(*sh, vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT);
			return sh;
		}

		constexpr size_t kRegisterCount = sizeof(LatteGPUState.contextRegister) / sizeof(uint32);

		// A shader's key: its program, and the registers its translation depends on (as Cemu's
		// shader cache keys it). The PS input table must be the registers' (LatteShader_UpdatePSInputs).
		uint64 VertexKey(const uint32* regs, uint64 program, const LatteFetchShader* fetch)
		{
			const auto& r = *(const LatteContextRegister*)regs;
			uint64 key = Mix(Mix(Mix(program, fetch->key), LatteSHRC_GetPSInputTable()->key), regs[Latte::REGADDR::PA_CL_VTE_CNTL] ^ 0x43F);
			key = Mix(Mix(Mix(key, r.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE() == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::POINTS),
				r.PA_CL_CLIP_CNTL.get_DX_CLIP_SPACE_DEF()), 1);
			for (uint32 u = 0; u < LATTE_NUM_MAX_TEX_UNITS; u++)
				key = Mix(key, regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS + u * 7 + 4] & 0x300);
			return key;
		}

		uint64 PixelKey(const uint32* regs, uint64 program)
		{
			uint64 key = Mix(Mix(Mix(program, LatteSHRC_GetPSInputTable()->key), regs[mmCB_SHADER_MASK]),
				regs[Latte::REGADDR::SX_ALPHA_TEST_CONTROL] & 0xF);
			for (uint32 u = 0; u < LATTE_NUM_MAX_TEX_UNITS; u++)
				key = Mix(Mix(key, regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + u * 7] & 7),
					regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + u * 7 + 4] & 0x300);
			return key | 1;
		}

		// Cemu's decompiler on the register file (and the PS input table), then glslang: sh gets the
		// decompiler's output and spirv the module's code. False, and a line in the log once, if either fails.
		bool Translate(bool vertex, uint64 key, const uint8* code, uint32 size, LatteFetchShader* fetch, Shader& sh, std::vector<uint32>& spirv)
		{
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
			sh.key = key;
			sh.dec = out.shader;
			sh.uniforms = out.uniformOffsetsVK;
			sh.mapping = out.resourceMappingVK;
			if (!sh.dec || sh.dec->hasError || !sh.dec->strBuf_shaderSource)
			{
				LogOnce(fmt::format("dec{:x}", key), [&] { return fmt::format("{} shader {:016x}: decompiler error, its draws are skipped",
					vertex ? "vertex" : "pixel", key); });
				return false;
			}
			std::string glsl = sh.dec->strBuf_shaderSource->c_str();
			// WWHD_RENDER_SHADERS=dir: every shader's GLSL as dir/<key>.<vs|ps>.glsl (the key is the one
			// WWHD_RENDER_TRACE prints)
			static const char* shaderDir = getenv("WWHD_RENDER_SHADERS");
			if (shaderDir && *shaderDir)
				if (FILE* f = fopen(fmt::format("{}/{:016x}.{}.glsl", shaderDir, key, vertex ? "vs" : "ps").c_str(), "wb"))
				{
					fwrite(glsl.data(), 1, glsl.size(), f);
					fclose(f);
				}
			std::string log;
			if (!CompileSpirv(glsl, vertex ? EShLangVertex : EShLangFragment, spirv, log))
			{
				LogOnce(fmt::format("glsl{:x}", key), [&] { return fmt::format("{} shader {:016x}: GLSL doesn't compile ({}), its draws are skipped",
					vertex ? "vertex" : "pixel", key, log.substr(0, 200)); });
				return false;
			}
			return true;
		}

		// a translated shader becomes one to draw with, and a record in the cache on disk
		void Finish(Shader& sh, bool vertex, const std::vector<uint32>& spirv)
		{
			VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			mi.codeSize = spirv.size() * 4;
			mi.pCode = spirv.data();
			Check(vkCreateShaderModule(s.device, &mi, nullptr, &sh.module), "vkCreateShaderModule");
			sh.layout = CreateLayout(sh, vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT);
			cache::AddShader(SerializeShader(sh, vertex, spirv));
		}

		// WWHD_SHADER_SOURCES (shader_list.cpp): the lines for a shader just translated, once the
		// registers its line keeps are shown to be enough: alone in the register file, they give the
		// same key and translate to the same record.
		std::set<uint64> s_capturedPrograms;
		uint32 s_capturedShaders = 0, s_captureFailures = 0;
		void CaptureShader(bool vertex, const Shader& sh, const std::vector<uint32>& spirv, const uint8* code, uint32 size,
			LatteFetchShader* fetch, uint64 fetchHash)
		{
			uint32* regs = LatteGPUState.contextRegister;
			uint64 program = Fnv(code, size);
			std::string listed = shaderlist::Registers(regs);
			std::vector<uint32> saved(regs, regs + kRegisterCount);
			shaderlist::SetRegisters(regs, kRegisterCount, listed);
			LatteShader_UpdatePSInputs(regs);
			Shader again;
			std::vector<uint32> spirvAgain;
			bool sameKey = (vertex ? VertexKey(regs, program, fetch) : PixelKey(regs, program)) == sh.key;
			bool same = sameKey && Translate(vertex, sh.key, code, size, fetch, again, spirvAgain) &&
				SerializeShader(again, vertex, spirvAgain) == SerializeShader(sh, vertex, spirv);
			memcpy(regs, saved.data(), kRegisterCount * sizeof(uint32));
			LatteShader_UpdatePSInputs(regs);
			s_capturedShaders++;
			if (!same)
			{
				if (s_captureFailures++ < 20)
					Log(fmt::format("shader list: {} shader {:016x} doesn't {} from the registers its line keeps", vertex ? "vertex" : "pixel",
						sh.key, sameKey ? "translate the same" : "get the same key"));
				return;
			}
			if (s_capturedPrograms.insert(program).second)
				shaderlist::Capture(fmt::format("program {:016x} {}", program, size));
			shaderlist::Capture(vertex ? fmt::format("vs {:016x} {:016x} {:016x} {}", sh.key, program, fetchHash, listed)
				: fmt::format("ps {:016x} {:016x} {}", sh.key, program, listed));
		}

		Shader* GetShader(bool vertex, uint64 key, const uint8* code, uint32 size, LatteFetchShader* fetch, uint64 fetchHash)
		{
			auto it = s_shaders.find(key);
			if (it != s_shaders.end())
				return it->second;
			auto start = std::chrono::steady_clock::now();
			Shader* sh = new Shader;
			s_shaders[key] = sh;
			std::vector<uint32> spirv;
			if (!Translate(vertex, key, code, size, fetch, *sh, spirv))
				return sh;
			Finish(*sh, vertex, spirv);
			if (shaderlist::Capturing())
				CaptureShader(vertex, *sh, spirv, code, size, fetch, fetchHash);
			s_sights.shaders++;
			s_sights.shaderMs += MsSince(start);
			return sh;
		}

		// ---- render targets (dynamic rendering) --------------------------------------------------------
		struct Targets
		{
			Image* color[8]{};
			Image* depth = nullptr;
			uint32 colorLayer[8]{}, depthLayer = 0;                 // array slices (CB_COLORn_VIEW, DB_DEPTH_VIEW)
			bool operator==(const Targets& o) const
			{
				return depth == o.depth && depthLayer == o.depthLayer && std::equal(std::begin(color), std::end(color), std::begin(o.color)) &&
					std::equal(std::begin(colorLayer), std::end(colorLayer), std::begin(o.colorLayer));
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
			for (Image* c : t.color)
				if (c)
					w = std::min(w, c->width), h = std::min(h, c->height);
			if (t.depth)
				w = std::min(w, t.depth->width), h = std::min(h, t.depth->height);
			// a pending whole-image clear (WWHD_CLEAR_LOADOP) becomes this pass's load op when the pass covers the image;
			// otherwise Transition records it first
			auto loadOp = [&](Image& img, uint32 layer, VkRenderingAttachmentInfo& a) {
				if (img.clearPending && layer == 0 && img.width == w && img.height == h)
				{
					img.clearPending = false;
					a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
					a.clearValue = img.clearValue;
				}
			};
			VkRenderingAttachmentInfo colors[8]{};
			uint32 count = 0;
			for (uint32 i = 0; i < 8; i++)
			{
				colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				if (!t.color[i])
					continue;
				colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
				loadOp(*t.color[i], t.colorLayer[i], colors[i]);
				Transition(*t.color[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
				colors[i].imageView = LayerView(*t.color[i], t.colorLayer[i]);
				colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				count = i + 1;
			}
			VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			if (t.depth)
			{
				depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
				loadOp(*t.depth, t.depthLayer, depth);
				Transition(*t.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
				depth.imageView = LayerView(*t.depth, t.depthLayer);
				depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			}
			if (timing::On())
			{
				const Image* first = nullptr;
				uint32 colors = 0;
				for (Image* c : t.color)
					if (c)
						first = first ? first : c, colors++;
				const uint32 layers = first ? first->layers : t.depth ? t.depth->layers : 1;
				timing::Mark(timing::Kind::Pass, fmt::format("{}x{} c{}{}{}{}", w, h, colors,
					first ? fmt::format(" f{}", (int)first->format) : "", t.depth ? " d" : "",
					layers > 1 ? fmt::format(" L{}", layers) : ""));
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

		// by the pair itself: a hash of two handles (pointers, often allocated side by side) collides
		VkPipelineLayout PipelineLayout(Shader* vs, Shader* ps)
		{
			auto it = s_layouts.find({ vs->layout, ps->layout });
			if (it != s_layouts.end())
				return it->second;
			VkDescriptorSetLayout sets[2] = { vs->layout, ps->layout };
			VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			ci.setLayoutCount = 2;
			ci.pSetLayouts = sets;
			VkPipelineLayout l;
			Check(vkCreatePipelineLayout(s.device, &ci, nullptr, &l), "vkCreatePipelineLayout");
			return s_layouts[{ vs->layout, ps->layout }] = l;
		}

		// A pipeline's recipe: its Vulkan state and its shaders' keys. The cache on disk keeps it
		// (shader_cache.cpp) so the pipeline can be built again before the game starts; its bytes
		// (Serialize) are its identity from run to run.
		struct PipelineDesc
		{
			uint64 vsKey = 0, psKey = 0;
			std::vector<VkVertexInputBindingDescription> bindings;
			std::vector<VkVertexInputAttributeDescription> attrs;
			VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			VkBool32 primitiveRestart = VK_FALSE, rasterizerDiscard = VK_FALSE, depthBias = VK_FALSE, depthClip = VK_TRUE;
			VkCullModeFlags cull = VK_CULL_MODE_NONE;
			VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			uint32 colorCount = 0;
			VkPipelineColorBlendAttachmentState blends[8]{};
			VkFormat colorFormats[8]{};
			VkBool32 logicOpEnable = VK_FALSE;
			VkLogicOp logicOp = VK_LOGIC_OP_COPY;
			VkBool32 depthTest = VK_FALSE, depthWrite = VK_FALSE, stencilTest = VK_FALSE;
			VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;
			VkStencilOpState front{}, back{};
			VkFormat depthFormat = VK_FORMAT_UNDEFINED, stencilFormat = VK_FORMAT_UNDEFINED;

			template<typename IO, typename V> static bool Fields(IO& io, V& d)
			{
				io.Pod(d.vsKey); io.Pod(d.psKey);
				io.Vec(d.bindings); io.Vec(d.attrs);
				io.Pod(d.topology); io.Pod(d.primitiveRestart); io.Pod(d.rasterizerDiscard); io.Pod(d.depthBias); io.Pod(d.depthClip);
				io.Pod(d.cull); io.Pod(d.frontFace);
				io.Pod(d.colorCount);
				for (uint32 i = 0; i < std::min<uint32>(d.colorCount, 8); i++)   // attachments past colorCount don't exist
				{
					io.Pod(d.blends[i]);
					io.Pod(d.colorFormats[i]);
				}
				io.Pod(d.logicOpEnable); io.Pod(d.logicOp);
				io.Pod(d.depthTest); io.Pod(d.depthWrite); io.Pod(d.stencilTest); io.Pod(d.depthCompare);
				io.Pod(d.front); io.Pod(d.back);
				io.Pod(d.depthFormat); io.Pod(d.stencilFormat);
				return d.colorCount <= 8;
			}
			std::vector<uint8> Serialize() const
			{
				Writer w;
				Fields(w, *this);
				return std::move(w.bytes);
			}
			bool Parse(std::span<const uint8> record)
			{
				Reader r{ record };
				return Fields(r, *this) && r.End();
			}
		};

		// the recipe for a draw, from the registers
		PipelineDesc Describe(Shader* vs, Shader* ps, LatteFetchShader* fetch, const Targets& t,
			Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE prim)
		{
			const auto& r = LatteGPUState.contextNew;
			const uint32* raw = LatteGPUState.contextRegister;
			PipelineDesc d;
			d.vsKey = vs->key;
			d.psKey = ps->key;

			// vertex input
			for (auto& g : fetch->bufferGroups)
			{
				std::optional<LatteConst::VertexFetchType2> fetchType;
				for (sint32 j = 0; j < g.attribCount; j++)
				{
					auto& a = g.attrib[j];
					uint32 location = vs->mapping.attributeMapping[a.semanticId];
					if (location == (uint32)-1)
						continue;
					d.attrs.push_back({ location, a.attributeBufferIndex, VertexFormat(a.format), a.offset });
					if (!fetchType)
						fetchType = a.fetchType;
				}
				uint32 stride = (raw[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >> 11) & 0xFFFF;
				d.bindings.push_back({ g.attributeBufferIndex, stride,
					fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX });
			}

			// input assembly (quads and quad strips arrive as triangle lists, see DecodeIndices)
			using P = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
			d.primitiveRestart = VK_TRUE;
			switch (prim)
			{
			case P::POINTS: d.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; d.primitiveRestart = VK_FALSE; break;
			case P::LINES: d.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; d.primitiveRestart = VK_FALSE; break;
			case P::LINE_STRIP: case P::LINE_LOOP: d.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
			case P::TRIANGLES: d.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; d.primitiveRestart = VK_FALSE; break;
			case P::TRIANGLE_FAN: d.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
			case P::TRIANGLE_STRIP: d.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
			default: d.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; d.primitiveRestart = VK_FALSE; break;
			}

			// rasterizer
			const auto& pm = r.PA_SU_SC_MODE_CNTL;
			d.rasterizerDiscard = r.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL();
			if (!r.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA())             // GX2SetSpecialState(0, true) workaround
				d.rasterizerDiscard = VK_FALSE;
			d.depthBias = pm.get_OFFSET_FRONT_ENABLED() ? VK_TRUE : VK_FALSE;
			uint32 cullFront = pm.get_CULL_FRONT(), cullBack = pm.get_CULL_BACK();
			d.cull = (cullFront && cullBack) ? VK_CULL_MODE_FRONT_AND_BACK : cullFront ? VK_CULL_MODE_FRONT_BIT
				: cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
			d.frontFace = pm.get_FRONT_FACE() == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
			d.depthClip = !r.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE();

			// blend
			uint32 blendMask = r.CB_COLOR_CONTROL.get_BLEND_MASK(), targetMask = r.CB_TARGET_MASK.get_MASK();
			for (uint32 i = 0; i < 8; i++)
			{
				d.colorFormats[i] = t.color[i] ? t.color[i]->format : VK_FORMAT_UNDEFINED;
				if (t.color[i])
					d.colorCount = i + 1;
				auto& b = d.blends[i];
				const auto& bc = r.CB_BLENDN_CONTROL[i];
				b.blendEnable = (blendMask & (1 << i)) && !IsIntegerFormat(d.colorFormats[i]);
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
			auto rop = r.CB_COLOR_CONTROL.get_ROP();
			d.logicOpEnable = rop != Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY;
			d.logicOp = rop == Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::SET ? VK_LOGIC_OP_SET
				: rop == Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::CLEAR ? VK_LOGIC_OP_CLEAR
				: rop == Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP::OR ? VK_LOGIC_OP_OR : VK_LOGIC_OP_COPY;

			// depth and stencil
			const auto& dc = r.DB_DEPTH_CONTROL;
			d.depthTest = dc.get_Z_ENABLE();
			d.depthWrite = dc.get_Z_WRITE_ENABLE();
			d.depthCompare = kCompare[(uint32)dc.get_Z_FUNC()];
			d.stencilTest = dc.get_STENCIL_ENABLE();
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
			d.front = face(false);
			d.back = face(true);
			d.depthFormat = t.depth ? t.depth->format : VK_FORMAT_UNDEFINED;
			d.stencilFormat = t.depth && (t.depth->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? t.depth->format : VK_FORMAT_UNDEFINED;
			return d;
		}

		// the pipeline for a recipe, through the driver's cache; any thread (preparing uses several)
		VkResult BuildPipeline(const PipelineDesc& d, Shader* vs, Shader* ps, VkPipelineLayout layout, VkPipeline& pipeline)
		{
			VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			vin.vertexBindingDescriptionCount = (uint32)d.bindings.size();
			vin.pVertexBindingDescriptions = d.bindings.data();
			vin.vertexAttributeDescriptionCount = (uint32)d.attrs.size();
			vin.pVertexAttributeDescriptions = d.attrs.data();
			VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			ia.topology = d.topology;
			ia.primitiveRestartEnable = d.primitiveRestart;
			VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			vp.viewportCount = vp.scissorCount = 1;
			VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			rs.rasterizerDiscardEnable = d.rasterizerDiscard;
			rs.polygonMode = VK_POLYGON_MODE_FILL;
			rs.depthClampEnable = VK_TRUE;
			rs.lineWidth = 1.0f;
			rs.depthBiasEnable = d.depthBias;
			rs.cullMode = d.cull;
			rs.frontFace = d.frontFace;
			VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT };
			clip.depthClipEnable = d.depthClip;
			if (s.depthClip)
				rs.pNext = &clip;
			VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			cb.logicOpEnable = d.logicOpEnable;
			cb.logicOp = d.logicOp;
			cb.attachmentCount = d.colorCount;
			cb.pAttachments = d.blends;
			VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			ds.depthTestEnable = d.depthTest;
			ds.depthWriteEnable = d.depthWrite;
			ds.depthCompareOp = d.depthCompare;
			ds.maxDepthBounds = 1.0f;
			ds.stencilTestEnable = d.stencilTest;
			ds.front = d.front;
			ds.back = d.back;
			VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BIAS };
			VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
			dy.dynamicStateCount = 4;
			dy.pDynamicStates = dyn;
			VkPipelineRenderingCreateInfo rinfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
			rinfo.colorAttachmentCount = d.colorCount;
			rinfo.pColorAttachmentFormats = d.colorFormats;
			rinfo.depthAttachmentFormat = d.depthFormat;
			rinfo.stencilAttachmentFormat = d.stencilFormat;
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
			return vkCreateGraphicsPipelines(s.device, cache::Driver(), 1, &pi, nullptr, &pipeline);
		}

		VkPipeline GetPipeline(Shader* vs, Shader* ps, LatteFetchShader* fetch, VkPipelineLayout layout, const Targets& t,
			Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE prim)
		{
			const uint32* raw = LatteGPUState.contextRegister;
			// everything the pipeline is built from (Mix of two values alone is their XOR's: start from 0,
			// or two modules allocated side by side collide with another pair)
			uint64 key = Mix(Mix(0, (uint64)vs->module), (uint64)ps->module);
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
			// new to this run: prepared from the cache on disk, or built now (and added to it)
			PipelineDesc d = Describe(vs, ps, fetch, t, prim);
			std::vector<uint8> recipe = d.Serialize();
			uint64 id = Fnv(recipe.data(), recipe.size());
			if (auto prepared = s_recipes.find(id); prepared != s_recipes.end())
				return s_pipelines[key] = prepared->second;
			auto start = std::chrono::steady_clock::now();
			VkPipeline pipeline;
			Check(BuildPipeline(d, vs, ps, layout, pipeline), "vkCreateGraphicsPipelines");
			cache::AddPipeline(recipe);
			if (shaderlist::Capturing())
				shaderlist::Capture("pipeline " + shaderlist::Hex(recipe));
			s_sights.pipelines++;
			s_sights.pipelineMs += MsSince(start);
			s_recipes[id] = pipeline;
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

		// Decoded index buffers, reused (index decoding was 6% of the render thread: docs/research/perf-baseline.md).
		// An entry is keyed by the draw's index parameters (the source's address, count, type, quads or quad strip, the
		// output's width, the restart index) and keeps a copy of the source's bytes: a draw whose source still holds
		// them (memcmp, vectorised) takes the entry's decoded indices, count and max, so the result is the decode's
		// exactly; any change decodes again. Auto-generated quads have no source: their key alone decides. Cleared past
		// 64 MB. WWHD_INDEXCACHE=0 off.
		struct IndexKey
		{
			MPTR addr; uint32 count, restart; uint8 type, quads, strip, u32;
			bool operator==(const IndexKey&) const = default;
		};
		struct IndexKeyHash
		{
			size_t operator()(const IndexKey& k) const
			{
				return std::hash<uint64>()(((uint64)k.addr << 32 | k.count) ^ ((uint64)k.restart << 8) ^
					((uint64)k.type << 4 | k.quads << 2 | k.strip << 1 | k.u32) * 0x9E3779B97F4A7C15ull);
			}
		};
		struct IndexEntry { std::vector<uint8> src, out; uint32 count = 0, max = 0; };
		std::unordered_map<IndexKey, IndexEntry, IndexKeyHash> s_indexCache;
		size_t s_indexCacheBytes = 0;
		uint64 s_indexHits = 0, s_indexMisses = 0;            // WWHD_RENDER_STATS's line
		bool IndexCacheOn()
		{
			static const bool on = [] { const char* e = getenv("WWHD_INDEXCACHE"); return !(e && atoi(e) == 0); }();
			return on;
		}

		Indices DecodeIndicesUncached(MPTR physIndices, uint32 count, Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE prim);

		Indices DecodeIndices(MPTR physIndices, uint32 count, Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE prim)
		{
			using P = Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE;
			using I = Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE;
			const bool quads = prim == P::QUADS, strip = prim == P::QUAD_STRIP;
			const I type = physIndices ? LatteGPUState.contextNew.VGT_DMA_INDEX_TYPE.get_INDEX_TYPE() : I::AUTO;
			if (!IndexCacheOn() || (type == I::AUTO && !quads && !strip))
				return DecodeIndicesUncached(physIndices, count, prim);
			const bool u32 = type == I::U32_BE || type == I::U32_LE || (type == I::AUTO && count > 0xFFFF);
			const uint32 elem = u32 ? 4 : 2;
			const uint32 restart = LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();
			const IndexKey key{ type == I::AUTO ? 0 : physIndices, count, restart, (uint8)type, quads, strip, u32 };
			const uint8* src = type == I::AUTO ? nullptr : memory_getPointerFromVirtualOffset(physIndices);
			const size_t srcBytes = src ? (size_t)count * elem : 0;
			auto it = s_indexCache.find(key);
			if (it != s_indexCache.end() && (!src || memcmp(it->second.src.data(), src, srcBytes) == 0))
			{
				const IndexEntry& e = it->second;
				Indices out;
				out.offset = RingAlloc((VkDeviceSize)e.out.size() + 4, 4);
				memcpy(s.ring.data + out.offset, e.out.data(), e.out.size());
				out.count = e.count;
				out.max = e.max;
				out.type = u32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
				s_indexHits++;
				return out;
			}
			s_indexMisses++;
			Indices out = DecodeIndicesUncached(physIndices, count, prim);
			if (s_indexCacheBytes > (64u << 20))
			{
				s_indexCache.clear();
				s_indexCacheBytes = 0;
				it = s_indexCache.end();
			}
			IndexEntry& e = it != s_indexCache.end() ? it->second : s_indexCache[key];
			s_indexCacheBytes -= e.src.size() + e.out.size();
			e.src.assign(src, src + srcBytes);
			e.out.assign(s.ring.data + out.offset, s.ring.data + out.offset + (size_t)out.count * elem);
			e.count = out.count;
			e.max = out.max;
			s_indexCacheBytes += e.src.size() + e.out.size();
			return out;
		}

		Indices DecodeIndicesUncached(MPTR physIndices, uint32 count, Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE prim)
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

		// WWHD_RENDER_SCALE: the draw's textures' sizes over the guest's, per stage and unit (Textures), and the guest's
		// over its target's (uf_fragCoordScale: gl_FragCoord in the guest's pixels); all 1 when off
		float s_texScale[2][LATTE_NUM_MAX_TEX_UNITS][2];
		float s_fragScale[2] = { 1.0f, 1.0f };

		// uniform variables (uniformData_updateUniformVars, LatteBufferCache_LoadRemappedUniforms)
		VkDeviceSize UniformVars(Shader& sh, bool vertex, float viewportW, float viewportH)
		{
			// filled in place in the ring (no copy through a vector): the same bytes over [0, size)
			uint32 size = sh.uniforms.offset_endOfBlock;
			VkDeviceSize off = RingAlloc(std::max<uint32>(size, 16), s.props.limits.minUniformBufferOffsetAlignment);
			uint8* data = s.ring.data + off;
			memset(data, 0, size);
			auto at = [&](sint32 offset) { return data + offset; };
			const auto& r = LatteGPUState.contextNew;
			uint32* regs = LatteGPUState.contextRegister;
			for (sint32 t = 0; t < LATTE_NUM_MAX_TEX_UNITS; t++)
				if (sh.uniforms.offset_texScale[t] >= 0)
				{
					memcpy(at(sh.uniforms.offset_texScale[t]), s_texScale[vertex][t], 8);   // 1 but WWHD_RENDER_SCALE's surfaces
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
				float v[4] = { s_fragScale[0], s_fragScale[1], (float)(sint32)viewportW, (float)(sint32)viewportH };
				memcpy(at(sh.uniforms.offset_fragCoordScale), v, 16);
			}
			return off;
		}

		// uniform blocks (LatteBufferCache_syncGPUUniformBuffers): the range the shader reads. The shader declares the
		// block as the quick buffer's size (Cemu's DetermineSize: the highest static index + 1, or the whole 64 KB with
		// a dynamic index), so it reads nothing past that: only that much is filled (the block's bytes, then zeros) and
		// taken from the ring. The descriptor's 64 KB range stays in the buffer (RingAlloc keeps 64 KB spare).
		// WWHD_UBLOCK_FULL=1: 64 KB filled and taken per block, as before (for A/Bs)
		VkDeviceSize UniformBlock(Shader& sh, bool vertex, uint32 index)
		{
			uint32 blockRegs = vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
			MPTR phys = LatteGPUState.contextRegister[blockRegs + index * 7 + 0];
			uint32 size = LatteGPUState.contextRegister[blockRegs + index * 7 + 1] + 1;
			uint32 readable = 65536;
			for (auto& q : sh.dec->list_quickBufferList)
				if (q.index == index)
				{
					size = std::min<uint32>(size, q.size);
					readable = std::min<uint32>(readable, q.size);
				}
			static const bool full = [] { const char* e = getenv("WWHD_UBLOCK_FULL"); return e && atoi(e) == 1; }();
			if (full)
				readable = 65536;
			VkDeviceSize off = RingAlloc(readable, s.props.limits.minUniformBufferOffsetAlignment);
			uint32 copy = phys ? std::min<uint32>(size, readable) : 0;
			if (copy)
				memcpy(s.ring.data + off, memory_getPointerFromPhysicalOffset(phys), copy);
			memset(s.ring.data + off + copy, 0, readable - copy);
			return off;
		}

		// the stage's textures, before anything else of the draw is allocated (they may upload)
		void Textures(Shader& sh, bool vertex, std::span<Image* const> attachments, std::vector<VkDescriptorImageInfo>& images)
		{
			images.resize(sh.mapping.getTextureCount());
			for (auto& unit : s_texScale[vertex])
				unit[0] = unit[1] = 1.0f;
			for (sint32 i = 0; i < sh.mapping.getTextureCount(); i++)
			{
				const sint32 unit = sh.mapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
				Sampled t = SampleTexture(sh.dec, vertex, unit, attachments);
				images[i] = { t.sampler, t.view, t.layout };
				if (unit >= 0 && unit < LATTE_NUM_MAX_TEX_UNITS)
					s_texScale[vertex][unit][0] = t.scaleX, s_texScale[vertex][unit][1] = t.scaleY;
			}
		}

		uint32 s_sets = 0, s_imageDescriptors = 0, s_bufferDescriptors = 0;  // allocated from the pool since the last submit

		// Descriptor sets reused (docs/research/deck-plan.md item 3): a set's contents are the shader's (its layout and
		// buffer bindings, which all point at the ring's start with a fixed range: the draw's data goes in the dynamic
		// offsets) and the images' (sampler, view, layout), so a draw whose shader and images equal an earlier draw's
		// since the pool was last reset binds that draw's set, without vkAllocateDescriptorSets and
		// vkUpdateDescriptorSets. Cleared with the pool (OnSubmitted) and whenever an image view is destroyed
		// (ForgetSets: a later view may get its handle). WWHD_SETCACHE=0: off.
		struct CachedSet
		{
			const Shader* sh;
			std::vector<VkDescriptorImageInfo> images;
			VkDescriptorSet set;
		};
		std::unordered_map<uint64, CachedSet> s_setCache;
		uint64 s_setHits = 0, s_setMisses = 0;                 // WWHD_RENDER_STATS's line

		uint64 SetKey(const Shader& sh, const std::vector<VkDescriptorImageInfo>& images)
		{
			auto mix = [](uint64 h, uint64 v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0xFF51AFD7ED558CCDull; };
			uint64 h = mix(0, (uint64)(uintptr_t)&sh);
			for (const auto& i : images)
				h = mix(mix(mix(h, (uint64)i.sampler), (uint64)i.imageView), (uint64)i.imageLayout);
			return h;
		}

		bool SameImages(const std::vector<VkDescriptorImageInfo>& a, const std::vector<VkDescriptorImageInfo>& b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
				if (a[i].sampler != b[i].sampler || a[i].imageView != b[i].imageView || a[i].imageLayout != b[i].imageLayout)
					return false;
			return true;
		}

		VkDescriptorSet Descriptors(Shader& sh, bool vertex, const std::vector<VkDescriptorImageInfo>& images,
			std::vector<uint32>& dynamicOffsets, float vpW, float vpH)
		{
			// the draw's data, in binding order: the uniform vars, then the blocks
			if (sh.mapping.uniformVarsBufferBindingPoint >= 0)
				dynamicOffsets.push_back((uint32)UniformVars(sh, vertex, vpW, vpH));
			for (uint8 i : sh.uniformBuffers)
				dynamicOffsets.push_back((uint32)UniformBlock(sh, vertex, i));
			static const bool cache = [] { const char* e = getenv("WWHD_SETCACHE"); return !(e && atoi(e) == 0); }();
			uint64 key = 0;
			if (cache)
			{
				key = SetKey(sh, images);
				auto it = s_setCache.find(key);
				if (it != s_setCache.end() && it->second.sh == &sh && SameImages(it->second.images, images))
				{
					s_setHits++;
					return it->second.set;
				}
				s_setMisses++;
			}
			VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			ai.descriptorPool = s.descriptors;
			ai.descriptorSetCount = 1;
			ai.pSetLayouts = &sh.layout;
			VkDescriptorSet set;
			Check(vkAllocateDescriptorSets(s.device, &ai, &set), "vkAllocateDescriptorSets");  // Reserve() made room
			s_sets++;
			s_imageDescriptors += (uint32)images.size();
			s_bufferDescriptors += (uint32)sh.uniformBuffers.size() + 1;
			static std::vector<VkWriteDescriptorSet> writes;    // the render thread's alone: kept, not allocated per draw
			writes.clear();
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
			}
			vkUpdateDescriptorSets(s.device, (uint32)writes.size(), writes.data(), 0, nullptr);
			if (cache)
				s_setCache[key] = CachedSet{ &sh, images, set };
			return set;
		}

		uint64 s_draws = 0, s_skipped = 0;
		uint32 s_statsEvery = 0;

		// WWHD_RENDER_TRACE=FRAME:ADDR logs every draw into the color target at ADDR (hex) during swap
		// interval FRAME (after the FRAME-1th swap): programs, alpha test, depth, and each pixel
		// texture unit with where its data comes from. For hunting a surface that differs from the
		// reference's (tools/reference: CEMU_TEX_DUMP_FRAME). ADDR 0: every draw of the frame, with
		// its targets and the area it draws.
		void TraceDraw(Shader* vs, Shader* ps, uint64 vsKey, uint64 psKey, const Targets& t,
			Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE prim, uint32 count, uint32 hostCount);

		// room for one draw's per-draw data (a submit in the middle of a draw would free what it already allocated)
		void Reserve()
		{
			if (s.ring.used + (64ull << 20) > s.ringEnd || s_sets + 2 > (1u << 16) || s_imageDescriptors + 64 > (1u << 18) ||
				s_bufferDescriptors + 64 > (1u << 17))
				SubmitAndWait();
		}
	}

	namespace
	{
		void TraceDraw(Shader* vs, Shader* ps, uint64 vsKey, uint64 psKey, const Targets& t,
			Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE prim, uint32 count, uint32 hostCount)
		{
			static uint32 frame = 0, addr = 0, px = UINT32_MAX, py = 0;
			static const bool on = [] {
				const char* e = getenv("WWHD_RENDER_TRACE");
				return e && sscanf(e, "%u:%x:%u,%u", &frame, &addr, &px, &py) >= 2;
			}();
			if (!on || (frame && s.frame + 1 != frame))              // frame 0: every frame, pixel changes only
				return;
			const uint32* regs = LatteGPUState.contextRegister;
			sint32 slot = -1;
			for (uint32 i = 0; i < 8 && slot < 0; i++)
				if (t.color[i] && (addr == 0 || (regs[mmCB_COLOR0_BASE + i] & 0xFFFFF800u) == (addr & 0xFFFFF800u)))
					slot = (sint32)i;
			if (slot < 0 && !(addr == 0 && t.depth))                // ADDR 0: every draw of the frame
				return;
			Image* target = slot >= 0 ? t.color[slot] : nullptr;
			static uint32 n = 0;
			const auto& r = LatteGPUState.contextNew;
			std::string line = fmt::format("trace #{} slot {} of {:02x} vs {:016x} ps {:016x} prim {} count {} ({}) alpha {:x} ref {} depth {:08x} blend {:08x} cull {:x} mask {:08x}",
				n++, slot, [&] { uint32 m = 0; for (uint32 i = 0; i < 8; i++) m |= t.color[i] ? 1u << i : 0; return m; }(), vsKey, psKey, (uint32)prim, count, hostCount, regs[Latte::REGADDR::SX_ALPHA_TEST_CONTROL], r.SX_ALPHA_REF.get_ALPHA_TEST_REF(),
				regs[Latte::REGADDR::DB_DEPTH_CONTROL], regs[Latte::REGADDR::CB_BLEND0_CONTROL], regs[Latte::REGADDR::PA_SU_SC_MODE_CNTL] & 3,
				regs[Latte::REGADDR::CB_TARGET_MASK]);
			if (addr == 0)                                           // which targets, and the area drawn
			{
				for (uint32 i = 0; i < 8; i++)
					if (t.color[i])
						line += fmt::format(" | c{} {:08x} fmt {:x} {}x{}", i, regs[mmCB_COLOR0_BASE + i] & 0xFFFFFF00, (uint32)LatteMRT::GetColorBufferFormat(i, r),
							t.color[i]->width, t.color[i]->height);
				if (t.depth)
					line += fmt::format(" | z {:08x} {}x{}", regs[mmDB_HTILE_DATA_BASE] << 8, t.depth->width, t.depth->height);
				line += fmt::format(" | scissor {},{}-{},{} vport {}x{}", r.PA_SC_GENERIC_SCISSOR_TL.get_TL_X(), r.PA_SC_GENERIC_SCISSOR_TL.get_TL_Y(),
					r.PA_SC_GENERIC_SCISSOR_BR.get_BR_X(), r.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y(), r.PA_CL_VPORT_XSCALE.get_SCALE() * 2, r.PA_CL_VPORT_YSCALE.get_SCALE() * -2);
			}
			for (sint32 i = 0; i < ps->mapping.getTextureCount(); i++)
			{
				uint32 unit = ps->mapping.getRelativeTextureUnitFromRelativeBindingPoint(i);
				const uint32* w = regs + Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + unit * 7;
				uint32 taddr = w[2] << 8, tile = (w[0] >> 3) & 0xF;
				if (Latte::TM_IsMacroTiled((Latte::E_HWTILEMODE)tile))
					taddr &= ~0x700u;
				const auto& tu = *(const _LatteRegisterSetTextureUnit*)w;
				uint32 fmt = (uint32)LatteTexture_ReconstructGX2Format(tu.word1, tu.word4);
				bool surface = s.surfaces.lower_bound({ taddr, 0 }) != s.surfaces.end() && s.surfaces.lower_bound({ taddr, 0 })->first.first == taddr;
				line += fmt::format(" | t{} {:08x} fmt {:x} {}x{} dim {} sel {:03x} mips {}-{} {}{}", unit, taddr, fmt, (w[0] >> 19) + 1, (w[1] & 0x1FFF) + 1,
					w[0] & 7, (w[4] >> 16) & 0xFFF, tu.word4.get_BASE_LEVEL(), tu.word5.get_LAST_LEVEL(), surface ? "surface" : "memory",
					ps->dec->textureUsesDepthCompare[unit] ? " cmp" : "");
			}
			// the data the draw reads: its vertex shader's constants (registers and uniform blocks) and
			// its vertex buffers, hashed (whether a draw sees new data from frame to frame);
			// WWHD_RENDER_TRACE_VS=key: that vertex shader's constants and vertices too (a probe)
			{
				static const uint64 dumpVs = [] { const char* e = getenv("WWHD_RENDER_TRACE_VS"); return e ? strtoull(e, nullptr, 16) : 0ull; }();
				uint64 hu = 0xCBF29CE484222325ull, hv = hu;
				auto mix = [](uint64& h, const uint8* p, size_t n) { h ^= HashBytes(p, n) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
				if (r.SQ_CONFIG.get_DX9_CONSTS())
					mix(hu, (const uint8*)(regs + mmSQ_ALU_CONSTANT0_0 + 0x400), 256 * 16);
				for (uint32 i = 0; i < 16; i++)
				{
					const MPTR ub = regs[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7];
					if (ub)
						mix(hu, memory_getPointerFromPhysicalOffset(ub), std::min<uint32>(regs[mmSQ_VTX_UNIFORM_BLOCK_START + i * 7 + 1] + 1, 65536));
					const MPTR vb = regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + i * 7];
					if (vb)
						mix(hv, memory_getPointerFromPhysicalOffset(vb), std::min<uint32>(regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + i * 7 + 1] + 1, 1u << 20));
				}
				// what the vertex shader itself reads: its uniform registers, remapped entries and blocks
				uint64 hs = 0xCBF29CE484222325ull;
				std::string floats;
				auto take = [&](const uint8* p, size_t n) {
					mix(hs, p, n);
					if (vsKey == dumpVs)
						for (size_t i = 0; i + 4 <= n && i < 256; i += 4)
						{
							uint32 w;
							memcpy(&w, p + i, 4);
							float x;
							memcpy(&x, &w, 4);
							floats += fmt::format(" {}", x);
						}
				};
				const uint32 aluConst = 0x400;
				if (vs->uniforms.offset_uniformRegister >= 0)
					take((const uint8*)(regs + mmSQ_ALU_CONSTANT0_0 + aluConst), vs->uniforms.count_uniformRegister * 16);
				if (vs->uniforms.offset_remapped >= 0)
				{
					if (r.SQ_CONFIG.get_DX9_CONSTS())
						for (auto& e : vs->dec->list_remappedUniformEntries_register)
							take((const uint8*)(regs + mmSQ_ALU_CONSTANT0_0 + aluConst + e.indexOffset / 4), 16);
					else
						for (auto& g : vs->dec->list_remappedUniformEntries_bufferGroups)
						{
							const MPTR phys = regs[mmSQ_VTX_UNIFORM_BLOCK_START + g.kcacheBankIdOffset / 4];
							for (auto& e : g.entries)
								if (phys)
									take(memory_base + phys + e.indexOffset, 16);
						}
				}
				for (uint8 index : vs->uniformBuffers)
				{
					const MPTR phys = regs[mmSQ_VTX_UNIFORM_BLOCK_START + index * 7];
					if (vsKey == dumpVs)
						floats += fmt::format(" [block {} at {:08x}]", index, phys);
					uint32 size = regs[mmSQ_VTX_UNIFORM_BLOCK_START + index * 7 + 1] + 1;
					for (auto& q : vs->dec->list_quickBufferList)
						if (q.index == index)
							size = std::min<uint32>(size, q.size);
					if (phys)
						take(memory_getPointerFromPhysicalOffset(phys), std::min<uint32>(size, 65536));
				}
				// the vertex buffers the fetch shader reads, each over the first 256 bytes
				std::string vb;
				for (uint32 i = 0; i < 16; i++)
				{
					const MPTR a = regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + i * 7];
					const uint32 n = regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + i * 7 + 1] + 1;
					if (a && n > 1)
					{
						vb += fmt::format(" b{} {:08x}:{:08x}", i, a, (uint32)HashBytes(memory_getPointerFromPhysicalOffset(a), std::min<uint32>(n, 256)));
						if (vsKey == dumpVs)                    // the first words, big-endian floats
							for (uint32 k = 0; k < std::min<uint32>(n / 4, 30); k++)
							{
								const uint32 w = _swapEndianU32(*(const uint32*)memory_getPointerFromPhysicalOffset(a + 4 * k));
								float x;
								memcpy(&x, &w, 4);
								vb += fmt::format(" {}", x);
							}
					}
				}
				line += fmt::format(" | data u {:08x} v {:08x} vbufs{} vs-reads {:08x}{}", (uint32)hu, (uint32)hv, vb, (uint32)hs, floats);
			}
			if (frame)
				Log(line);
			// with :X,Y, the pixel after the draw, when it changed
			if (target && px != UINT32_MAX && px < target->width && py < target->height)
			{
				static VkBuffer buf = VK_NULL_HANDLE;
				static uint32* mapped = nullptr;
				static uint32 last = 0xDEADBEEF;
				if (!buf)
				{
					VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
					bci.size = 64;
					bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
					Check(vkCreateBuffer(s.device, &bci, nullptr, &buf), "vkCreateBuffer");
					VkMemoryRequirements req;
					vkGetBufferMemoryRequirements(s.device, buf, &req);
					VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
					ai.allocationSize = req.size;
					ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
					VkDeviceMemory mem;
					Check(vkAllocateMemory(s.device, &ai, nullptr, &mem), "vkAllocateMemory");
					vkBindBufferMemory(s.device, buf, mem, 0);
					vkMapMemory(s.device, mem, 0, 64, 0, (void**)&mapped);
				}
				EndRendering();
				Transition(*target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				VkBufferImageCopy c{};
				c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				c.imageOffset = { (sint32)px, (sint32)py, 0 };
				c.imageExtent = { 1, 1, 1 };
				vkCmdCopyImageToBuffer(s.cmd, target->image, target->layout, buf, 1, &c);
				HostReadBarrier();
				SubmitAndWait();
				if (mapped[0] != last)
				{
					Log(fmt::format("trace #{} frame {}: pixel {},{} {:08x} -> {:08x}{}", n - 1, s.frame + 1, px, py, last, mapped[0],
						frame ? std::string() : " by " + line));
					last = mapped[0];
				}
			}
		}
	}

	void DrawInit()
	{
		InstallNoopRenderer();
		glslang::InitializeProcess();
		TextureInit();
		cache::Open();
		if (const char* e = getenv("WWHD_RENDER_STATS"))
			s_statsEvery = (uint32)atoi(e);
	}

	void EndRendering()
	{
		if (s_rendering)
		{
			vkCmdEndRendering(s.cmd);
			s_rendering = false;
			timing::Mark(timing::Kind::Other);
		}
	}

	void OnSubmitted()
	{
		s_sets = s_imageDescriptors = s_bufferDescriptors = 0;
		s_setCache.clear();
	}

	void ForgetSets()
	{
		s_setCache.clear();
	}

	// WWHD_RENDER_STATS=N: a line every N frames
	void DrawStats(uint32 frame)
	{
		if (s_statsEvery && frame % s_statsEvery == 0)
			Log(fmt::format("frame {}: {} draws, {} skipped; {} shaders, {} pipelines; decoded indices {} reused, {} decoded; "
				"descriptor sets {} reused, {} written",
				frame, s_draws, s_skipped, s_shaders.size(), s_pipelines.size(), s_indexHits, s_indexMisses, s_setHits, s_setMisses));
	}

	// The first start without hitches (D20): everything the shader list (shader_list.cpp) has that the
	// cache on disk doesn't yet is made now, as the run that captured it made it: its fetch shaders from
	// their code and registers, its shaders translated from the programs in the game's files with the
	// registers they were first met with (each must give its listed key again), and its pipeline recipes
	// added to the cache's. Returns how many shaders it translated; `steps` is how far it moved the
	// progress bar. WWHD_SHADER_LIST=none: no list.
	static uint32 PrepareFromList(std::vector<std::vector<uint8>>& recipes, const std::function<void(uint32 done, uint32 total)>& progress,
		uint32& steps)
	{
		shaderlist::List list;
		const char* e = getenv("WWHD_SHADER_LIST");
		if ((e && (!*e || !strcmp(e, "none"))) || !shaderlist::Read(list))
			return 0;
		auto start = std::chrono::steady_clock::now();
		uint32* regs = LatteGPUState.contextRegister;
		std::vector<uint32> saved(regs, regs + kRegisterCount);
		for (auto& [hash, f] : list.fetches)
			if (!s_fetchShaders.count(hash) && shaderlist::SetRegisters(regs, kRegisterCount, f.registers))
				s_fetchShaders[hash] = LatteShaderRecompiler_createFetchShader(LatteFetchShader::CalculateCacheHash(f.code.data(), (uint32)f.code.size()),
					regs, (uint32*)f.code.data(), (uint32)f.code.size());
		std::set<uint64> known;
		for (auto& r : recipes)
			known.insert(Fnv(r.data(), r.size()));
		uint32 pipelines = 0;
		for (auto& r : list.pipelines)
			if (known.insert(Fnv(r.data(), r.size())).second)
			{
				cache::AddPipeline(r);
				recipes.push_back(r);
				pipelines++;
			}
		std::vector<const shaderlist::List::Shader*> todo;
		std::set<uint64> keys, programs;
		for (auto& sh : list.shaders)
			if (!s_shaders.count(sh.key) && keys.insert(sh.key).second)
			{
				todo.push_back(&sh);
				programs.insert(sh.program);
			}
		uint32 translated = 0, noProgram = 0, otherKey = 0, failed = 0;
		steps = (uint32)todo.size();
		if (!todo.empty())
		{
			const uint32 total = (uint32)(todo.size() + recipes.size());
			progress(0, total);
			std::unordered_map<uint64, std::vector<uint8>> code = shaderlist::FindPrograms(list, programs);
			auto shown = std::chrono::steady_clock::now();
			for (uint32 i = 0; i < todo.size(); i++)
			{
				if (std::chrono::steady_clock::now() - shown > std::chrono::milliseconds(30))
				{
					progress(i, total);
					shown = std::chrono::steady_clock::now();
				}
				const shaderlist::List::Shader& l = *todo[i];
				auto program = code.find(l.program);
				auto fetch = s_fetchShaders.find(l.fetch);
				if (program == code.end() || (l.vertex && fetch == s_fetchShaders.end()))
				{
					noProgram++;
					continue;
				}
				const std::vector<uint8>& c = program->second;
				shaderlist::SetRegisters(regs, kRegisterCount, l.registers);
				LatteShader_UpdatePSInputs(regs);
				uint64 h = Fnv(c.data(), c.size());
				if ((l.vertex ? VertexKey(regs, h, fetch->second) : PixelKey(regs, h)) != l.key)
				{
					otherKey++;
					continue;
				}
				Shader* sh = new Shader;
				std::vector<uint32> spirv;
				if (!Translate(l.vertex, l.key, c.data(), (uint32)c.size(), l.vertex ? fetch->second : nullptr, *sh, spirv))
				{
					delete sh;
					failed++;
					continue;
				}
				Finish(*sh, l.vertex, spirv);
				s_shaders[l.key] = sh;
				translated++;
			}
			memcpy(regs, saved.data(), kRegisterCount * sizeof(uint32));
			LatteShader_UpdatePSInputs(regs);
		}
		else
			memcpy(regs, saved.data(), kRegisterCount * sizeof(uint32));
		Log(fmt::format("shader list: {} shaders translated from the game's files in {:.1f} s ({} without their program, {} with another "
			"key, {} failed), {} pipelines added", translated, MsSince(start) / 1000, noProgram, otherKey, failed, pipelines));
		return translated;
	}

	void PrepareShaders(const std::function<void(uint32 done, uint32 total)>& progress)
	{
		if (!RendererOn())
			return;
		auto start = std::chrono::steady_clock::now();
		if (shaderlist::Capturing())
		{
			Log("shader list: capturing (WWHD_SHADER_SOURCES), so nothing is prepared: every shader and pipeline is met for the first time");
			return;
		}
		// shaders: SPIR-V into modules (the driver compiles them with their pipelines)
		uint32 damaged = 0;
		for (auto& record : cache::Shaders())
			if (Shader* sh = LoadShader(record))
				s_shaders.try_emplace(sh->key, sh);
			else
				damaged++;
		// the shader list: what playthroughs saw and this cache doesn't have yet, from the game's files
		std::vector<std::vector<uint8>> recipes = cache::Pipelines();
		uint32 steps = 0;
		uint32 translated = PrepareFromList(recipes, progress, steps);
		// pipelines: the recipes whose shaders are there, built on all cores but this one's
		struct Job
		{
			PipelineDesc d;
			uint64 id;
			Shader *vs, *ps;
			VkPipelineLayout layout;
			VkPipeline pipeline = VK_NULL_HANDLE;
		};
		std::vector<Job> jobs;
		for (auto& record : recipes)
		{
			Job j;
			if (!j.d.Parse(record))
			{
				damaged++;
				continue;
			}
			auto vs = s_shaders.find(j.d.vsKey), ps = s_shaders.find(j.d.psKey);
			if (vs == s_shaders.end() || ps == s_shaders.end() || !vs->second->module || !ps->second->module)
				continue;
			j.id = Fnv(record.data(), record.size());
			j.vs = vs->second;
			j.ps = ps->second;
			j.layout = PipelineLayout(j.vs, j.ps);
			jobs.push_back(std::move(j));
		}
		std::atomic<uint32> next{ 0 }, done{ 0 }, failed{ 0 };
		uint32 total = (uint32)jobs.size();
		// WWHD_SHADER_THREADS=n: how many build pipelines (default: all cores but this one's)
		uint32 threads = std::clamp(std::thread::hardware_concurrency(), 2u, 64u) - 1;
		if (const char* e = getenv("WWHD_SHADER_THREADS"); e && atoi(e) > 0)
			threads = (uint32)atoi(e);
		std::vector<std::thread> pool;
		for (uint32 i = 0; i < std::min(threads, total); i++)
			pool.emplace_back([&] {
				for (uint32 k; (k = next++) < total; done++)
					if (BuildPipeline(jobs[k].d, jobs[k].vs, jobs[k].ps, jobs[k].layout, jobs[k].pipeline) != VK_SUCCESS)
					{
						jobs[k].pipeline = VK_NULL_HANDLE;
						failed++;
					}
			});
		while (done < total)
		{
			progress(steps + done, steps + total);
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
		}
		for (auto& t : pool)
			t.join();
		progress(steps + total, steps + total);
		for (auto& j : jobs)
			if (j.pipeline)
				s_recipes.try_emplace(j.id, j.pipeline);
		if (total)
			cache::Save();
		Log(fmt::format("shader cache: prepared {} shaders ({} translated from the game's files) and {} pipelines in {:.1f} s on {} threads "
			"({} failed, {} records damaged)", s_shaders.size(), translated, s_recipes.size(), MsSince(start) / 1000, std::min(threads, total),
			failed.load(), damaged));
	}

	FirstSights TakeFirstSights()
	{
		return std::exchange(s_sights, FirstSights{});
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
		uint64 fsHash = ProgramHash(fsCode, fsSize);
		LatteFetchShader*& fetch = s_fetchShaders[fsHash];
		if (!fetch)
		{
			fetch = LatteShaderRecompiler_createFetchShader(LatteFetchShader::CalculateCacheHash((void*)fsCode, fsSize), regs, (uint32*)fsCode, fsSize);
			if (shaderlist::Capturing())
				shaderlist::Capture(fmt::format("fetch {:016x} {} {}", fsHash, shaderlist::Hex({ fsCode, fsSize }), shaderlist::Registers(regs)));
		}
		uint64 vsKey = VertexKey(regs, ProgramHash(vsCode, vsSize), fetch), psKey = PixelKey(regs, ProgramHash(psCode, psSize));
		static const uint64 skipVs = [] { const char* e = getenv("WWHD_RENDER_SKIP_VS"); return e ? strtoull(e, nullptr, 16) : 0ull; }();
		if (skipVs && vsKey == skipVs)
			return skip("WWHD_RENDER_SKIP_VS (a probe)");
		Shader* vs = GetShader(true, vsKey, vsCode, vsSize, fetch, fsHash);
		Shader* ps = GetShader(false, psKey, psCode, psSize, nullptr, 0);
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
			t.colorLayer[i] = regs[mmCB_COLOR0_VIEW + i] & 0x7FF;
			t.color[i] = &Surface(base, (uint32)LatteMRT::GetColorBufferFormat(i, r), false, pitch, height, t.colorLayer[i] + 1);
		}
		uint32 scissorX = r.PA_SC_GENERIC_SCISSOR_TL.get_TL_X(), scissorY = r.PA_SC_GENERIC_SCISSOR_TL.get_TL_Y();
		uint32 scissorR = r.PA_SC_GENERIC_SCISSOR_BR.get_BR_X(), scissorB = r.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y();
		if (LatteMRT::GetActiveDepthBufferMask(r))
		{
			uint32 base = regs[mmDB_HTILE_DATA_BASE] << 8, size = regs[mmDB_DEPTH_SIZE];
			uint32 pitch = (size & 0x3FF) + 1, height = ((((size >> 10) & 0xFFFFF) + 1) / pitch) << 3;
			pitch <<= 3;
			if (base && scissorR <= pitch && scissorB <= height)
			{
				t.depthLayer = regs[mmDB_DEPTH_VIEW] & 0x7FF;
				t.depth = &Surface(base, (uint32)LatteMRT::GetDepthBufferFormat(r), true, std::max(pitch, 2u), std::max(height, 2u), t.depthLayer + 1);
			}
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
		static std::vector<VkDescriptorImageInfo> vsImages, psImages;   // per-draw lists, kept (not allocated per draw)
		Textures(*vs, true, { attachments, nAttachments }, vsImages);
		Textures(*ps, false, { attachments, nAttachments }, psImages);
		Reserve();

		Indices idx = DecodeIndices(physIndices, count, prim);
		// vertex buffers: the range the draw can reach (LatteBufferCache_Sync)
		uint32 baseVertex = regs[mmSQ_VTX_BASE_VTX_LOC], baseInstance = regs[mmSQ_VTX_START_INST_LOC];
		uint32 instances = r.VGT_DMA_NUM_INSTANCES.get_NUM_INSTANCES();
		static std::vector<std::pair<uint32, VkDeviceSize>> vbufs;
		vbufs.clear();
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
		if (nearZ < 0.0f || nearZ > 1.0f || farZ < 0.0f || farZ > 1.0f)
			LogOnce(fmt::format("zrange{}_{}_{}", nearZ, farZ, t.depth ? t.depth->width : 0), [&] {
				return fmt::format("depth range {}..{} outside 0..1 (depth target {}x{}, halfZ {})", nearZ, farZ,
					t.depth ? t.depth->width : 0, t.depth ? t.depth->height : 0, halfZ); });

		// WWHD_RENDER_SCALE: the target's own size over the guest's (the first attachment's; all share the scale)
		const Image* first = attachments[0];
		const float fx = (float)first->width / (float)first->gw, fy = (float)first->height / (float)first->gh;
		for (uint32 i = 1; i < nAttachments; i++)
			if (attachments[i]->scale != first->scale)
				LogOnce("scalemix", [&] { return fmt::format("render scale: a pass into {}x{} and {}x{}: one scaled, one not (frame {})",
					first->gw, first->gh, attachments[i]->gw, attachments[i]->gh, s.frame + 1); });
		s_fragScale[0] = 1.0f / fx, s_fragScale[1] = 1.0f / fy;

		static std::vector<uint32> dynamicOffsets;
		dynamicOffsets.clear();
		VkDescriptorSet sets[2] = { Descriptors(*vs, true, vsImages, dynamicOffsets, vpW, vpH), VK_NULL_HANDLE };
		sets[1] = Descriptors(*ps, false, psImages, dynamicOffsets, vpW, vpH);
		VkPipelineLayout layout = PipelineLayout(vs, ps);
		VkPipeline pipeline = GetPipeline(vs, ps, fetch, layout, t, prim);

		BeginRendering(t);
		// unchanged binds since the last draw in this command buffer are skipped (renderer_internal.h's Bound)
		static const bool bindCache = [] { const char* e = getenv("WWHD_BINDCACHE"); return !(e && atoi(e) == 0); }();
		auto& b = s.bound;
		const bool known = bindCache && b.valid;
		if (!known || b.pipeline != pipeline)
			vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, (uint32)dynamicOffsets.size(), dynamicOffsets.data());
		for (auto& [binding, off] : vbufs)
			vkCmdBindVertexBuffers(s.cmd, binding, 1, &s.ring.buffer, &off);
		VkViewport viewport{ vpX, vpY + vpH, vpW, -vpH, std::clamp(nearZ, 0.0f, 1.0f), std::clamp(farZ, 0.0f, 1.0f) };
		if (fx != 1.0f || fy != 1.0f)
			viewport.x *= fx, viewport.y *= fy, viewport.width *= fx, viewport.height *= fy;
		if (!known || memcmp(&b.viewport, &viewport, sizeof(viewport)) != 0)
			vkCmdSetViewport(s.cmd, 0, 1, &viewport);
		if (fx != 1.0f || fy != 1.0f)                           // the pixels whose centres the guest's would cover, at least
		{
			scissorX = (uint32)std::floor(scissorX * fx), scissorY = (uint32)std::floor(scissorY * fy);
			scissorR = (uint32)std::ceil(scissorR * fx), scissorB = (uint32)std::ceil(scissorB * fy);
		}
		VkRect2D scissor{ { (sint32)scissorX, (sint32)scissorY }, { scissorR - std::min(scissorX, scissorR), scissorB - std::min(scissorY, scissorB) } };
		if (!known || memcmp(&b.scissor, &scissor, sizeof(scissor)) != 0)
			vkCmdSetScissor(s.cmd, 0, 1, &scissor);
		const float* blend = (const float*)(regs + Latte::REGADDR::CB_BLEND_RED);
		if (!known || memcmp(b.blend, blend, sizeof(b.blend)) != 0)
			vkCmdSetBlendConstants(s.cmd, blend);
		const float bias[3] = { r.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET(), r.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP(),
			r.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE() / 16.0f };
		if (!known || memcmp(b.bias, bias, sizeof(bias)) != 0)
			vkCmdSetDepthBias(s.cmd, bias[0], bias[1], bias[2]);
		b.valid = true;
		b.pipeline = pipeline;
		b.viewport = viewport;
		b.scissor = scissor;
		memcpy(b.blend, blend, sizeof(b.blend));
		memcpy(b.bias, bias, sizeof(bias));
		if (idx.type != VK_INDEX_TYPE_NONE_KHR)
		{
			vkCmdBindIndexBuffer(s.cmd, s.ring.buffer, idx.offset, idx.type);
			vkCmdDrawIndexed(s.cmd, idx.count, instances, 0, (sint32)baseVertex, baseInstance);
		}
		else
			vkCmdDraw(s.cmd, idx.count, instances, baseVertex, baseInstance);
		s_draws++;
		TraceDraw(vs, ps, vsKey, psKey, t, prim, count, idx.count);
	}
}
