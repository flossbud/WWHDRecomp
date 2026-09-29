// Textures (docs/recompiler-design.md D13; milestone G2c): what a draw's texture units sample.
//
// A texture unit whose address is a surface this renderer drew into (render to texture) samples
// that surface. Any other texture is untiled and decoded from guest memory by Cemu's texture
// loader (Core/LatteTextureLoader.cpp: the address library and the TextureDecoder classes, kept in
// wwhd-null for this), into a Vulkan image in the format Cemu's Vulkan renderer picks for it. A
// loaded texture is hashed on its first use in each frame and reloaded when its data changed.
// Samplers and view swizzles follow the registers as Cemu's Vulkan renderer reads them.
//
// Derived in part from Cemu (Renderer/Vulkan/VulkanRenderer.cpp GetTextureFormatInfoVK,
// VulkanRendererCore.cpp draw_getOrCreateDescriptorSet, LatteTextureViewVk.cpp,
// Core/LatteTextureLegacy.cpp, Core/LatteTextureLoader.cpp); Mozilla Public License 2.0.
#include "renderer_internal.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N& texUnitWord1,
	const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N& texUnitWord4);  // latte_glue.cpp
void LatteTextureLoader_begin(LatteTextureLoaderCtx* textureLoader, uint32 sliceIndex, uint32 mipIndex, MPTR physImagePtr,
	MPTR physMipPtr, Latte::E_GX2SURFFMT format, Latte::E_DIM dim, uint32 width, uint32 height, uint32 depth, uint32 mipLevels,
	uint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle);

namespace wwhd::gpu
{
	namespace
	{
		template<typename F>
		void LogOnce(const std::string& what, F&& msg)
		{
			static std::set<std::string> seen;
			if (seen.insert(what).second)
				Log(msg());
		}

		// ---- formats (VulkanRenderer::GetTextureFormatInfoVK) ------------------------------------------
		struct TexFormat
		{
			VkFormat vk = VK_FORMAT_UNDEFINED;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			TextureDecoder* decoder = nullptr;
		};
		bool s_d24s8 = false;

