// fsr3.cpp: AMD FidelityFX Super Resolution 3.1's upscaler (b-fsr3, session bottom) over the render scale, with the
// renderer's motion vectors, reactive mask and jitter (motion.cpp). WWHD_UPSCALER=fsr3 (WWHD_MOTION and WWHD_JITTER
// go with it). AMD's host code (src/third_party/fsr3, SDK v1.1.4, MIT) runs as it is; this file is its backend, the
// FfxInterface it calls for resources, pipelines and GPU work, written for this renderer instead of the SDK's own
// Vulkan backend: the passes' GLSL (vendored) is compiled here with glslang (the SDK compiles permutations ahead of
// time with a Windows tool), and each pipeline's bindings come from glslang's reflection, by the names AMD's host code
// looks them up by. Every FSR resource lives in VK_IMAGE_LAYOUT_GENERAL; each dispatch is followed by a barrier for
// all compute work (correctness first: 9 dispatches a frame). Off: nothing here runs.
#include "fsr3_compat.h"
#include "renderer_internal.h"
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <map>
#include <memory>
#include "fsr3_sources.inc"

// AMD's host code reports through these (ffx_message.h, ffx_assert.h: the SDK's own versions print to a Windows debugger)
static ffxMessageCallback s_ffxMessage = nullptr;
void ffxSetPrintMessageCallback(ffxMessageCallback callback, uint32_t)
{
	s_ffxMessage = callback;
}
void ffxPrintMessage(uint32_t type, const wchar_t* message)
{
	char text[512];
	snprintf(text, sizeof(text), "%ls", message ? message : L"");
	wwhd::gpu::Log(fmt::format("fsr3: {} {}", type == FFX_MESSAGE_TYPE_ERROR ? "error:" : "warning:", text));
}
bool ffxAssertReport(const char* file, int32_t line, const char* condition, const char* msg)
{
	wwhd::gpu::Log(fmt::format("fsr3: assertion at {}:{}: {} {}", file ? file : "?", line, condition ? condition : "", msg ? msg : ""));
	return false;
}
void ffxAssertSetPrintingCallback(FfxAssertCallback) {}

namespace wwhd::gpu::fsr3
{
	namespace
	{
		// ---- resources --------------------------------------------------------------------------------------
		struct Resource
		{
			Image own;                                           // made here (internal, shared); the image itself
			Image* image = nullptr;                              // the image (own, or a registered one of the renderer's)
			FfxResourceDescription desc{};
			std::vector<VkImageView> mipViews;                   // storage views, one per mip
			VkImageView sampled = VK_NULL_HANDLE;                // sampled view: all mips (depth: the depth aspect alone)
			bool external = false;
			bool cachedViews = false;                            // a registered image's views: s_viewCache's, not its own
			~Resource()
			{
				if (cachedViews)
					return;
				for (VkImageView v : mipViews)
					vkDestroyImageView(s.device, v, nullptr);
				if (sampled)
					vkDestroyImageView(s.device, sampled, nullptr);
			}
		};
		std::vector<std::unique_ptr<Resource>> s_resources;     // index = FfxResourceInternal::internalIndex
		std::vector<int32_t> s_dynamic;                          // registered this dispatch (unregistered after it)
		std::vector<std::unique_ptr<Resource>> s_retired[2];    // unregistered, by frame slot: freed when the slot comes round
		std::unordered_map<uint64, std::pair<VkImageView, VkImageView>> s_viewCache;   // a registered image's sampled, storage views
		std::unordered_map<VkImage, std::vector<uint64>> s_viewImages;               // their keys by image (Forget)

		VkFormat Format(FfxSurfaceFormat f)
		{
			switch (f)
			{
			case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
			case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
			case FFX_SURFACE_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
			case FFX_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
			case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
			case FFX_SURFACE_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
			case FFX_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
			case FFX_SURFACE_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_UINT;
			case FFX_SURFACE_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
			case FFX_SURFACE_FORMAT_R16_UINT: return VK_FORMAT_R16_UINT;
			case FFX_SURFACE_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
			case FFX_SURFACE_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
			case FFX_SURFACE_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
			case FFX_SURFACE_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
			case FFX_SURFACE_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
			case FFX_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
			default: return VK_FORMAT_UNDEFINED;
			}
		}

		uint32 TexelBytes(VkFormat f)
		{
			switch (f)
			{
			case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
			case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: return 8;
			case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R8G8_UNORM: return 2;
			case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: return 1;
			default: return 4;
			}
		}

		VkImageView MakeView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32 mip, uint32 mips)
		{
			VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			vi.image = image;
			vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vi.format = format;
			vi.subresourceRange = { aspect, mip, mips, 0, 1 };
			VkImageView v;
			Check(vkCreateImageView(s.device, &vi, nullptr, &v), "vkCreateImageView");
			return v;
		}

		void DestroyViews(Resource& r)
		{
			for (VkImageView v : r.mipViews)
				vkDestroyImageView(s.device, v, nullptr);
			r.mipViews.clear();
			if (r.sampled)
				vkDestroyImageView(s.device, r.sampled, nullptr);
			r.sampled = VK_NULL_HANDLE;
		}

		// a staging buffer for initial data, freed when its frame slot comes round again
		struct Staging { VkBuffer buffer; VkDeviceMemory memory; };
		std::vector<Staging> s_staging, s_retiredStaging[2];

