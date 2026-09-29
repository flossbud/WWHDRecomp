// The Vulkan renderer: device, surfaces, clears, scan-out and frame capture (see renderer.h).
//
// Surfaces. The game's colour and depth buffers live in guest memory; here each one is a VkImage
// named by its guest address and GX2 format. Rendering happens only in those images: guest memory
// never sees it (nothing on the route reads rendered pixels back with the CPU, G0). A buffer
// described larger later (the registers give padded sizes, 1920x1088 for a 1920x1080 target) gets
// a larger image, with the old contents copied over.
//
// Recording. Everything goes into one command buffer that is submitted, and waited for, at each
// swap: simple and slow, which is fine for lavapipe and for correctness first.
#include "renderer.h"
#include "vk.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include <map>
#include <set>

namespace wwhd::gpu
{
	using namespace wwhd::vk;

	namespace
	{
		struct Image
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkImageAspectFlags aspect = 0;
			uint32 width = 0, height = 0;
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		};

		struct State
		{
			bool on = false;
			VkInstance instance = VK_NULL_HANDLE;
			VkPhysicalDevice physical = VK_NULL_HANDLE;
			VkDevice device = VK_NULL_HANDLE;
			VkQueue queue = VK_NULL_HANDLE;
			uint32 queueFamily = 0;
			VkPhysicalDeviceMemoryProperties memory{};
			VkCommandPool pool = VK_NULL_HANDLE;
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			VkFence fence = VK_NULL_HANDLE;
			std::map<std::pair<uint32, uint32>, Image> surfaces;       // (physical address, GX2 format)
			Image scan[2];                                              // TV, DRC
			uint32 frame = 0;
			std::set<uint32> shotFrames;
			std::string shotDir = ".";
		};
		State s;

		void Log(const std::string& msg)
		{
			cemuLog_log(LogType::Force, "vk renderer: {}", msg);
		}

		[[noreturn]] void Fail(const std::string& msg)
		{
			Log("FATAL: " + msg);
			fprintf(stderr, "vk renderer: FATAL: %s\n", msg.c_str());
			std::abort();
		}

		void Check(VkResult r, const char* what)
		{
			if (r != VK_SUCCESS)
				Fail(fmt::format("{} failed ({})", what, (int)r));
		}

		uint32 MemoryType(uint32 bits, VkMemoryPropertyFlags props)
		{
			for (uint32 i = 0; i < s.memory.memoryTypeCount; i++)
				if ((bits & (1u << i)) && (s.memory.memoryTypes[i].propertyFlags & props) == props)
					return i;
			Fail("no suitable memory type");
		}