		TexFormat TextureFormat(Latte::E_GX2SURFFMT format, bool isDepth)
		{
			using F = Latte::E_GX2SURFFMT;
			TexFormat f;
			if (isDepth)
			{
				f.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
				switch (format)
				{
				case F::D24_S8_UNORM:
					f.aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
					f.vk = s_d24s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
					break;                                              // depth-stencil data isn't uploaded (see Load)
				case F::D24_S8_FLOAT: f.vk = VK_FORMAT_D32_SFLOAT_S8_UINT; f.aspect |= VK_IMAGE_ASPECT_STENCIL_BIT; break;
				case F::D32_FLOAT: f.vk = VK_FORMAT_D32_SFLOAT; f.decoder = TextureDecoder_R32_FLOAT::getInstance(); break;
				case F::D16_UNORM: f.vk = VK_FORMAT_D16_UNORM; f.decoder = TextureDecoder_R16_UNORM::getInstance(); break;
				case F::D32_S8_FLOAT: f.vk = VK_FORMAT_D32_SFLOAT_S8_UINT; f.aspect |= VK_IMAGE_ASPECT_STENCIL_BIT; break;
				default:
					LogOnce(fmt::format("dtex{:x}", (uint32)format), [&] { return fmt::format("depth texture format {:#x} not mapped", (uint32)format); });
					f.vk = VK_FORMAT_D16_UNORM;
					break;
				}
				return f;
			}
			if (format == (F::R16_G16_B16_A16_FLOAT | F::FMT_BIT_SRGB))
				format = F::R16_G16_B16_A16_FLOAT;
			auto set = [&](VkFormat vk, TextureDecoder* d) { f.vk = vk; f.decoder = d; };
			switch (format)
			{
			case F::R32_G32_B32_A32_FLOAT: set(VK_FORMAT_R32G32B32A32_SFLOAT, TextureDecoder_R32_G32_B32_A32_FLOAT::getInstance()); break;
			case F::R32_G32_B32_A32_UINT: set(VK_FORMAT_R32G32B32A32_UINT, TextureDecoder_R32_G32_B32_A32_UINT::getInstance()); break;
			case F::R16_G16_B16_A16_FLOAT: set(VK_FORMAT_R16G16B16A16_SFLOAT, TextureDecoder_R16_G16_B16_A16_FLOAT::getInstance()); break;
			case F::R16_G16_B16_A16_UINT: set(VK_FORMAT_R16G16B16A16_UINT, TextureDecoder_R16_G16_B16_A16_UINT::getInstance()); break;
			case F::R16_G16_B16_A16_UNORM: set(VK_FORMAT_R16G16B16A16_UNORM, TextureDecoder_R16_G16_B16_A16::getInstance()); break;
			case F::R16_G16_B16_A16_SNORM: set(VK_FORMAT_R16G16B16A16_SNORM, TextureDecoder_R16_G16_B16_A16::getInstance()); break;
			case F::R8_G8_B8_A8_UNORM: set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R8_G8_B8_A8::getInstance()); break;
			case F::R8_G8_B8_A8_SNORM: set(VK_FORMAT_R8G8B8A8_SNORM, TextureDecoder_R8_G8_B8_A8::getInstance()); break;
			case F::R8_G8_B8_A8_SRGB: set(VK_FORMAT_R8G8B8A8_SRGB, TextureDecoder_R8_G8_B8_A8::getInstance()); break;
			case F::R8_G8_B8_A8_UINT: set(VK_FORMAT_R8G8B8A8_UINT, TextureDecoder_R8_G8_B8_A8::getInstance()); break;
			case F::R8_G8_B8_A8_SINT: set(VK_FORMAT_R8G8B8A8_SINT, TextureDecoder_R8_G8_B8_A8::getInstance()); break;
			case F::R32_G32_FLOAT: set(VK_FORMAT_R32G32_SFLOAT, TextureDecoder_R32_G32_FLOAT::getInstance()); break;
			case F::R32_G32_UINT: set(VK_FORMAT_R32G32_UINT, TextureDecoder_R32_G32_UINT::getInstance()); break;
			case F::R16_G16_UNORM: set(VK_FORMAT_R16G16_UNORM, TextureDecoder_R16_G16::getInstance()); break;
			case F::R16_G16_FLOAT: set(VK_FORMAT_R16G16_SFLOAT, TextureDecoder_R16_G16_FLOAT::getInstance()); break;
			case F::R8_G8_UNORM: set(VK_FORMAT_R8G8_UNORM, TextureDecoder_R8_G8::getInstance()); break;
			case F::R8_G8_SNORM: set(VK_FORMAT_R8G8_SNORM, TextureDecoder_R8_G8::getInstance()); break;
			case F::R4_G4_UNORM: set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R4G4_UNORM_To_RGBA8::getInstance()); break;
			case F::R32_FLOAT: set(VK_FORMAT_R32_SFLOAT, TextureDecoder_R32_FLOAT::getInstance()); break;
			case F::R32_UINT: set(VK_FORMAT_R32_UINT, TextureDecoder_R32_UINT::getInstance()); break;
			case F::R16_FLOAT: set(VK_FORMAT_R16_SFLOAT, TextureDecoder_R16_FLOAT::getInstance()); break;
			case F::R16_UNORM: set(VK_FORMAT_R16_UNORM, TextureDecoder_R16_UNORM::getInstance()); break;
			case F::R16_SNORM: set(VK_FORMAT_R16_SNORM, TextureDecoder_R16_SNORM::getInstance()); break;
			case F::R16_UINT: set(VK_FORMAT_R16_UINT, TextureDecoder_R16_UINT::getInstance()); break;
			case F::R8_UNORM: set(VK_FORMAT_R8_UNORM, TextureDecoder_R8::getInstance()); break;
			case F::R8_SNORM: set(VK_FORMAT_R8_SNORM, TextureDecoder_R8::getInstance()); break;
			case F::R8_UINT: set(VK_FORMAT_R8_UINT, TextureDecoder_R8_UINT::getInstance()); break;
			// the packed 16-bit formats as their RGBA8 fallbacks (the same colours, fewer formats to query)
			case F::R5_G6_B5_UNORM: set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R5G6B5_UNORM_To_RGBA8::getInstance()); break;
			case F::R5_G5_B5_A1_UNORM: set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R5_G5_B5_A1_UNORM_swappedRB_To_RGBA8::getInstance()); break;
			case F::A1_B5_G5_R5_UNORM: set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_A1_B5_G5_R5_UNORM_vulkan_To_RGBA8::getInstance()); break;
			case F::R4_G4_B4_A4_UNORM: set(VK_FORMAT_R8G8B8A8_UNORM, TextureDecoder_R4G4B4A4_UNORM_To_RGBA8::getInstance()); break;
			case F::R11_G11_B10_FLOAT: set(VK_FORMAT_B10G11R11_UFLOAT_PACK32, TextureDecoder_R11_G11_B10_FLOAT::getInstance()); break;
			case F::R10_G10_B10_A2_UNORM: set(VK_FORMAT_A2B10G10R10_UNORM_PACK32, TextureDecoder_R10_G10_B10_A2_UNORM::getInstance()); break;
			case F::R10_G10_B10_A2_SNORM: set(VK_FORMAT_R16G16B16A16_SNORM, TextureDecoder_R10_G10_B10_A2_SNORM_To_RGBA16::getInstance()); break;
			case F::R10_G10_B10_A2_SRGB: set(VK_FORMAT_A2B10G10R10_UNORM_PACK32, TextureDecoder_R10_G10_B10_A2_UNORM::getInstance()); break;
			case F::BC1_SRGB: set(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, TextureDecoder_BC1::getInstance()); break;
			case F::BC1_UNORM: set(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, TextureDecoder_BC1::getInstance()); break;
			case F::BC2_UNORM: set(VK_FORMAT_BC2_UNORM_BLOCK, TextureDecoder_BC2::getInstance()); break;
			case F::BC2_SRGB: set(VK_FORMAT_BC2_SRGB_BLOCK, TextureDecoder_BC2::getInstance()); break;
			case F::BC3_UNORM: set(VK_FORMAT_BC3_UNORM_BLOCK, TextureDecoder_BC3::getInstance()); break;
			case F::BC3_SRGB: set(VK_FORMAT_BC3_SRGB_BLOCK, TextureDecoder_BC3::getInstance()); break;
			case F::BC4_UNORM: set(VK_FORMAT_BC4_UNORM_BLOCK, TextureDecoder_BC4::getInstance()); break;
			case F::BC4_SNORM: set(VK_FORMAT_BC4_SNORM_BLOCK, TextureDecoder_BC4::getInstance()); break;
			case F::BC5_UNORM: set(VK_FORMAT_BC5_UNORM_BLOCK, TextureDecoder_BC5::getInstance()); break;
			case F::BC5_SNORM: set(VK_FORMAT_BC5_SNORM_BLOCK, TextureDecoder_BC5::getInstance()); break;
			case F::R24_X8_UNORM: set(VK_FORMAT_R32_SFLOAT, TextureDecoder_R24_X8::getInstance()); break;
			case F::X24_G8_UINT: set(VK_FORMAT_R8G8B8A8_UINT, TextureDecoder_X24_G8_UINT::getInstance()); break;
			case F::R32_X8_FLOAT: set(VK_FORMAT_R32_SFLOAT, TextureDecoder_NullData64::getInstance()); break;
			default:
				LogOnce(fmt::format("tex{:x}", (uint32)format), [&] { return fmt::format("texture format {:#x} not mapped", (uint32)format); });
				break;
			}
			return f;
		}

		// LatteTextureVk_AdjustTextureCompSel
		uint32 AdjustCompSel(Latte::E_GX2SURFFMT format, uint32 sel)
		{
			using F = Latte::E_GX2SURFFMT;
			switch (format)
			{
			case F::R8_UNORM: case F::R8_SNORM: case F::BC4_UNORM: case F::BC4_SNORM:
				return sel >= 1 && sel <= 3 ? 0 : sel;
			case F::A1_B5_G5_R5_UNORM: case F::A2_B10_G10_R10_UNORM:
				return sel <= 3 ? 3 - sel : sel;
			case F::BC5_UNORM: case F::BC5_SNORM:
				return sel == 3 ? 1 : sel;
			case F::X24_G8_UINT:
				return sel <= 3 ? 3 : sel;
			case F::R4_G4_UNORM:
				return sel == 0 ? 1 : sel == 1 ? 0 : sel;
			default:
				return sel;
			}
		}

		VkComponentMapping Components(Latte::E_GX2SURFFMT format, uint32 word4)
		{
			static const VkComponentSwizzle t[8] = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
				VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO };
			return { t[AdjustCompSel(format, (word4 >> 16) & 7)], t[AdjustCompSel(format, (word4 >> 19) & 7)],
				t[AdjustCompSel(format, (word4 >> 22) & 7)], t[AdjustCompSel(format, (word4 >> 25) & 7)] };
		}

		VkImageViewType ViewType(Latte::E_DIM dim)
		{
			switch (dim)
			{
			case Latte::E_DIM::DIM_1D: return VK_IMAGE_VIEW_TYPE_1D;
			case Latte::E_DIM::DIM_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
			case Latte::E_DIM::DIM_2D_ARRAY: case Latte::E_DIM::DIM_2D_ARRAY_MSAA: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			case Latte::E_DIM::DIM_3D: return VK_IMAGE_VIEW_TYPE_3D;
			case Latte::E_DIM::DIM_CUBEMAP: return VK_IMAGE_VIEW_TYPE_CUBE;
			default: return VK_IMAGE_VIEW_TYPE_2D;
			}
		}

		// ---- placeholders: for units with no texture, or one this renderer can't provide ----------------
		struct Placeholder { Image img; VkImageView view[8]{}; };
		Placeholder s_nullColor, s_nullDepth;

		void CreatePlaceholder(Placeholder& p, VkFormat format, VkImageAspectFlags aspect)
		{
			// one 1x1 image with 6 layers serves 2D, 2D array and cube views (a 3D view would need a 3D image;
			// it falls back to 2D, which doesn't match the shader, so a 3D texture unit must never get here)
			VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
			ci.imageType = VK_IMAGE_TYPE_2D;
			ci.format = format;
			ci.extent = { 1, 1, 1 };
			ci.mipLevels = 1;
			ci.arrayLayers = 6;
			ci.samples = VK_SAMPLE_COUNT_1_BIT;
			ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			Check(vkCreateImage(s.device, &ci, nullptr, &p.img.image), "vkCreateImage");
			VkMemoryRequirements req;
			vkGetImageMemoryRequirements(s.device, p.img.image, &req);
			VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			ai.allocationSize = req.size;
			ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			Check(vkAllocateMemory(s.device, &ai, nullptr, &p.img.memory), "vkAllocateMemory");
			vkBindImageMemory(s.device, p.img.image, p.img.memory, 0);
			p.img.aspect = aspect;
			p.img.format = format;
			VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = p.img.image;
			b.subresourceRange = { aspect, 0, 1, 0, 6 };
			vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
			{
				VkClearDepthStencilValue v{};
				vkCmdClearDepthStencilImage(s.cmd, p.img.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &b.subresourceRange);
			}
			else
			{
				VkClearColorValue v{};
				vkCmdClearColorImage(s.cmd, p.img.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &b.subresourceRange);
			}
			p.img.layout = VK_IMAGE_LAYOUT_GENERAL;
			for (uint32 d = 0; d < 8; d++)
			{
				VkImageViewType type = ViewType((Latte::E_DIM)d);
				if (type == VK_IMAGE_VIEW_TYPE_3D)
					continue;
				VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				vi.image = p.img.image;
				vi.viewType = type;
				vi.format = format;
				vi.subresourceRange = { aspect, 0, 1, 0, type == VK_IMAGE_VIEW_TYPE_CUBE ? 6u : 1u };
				Check(vkCreateImageView(s.device, &vi, nullptr, &p.view[d]), "vkCreateImageView");
			}
		}

		// ---- samplers (draw_getOrCreateDescriptorSet) -------------------------------------------------
		std::unordered_map<uint64, VkSampler> s_samplers;
		bool s_customBorder = false, s_anisotropy = false;

		VkSampler Sampler(const LatteDecompilerShader* dec, bool vertex, uint32 unit, bool compare)
		{
			uint32 stageIndex = dec->textureUnitSamplerAssignment[unit];
			VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
			VkSamplerCustomBorderColorCreateInfoEXT border{ VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT };
			si.magFilter = si.minFilter = VK_FILTER_LINEAR;
			si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			si.maxLod = 0.25f;
			if (stageIndex != LATTE_DECOMPILER_SAMPLER_NONE)
			{
				uint32 index = stageIndex + (vertex ? Latte::SAMPLER_BASE_INDEX_VERTEX : Latte::SAMPLER_BASE_INDEX_PIXEL);
				const _LatteRegisterSetSampler& w = LatteGPUState.contextNew.SQ_TEX_SAMPLER[index];
				using W0 = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0;
				uint32 minLod = w.WORD1.get_MIN_LOD(), maxLod = w.WORD1.get_MAX_LOD();
				auto mip = w.WORD0.get_MIP_FILTER();
				si.mipmapMode = mip == W0::E_Z_FILTER::LINEAR || (mip != W0::E_Z_FILTER::NONE && mip != W0::E_Z_FILTER::POINT)
					? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
				si.minLod = mip == W0::E_Z_FILTER::NONE ? 0.0f : (float)minLod / 64.0f;
				si.maxLod = mip == W0::E_Z_FILTER::NONE ? 0.25f : (float)maxLod / 64.0f;
				auto fmin = w.WORD0.get_XY_MIN_FILTER(), fmag = w.WORD0.get_XY_MAG_FILTER();
				bool pointMin = fmin == W0::E_XY_FILTER::POINT || fmin == W0::E_XY_FILTER::ANISO_POINT;
				si.minFilter = pointMin ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
				si.magFilter = (fmag == W0::E_XY_FILTER::POINT || fmin == W0::E_XY_FILTER::ANISO_POINT) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
				static const VkSamplerAddressMode clamp[8] = { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
					VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
					VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER };
				si.addressModeU = clamp[(uint32)w.WORD0.get_CLAMP_X() & 7];
				si.addressModeV = clamp[(uint32)w.WORD0.get_CLAMP_Y() & 7];
				si.addressModeW = clamp[(uint32)w.WORD0.get_CLAMP_Z() & 7];
				uint32 aniso = (uint32)w.WORD0.get_MAX_ANISO_RATIO();
				if (aniso > 0 && s_anisotropy)
				{
					si.anisotropyEnable = VK_TRUE;
					si.maxAnisotropy = std::min((float)(1 << aniso), s.props.limits.maxSamplerAnisotropy);
				}
				si.mipLodBias = (float)w.WORD1.get_LOD_BIAS() / 64.0f;
				if (compare)
				{
					static const VkCompareOp ops[8] = { VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
						VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS };
					si.compareEnable = VK_TRUE;
					si.compareOp = ops[(uint32)w.WORD0.get_DEPTH_COMPARE_FUNCTION() & 7];
				}
				auto bt = w.WORD0.get_BORDER_COLOR_TYPE();
				if (bt == W0::E_BORDER_COLOR_TYPE::TRANSPARENT_BLACK)
					si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
				else if (bt == W0::E_BORDER_COLOR_TYPE::OPAQUE_BLACK)
					si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
				else if (bt == W0::E_BORDER_COLOR_TYPE::OPAQUE_WHITE)
					si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
				else if (s_customBorder)
				{
					const _LatteRegisterSetSamplerBorderColor& c = (vertex ? LatteGPUState.contextNew.TD_VS_SAMPLER_BORDER_COLOR
						: LatteGPUState.contextNew.TD_PS_SAMPLER_BORDER_COLOR)[stageIndex];
					border.format = VK_FORMAT_UNDEFINED;
					border.customBorderColor.float32[0] = c.red.get_channelValue();
					border.customBorderColor.float32[1] = c.green.get_channelValue();
					border.customBorderColor.float32[2] = c.blue.get_channelValue();
					border.customBorderColor.float32[3] = c.alpha.get_channelValue();
					si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
					si.pNext = &border;
				}
				else
					si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			}
			else if (compare)
			{
				si.compareEnable = VK_TRUE;
				si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
			}
			uint64 key = 0xCBF29CE484222325ull;
			auto mix = [&](const void* p, size_t n) { for (size_t i = 0; i < n; i++) key = (key ^ ((const uint8*)p)[i]) * 0x100000001B3ull; };
			mix(&si.magFilter, offsetof(VkSamplerCreateInfo, unnormalizedCoordinates) - offsetof(VkSamplerCreateInfo, magFilter));
			if (si.pNext)
				mix(&border.customBorderColor, sizeof(border.customBorderColor));
			VkSampler& sampler = s_samplers[key];
			if (!sampler)
				Check(vkCreateSampler(s.device, &si, nullptr, &sampler), "vkCreateSampler");
			return sampler;
		}

		// ---- textures from guest memory -------------------------------------------------------------------
		struct Texture
		{
			Image img;                                                 // img.layout is that of every subresource
			VkImageType type = VK_IMAGE_TYPE_2D;
			uint32 depth = 1, layers = 1, mips = 1;
			uint64 hash = 0;
			uint32 checkedFrame = UINT32_MAX;
			std::unordered_map<uint64, VkImageView> views;
		};
		std::unordered_map<uint64, Texture> s_textures;

		uint64 HashMemory(const uint8* p, size_t n)
		{
			uint64 h[4] = { 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
			size_t words = n / 8;
			const uint64* w = (const uint64*)p;
			size_t i = 0;
			for (; i + 4 <= words; i += 4)
				for (int k = 0; k < 4; k++)
					h[k] = (std::rotl(h[k] ^ w[i + k], 29) + w[i + k]) * 0x9FB21C651E98DF25ull;
			uint64 r = h[0] ^ std::rotl(h[1], 17) ^ std::rotl(h[2], 31) ^ std::rotl(h[3], 47);
			for (size_t j = i * 8; j < n; j++)
				r = (r ^ p[j]) * 0x100000001B3ull;
			return r ^ n;
		}

		struct TexDesc
		{
			MPTR phys, physMip;
			Latte::E_GX2SURFFMT format;
			Latte::E_DIM dim;
			Latte::E_HWTILEMODE tileMode;
			uint32 width, height, depth, pitch, swizzle, mips;
			bool isDepth;
		};

		void Upload(Texture& t, const TexDesc& d, const TexFormat& f)
		{
			EndRendering();
			VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;                   // everything is rewritten
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = t.img.image;
			b.subresourceRange = { f.aspect, 0, t.mips, 0, t.layers };
			vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			if (!f.decoder || (f.aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
			{
				// no decoder (or depth-stencil data, which Cemu doesn't upload either): zero
				if (f.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
				{
					VkClearDepthStencilValue v{};
					vkCmdClearDepthStencilImage(s.cmd, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &b.subresourceRange);
				}
				else
				{
					VkClearColorValue v{};
					vkCmdClearColorImage(s.cmd, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &b.subresourceRange);
				}
			}
			else
			{
				for (uint32 mip = 0; mip < t.mips; mip++)
				{
					// LatteTexture_ReloadData: slices per mip (3D textures shrink in depth)
					uint32 slices = d.dim == Latte::E_DIM::DIM_3D ? std::max(d.depth >> mip, 1u) : t.layers;
					for (uint32 slice = 0; slice < slices; slice++)
					{
						LatteTextureLoaderCtx ctx{};
						LatteTextureLoader_begin(&ctx, slice, mip, d.phys, d.physMip, d.format, d.dim, d.width, d.height, d.depth, d.mips,
							d.pitch, d.tileMode, d.swizzle);
						ctx.decodedTexelCountX = f.decoder->getTexelCountX(&ctx);
						ctx.decodedTexelCountY = f.decoder->getTexelCountY(&ctx);
						uint32 size = f.decoder->calculateImageSize(&ctx);
						VkDeviceSize off = RingAlloc(size, 16);
						f.decoder->decode(&ctx, s.ring.data + off);
						VkBufferImageCopy r{};
						r.bufferOffset = off;
						r.imageSubresource = { f.aspect, mip, t.type == VK_IMAGE_TYPE_3D ? 0 : slice, 1 };
						r.imageOffset = { 0, 0, t.type == VK_IMAGE_TYPE_3D ? (sint32)slice : 0 };
						r.imageExtent = { (uint32)ctx.width, (uint32)ctx.height, 1 };
						vkCmdCopyBufferToImage(s.cmd, s.ring.buffer, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
					}
				}
			}
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			t.img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}

		Texture* Load(const TexDesc& d)
		{
			uint64 key = 0xCBF29CE484222325ull;
			for (uint64 v : { (uint64)d.phys, (uint64)d.physMip, (uint64)d.format, (uint64)d.dim, (uint64)d.tileMode, (uint64)d.width,
				(uint64)d.height, (uint64)d.depth, (uint64)d.pitch, (uint64)d.swizzle, (uint64)d.mips, (uint64)d.isDepth })
				key = (key ^ v) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull;
			TexFormat f = TextureFormat(d.format, d.isDepth);
			if (f.vk == VK_FORMAT_UNDEFINED)
				return nullptr;
			Texture& t = s_textures[key];
			// the data's range: level 0 (mip chains change with it)
			LatteAddrLib::AddrSurfaceInfo_OUT info{};
			LatteAddrLib::GX2CalculateSurfaceInfo(d.format, d.width, d.height, d.depth, d.dim, Latte::MakeGX2TileMode(d.tileMode), 0, 0, &info);
			if (!t.img.image)
			{
				t.type = d.dim == Latte::E_DIM::DIM_3D ? VK_IMAGE_TYPE_3D
					: (d.dim == Latte::E_DIM::DIM_1D || d.dim == Latte::E_DIM::DIM_1D_ARRAY) ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
				t.depth = t.type == VK_IMAGE_TYPE_3D ? d.depth : 1;
				t.layers = t.type == VK_IMAGE_TYPE_3D ? 1 : d.depth;
				uint32 maxMips = 1;
				for (uint32 m = std::max({ d.width, d.height, t.depth }); m > 1; m >>= 1)
					maxMips++;
				t.mips = std::clamp(d.mips, 1u, maxMips);
				VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
				ci.flags = d.dim == Latte::E_DIM::DIM_CUBEMAP && t.layers % 6 == 0 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
				ci.imageType = t.type;
				ci.format = f.vk;
				ci.extent = { d.width, t.type == VK_IMAGE_TYPE_1D ? 1 : d.height, t.depth };
				ci.mipLevels = t.mips;
				ci.arrayLayers = t.layers;
				ci.samples = VK_SAMPLE_COUNT_1_BIT;
				ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				Check(vkCreateImage(s.device, &ci, nullptr, &t.img.image), "vkCreateImage");
				VkMemoryRequirements req;
				vkGetImageMemoryRequirements(s.device, t.img.image, &req);
				VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
				ai.allocationSize = req.size;
				ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
				Check(vkAllocateMemory(s.device, &ai, nullptr, &t.img.memory), "vkAllocateMemory");
				Check(vkBindImageMemory(s.device, t.img.image, t.img.memory, 0), "vkBindImageMemory");
				t.img.format = f.vk;
				t.img.aspect = f.aspect;
				t.img.width = d.width;
				t.img.height = d.height;
			}
			if (t.checkedFrame != s.frame)
			{
				t.checkedFrame = s.frame;
				uint64 h = HashMemory(memory_getPointerFromPhysicalOffset(d.phys), (size_t)info.surfSize);
				if (h != t.hash || t.img.layout == VK_IMAGE_LAYOUT_UNDEFINED)
				{
					t.hash = h;
					Upload(t, d, f);
				}
			}
			return &t;
		}

		VkImageView View(VkImage image, VkFormat format, VkImageAspectFlags aspect, VkImageViewType type, uint32 baseMip, uint32 mips,
			uint32 baseLayer, uint32 layers, VkComponentMapping comp, std::unordered_map<uint64, VkImageView>& cache)
		{
			if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
				aspect = VK_IMAGE_ASPECT_DEPTH_BIT;                   // sampling reads depth only
			uint64 key = ((uint64)type << 60) ^ ((uint64)baseMip << 52) ^ ((uint64)mips << 44) ^ ((uint64)baseLayer << 30) ^ ((uint64)layers << 16)
				^ ((uint64)comp.r) ^ ((uint64)comp.g << 3) ^ ((uint64)comp.b << 6) ^ ((uint64)comp.a << 9);
			VkImageView& v = cache[key];
			if (!v)
			{
				VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				vi.image = image;
				vi.viewType = type;
				vi.format = format;
				vi.components = comp;
				vi.subresourceRange = { aspect, baseMip, mips, baseLayer, layers };
				Check(vkCreateImageView(s.device, &vi, nullptr, &v), "vkCreateImageView");
			}
			return v;
		}

		std::unordered_map<VkImage, std::unordered_map<uint64, VkImageView>> s_surfaceViewCache;

		// A copy of a surface's top-left corner, for a texture that samples it at another size (render
		// targets are allocated padded, e.g. 1920x1088 for a 1920x1080 texture: normalized coordinates
		// need the texture's own size), in another format of the same texel size, or by a draw that
		// also renders into it. Cemu keeps size-exact textures and copies between them likewise. Taken
		// again only when the surface was written since.
		struct Copy { Image img; uint64 written = UINT64_MAX; };
		std::map<std::tuple<VkImage, uint32, uint32, VkFormat>, Copy> s_copies;

		uint32 TexelSize(VkFormat f)
		{
			switch (f)
			{
			case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: return 1;
			case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM:
			case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: return 2;
			case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
			case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return 8;
			case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
			default: return 4;
			}
		}

		Image& CopyOf(Image& surface, uint32 w, uint32 h, VkFormat format)
		{
			Copy& c = s_copies[{ surface.image, w, h, format }];
			if (!c.img.image)
			{
				c.img = CreateImage(format, surface.aspect, w, h, VK_IMAGE_USAGE_SAMPLED_BIT);
				ForgetImage(c.img.image);                              // a reused handle's stale views
			}
			if (c.written != surface.written)
			{
				EndRendering();
				Transition(c.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				if (w > surface.width || h > surface.height)           // the part the surface doesn't cover
				{
					VkImageSubresourceRange all{ c.img.aspect, 0, 1, 0, 1 };
					if (c.img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
					{
						VkClearDepthStencilValue v{};
						vkCmdClearDepthStencilImage(s.cmd, c.img.image, c.img.layout, &v, 1, &all);
					}
					else
					{
						VkClearColorValue v{};
						vkCmdClearColorImage(s.cmd, c.img.image, c.img.layout, &v, 1, &all);
					}
				}
				Transition(surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				VkImageCopy r{};
				r.srcSubresource = r.dstSubresource = { surface.aspect, 0, 0, 1 };
				r.extent = { std::min(w, surface.width), std::min(h, surface.height), 1 };
				vkCmdCopyImage(s.cmd, surface.image, surface.layout, c.img.image, c.img.layout, 1, &r);
				Transition(c.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				c.written = surface.written;
			}
			return c.img;
		}
	}

	namespace
	{
		// The surface last written at an address (the game reuses memory for transient targets of
		// other formats within a frame), one in `format` on a tie.
		Image* SurfaceAt(uint32 addr, uint32 format)
		{
			Image* surface = nullptr;
			for (auto it = s.surfaces.lower_bound({ addr, 0 }); it != s.surfaces.end() && it->first.first == addr; ++it)
			{
				Image& c = it->second;
				if (!c.image)
					continue;
				if (!surface || c.written > surface->written || (c.written == surface->written && (it->first.second & 0xFFFF) == format))
					surface = &c;
			}
			return surface;
		}

		// A texture with mips whose levels were rendered as separate targets (the bloom chain: each
		// level is drawn at its own address): one image, level L copied from the surface at level
		// L's address, each level again when its surface was written since.
		struct Chain { Image img; uint32 mips = 0; std::vector<uint64> written; std::unordered_map<uint64, VkImageView> views; };
		std::unordered_map<uint64, Chain> s_chains;

		Chain* ChainOf(const TexDesc& d, Image& base, VkFormat format)
		{
			uint32 maxMips = 1;
			for (uint32 m = std::max(d.width, d.height); m > 1; m >>= 1)
				maxMips++;
			uint32 mips = std::clamp(d.mips, 1u, maxMips);
			uint64 key = 0xCBF29CE484222325ull;
			for (uint64 v : { (uint64)d.phys, (uint64)d.physMip, (uint64)d.format, (uint64)d.width, (uint64)d.height, (uint64)mips, (uint64)format })
				key = (key ^ v) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull;
			Chain& c = s_chains[key];
			VkImageSubresourceRange all{ base.aspect, 0, mips, 0, 1 };
			auto barrier = [&](VkImageLayout from, VkImageLayout to) {
				VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
				b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
				b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				b.oldLayout = from;
				b.newLayout = to;
				b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = c.img.image;
				b.subresourceRange = all;
				vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			};
			if (!c.img.image)
			{
				VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
				ci.imageType = VK_IMAGE_TYPE_2D;
				ci.format = format;
				ci.extent = { d.width, d.height, 1 };
				ci.mipLevels = mips;
				ci.arrayLayers = 1;
				ci.samples = VK_SAMPLE_COUNT_1_BIT;
				ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				Check(vkCreateImage(s.device, &ci, nullptr, &c.img.image), "vkCreateImage");
				VkMemoryRequirements req;
				vkGetImageMemoryRequirements(s.device, c.img.image, &req);
				VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
				ai.allocationSize = req.size;
				ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
				Check(vkAllocateMemory(s.device, &ai, nullptr, &c.img.memory), "vkAllocateMemory");
				Check(vkBindImageMemory(s.device, c.img.image, c.img.memory, 0), "vkBindImageMemory");
				c.img.format = format;
				c.img.aspect = base.aspect;
				c.img.width = d.width;
				c.img.height = d.height;
				c.mips = mips;
				c.written.assign(mips, UINT64_MAX);
				EndRendering();
				barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);   // levels no surface covers read zero
				VkClearColorValue zero{};
				if (!(base.aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
					vkCmdClearColorImage(s.cmd, c.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
				barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			}
			bool copying = false;
			for (uint32 level = 0; level < c.mips; level++)
			{
				uint32 addr, size;
				sint32 sub;
				LatteAddrLib::CalculateMipAndSliceAddr(d.phys, d.physMip, d.format, d.width, d.height, d.depth, d.dim, d.tileMode, d.swizzle, 0,
					level, 0, &addr, &size, &sub);
				Image* src = level == 0 ? &base : SurfaceAt(addr & ~0x700u, (uint32)d.format);
				if (!src || src->written == c.written[level] || TexelSize(src->format) != TexelSize(format) ||
					(src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != (base.aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
					continue;
				if (!copying)
				{
					EndRendering();
					barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
					copying = true;
				}
				Transition(*src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				VkImageCopy r{};
				r.srcSubresource = { src->aspect, 0, 0, 1 };
				r.dstSubresource = { base.aspect, level, 0, 1 };
				r.extent = { std::min(std::max(d.width >> level, 1u), src->width), std::min(std::max(d.height >> level, 1u), src->height), 1 };
				vkCmdCopyImage(s.cmd, src->image, src->layout, c.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
				c.written[level] = src->written;
			}
			if (copying)
				barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			return &c;
		}
	}

	void ForgetImage(VkImage image)
	{
		for (auto it = s_copies.lower_bound({ image, 0, 0, VK_FORMAT_UNDEFINED }); it != s_copies.end() && std::get<0>(it->first) == image;)
		{
			Image& img = it->second.img;
			ForgetImage(img.image);
			if (img.view)
				vkDestroyImageView(s.device, img.view, nullptr);
			vkDestroyImage(s.device, img.image, nullptr);
			vkFreeMemory(s.device, img.memory, nullptr);
			it = s_copies.erase(it);
		}
		auto it = s_surfaceViewCache.find(image);
		if (it == s_surfaceViewCache.end())
			return;
		for (auto& [key, view] : it->second)
			vkDestroyImageView(s.device, view, nullptr);
		s_surfaceViewCache.erase(it);
	}

	void TextureInit()
	{
		CreatePlaceholder(s_nullColor, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
		CreatePlaceholder(s_nullDepth, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(s.physical, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
		s_d24s8 = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
		vkGetPhysicalDeviceFormatProperties(s.physical, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &fp);
		if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
			Log("the device can't sample BC formats; compressed textures will fail");
		s_customBorder = s.customBorder;
		s_anisotropy = s.anisotropy;
	}

	Sampled SampleTexture(const LatteDecompilerShader* dec, bool vertex, uint32 unit, std::span<Image* const> attachments)
	{
		const bool compare = dec->textureUsesDepthCompare[unit];
		const Latte::E_DIM shaderDim = dec->textureUnitDim[unit];
		VkSampler sampler = Sampler(dec, vertex, unit, compare);
		auto placeholder = [&]() -> Sampled {
			const Placeholder& p = compare ? s_nullDepth : s_nullColor;
			VkImageView v = p.view[(uint32)shaderDim & 7] ? p.view[(uint32)shaderDim & 7] : p.view[(uint32)Latte::E_DIM::DIM_2D];
			return { v, sampler, VK_IMAGE_LAYOUT_GENERAL };
		};
		uint32 base = vertex ? Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
		const auto& tex = *(const _LatteRegisterSetTextureUnit*)(LatteGPUState.contextRegister + base + unit * 7);
		const uint32 word4 = LatteGPUState.contextRegister[base + unit * 7 + 4];

		// LatteTexture_updateTexturesForStage
		TexDesc d{};
		d.phys = tex.word2.get_BASE_ADDRESS() << 8;
		if (!d.phys)
			return placeholder();
		d.physMip = tex.word3.get_MIP_ADDRESS() << 8;
		d.dim = tex.word0.get_DIM();
		d.pitch = (tex.word0.get_PITCH() + 1) << 3;
		d.width = tex.word0.get_WIDTH() + 1;
		d.tileMode = tex.word0.get_TILE_MODE();
		d.depth = tex.word1.get_DEPTH();
		if (d.dim == Latte::E_DIM::DIM_2D_ARRAY || d.dim == Latte::E_DIM::DIM_3D || d.dim == Latte::E_DIM::DIM_2D_ARRAY_MSAA ||
			d.dim == Latte::E_DIM::DIM_1D_ARRAY)
			d.depth++;
		else
		{
			if (d.dim == Latte::E_DIM::DIM_CUBEMAP)
				d.depth = 6 * (d.depth + 1);
			if (d.depth == 0)
				d.depth = 1;
		}
		d.height = tex.word1.get_HEIGHT() + 1;
		if (d.dim == Latte::E_DIM::DIM_1D || d.dim == Latte::E_DIM::DIM_1D_ARRAY)
			d.height = 1;
		if (Latte::IsCompressedFormat(tex.word1.get_DATA_FORMAT()))
			d.pitch /= 4;
		uint32 firstSlice = tex.word5.get_BASE_ARRAY();
		uint32 numSlices = tex.word5.get_LAST_ARRAY() + 1 - firstSlice;
		uint32 firstMip = tex.word4.get_BASE_LEVEL();
		uint32 lastMip = tex.word5.get_LAST_LEVEL();
		d.format = LatteTexture_ReconstructGX2Format(tex.word1, tex.word4);
		if (d.dim == Latte::E_DIM::DIM_2D_MSAA)
			firstMip = lastMip = 0;
		d.mips = lastMip + 1;
		if (Latte::TM_IsMacroTiled(d.tileMode))
		{
			d.swizzle = d.phys & 0x700;
			d.phys &= ~0x700u;
		}
		d.isDepth = compare;
		const VkImageViewType viewType = ViewType(shaderDim);
		const VkComponentMapping comp = Components(d.format, word4);

		// render to texture
		if (Image* surface = SurfaceAt(d.phys, (uint32)d.format))
		{
			if (viewType != VK_IMAGE_VIEW_TYPE_2D && viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY)
			{
				LogOnce(fmt::format("rtdim{:x}", d.phys), [&] { return fmt::format("render target {:#x} sampled as dim {}; placeholder", d.phys, (uint32)shaderDim); });
				return placeholder();
			}
			Image* img = surface;
			VkFormat format = surface->format;
			if (!(surface->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
			{
				TexFormat tf = TextureFormat(d.format, false);
				if (tf.vk != VK_FORMAT_UNDEFINED && tf.vk != format && TexelSize(tf.vk) == TexelSize(format) &&
					tf.decoder && !Latte::IsCompressedFormat(d.format))
					format = tf.vk;
			}
			bool feedback = std::find(attachments.begin(), attachments.end(), surface) != attachments.end();
			if (d.mips > 1 && !feedback)
			{
				Chain* c = ChainOf(d, *surface, format);
				firstMip = std::min(firstMip, c->mips - 1);
				uint32 mips = std::min(lastMip, c->mips - 1) + 1 - firstMip;
				VkImageView v = View(c->img.image, c->img.format, c->img.aspect, viewType, firstMip, mips, 0, 1, comp, c->views);
				return { v, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
			}
			if (feedback || d.width != surface->width || d.height != surface->height || format != surface->format)
				img = &CopyOf(*surface, d.width, d.height, format);
			else if (surface->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			{
				EndRendering();
				Transition(*surface, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			}
			VkImageView v = View(img->image, img->format, img->aspect, viewType, 0, 1, 0, 1, comp, s_surfaceViewCache[img->image]);
			return { v, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		}

		// from guest memory
		Texture* t = Load(d);
		if (!t)
			return placeholder();
		if ((viewType == VK_IMAGE_VIEW_TYPE_3D) != (t->type == VK_IMAGE_TYPE_3D) ||
			((viewType == VK_IMAGE_VIEW_TYPE_1D || viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY) != (t->type == VK_IMAGE_TYPE_1D)))
		{
			LogOnce(fmt::format("dim{}-{}", (uint32)shaderDim, (uint32)d.dim), [&] {
				return fmt::format("a shader samples dim {} from a dim {} texture; placeholder", (uint32)shaderDim, (uint32)d.dim); });
			return placeholder();
		}
		firstMip = std::min(firstMip, t->mips - 1);
		uint32 mips = std::min(lastMip, t->mips - 1) + 1 - firstMip;
		if (t->type == VK_IMAGE_TYPE_3D)
			firstSlice = 0, numSlices = 1;
		else
		{
			firstSlice = std::min(firstSlice, t->layers - 1);
			numSlices = std::clamp(numSlices, 1u, t->layers - firstSlice);
		}
		if (viewType == VK_IMAGE_VIEW_TYPE_CUBE)
		{
			if (numSlices < 6 || firstSlice + 6 > t->layers)
				return placeholder();
			numSlices = 6;
		}
		else if (viewType == VK_IMAGE_VIEW_TYPE_2D || viewType == VK_IMAGE_VIEW_TYPE_1D)
			numSlices = 1;
		VkImageView v = View(t->img.image, t->img.format, t->img.aspect, viewType, firstMip, mips, firstSlice, numSlices, comp, t->views);
		return { v, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	}

	// IT_HLE_COPY_SURFACE_NEW (GX2CopySurface; LatteCP_itHLECopySurfaceNew): rect x, y, w, h, then
	// source and destination as address, swizzle, format, pitch, height, slice, dim, tile mode, AA.
	// For now only reported (WWHD_RENDER_COPIES=1): G0 saw 50, all at boot.
	void RendererCopySurface(const uint32be* p, uint32 nWords)
	{
		static const bool report = getenv("WWHD_RENDER_COPIES") != nullptr;
		if (!report || nWords < 22)
			return;
		auto surfaceAt = [](uint32 addr) {
			const Image* found = nullptr;
			uint32 fmt = 0;
			for (auto it = s.surfaces.lower_bound({ addr, 0 }); it != s.surfaces.end() && it->first.first == addr; ++it)
				if (!found || it->second.written > found->written)
					found = &it->second, fmt = it->first.second;
			return found ? fmt::format("surface {:x} {}x{}", fmt, found->width, found->height) : std::string("memory");
		};
		Log(fmt::format("copy frame {} rect {},{} {}x{}: src {:08x} fmt {:x} pitch {} h {} slice {} dim {} tm {} ({}) -> dst {:08x} fmt {:x} pitch {} h {} slice {} dim {} tm {} ({})",
			s.frame, (uint32)p[0], (uint32)p[1], (uint32)p[2], (uint32)p[3],
			(uint32)p[4], (uint32)p[6], (uint32)p[7], (uint32)p[8], (uint32)p[9], (uint32)p[10], (uint32)p[11], surfaceAt(p[4]),
			(uint32)p[13], (uint32)p[15], (uint32)p[16], (uint32)p[17], (uint32)p[18], (uint32)p[19], (uint32)p[20], surfaceAt(p[13])));
	}
}