		FfxErrorCode CreateResource(FfxInterface*, const FfxCreateResourceDescription* d, FfxUInt32, FfxResourceInternal* out)
		{
			const FfxResourceDescription& rd = d->resourceDescription;
			if (rd.type != FFX_RESOURCE_TYPE_TEXTURE2D)
			{
				Log(fmt::format("fsr3: a resource of type {} asked for (only 2D textures are made here)", (int)rd.type));
				return FFX_ERROR_INVALID_ARGUMENT;
			}
			const VkFormat format = Format(rd.format);
			if (format == VK_FORMAT_UNDEFINED)
			{
				Log(fmt::format("fsr3: surface format {} not mapped", (int)rd.format));
				return FFX_ERROR_INVALID_ARGUMENT;
			}
			auto res = std::make_unique<Resource>();
			res->desc = rd;
			uint32 mips = rd.mipCount;
			if (mips == 0)
				for (uint32 m = std::max(rd.width, rd.height); m; m >>= 1)
					mips++;
			res->desc.mipCount = mips;
			Image& img = res->own;
			VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			ci.imageType = VK_IMAGE_TYPE_2D;
			ci.format = format;
			ci.extent = { rd.width, rd.height, 1 };
			ci.mipLevels = mips;
			ci.arrayLayers = 1;
			ci.samples = VK_SAMPLE_COUNT_1_BIT;
			ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			Check(vkCreateImage(s.device, &ci, nullptr, &img.image), "vkCreateImage");
			VkMemoryRequirements req;
			vkGetImageMemoryRequirements(s.device, img.image, &req);
			VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			ai.allocationSize = req.size;
			ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			Check(vkAllocateMemory(s.device, &ai, nullptr, &img.memory), "vkAllocateMemory");
			Check(vkBindImageMemory(s.device, img.image, img.memory, 0), "vkBindImageMemory");
			img.format = format;
			img.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			img.width = img.gw = rd.width;
			img.height = img.gh = rd.height;
			// into GENERAL, every mip, and the initial data in
			EndRendering();
			VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = img.image;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1 };
			vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			img.layout = VK_IMAGE_LAYOUT_GENERAL;
			const FfxResourceInitData& init = d->initData;
			if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE || init.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER)
			{
				const VkDeviceSize bytes = (VkDeviceSize)rd.width * rd.height * TexelBytes(format);
				Staging st{};
				VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
				bi.size = bytes;
				bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
				Check(vkCreateBuffer(s.device, &bi, nullptr, &st.buffer), "vkCreateBuffer");
				vkGetBufferMemoryRequirements(s.device, st.buffer, &req);
				ai.allocationSize = req.size;
				ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
				Check(vkAllocateMemory(s.device, &ai, nullptr, &st.memory), "vkAllocateMemory");
				Check(vkBindBufferMemory(s.device, st.buffer, st.memory, 0), "vkBindBufferMemory");
				uint8* p;
				Check(vkMapMemory(s.device, st.memory, 0, bytes, 0, (void**)&p), "vkMapMemory");
				if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE)
					memset(p, init.value, bytes);
				else
					memcpy(p, init.buffer, std::min<VkDeviceSize>(bytes, init.size));
				vkUnmapMemory(s.device, st.memory);
				VkBufferImageCopy c{};
				c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				c.imageExtent = { rd.width, rd.height, 1 };
				vkCmdCopyBufferToImage(s.cmd, st.buffer, img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
				s_staging.push_back(st);
			}
			for (uint32 m = 0; m < mips; m++)
				res->mipViews.push_back(MakeView(img.image, format, VK_IMAGE_ASPECT_COLOR_BIT, m, 1));
			res->sampled = MakeView(img.image, format, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips);
			res->image = &res->own;
			out->internalIndex = (int32_t)s_resources.size();
			s_resources.push_back(std::move(res));
			return FFX_OK;
		}

		FfxErrorCode DestroyResource(FfxInterface*, FfxResourceInternal r, FfxUInt32)
		{
			if (r.internalIndex < 0 || r.internalIndex >= (int32_t)s_resources.size() || !s_resources[r.internalIndex])
				return FFX_OK;
			Resource& res = *s_resources[r.internalIndex];
			WaitPending();
			DestroyViews(res);
			if (!res.external)
				DestroyImage(res.own);
			s_resources[r.internalIndex].reset();
			return FFX_OK;
		}

		// FfxResource::resource is a Resource* made here (the shared resources), or an ExternalImage* (the renderer's)
		struct ExternalImage { Image* image; VkImageAspectFlags aspect; VkComponentMapping swizzle; };

		FfxErrorCode RegisterResource(FfxInterface*, const FfxResource* in, FfxUInt32, FfxResourceInternal* out)
		{
			if (!in->resource)
			{
				out->internalIndex = -1;
				return FFX_OK;
			}
			if (in->description.flags & FFX_RESOURCE_FLAGS_ALIASABLE)  // never set by us; the shared ones are ours
				;
			for (size_t i = 0; i < s_resources.size(); i++)          // one of ours (a shared resource)
				if (s_resources[i] && s_resources[i].get() == in->resource)
				{
					out->internalIndex = (int32_t)i;
					return FFX_OK;
				}
			const ExternalImage& e = *(const ExternalImage*)in->resource;
			auto res = std::make_unique<Resource>();
			res->external = true;
			res->cachedViews = true;
			res->image = e.image;
			res->desc = in->description;
			// the views, made once an image (they were a third of FSR 3's render-thread time on the worker's driver,
			// made and destroyed every frame); dropped when the renderer destroys the image (Forget)
			const uint64 key = (uint64)(uintptr_t)e.image->image ^ ((uint64)e.aspect << 56) ^ ((uint64)e.swizzle.r << 48);
			auto& views = s_viewCache[key];
			if (!views.first)
			{
				VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				vi.image = e.image->image;
				vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
				vi.format = e.image->format;
				vi.components = e.swizzle;
				vi.subresourceRange = { e.aspect, 0, 1, 0, 1 };
				Check(vkCreateImageView(s.device, &vi, nullptr, &views.first), "vkCreateImageView");
				if (!(e.aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
				{
					vi.components = {};
					Check(vkCreateImageView(s.device, &vi, nullptr, &views.second), "vkCreateImageView");
				}
				s_viewImages[e.image->image].push_back(key);
			}
			res->sampled = views.first;
			if (views.second)
				res->mipViews.push_back(views.second);
			out->internalIndex = (int32_t)s_resources.size();
			s_dynamic.push_back(out->internalIndex);
			s_resources.push_back(std::move(res));
			return FFX_OK;
		}

		FfxErrorCode UnregisterResources(FfxInterface*, FfxCommandList, FfxUInt32)
		{
			for (int32_t i : s_dynamic)                             // their views die once the GPU is past this frame
				if (s_resources[i])
					s_retired[s.slot].push_back(std::move(s_resources[i]));
			s_dynamic.clear();
			while (!s_resources.empty() && !s_resources.back())
				s_resources.pop_back();
			return FFX_OK;
		}

		FfxResource GetResource(FfxInterface*, FfxResourceInternal r)
		{
			FfxResource out{};
			if (r.internalIndex >= 0 && r.internalIndex < (int32_t)s_resources.size() && s_resources[r.internalIndex])
			{
				out.resource = s_resources[r.internalIndex].get();
				out.description = s_resources[r.internalIndex]->desc;
				out.state = FFX_RESOURCE_STATE_UNORDERED_ACCESS;
			}
			return out;
		}

		FfxResourceDescription GetResourceDescription(FfxInterface*, FfxResourceInternal r)
		{
			if (r.internalIndex >= 0 && r.internalIndex < (int32_t)s_resources.size() && s_resources[r.internalIndex])
				return s_resources[r.internalIndex]->desc;
			return {};
		}

		// ---- constant buffers: copies until the jobs run --------------------------------------------------
		std::vector<std::vector<uint32>> s_constants;
		FfxErrorCode StageConstants(FfxInterface*, void* data, FfxUInt32 size, FfxConstantBuffer* cb)
		{
			auto& v = s_constants.emplace_back((size + 3) / 4);
			memcpy(v.data(), data, size);
			cb->data = v.data();
			cb->num32BitEntries = (size + 3) / 4;
			return FFX_OK;
		}

		// ---- pipelines -------------------------------------------------------------------------------------
		struct Pipeline
		{
			VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
			VkPipelineLayout layout = VK_NULL_HANDLE;
			VkPipeline pipeline = VK_NULL_HANDLE;
			uint32 cbBindings[FFX_MAX_NUM_CONST_BUFFERS]{}, cbSizes[FFX_MAX_NUM_CONST_BUFFERS]{};
		};
		VkSampler s_point = VK_NULL_HANDLE, s_linear = VK_NULL_HANDLE;

		const char* PassFile(FfxPass pass)
		{
			switch ((FfxFsr3UpscalerPass)pass)
			{
			case FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS: return "shaders/fsr3upscaler/ffx_fsr3upscaler_prepare_inputs_pass.glsl";
			case FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID: return "shaders/fsr3upscaler/ffx_fsr3upscaler_luma_pyramid_pass.glsl";
			case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID: return "shaders/fsr3upscaler/ffx_fsr3upscaler_shading_change_pyramid_pass.glsl";
			case FFX_FSR3UPSCALER_PASS_SHADING_CHANGE: return "shaders/fsr3upscaler/ffx_fsr3upscaler_shading_change_pass.glsl";
			case FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY: return "shaders/fsr3upscaler/ffx_fsr3upscaler_prepare_reactivity_pass.glsl";
			case FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY: return "shaders/fsr3upscaler/ffx_fsr3upscaler_luma_instability_pass.glsl";
			case FFX_FSR3UPSCALER_PASS_ACCUMULATE: case FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN:
				return "shaders/fsr3upscaler/ffx_fsr3upscaler_accumulate_pass.glsl";
			case FFX_FSR3UPSCALER_PASS_RCAS: return "shaders/fsr3upscaler/ffx_fsr3upscaler_rcas_pass.glsl";
			case FFX_FSR3UPSCALER_PASS_DEBUG_VIEW: return "shaders/fsr3upscaler/ffx_fsr3upscaler_debug_view_pass.glsl";
			case FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE: return "shaders/fsr3upscaler/ffx_fsr3upscaler_autogen_reactive_pass.glsl";
			default: return nullptr;
			}
		}

		const char* Source(const std::string& path)
		{
			for (const auto& f : kFsr3Sources)
				if (path == f.path)
					return f.text;
			return nullptr;
		}

		// the vendored headers, by the paths the shaders include them by (relative to the GPU include directory, or
		// to the including file's directory)
		class Includer : public glslang::TShader::Includer
		{
		public:
			IncludeResult* includeLocal(const char* name, const char* includer, size_t) override
			{
				std::string dir = includer ? includer : "";
				dir = dir.find('/') != std::string::npos ? dir.substr(0, dir.rfind('/') + 1) : "";
				if (const char* t = Source(dir + name))
					return new IncludeResult(dir + name, t, strlen(t), nullptr);
				return includeSystem(name, includer, 0);
			}
			IncludeResult* includeSystem(const char* name, const char*, size_t) override
			{
				if (const char* t = Source(name))
					return new IncludeResult(name, t, strlen(t), nullptr);
				return nullptr;
			}
			void releaseInclude(IncludeResult* r) override { delete r; }
		};

		std::string ConstantBufferName(std::string block)
		{
			if (block.size() > 2 && block.compare(block.size() - 2, 2, "_t") == 0)
				block.resize(block.size() - 2);
			if (block == "cbFSR3UPSCALER")
				return "cbFSR3Upscaler";
			return block;                                        // cbSPD, cbRCAS, cbGenerateReactive
		}

		void Widen(const std::string& in, wchar_t (&out)[FFX_RESOURCE_NAME_SIZE])
		{
			size_t i = 0;
			for (; i < in.size() && i + 1 < FFX_RESOURCE_NAME_SIZE; i++)
				out[i] = (wchar_t)in[i];
			out[i] = 0;
		}

		FfxErrorCode CreatePipeline(FfxInterface*, FfxEffect effect, FfxPass pass, uint32_t options, const FfxPipelineDescription* desc,
			FfxUInt32, FfxPipelineState* out)
		{
			const char* file = PassFile(pass);
			const char* text = file ? Source(file) : nullptr;
			if (effect != FFX_EFFECT_FSR3UPSCALER || !text)
				return FFX_ERROR_INVALID_ARGUMENT;
			// the permutation's defines (CMakeCompileFSR3UpscalerShaders.txt, the Vulkan backend's arguments)
			std::string defines = "#define FFX_GPU 1\n#define FFX_GLSL 1\n"
				"#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF 0\n#define FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF 0\n"
				"#define FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF 1\n#define FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF 0\n"
				"#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 2\n";
			auto flag = [&](const char* name, uint32 bit) { defines += fmt::format("#define {} {}\n", name, (options & bit) ? 1 : 0); };
			flag("FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE", 1u << 0);
			flag("FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT", 1u << 1);
			flag("FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS", 1u << 2);
			flag("FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS", 1u << 3);
			flag("FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH", 1u << 4);
			flag("FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING", 1u << 5);
			if (options & (1u << 7))
				defines += "#define FFX_HALF 1\n";
			// the shader's own text after its #version line, the defines before the rest
			std::string src = text;
			const size_t v = src.find("#version");
			const size_t eol = v == std::string::npos ? 0 : src.find('\n', v) + 1;
			src = src.substr(0, eol) + defines + src.substr(eol);
			glslang::TShader shader(EShLangCompute);
			const char* strings[] = { src.c_str() };
			const char* names[] = { file };
			shader.setStringsWithLengthsAndNames(strings, nullptr, names, 1);
			shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
			shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
			shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
			Includer includer;
			const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
			if (!shader.parse(GetDefaultResources(), 450, false, messages, includer))
			{
				Log(fmt::format("fsr3: {} (options {:x}) doesn't compile: {}", file, options, std::string(shader.getInfoLog()).substr(0, 400)));
				return FFX_ERROR_BACKEND_API_ERROR;
			}
			glslang::TProgram program;
			program.addShader(&shader);
			if (!program.link(messages) || !program.buildReflection(EShReflectionDefault | EShReflectionSeparateBuffers))
			{
				Log(fmt::format("fsr3: {} doesn't link: {}", file, program.getInfoLog()));
				return FFX_ERROR_BACKEND_API_ERROR;
			}
			std::vector<uint32> spirv;
			glslang::SpvOptions opt;
			opt.disableOptimizer = false;
			glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv, &opt);
			// bindings from the reflection: textures (SRVs), storage images (UAVs), uniform blocks, the two samplers
			auto* p = new Pipeline;
			std::vector<VkDescriptorSetLayoutBinding> bindings;
			memset(out, 0, sizeof(*out));
			// FSR's GLSL names its bindings by kind: r_* sampled textures, rw_* storage images, s_* the two samplers
			for (int i = 0; i < program.getNumUniformVariables(); i++)
			{
				const glslang::TObjectReflection& u = program.getUniform(i);
				if (u.getBinding() < 0)
					continue;
				const uint32 binding = (uint32)u.getBinding();
				const std::string name = u.name.substr(0, u.name.find('['));
				const uint32 count = std::max(u.size, 1);
				if (name.rfind("s_", 0) == 0)
				{
					static VkSampler* immutable[] = { &s_point, &s_linear };
					bindings.push_back({ binding, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
						name == "s_PointClamp" ? immutable[0] : immutable[1] });
				}
				else if (name.rfind("rw_", 0) == 0)
				{
					bindings.push_back({ binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr });
					for (uint32 k = 0; k < count && out->uavTextureCount < FFX_MAX_NUM_UAVS; k++)
					{
						auto& b = out->uavTextureBindings[out->uavTextureCount++];
						b.slotIndex = binding;
						b.arrayIndex = k;
						Widen(count > 1 ? name + fmt::format("{}", k) : name, b.name);
					}
				}
				else if (name.rfind("r_", 0) == 0)
				{
					bindings.push_back({ binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr });
					auto& b = out->srvTextureBindings[out->srvTextureCount++];
					b.slotIndex = binding;
					Widen(name, b.name);
				}
				else
					Log(fmt::format("fsr3: {}: a binding of unknown kind: {}", file, name));
			}
			for (int i = 0; i < program.getNumUniformBlocks(); i++)
			{
				const glslang::TObjectReflection& u = program.getUniformBlock(i);
				if (u.getBinding() < 0 || out->constCount >= FFX_MAX_NUM_CONST_BUFFERS)
					continue;
				bindings.push_back({ (uint32)u.getBinding(), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr });
				const uint32 c = out->constCount++;
				p->cbBindings[c] = (uint32)u.getBinding();
				p->cbSizes[c] = (uint32)u.size;
				auto& b = out->constantBufferBindings[c];
				b.slotIndex = (uint32)u.getBinding();
				Widen(ConstantBufferName(u.name), b.name);
			}
			VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
			li.bindingCount = (uint32)bindings.size();
			li.pBindings = bindings.data();
			Check(vkCreateDescriptorSetLayout(s.device, &li, nullptr, &p->setLayout), "vkCreateDescriptorSetLayout");
			VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			pli.setLayoutCount = 1;
			pli.pSetLayouts = &p->setLayout;
			Check(vkCreatePipelineLayout(s.device, &pli, nullptr, &p->layout), "vkCreatePipelineLayout");
			VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			mi.codeSize = spirv.size() * 4;
			mi.pCode = spirv.data();
			VkShaderModule module;
			Check(vkCreateShaderModule(s.device, &mi, nullptr, &module), "vkCreateShaderModule");
			VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
			ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			ci.stage.module = module;
			ci.stage.pName = "main";
			ci.layout = p->layout;
			Check(vkCreateComputePipelines(s.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p->pipeline), "vkCreateComputePipelines");
			vkDestroyShaderModule(s.device, module, nullptr);
			out->pipeline = p;
			out->passId = pass;
			if (desc)
				wcscpy_s(out->name, desc->name);
			return FFX_OK;
		}

		FfxErrorCode DestroyPipeline(FfxInterface*, FfxPipelineState* state, FfxUInt32)
		{
			if (auto* p = (Pipeline*)state->pipeline)
			{
				WaitPending();
				vkDestroyPipeline(s.device, p->pipeline, nullptr);
				vkDestroyPipelineLayout(s.device, p->layout, nullptr);
				vkDestroyDescriptorSetLayout(s.device, p->setLayout, nullptr);
				delete p;
				state->pipeline = nullptr;
			}
			return FFX_OK;
		}

		// ---- the GPU work ----------------------------------------------------------------------------------
		std::vector<FfxGpuJobDescription> s_jobs;
		FfxErrorCode ScheduleGpuJob(FfxInterface*, const FfxGpuJobDescription* job)
		{
			s_jobs.push_back(*job);
			return FFX_OK;
		}

		VkDescriptorPool s_pools[2]{};
		uint32 s_poolFrame[2] = { UINT32_MAX, UINT32_MAX };

		VkDescriptorPool Pool()
		{
			const uint32 slot = s.slot;
			if (!s_pools[slot])
			{
				VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024 },
					{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 256 } };
				VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
				pi.maxSets = 128;
				pi.poolSizeCount = 4;
				pi.pPoolSizes = sizes;
				Check(vkCreateDescriptorPool(s.device, &pi, nullptr, &s_pools[slot]), "vkCreateDescriptorPool");
			}
			if (s_poolFrame[slot] != s.frame)                        // this slot's last frame is done (its fence waited)
			{
				vkResetDescriptorPool(s.device, s_pools[slot], 0);
				s_poolFrame[slot] = s.frame;
			}
			return s_pools[slot];
		}

		void ComputeBarrier()
		{
			VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
		}

		Resource* Res(FfxResourceInternal r)
		{
			return r.internalIndex >= 0 && r.internalIndex < (int32_t)s_resources.size() ? s_resources[r.internalIndex].get() : nullptr;
		}

		void RunCompute(const FfxComputeJobDescription& job)
		{
			auto* p = (Pipeline*)job.pipeline.pipeline;
			VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			ai.descriptorPool = Pool();
			ai.descriptorSetCount = 1;
			ai.pSetLayouts = &p->setLayout;
			VkDescriptorSet set;
			Check(vkAllocateDescriptorSets(s.device, &ai, &set), "vkAllocateDescriptorSets");
			std::vector<VkWriteDescriptorSet> writes;
			std::vector<VkDescriptorImageInfo> images(FFX_MAX_NUM_SRVS + FFX_MAX_NUM_UAVS);
			std::vector<VkDescriptorBufferInfo> buffers(FFX_MAX_NUM_CONST_BUFFERS);
			uint32 n = 0;
			for (uint32 i = 0; i < job.pipeline.srvTextureCount; i++)
			{
				Resource* r = Res(job.srvTextures[i].resource);
				if (!r)
					continue;
				if (r->external)
					Transition(*r->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				images[n] = { VK_NULL_HANDLE, r->sampled, r->external ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL };
				VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
				w.dstSet = set;
				w.dstBinding = job.pipeline.srvTextureBindings[i].slotIndex;
				w.descriptorCount = 1;
				w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				w.pImageInfo = &images[n++];
				writes.push_back(w);
			}
			for (uint32 i = 0; i < job.pipeline.uavTextureCount; i++)
			{
				Resource* r = Res(job.uavTextures[i].resource);
				if (!r || r->mipViews.empty())
					continue;
				if (r->external)
					Transition(*r->image, VK_IMAGE_LAYOUT_GENERAL);
				const uint32 mip = std::min<uint32>(job.uavTextures[i].mip, (uint32)r->mipViews.size() - 1);
				images[n] = { VK_NULL_HANDLE, r->mipViews[mip], VK_IMAGE_LAYOUT_GENERAL };
				VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
				w.dstSet = set;
				w.dstBinding = job.pipeline.uavTextureBindings[i].slotIndex;
				w.dstArrayElement = job.pipeline.uavTextureBindings[i].arrayIndex;
				w.descriptorCount = 1;
				w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				w.pImageInfo = &images[n++];
				writes.push_back(w);
			}
			for (uint32 i = 0; i < job.pipeline.constCount; i++)
			{
				const uint32 bytes = std::max(p->cbSizes[i], job.cbs[i].num32BitEntries * 4u);
				const VkDeviceSize off = RingAlloc(bytes, s.props.limits.minUniformBufferOffsetAlignment);
				memset(s.ring.data + off, 0, bytes);
				if (job.cbs[i].data)
					memcpy(s.ring.data + off, job.cbs[i].data, job.cbs[i].num32BitEntries * 4u);
				buffers[i] = { s.ring.buffer, off, bytes };
				VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
				w.dstSet = set;
				w.dstBinding = p->cbBindings[i];
				w.descriptorCount = 1;
				w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				w.pBufferInfo = &buffers[i];
				writes.push_back(w);
			}
			vkUpdateDescriptorSets(s.device, (uint32)writes.size(), writes.data(), 0, nullptr);
			vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
			vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, nullptr);
			vkCmdDispatch(s.cmd, job.dimensions[0], job.dimensions[1], job.dimensions[2]);
			ComputeBarrier();
		}

		FfxErrorCode ExecuteGpuJobs(FfxInterface*, FfxCommandList, FfxUInt32)
		{
			EndRendering();
			ComputeBarrier();
			for (const auto& job : s_jobs)
			{
				switch (job.jobType)
				{
				case FFX_GPU_JOB_CLEAR_FLOAT:
				{
					Resource* r = Res(job.clearJobDescriptor.target);
					if (!r)
						break;
					VkClearColorValue c;
					memcpy(c.float32, job.clearJobDescriptor.color, 16);
					VkImageSubresourceRange all{ VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
					vkCmdClearColorImage(s.cmd, r->image->image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &all);
					ComputeBarrier();
					break;
				}
				case FFX_GPU_JOB_COPY:
				{
					Resource* src = Res(job.copyJobDescriptor.src), *dst = Res(job.copyJobDescriptor.dst);
					if (!src || !dst)
						break;
					VkImageCopy c{};
					c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
					c.extent = { std::min(src->image->width, dst->image->width), std::min(src->image->height, dst->image->height), 1 };
					vkCmdCopyImage(s.cmd, src->image->image, VK_IMAGE_LAYOUT_GENERAL, dst->image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
					ComputeBarrier();
					break;
				}
				case FFX_GPU_JOB_COMPUTE:
					RunCompute(job.computeJobDescriptor);
					break;
				default:
					break;
				}
			}
			s_jobs.clear();
			s_constants.clear();
			s.bound.valid = false;
			return FFX_OK;
		}

		// ---- the rest of the interface ---------------------------------------------------------------------
		FfxVersionNumber GetSDKVersion(FfxInterface*) { return FFX_SDK_MAKE_VERSION(1, 1, 4); }
		FfxErrorCode GetEffectGpuMemoryUsage(FfxInterface*, FfxUInt32, FfxEffectMemoryUsage* out) { *out = {}; return FFX_OK; }
		FfxErrorCode CreateBackendContext(FfxInterface*, FfxEffect, FfxEffectBindlessConfig*, FfxUInt32* out) { *out = 0; return FFX_OK; }
		FfxErrorCode DestroyBackendContext(FfxInterface*, FfxUInt32) { return FFX_OK; }
		FfxErrorCode GetDeviceCapabilities(FfxInterface*, FfxDeviceCapabilities* caps)
		{
			*caps = {};
			caps->maximumSupportedShaderModel = FFX_SHADER_MODEL_6_5;
			caps->waveLaneCountMin = 32;
			caps->waveLaneCountMax = 64;
			caps->fp16Supported = s.subgroupHalf;
			return FFX_OK;
		}
		FfxErrorCode RegisterStaticResource(FfxInterface*, const FfxStaticResourceDescription*, FfxUInt32) { return FFX_ERROR_INVALID_ARGUMENT; }
		FfxErrorCode MapResource(FfxInterface*, FfxResourceInternal, void**) { return FFX_ERROR_INVALID_ARGUMENT; }
		FfxErrorCode UnmapResource(FfxInterface*, FfxResourceInternal) { return FFX_OK; }

		// ---- the context --------------------------------------------------------------------------------------
		struct State
		{
			bool tried = false, ok = false;
			FfxInterface iface{};
			std::vector<uint8> scratch;
			std::unique_ptr<FfxFsr3UpscalerContext> context;
			uint32 maxW = 0, maxH = 0;                           // the context's render size bound (the guest's)
			Resource* shared[3]{};                               // dilated depth, dilated motion, reconstructed previous depth
			uint32 frames = 0;
		};
		State s_fsr3;
	}

	bool On()
	{
		static const bool on = [] { const char* e = getenv("WWHD_UPSCALER"); return e && strcmp(e, "fsr3") == 0; }();
		return on;
	}

	void Forget(VkImage image)
	{
		auto it = s_viewImages.find(image);
		if (it == s_viewImages.end())
			return;
		for (uint64 key : it->second)
			if (auto v = s_viewCache.find(key); v != s_viewCache.end())
			{
				if (v->second.first)
					vkDestroyImageView(s.device, v->second.first, nullptr);
				if (v->second.second)
					vkDestroyImageView(s.device, v->second.second, nullptr);
				s_viewCache.erase(v);
			}
		s_viewImages.erase(it);
	}

	namespace
	{
		bool Init(uint32 w, uint32 h)
		{
			State& f = s_fsr3;
			if (f.context && (w > f.maxW || h > f.maxH))         // larger than the context was made for: made again
			                                                      // (smaller is a dispatch's own size: surface fit's 1080 rows)
			{
				SubmitAndWait();                                    // the GPU idle: the recording frame may use them too
				ffxFsr3UpscalerContextDestroy(f.context.get());
				for (Resource*& r : f.shared)
					if (r)
						DestroyResource(nullptr, { (int32_t)(std::find_if(s_resources.begin(), s_resources.end(),
							[&](const auto& p) { return p.get() == r; }) - s_resources.begin()) }, 0), r = nullptr;
				f.context.reset();
			}
			if (f.context)
				return true;
			if (f.tried && !f.ok)
				return false;
			f.tried = true;
			VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
			si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			si.maxLod = VK_LOD_CLAMP_NONE;
			if (!s_point)
			{
				si.magFilter = si.minFilter = VK_FILTER_NEAREST;
				Check(vkCreateSampler(s.device, &si, nullptr, &s_point), "vkCreateSampler");
				si.magFilter = si.minFilter = VK_FILTER_LINEAR;
				si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
				Check(vkCreateSampler(s.device, &si, nullptr, &s_linear), "vkCreateSampler");
			}
			FfxInterface& i = f.iface;
			i.fpGetSDKVersion = GetSDKVersion;
			i.fpGetEffectGpuMemoryUsage = GetEffectGpuMemoryUsage;
			i.fpCreateBackendContext = CreateBackendContext;
			i.fpGetDeviceCapabilities = GetDeviceCapabilities;
			i.fpDestroyBackendContext = DestroyBackendContext;
			i.fpCreateResource = CreateResource;
			i.fpRegisterResource = RegisterResource;
			i.fpGetResource = GetResource;
			i.fpUnregisterResources = UnregisterResources;
			i.fpRegisterStaticResource = RegisterStaticResource;
			i.fpGetResourceDescription = GetResourceDescription;
			i.fpDestroyResource = DestroyResource;
			i.fpMapResource = MapResource;
			i.fpUnmapResource = UnmapResource;
			i.fpStageConstantBufferDataFunc = StageConstants;
			i.fpCreatePipeline = CreatePipeline;
			i.fpDestroyPipeline = DestroyPipeline;
			i.fpScheduleGpuJob = ScheduleGpuJob;
			i.fpExecuteGpuJobs = ExecuteGpuJobs;
			f.scratch.resize(1 << 20);
			i.scratchBuffer = f.scratch.data();
			i.scratchBufferSize = f.scratch.size();
			i.device = s.device;
			FfxFsr3UpscalerContextDescription d{};
			d.flags = FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
			d.maxRenderSize = { w, h };
			d.maxUpscaleSize = { w, h };
			d.backendInterface = i;
			f.context = std::make_unique<FfxFsr3UpscalerContext>();
			if (const FfxErrorCode e = ffxFsr3UpscalerContextCreate(f.context.get(), &d); e != FFX_OK)
			{
				Log(fmt::format("fsr3: the context wasn't made (error {:#x}); FSR 3 is off", (uint32)e));
				f.context.reset();
				return f.ok = false;
			}
			FfxFsr3UpscalerSharedResourceDescriptions shared{};
			ffxFsr3UpscalerGetSharedResourceDescriptions(f.context.get(), &shared);
			const FfxCreateResourceDescription* descs[3] = { &shared.dilatedDepth, &shared.dilatedMotionVectors, &shared.reconstructedPrevNearestDepth };
			for (int k = 0; k < 3; k++)
			{
				FfxResourceInternal r;
				if (CreateResource(nullptr, descs[k], 0, &r) != FFX_OK)
					return f.ok = false;
				f.shared[k] = s_resources[r.internalIndex].get();
			}
			f.maxW = w, f.maxH = h;
			Log(fmt::format("fsr3: AMD FidelityFX FSR 3.1 upscaler (SDK v1.1.4) up to {}x{}, {} (WWHD_UPSCALER=fsr3)", w, h,
				s.subgroupHalf ? "fp16" : "fp32"));
			return f.ok = true;
		}

		FfxResource External(ExternalImage& e, FfxResourceStates state)
		{
			FfxResource r{};
			r.resource = &e;
			r.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
			r.description.format = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
			r.description.width = e.image->width;
			r.description.height = e.image->height;
			r.description.depth = 1;
			r.description.mipCount = 1;
			r.state = state;
			return r;
		}

		FfxResource Shared(Resource* r)
		{
			FfxResource out{};
			out.resource = r;
			out.description = r->desc;
			out.state = FFX_RESOURCE_STATE_UNORDERED_ACCESS;
			return out;
		}
	}

	bool Upscale(Image& color, Image& depth, Image& motionTarget, Image& out, float jitterX, float jitterY, bool reset)
	{
		// its CPU time on the render thread, a line every 600 dispatches (the first's context and pipelines left out)
		struct Timer
		{
			std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
			~Timer()
			{
				static double s_ms = 0;
				static uint32 s_n = 0;
				if (s_fsr3.frames < 2)
					return;
				s_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				if (++s_n % 600 == 0)
				{
					Log(fmt::format("fsr3: {:.3f} ms of the render thread a dispatch (the last 600)", s_ms / 600));
					s_ms = 0;
				}
			}
		} timer;
		if (!Init(out.width, out.height))
			return false;
		EndRendering();
		timing::Scope span(timing::Kind::Upscale, !timing::On() ? std::string() : fmt::format("fsr3 {}x{} -> {}x{}", color.width,
			color.height, out.width, out.height));
		s_retired[s.slot].clear();                                // this slot's last frame is done
		for (const Staging& st : s_retiredStaging[s.slot])
		{
			vkDestroyBuffer(s.device, st.buffer, nullptr);
			vkFreeMemory(s.device, st.memory, nullptr);
		}
		s_retiredStaging[s.slot].clear();
		for (const Staging& st : s_staging)
			s_retiredStaging[s.slot].push_back(st);
		s_staging.clear();
		const VkComponentMapping id{}, blue{ VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_B };
		ExternalImage c{ &color, VK_IMAGE_ASPECT_COLOR_BIT, id }, d{ &depth, VK_IMAGE_ASPECT_DEPTH_BIT, id },
			m{ &motionTarget, VK_IMAGE_ASPECT_COLOR_BIT, id }, r{ &motionTarget, VK_IMAGE_ASPECT_COLOR_BIT, blue }, o{ &out, VK_IMAGE_ASPECT_COLOR_BIT, id };
		FfxFsr3UpscalerDispatchDescription dd{};
		dd.commandList = s.cmd;
		dd.color = External(c, FFX_RESOURCE_STATE_COMPUTE_READ);
		dd.depth = External(d, FFX_RESOURCE_STATE_COMPUTE_READ);
		dd.motionVectors = External(m, FFX_RESOURCE_STATE_COMPUTE_READ);
		dd.reactive = External(r, FFX_RESOURCE_STATE_COMPUTE_READ);
		dd.output = External(o, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
		dd.dilatedDepth = Shared(s_fsr3.shared[0]);
		dd.dilatedMotionVectors = Shared(s_fsr3.shared[1]);
		dd.reconstructedPrevNearestDepth = Shared(s_fsr3.shared[2]);
		auto signs = [](const char* var, float x, float y) {
			if (const char* e = getenv(var))
				sscanf(e, "%f,%f", &x, &y);
			return std::pair{ x, y };
		};
		static const auto mvSign = signs("WWHD_FSR3_MV", -1.0f, 1.0f), jitterSign = signs("WWHD_FSR3_JITTER", 1.0f, 1.0f);
		dd.jitterOffset = { jitterSign.first * jitterX, jitterSign.second * jitterY };
		// the motion target holds (current - previous) in clip units halved; FSR wants (previous - current) in pixels of the
		// render size: x flipped, y not (the viewport's flip). Tested on tour3 at 50%: of the four sign pairs this one is
		// nearest 100% (PSNR 35.0, the others 33.6-34.3). WWHD_FSR3_MV=sx,sy and WWHD_FSR3_JITTER=sx,sy for tests
		dd.motionVectorScale = { mvSign.first * color.width, mvSign.second * color.height };
		dd.renderSize = { color.width, color.height };
		dd.upscaleSize = { out.width, out.height };
		static const float sharpness = [] { const char* e = getenv("WWHD_FSR_SHARPNESS"); return e ? (float)atof(e) : 0.2f; }();
		dd.enableSharpening = sharpness > 0.0f;
		dd.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
		dd.frameTimeDelta = 1000.0f / 60.0f;
		dd.preExposure = 1.0f;
		dd.reset = reset || s_fsr3.frames == 0;
		dd.cameraNear = 1.0f;                                    // the game's camera, roughly (its depth isn't linearised here)
		dd.cameraFar = 100000.0f;
		dd.cameraFovAngleVertical = 1.047f;
		dd.viewSpaceToMetersFactor = 0.01f;
		if (const FfxErrorCode e = ffxFsr3UpscalerContextDispatch(s_fsr3.context.get(), &dd); e != FFX_OK)
		{
			static uint32 s_logged = 0;
			if (s_logged++ < 4)
				Log(fmt::format("fsr3: dispatch failed (error {:#x})", (uint32)e));
			return false;
		}
		s_fsr3.frames++;
		return true;
	}
}