		// ---- device ------------------------------------------------------------------------------
		bool Init()
		{
			if (!Load())
			{
				Log("libvulkan not found");
				return false;
			}
			VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
			app.pApplicationName = "wwhd";
			app.apiVersion = VK_API_VERSION_1_3;
			VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
			ici.pApplicationInfo = &app;
			Check(vkCreateInstance(&ici, nullptr, &s.instance), "vkCreateInstance");
			LoadInstance(s.instance);
			uint32 n = 0;
			vkEnumeratePhysicalDevices(s.instance, &n, nullptr);
			std::vector<VkPhysicalDevice> devices(n);
			vkEnumeratePhysicalDevices(s.instance, &n, devices.data());
			if (devices.empty())
				Fail("no Vulkan device");
			s.physical = devices[0];                                    // VK_ICD_FILENAMES picks lavapipe (run.sh REF_GPU=llvmpipe)
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(s.physical, &props);
			vkGetPhysicalDeviceMemoryProperties(s.physical, &s.memory);
			vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &n, nullptr);
			std::vector<VkQueueFamilyProperties> families(n);
			vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &n, families.data());
			s.queueFamily = UINT32_MAX;
			for (uint32 i = 0; i < n; i++)
				if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
				{
					s.queueFamily = i;
					break;
				}
			if (s.queueFamily == UINT32_MAX)
				Fail("no graphics queue");
			float priority = 1.0f;
			VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
			qci.queueFamilyIndex = s.queueFamily;
			qci.queueCount = 1;
			qci.pQueuePriorities = &priority;
			VkPhysicalDeviceVulkan13Features v13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
			v13.dynamicRendering = VK_TRUE;
			VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
			dci.pNext = &v13;
			dci.queueCreateInfoCount = 1;
			dci.pQueueCreateInfos = &qci;
			Check(vkCreateDevice(s.physical, &dci, nullptr, &s.device), "vkCreateDevice");
			LoadDevice(s.device);
			vkGetDeviceQueue(s.device, s.queueFamily, 0, &s.queue);
			VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			pci.queueFamilyIndex = s.queueFamily;
			Check(vkCreateCommandPool(s.device, &pci, nullptr, &s.pool), "vkCreateCommandPool");
			VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			cai.commandPool = s.pool;
			cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cai.commandBufferCount = 1;
			Check(vkAllocateCommandBuffers(s.device, &cai, &s.cmd), "vkAllocateCommandBuffers");
			VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			Check(vkCreateFence(s.device, &fci, nullptr, &s.fence), "vkCreateFence");
			VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			Check(vkBeginCommandBuffer(s.cmd, &bi), "vkBeginCommandBuffer");

			// the reference's screenshot settings (cemu-patches/0007)
			if (const char* spec = getenv("CEMU_SHOT_FRAMES"))
			{
				std::stringstream ss(spec);
				for (std::string item; std::getline(ss, item, ',');)
				{
					uint32 a = 0, b = 0, step = 1;
					if (sscanf(item.c_str(), "%u-%u/%u", &a, &b, &step) >= 2)
						for (uint32 f = a; f <= b; f += std::max(step, 1u))
							s.shotFrames.insert(f);
					else if (sscanf(item.c_str(), "%u", &a) == 1)
						s.shotFrames.insert(a);
				}
			}
			if (const char* dir = getenv("CEMU_SHOT_DIR"); dir && *dir)
				s.shotDir = dir;
			Log(fmt::format("on {} (Vulkan {}.{})", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
				VK_API_VERSION_MINOR(props.apiVersion)));
			return true;
		}

		void SubmitAndWait()
		{
			Check(vkEndCommandBuffer(s.cmd), "vkEndCommandBuffer");
			VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
			si.commandBufferCount = 1;
			si.pCommandBuffers = &s.cmd;
			Check(vkQueueSubmit(s.queue, 1, &si, s.fence), "vkQueueSubmit");
			Check(vkWaitForFences(s.device, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
			vkResetFences(s.device, 1, &s.fence);
			vkResetCommandBuffer(s.cmd, 0);
			VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			Check(vkBeginCommandBuffer(s.cmd, &bi), "vkBeginCommandBuffer");
		}

		// ---- images ------------------------------------------------------------------------------
		// every transition waits for everything before it: correctness first
		void Transition(Image& img, VkImageLayout layout)
		{
			if (img.layout == layout)
				return;
			VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			b.oldLayout = img.layout;
			b.newLayout = layout;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = img.image;
			b.subresourceRange = { img.aspect, 0, 1, 0, 1 };
			vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
				0, nullptr, 1, &b);
			img.layout = layout;
		}

		Image CreateImage(VkFormat format, VkImageAspectFlags aspect, uint32 w, uint32 h, VkImageUsageFlags usage)
		{
			Image img;
			img.format = format;
			img.aspect = aspect;
			img.width = w;
			img.height = h;
			VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			ci.imageType = VK_IMAGE_TYPE_2D;
			ci.format = format;
			ci.extent = { w, h, 1 };
			ci.mipLevels = 1;
			ci.arrayLayers = 1;
			ci.samples = VK_SAMPLE_COUNT_1_BIT;
			ci.tiling = VK_IMAGE_TILING_OPTIMAL;
			ci.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			Check(vkCreateImage(s.device, &ci, nullptr, &img.image), "vkCreateImage");
			VkMemoryRequirements req;
			vkGetImageMemoryRequirements(s.device, img.image, &req);
			VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			ai.allocationSize = req.size;
			ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			Check(vkAllocateMemory(s.device, &ai, nullptr, &img.memory), "vkAllocateMemory");
			Check(vkBindImageMemory(s.device, img.image, img.memory, 0), "vkBindImageMemory");
			return img;
		}

		// ---- formats -----------------------------------------------------------------------------
		struct Format { VkFormat vk; VkImageAspectFlags aspect; };

		Format ColorFormat(uint32 gx2)
		{
			const uint32 hw = gx2 & 0x3F;
			const bool isInt = gx2 & 0x100, isSigned = gx2 & 0x200, srgb = gx2 & 0x400;
			auto pick = [&](VkFormat unorm, VkFormat snorm, VkFormat uint, VkFormat sint) {
				return isInt ? (isSigned ? sint : uint) : (isSigned ? snorm : unorm);
			};
			VkFormat f = VK_FORMAT_UNDEFINED;
			switch (hw)
			{
			case 0x01: f = pick(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT); break;
			case 0x07: f = pick(VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT); break;
			case 0x1A: f = srgb ? VK_FORMAT_R8G8B8A8_SRGB : pick(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
				VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT); break;
			case 0x19: f = pick(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
				VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32); break;
			case 0x05: f = pick(VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT); break;
			case 0x06: f = VK_FORMAT_R16_SFLOAT; break;
			case 0x0F: f = pick(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT); break;
			case 0x10: f = VK_FORMAT_R16G16_SFLOAT; break;
			case 0x1F: f = pick(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
				VK_FORMAT_R16G16B16A16_SINT); break;
			case 0x20: f = VK_FORMAT_R16G16B16A16_SFLOAT; break;
			case 0x0D: f = isSigned ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT; break;
			case 0x0E: f = VK_FORMAT_R32_SFLOAT; break;
			case 0x1D: f = isSigned ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT; break;
			case 0x1E: f = VK_FORMAT_R32G32_SFLOAT; break;
			case 0x22: f = isSigned ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT; break;
			case 0x23: f = VK_FORMAT_R32G32B32A32_SFLOAT; break;
			case 0x16: f = VK_FORMAT_B10G11R11_UFLOAT_PACK32; break;   // R11_G11_B10_FLOAT, as Cemu maps it
			case 0x08: f = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
			case 0x0B: f = VK_FORMAT_R4G4B4A4_UNORM_PACK16; break;
			case 0x0A: f = VK_FORMAT_R5G5B5A1_UNORM_PACK16; break;
			}
			if (f == VK_FORMAT_UNDEFINED)
				Fail(fmt::format("colour format {:#x} not mapped yet", gx2));
			return { f, VK_IMAGE_ASPECT_COLOR_BIT };
		}

		Format DepthFormat(uint32 gx2)
		{
			switch (gx2)
			{
			case 0x005: return { VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT };                 // D16_UNORM
			case 0x80E: return { VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT };                // D32_FLOAT (HWFMT_32_FLOAT | float)
			case 0x011: case 0x811: case 0x81C:                                                    // D24_S8 (unorm/float), D32_S8
				return { VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT };
			}
			Fail(fmt::format("depth format {:#x} not mapped yet", gx2));
		}

		// ---- surfaces ----------------------------------------------------------------------------
		Image& Surface(uint32 addr, uint32 gx2, bool depth, uint32 w, uint32 h)
		{
			Image& img = s.surfaces[{ addr, gx2 | (depth ? 0x80000000u : 0) }];
			if (img.image && img.width >= w && img.height >= h)
				return img;
			Format f = depth ? DepthFormat(gx2) : ColorFormat(gx2);
			VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
				(depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
			Image grown = CreateImage(f.vk, f.aspect, std::max(w, img.width), std::max(h, img.height), usage);
			Transition(grown, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);   // start from zero, not undefined contents
			VkImageSubresourceRange all{ f.aspect, 0, 1, 0, 1 };
			if (depth)
			{
				VkClearDepthStencilValue zero{};
				vkCmdClearDepthStencilImage(s.cmd, grown.image, grown.layout, &zero, 1, &all);
			}
			else
			{
				VkClearColorValue zero{};
				vkCmdClearColorImage(s.cmd, grown.image, grown.layout, &zero, 1, &all);
			}
			if (img.image)                                              // keep what was rendered so far
			{
				Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				Transition(grown, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				VkImageCopy c{};
				c.srcSubresource = c.dstSubresource = { img.aspect, 0, 0, 1 };
				c.extent = { img.width, img.height, 1 };
				vkCmdCopyImage(s.cmd, img.image, img.layout, grown.image, grown.layout, 1, &c);
				SubmitAndWait();                                        // then the old image can go
				vkDestroyImage(s.device, img.image, nullptr);
				vkFreeMemory(s.device, img.memory, nullptr);
			}
			img = grown;
			return img;
		}

		// ---- frame capture (as the reference's screenshots: an RGBA8 copy of the TV image) --------
		void WritePPM(const Image& tv, uint32 frame)
		{
			VkDeviceSize size = (VkDeviceSize)tv.width * tv.height * 4;
			VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			bci.size = size;
			bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			VkBuffer buf;
			Check(vkCreateBuffer(s.device, &bci, nullptr, &buf), "vkCreateBuffer");
			VkMemoryRequirements req;
			vkGetBufferMemoryRequirements(s.device, buf, &req);
			VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			ai.allocationSize = req.size;
			ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VkDeviceMemory mem;
			Check(vkAllocateMemory(s.device, &ai, nullptr, &mem), "vkAllocateMemory");
			vkBindBufferMemory(s.device, buf, mem, 0);
			Image& img = const_cast<Image&>(tv);
			Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			VkBufferImageCopy r{};
			r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			r.imageExtent = { tv.width, tv.height, 1 };
			vkCmdCopyImageToBuffer(s.cmd, img.image, img.layout, buf, 1, &r);
			SubmitAndWait();
			uint8* px;
			vkMapMemory(s.device, mem, 0, size, 0, (void**)&px);
			std::string path = fmt::format("{}/f{:06}.tv.ppm", s.shotDir, frame);
			if (FILE* f = fopen((path + ".part").c_str(), "wb"))
			{
				fprintf(f, "P6\n%u %u\n255\n", tv.width, tv.height);
				std::vector<uint8> rgb((size_t)tv.width * tv.height * 3);
				for (size_t i = 0, n = (size_t)tv.width * tv.height; i < n; i++)
					memcpy(&rgb[i * 3], &px[i * 4], 3);
				fwrite(rgb.data(), 1, rgb.size(), f);
				fclose(f);
				rename((path + ".part").c_str(), path.c_str());
			}
			vkUnmapMemory(s.device, mem);
			vkDestroyBuffer(s.device, buf, nullptr);
			vkFreeMemory(s.device, mem, nullptr);
		}
	}

	bool RendererOn()
	{
		static const bool on = [] {
			const char* r = getenv("WWHD_RENDER");
			return r && strcmp(r, "vk") == 0 && Init();
		}();
		return on;
	}

	// IT_HLE_CLEAR_COLOR_DEPTH_STENCIL (Cemu's gx2: GX2ClearColor/GX2ClearDepthStencilEx/GX2ClearBuffersEx);
	// layout as LatteCP_itHLEClearColorDepthStencil reads it
	void RendererClear(const uint32be* p, uint32 nWords)
	{
		if (nWords < 23)
			return;
		uint32 mask = p[0];
		if ((mask & 1) && (uint32)p[1])
		{
			uint32 w = std::max<uint32>(p[4], p[6]), h = p[5];
			Image& img = Surface(p[1], p[2], false, w, h);
			Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkClearColorValue c;
			c.float32[0] = (float)(uint32)p[17] / 255.0f;
			c.float32[1] = (float)(uint32)p[18] / 255.0f;
			c.float32[2] = (float)(uint32)p[19] / 255.0f;
			c.float32[3] = (float)(uint32)p[20] / 255.0f;
			VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdClearColorImage(s.cmd, img.image, img.layout, &c, 1, &range);
		}
		if ((mask & 6) && (uint32)p[9])
		{
			uint32 w = std::max<uint32>(p[12], p[14]), h = p[13];
			Image& img = Surface(p[9], p[10], true, w, h);
			Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkClearDepthStencilValue v;
			uint32 depthBits = p[21];
			memcpy(&v.depth, &depthBits, 4);
			v.stencil = p[22];
			VkImageAspectFlags aspect = ((mask & 2) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
				((mask & 4) && (img.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
			if (aspect)
			{
				VkImageSubresourceRange range{ aspect, 0, 1, 0, 1 };
				vkCmdClearDepthStencilImage(s.cmd, img.image, img.layout, &v, 1, &range);
			}
		}
	}

	// IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER: address, width, height, pitch, tile mode, swizzle, slice,
	// format, scan target (1 TV, 4 DRC)
	void RendererCopyToScanBuffer(const uint32be* p, uint32 nWords)
	{
		if (nWords < 9)
			return;
		uint32 addr = p[0], w = p[1], h = p[2], pitch = p[3], gx2 = p[7], target = p[8];
		Image& src = Surface(addr, gx2, false, std::max(w, pitch), h);
		Image& dst = s.scan[target == 1 ? 0 : 1];
		// the reference's screenshot is an RGBA8 blit of this buffer, sRGB if the scan buffer is
		VkFormat f = (target == 1 ? LatteGPUState.tvBufferUsesSRGB : LatteGPUState.drcBufferUsesSRGB)
			? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		if (!dst.image || dst.width != w || dst.height != h || dst.format != f)
			dst = CreateImage(f, VK_IMAGE_ASPECT_COLOR_BIT, w, h, 0);
		Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		Transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageBlit b{};
		b.srcSubresource = b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		b.srcOffsets[1] = { (sint32)w, (sint32)h, 1 };
		b.dstOffsets[1] = { (sint32)w, (sint32)h, 1 };
		vkCmdBlitImage(s.cmd, src.image, src.layout, dst.image, dst.layout, 1, &b, VK_FILTER_NEAREST);
	}

	// IT_HLE_TRIGGER_SCANBUFFER_SWAP: one per GX2SwapScanBuffers, so frame N is the Nth swap
	void RendererSwap()
	{
		s.frame++;
		if (s.shotFrames.count(s.frame) && s.scan[0].image)
			WritePPM(s.scan[0], s.frame);
		else
			SubmitAndWait();
	}
}
