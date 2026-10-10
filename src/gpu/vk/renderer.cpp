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
#include <chrono>
#include <atomic>
#include "renderer_internal.h"
bool PPCTimer_isVirtualClock();                                  // src/runtime/espresso/PPCTimer.cpp
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include <cfloat>

namespace wwhd::gpu
{
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
	static bool Init()
	{
		if (!Load())
		{
			Log("libvulkan not found");
			return false;
		}
		VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app.pApplicationName = "wwhd";
		app.apiVersion = VK_API_VERSION_1_3;
		std::vector<const char*> instanceExtensions = WindowInstanceExtensions();
		VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		ici.pApplicationInfo = &app;
		ici.enabledExtensionCount = (uint32)instanceExtensions.size();
		ici.ppEnabledExtensionNames = instanceExtensions.data();
		Check(vkCreateInstance(&ici, nullptr, &s.instance), "vkCreateInstance");
		LoadInstance(s.instance);
		CreateWindowSurface();
		uint32 n = 0;
		vkEnumeratePhysicalDevices(s.instance, &n, nullptr);
		std::vector<VkPhysicalDevice> devices(n);
		vkEnumeratePhysicalDevices(s.instance, &n, devices.data());
		if (devices.empty())
			Fail("no Vulkan device");
		s.physical = devices[0];                                    // VK_ICD_FILENAMES picks lavapipe (run.sh REF_GPU=llvmpipe)
		VkPhysicalDeviceProperties& props = s.props;
		vkGetPhysicalDeviceProperties(s.physical, &props);
		vkGetPhysicalDeviceMemoryProperties(s.physical, &s.memory);
		vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &n, nullptr);
		std::vector<VkQueueFamilyProperties> families(n);
		vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &n, families.data());
		s.queueFamily = UINT32_MAX;
		for (uint32 i = 0; i < n; i++)
			if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && CanPresent(i))
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
		// every feature the device has (Cemu's pipelines use independent blend, logic ops, dual-source
		// blending, depth clamp), dynamic rendering, and depth-clip control when the device has it
		VkPhysicalDeviceDepthClipEnableFeaturesEXT clip{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT };
		VkPhysicalDeviceCustomBorderColorFeaturesEXT border{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
		VkPhysicalDeviceVulkan13Features v13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		// of 1.1 and 1.2 only half-precision arithmetic and 16-bit storage (FSR 1's fp16 path), when the device has them
		VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceVulkan11Features v11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
		VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		features.pNext = &v13;
		v13.pNext = &v12;
		v12.pNext = &v11;
		vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &n, nullptr);
		std::vector<VkExtensionProperties> exts(n);
		vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &n, exts.data());
		std::vector<const char*> enable;
		if (HasWindow())
			enable.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		void** chain = &v11.pNext;
		for (auto& e : exts)
		{
			if (!strcmp(e.extensionName, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME))
			{
				enable.push_back(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
				*chain = &clip;
				chain = &clip.pNext;
			}
			if (!strcmp(e.extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME))
			{
				enable.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
				*chain = &border;
				chain = &border.pNext;
			}
		}
		vkGetPhysicalDeviceFeatures2(s.physical, &features);
		if (!v13.dynamicRendering)
			Fail("no dynamic rendering");
		{
			const VkBool32 f16 = v12.shaderFloat16, s16 = v11.storageBuffer16BitAccess, sub = v12.shaderSubgroupExtendedTypes;
			void* next12 = v12.pNext, *next11 = v11.pNext;
			v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, next12 };
			v11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, next11 };
			v12.shaderFloat16 = f16;
			v12.shaderSubgroupExtendedTypes = sub;                   // FSR 3's fp16 passes: subgroup operations on halves
			v11.storageBuffer16BitAccess = s16;
			s.float16 = f16 && s16;
			s.subgroupHalf = s.float16 && sub;
		}
		s.depthClip = clip.depthClipEnable;
		s.customBorder = border.customBorderColors && border.customBorderColorWithoutFormat;
		s.anisotropy = features.features.samplerAnisotropy;
		VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		dci.pNext = &features;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = (uint32)enable.size();
		dci.ppEnabledExtensionNames = enable.data();
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
		s.bound = {};

		// per-frame data: one host-visible buffer for uniforms, vertices and indices
		s.ring.size = LazyDrawDone() ? 512ull << 20 : 256ull << 20;   // the lazy path: a whole ring's 256 MB per slot
		VkBufferCreateInfo rbi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		rbi.size = s.ring.size;
		rbi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		Check(vkCreateBuffer(s.device, &rbi, nullptr, &s.ring.buffer), "vkCreateBuffer");
		VkMemoryRequirements rreq;
		vkGetBufferMemoryRequirements(s.device, s.ring.buffer, &rreq);
		VkMemoryAllocateInfo rai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		rai.allocationSize = rreq.size;
		rai.memoryTypeIndex = MemoryType(rreq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		Check(vkAllocateMemory(s.device, &rai, nullptr, &s.ring.memory), "vkAllocateMemory");
		Check(vkBindBufferMemory(s.device, s.ring.buffer, s.ring.memory, 0), "vkBindBufferMemory");
		Check(vkMapMemory(s.device, s.ring.memory, 0, s.ring.size, 0, (void**)&s.ring.data), "vkMapMemory");
		VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 << 18 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 << 17 } };
		VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		dpi.maxSets = 1 << 16;
		dpi.poolSizeCount = 2;
		dpi.pPoolSizes = sizes;
		Check(vkCreateDescriptorPool(s.device, &dpi, nullptr, &s.descriptors), "vkCreateDescriptorPool");
		s.cmds[0] = s.cmd, s.fences[0] = s.fence, s.pools[0] = s.descriptors;
		s.ringBase = 0, s.ringEnd = s.ring.size;
		if (LazyDrawDone())
		{
			Check(vkAllocateCommandBuffers(s.device, &cai, &s.cmds[1]), "vkAllocateCommandBuffers");
			Check(vkCreateFence(s.device, &fci, nullptr, &s.fences[1]), "vkCreateFence");
			Check(vkCreateDescriptorPool(s.device, &dpi, nullptr, &s.pools[1]), "vkCreateDescriptorPool");
			s.ringEnd = s.ring.size / 2;                              // slot 0: the first half, slot 1 the second
			Log("lazy GX2DrawDone: two frames in flight (WWHD_LAZY_DRAWDONE)");
		}
		DrawInit();
		timing::Init(DynamicScale());

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

	// the render thread's waits for the GPU's fence (the frame log's fence_ms: how much of the game's GX2DrawDone
	// wait is the GPU's, not the command processing's)
	std::atomic<uint64> s_fenceWaitNs = 0;
	uint64 TakeFenceWaitNs()
	{
		return s_fenceWaitNs.exchange(0);
	}

	namespace
	{
		void WaitFence(VkFence fence)
		{
			const auto waitFrom = std::chrono::steady_clock::now();
			Check(vkWaitForFences(s.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
			s_fenceWaitNs += (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitFrom).count();
			vkResetFences(s.device, 1, &fence);
		}

		// the current slot's resources, fresh for recording (its fence already waited for)
		void BeginSlot()
		{
			vkResetCommandBuffer(s.cmd, 0);
			s.ring.used = s.ringBase;                                 // the GPU is done with this slot's data
			vkResetDescriptorPool(s.device, s.descriptors, 0);
			OnSubmitted();
			VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			Check(vkBeginCommandBuffer(s.cmd, &bi), "vkBeginCommandBuffer");
			s.bound = {};
			timing::Begin();
			// the lazy path: the last frame's submit wasn't waited for, so its work may still run on the GPU, and
			// barriers are recorded only where an image changes layout. An image written at the end of one frame and
			// again at the start of the next in the same layout would be a hazard with nothing between: this orders
			// everything here after everything submitted before, as the fence wait did (deck-plan item 2's review,
			// problem 2). The CPU no longer waits; only the GPU keeps the order. Without the lazy path, nothing.
			if (s.cmds[1])
			{
				VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
				b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
				b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
					nullptr, 0, nullptr);
			}
		}

		void Submit(VkSemaphore signal = VK_NULL_HANDLE)
		{
			EndRendering();
			timing::End();                                            // the last span's end
			Check(vkEndCommandBuffer(s.cmd), "vkEndCommandBuffer");
			VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
			si.commandBufferCount = 1;
			si.pCommandBuffers = &s.cmd;
			si.signalSemaphoreCount = signal ? 1 : 0;
			si.pSignalSemaphores = &signal;
			Check(vkQueueSubmit(s.queue, 1, &si, s.fence), "vkQueueSubmit");
		}
	}

	// Real time only, never under the virtual clock: the checks keep the full wait, so they stay exact by
	// construction. With a window the present waits for the swap's submit by a semaphore (present.cpp).
	// WWHD_LAZY_DRAWDONE=2 (a test) also under the virtual clock: the guest's time is its cycles there, so the
	// checks then compare the lazy path's pictures with the references (captures byte-identical: it renders the same)
	bool LazyDrawDone()
	{
		static const bool on = [] {
			const char* e = getenv("WWHD_LAZY_DRAWDONE");
			const int v = e ? atoi(e) : 0;
			const bool lazy = v == 2 || v == 3 || (v == 1 && !PPCTimer_isVirtualClock());
			// FSR 3 on the render thread with the lazy path crashed the command processor in a third of the worker's
			// runs (qa's q-lazyring): off with it until that's fixed
			if (lazy && fsr3::On())
			{
				Log("lazy GX2DrawDone off: WWHD_UPSCALER=fsr3 (the lazy path with FSR 3 crashes the command processor; qa's q-lazyring)");
				return false;
			}
			return lazy;
		}();
		return on;
	}

	void WaitPending()
	{
		for (uint32 i = 0; i < 2; i++)
			if (s.pending[i])
			{
				WaitFence(s.fences[i]);
				s.pending[i] = false;
			}
	}

	void SubmitAndWait()
	{
		Submit();
		WaitFence(s.fence);
		const uint32 other = s.slot ^ 1;
		if (s.pending[other])                                       // the other slot's frame too: the GPU idle
		{
			WaitFence(s.fences[other]);
			s.pending[other] = false;
		}
		BeginSlot();
	}

	// The swap's submit. Off: SubmitAndWait. On: submitted and not waited for; recording goes on in the other slot,
	// which waits only for its own last frame (submitted a frame earlier), so the swap's timestamp, and with it the
	// game's GX2DrawDone, no longer waits for the GPU to finish the frame (1.45 of its 1.58 ms a frame on the 13700K).
	void SubmitFrame()
	{
		if (!s.cmds[1])
			return SubmitAndWait();
		Submit(PresentSemaphore());                                 // a window's present waits for it (none without one)
		s.pending[s.slot] = true;
		s.slot ^= 1;
		s.cmd = s.cmds[s.slot], s.fence = s.fences[s.slot], s.descriptors = s.pools[s.slot];
		s.ringBase = s.slot ? s.ring.size / 2 : 0;
		s.ringEnd = s.slot ? s.ring.size : s.ring.size / 2;
		if (s.pending[s.slot])
		{
			WaitFence(s.fence);
			s.pending[s.slot] = false;
		}
		// WWHD_LAZY_DRAWDONE=3 (a test): the two slots and the ring's alternating halves, but every frame still waited
		// for: what differs from =0 is then the layout alone (a draw reading past its ring allocation), not the timing
		static const bool waitAll = [] { const char* e = getenv("WWHD_LAZY_DRAWDONE"); return e && atoi(e) == 3; }();
		if (waitAll && s.pending[s.slot ^ 1])
		{
			WaitFence(s.fences[s.slot ^ 1]);
			s.pending[s.slot ^ 1] = false;
		}
		BeginSlot();
	}

	// After a copy into a host-visible buffer that the host reads once the fence is waited for: the fence makes the
	// copy's writes available to the device, not visible to the host; this barrier does that (the spec's
	// "host access to device memory" rule). It records no work on any image or buffer.
	void HostReadBarrier()
	{
		VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
	}

	// ---- images ------------------------------------------------------------------------------
	namespace
	{
		// Narrow barriers (gpu-plan.md item 2; the default from session bottom's A/B, WWHD_BARRIERS=all: the old ones, every
		// transition waiting for everything): a transition waits only for the stages that used
		// the image in its old layout, and makes their writes visible to the stages of the new one, so the GPU can
		// overlap work on other images (the next pass's vertex work with this one's fragments). Each layout here has
		// one use: attachments (read and written in their pass), sampling, a transfer's source or destination. A
		// second write in the same layout (the next pass into an attachment, a copy after a clear) gets a barrier of
		// its own: by default an unrelated transition in between orders it, as every barrier waits for everything.
		// Images that change outside Transition (uploads, the mip chains, the window's) keep their own barriers.
		bool NarrowBarriers()
		{
			static const bool on = [] { const char* e = getenv("WWHD_BARRIERS"); return !e || strcmp(e, "all") != 0; }();
			return on;
		}

		struct LayoutUse { VkPipelineStageFlags stages; VkAccessFlags write, read; };
		LayoutUse UseOf(VkImageLayout layout)
		{
			switch (layout)
			{
			case VK_IMAGE_LAYOUT_UNDEFINED:
				return { VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0 };
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				return { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					VK_ACCESS_COLOR_ATTACHMENT_READ_BIT };
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
				return { VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
					VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT };
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:            // the renderer draws without geometry shaders
				return { VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT };
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				return { VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_READ_BIT };
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				return { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0 };
			default:
				return { VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT };
			}
		}
	}

	// narrow by default (above); WWHD_BARRIERS=all: every transition waits for everything before it
	bool DeferClear(Image& img, const VkClearValue& value)
	{
		static const bool on = [] {
			const char* e = getenv("WWHD_CLEAR_LOADOP");
			const bool v = !e || atoi(e) != 0;
			if (!v)
				Log("clears recorded where the game makes them (WWHD_CLEAR_LOADOP=0)");
			return v;
		}();
		if (!on || img.layers != 1 || !(img.aspect & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT)))
			return false;
		img.clearPending = true;
		img.clearValue = value;
		return true;
	}

	void FlushClear(Image& img)
	{
		if (!img.clearPending)
			return;
		img.clearPending = false;
		Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageSubresourceRange all{ img.aspect, 0, 1, 0, 1 };
		if (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
			vkCmdClearDepthStencilImage(s.cmd, img.image, img.layout, &img.clearValue.depthStencil, 1, &all);
		else
			vkCmdClearColorImage(s.cmd, img.image, img.layout, &img.clearValue.color, 1, &all);
	}

	void Transition(Image& img, VkImageLayout layout)
	{
		FlushClear(img);
		const bool narrow = NarrowBarriers();
		const LayoutUse from = UseOf(img.layout), to = UseOf(layout);
		if (img.layout == layout && (!narrow || !to.write))
			return;
		VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		b.srcAccessMask = narrow ? from.write : VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = narrow ? to.write | to.read : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.oldLayout = img.layout;
		b.newLayout = layout;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = img.image;
		b.subresourceRange = { img.aspect, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
		vkCmdPipelineBarrier(s.cmd, narrow ? from.stages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			narrow ? to.stages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		img.layout = layout;
	}

	Image CreateImage(VkFormat format, VkImageAspectFlags aspect, uint32 w, uint32 h, VkImageUsageFlags usage, uint32 layers)
	{
		Image img;
		img.format = format;
		img.aspect = aspect;
		img.width = img.gw = w;
		img.height = img.gh = h;
		img.layers = layers;
		VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		ci.imageType = VK_IMAGE_TYPE_2D;
		ci.format = format;
		ci.extent = { w, h, 1 };
		ci.mipLevels = 1;
		ci.arrayLayers = layers;
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
		if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
			VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			vi.image = img.image;
			vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vi.format = format;
			vi.subresourceRange = { aspect, 0, 1, 0, 1 };
			Check(vkCreateImageView(s.device, &vi, nullptr, &img.view), "vkCreateImageView");
		}
		return img;
	}

	VkImageView LayerView(Image& img, uint32 layer)
	{
		if (layer == 0)
			return img.view;
		if (img.layerViews.size() < layer)
			img.layerViews.resize(layer, VK_NULL_HANDLE);
		VkImageView& v = img.layerViews[layer - 1];
		if (!v)
		{
			VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			vi.image = img.image;
			vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vi.format = img.format;
			vi.subresourceRange = { img.aspect, 0, 1, layer, 1 };
			Check(vkCreateImageView(s.device, &vi, nullptr, &v), "vkCreateImageView");
		}
		return v;
	}

	void DestroyImage(Image& img)
	{
		ForgetImage(img.image);
		if (fsr3::On())
			fsr3::Forget(img.image);
		ForgetSets();
		for (VkImageView v : img.layerViews)
			if (v)
				vkDestroyImageView(s.device, v, nullptr);
		if (img.view)
			vkDestroyImageView(s.device, img.view, nullptr);
		vkDestroyImage(s.device, img.image, nullptr);
		vkFreeMemory(s.device, img.memory, nullptr);
		img = Image{};
	}

	VkDeviceSize RingAlloc(VkDeviceSize size, VkDeviceSize align)
	{
		VkDeviceSize at = (std::max(s.ring.used, s.ringBase) + align - 1) & ~(align - 1);
		if (at + size + 65536 > s.ringEnd)                          // 64 KB spare: uniform buffer descriptors span that much
		{
			SubmitAndWait();
			at = s.ringBase;
			if (size + 65536 > s.ringEnd - s.ringBase)
				Fail(fmt::format("{} bytes of draw data don't fit the ring", size));
		}
		s.ring.used = at + size;
		// WWHD_RING_POISON=1 (a test): the 64 KB after each allocation zeroed, so a draw that reads past its data (a
		// uniform buffer's 64 KB range) reads zeros instead of what an earlier draw left there
		static const bool poison = [] { const char* e = getenv("WWHD_RING_POISON"); return e && atoi(e) == 1; }();
		if (poison)
			memset(s.ring.data + at + size, 0, (size_t)std::min<VkDeviceSize>(65536, s.ringEnd - (at + size)));
		return at;
	}

	// ---- formats -----------------------------------------------------------------------------

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

	// ---- render scale (gpu-plan.md item 3) ----------------------------------------------------
	// WWHD_RENDER_SCALE=0.5..2: the game's screen-sized render targets at that scale (1, the default and every check's:
	// the guest's sizes, nothing changes). Read once at start. Only the targets the TV's size (1920x1088, the scene's and
	// its full-screen passes': nearly all the pixels drawn) are scaled: the half-size ones are its effects, sampled with
	// offsets sized in their own texels (ambient occlusion at half scale lost half its strength, the edges a light
	// halo), and the shadow maps' resolution is their own; both cost little next to the full-size passes.
	// WWHD_RENDER_SCALE=auto: dynamic resolution (session bottom): the scale moves between WWHD_RENDER_SCALE_MIN (0.5)
	// and WWHD_RENDER_SCALE_MAX (1) in steps of 1/8 (whole pixels: 1920 and 1088 are multiples of 16): down while
	// frames miss the frame rate and the GPU's time a frame (gpu_timing's light mode) is near a budget
	// (WWHD_RENDER_SCALE_BUDGET ms, default 92% of a frame at the frame rate, WWHD_60FPS), up by trying (ChooseScale).
	// A change rescales every scaled surface at the swap (RescaleSurfaces).
	namespace
	{
		struct ScaleConfig { bool dynamic = false; float start = 1.0f, min = 0.5f, max = 1.0f, budget = 0.0f; };
		const ScaleConfig& Config()
		{
			static const ScaleConfig c = [] {
				ScaleConfig c;
				const char* e = getenv("WWHD_RENDER_SCALE");
				auto num = [](const char* name, float def) { const char* v = getenv(name); return v && atof(v) > 0 ? (float)atof(v) : def; };
				if (e && strcmp(e, "auto") == 0)
				{
					c.dynamic = true;
					c.max = std::clamp(num("WWHD_RENDER_SCALE_MAX", 1.0f), 0.5f, 2.0f);
					c.min = std::clamp(num("WWHD_RENDER_SCALE_MIN", 0.5f), 0.5f, c.max);
					const char* sixty = getenv("WWHD_60FPS");
					c.budget = num("WWHD_RENDER_SCALE_BUDGET", (sixty && atoi(sixty) != 0 ? 1000.0f / 60.0f : 1000.0f / 30.0f) * 0.92f);
					c.start = c.max;
				}
				else
				{
					const float v = e ? (float)atof(e) : 1.0f;
					c.start = v > 0.0f ? std::clamp(v, 0.5f, 2.0f) : 1.0f;
				}
				return c;
			}();
			return c;
		}
		float s_scale = 0.0f;                                     // 0: not read yet
	}

	float RenderScale()
	{
		if (s_scale == 0.0f)
			s_scale = Config().start;
		return s_scale;
	}

	bool DynamicScale()
	{
		return Config().dynamic;
	}

	bool ScaledSurface(uint32 gw, uint32 gh)
	{
		return (DynamicScale() || RenderScale() != 1.0f) && gw >= 1280 && gh >= 720;
	}

	uint32 ScaledBy(uint32 guest, float scale)
	{
		return scale == 1.0f ? guest : std::max(1u, (uint32)std::ceil(guest * scale - 0.001f));
	}

	uint32 Scaled(uint32 guest, bool scaled)
	{
		return scaled ? ScaledBy(guest, RenderScale()) : guest;
	}

	// ---- surfaces ----------------------------------------------------------------------------
	// Surface fit (gpu-plan.md item 2; the default from session bottom's A/B and proof sweep, WWHD_SURFACE_FIT=0 turns
	// it off): a render target is often taller than anything reads of it
	// (1920x1088, its height padded for tiling, sampled as a 1920x1080 texture), and a texture of another size than its
	// surface is a copy (CopyOf) every frame it was drawn. At each swap a surface that every read so far wanted fewer
	// rows of is made that many rows tall (its rows kept): it's sampled directly, and the passes into it draw only
	// those rows (the render area is the smallest attachment). Draws into the rows cut off are lost, which is exact
	// as long as nothing reads them. So only targets the size of the screen (the TV's scan buffer: 1920x1080 read
	// from 1920x1088, the copies that cost) are fitted, once their reads have wanted the same rows for kFitStable
	// frames, and never one a taller texture was read from. (Smaller ones are left: the start's loading, frame 902 on
	// every route from the 100% save, reads a 960x544 target as 1080 rows, after 900 frames of 540.) A read that
	// still wants more rows than a surface was fitted to is logged ("surface fit: read past"), and the surface goes
	// back to its full height for good: that frame may differ, the log says so. WWHD_SURFACE_FIT=proof changes
	// nothing and logs the same reads against the heights it would fit to: the proof that the rows cut off are never
	// read, on any route (tools/sixty/tests/fit_proof.sh).
	namespace
	{
		int SurfaceFitMode()                                      // 0 off, 1 fit, 2 proof
		{
			static const int mode = [] {
				const char* e = getenv("WWHD_SURFACE_FIT");
				return !e ? 1 : strcmp(e, "proof") == 0 ? 2 : atoi(e) != 0 ? 1 : 0;
			}();
			return mode;
		}
		uint32 s_fitReadsPast = 0;
		constexpr uint32 kFitStable = 300;

		void FitSurfaces()
		{
			const int mode = SurfaceFitMode();
			if (!mode)
				return;
			for (auto& [key, img] : s.surfaces)
			{
				if (!img.image || img.fitH || img.noFit || !img.readH || img.readH >= img.gh || s.frame - img.readSince < kFitStable)
					continue;
				if (img.gw != s.scan[0].gw || img.readH != s.scan[0].gh)
					continue;                                       // only the screen's size (the copies that cost)
				img.fitH = img.readH;
				Log(fmt::format("surface fit: {:08x} fmt {:x} {}x{} {} to {} rows{}", key.first, key.second & 0x7FFFFFFF, img.width,
					img.height, mode == 1 ? "fitted" : "would be fitted", img.readH, mode == 1 ? "" : " (proof)"));
				if (mode != 1)
					continue;
				EndRendering();
				Image fitted = CreateImage(img.format, img.aspect, img.width, ScaledBy(img.readH, img.scale), VK_IMAGE_USAGE_SAMPLED_BIT |
					((img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
					img.layers);
				Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				Transition(fitted, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				VkImageCopy c{};
				c.srcSubresource = c.dstSubresource = { img.aspect, 0, 0, img.layers };
				c.extent = { img.width, fitted.height, 1 };
				vkCmdCopyImage(s.cmd, img.image, img.layout, fitted.image, fitted.layout, 1, &c);
				fitted.gw = img.gw, fitted.gh = img.readH, fitted.scaled = img.scaled, fitted.scale = img.scale;
				SubmitAndWait();                                    // then the old image can go
				const uint32 bytes = img.bytes, readH = img.readH, readSince = img.readSince, fitH = img.fitH;
				const uint64 written = img.written, resetFor = img.resetFor;
				DestroyImage(img);
				img = fitted;
				img.bytes = bytes, img.readH = readH, img.readSince = readSince, img.fitH = fitH;
				img.written = written, img.resetFor = resetFor;
			}
		}
	}

	// ---- the HUD over an upscaled image (WWHD_UPSCALER=fsr1, fsr1.cpp) --------------------------------------
	// The game draws its 3D scene, then its post effects (full-screen passes sampling other targets), then its HUD (2D
	// layouts sampling fonts and icons from memory) into the TV surface, which it copies to the TV scan buffer. With a
	// render scale the TV surface is scaled, the HUD with it. HudBegins, at the HUD's first draw (draw.cpp: a draw into
	// the TV surface alone, sampling only memory textures, after a pass into it alone that sampled surfaces), upscales
	// the scene with FSR 1 into an image of the guest's size and makes that the TV surface for the rest of the frame,
	// so the HUD is drawn at full resolution. A later pass into it with other scaled targets (the lock-on cursor's 3D
	// draws with the scene's depth, among the HUD's on en-tn) gets full-size stand-ins for those, blitted up
	// (HudPromote). The swap puts the scaled images back (the stand-ins' pictures blitted down). A frame without that
	// order (no post pass: the title, loading) gets FSR 1 at the scan copy instead, HUD included.
	namespace
	{
		bool CanBlit(VkFormat f, bool linear);                    // below (dynamic resolution's)

		struct Promoted
		{
			Image scaled;                                         // the map's image while the full one stands in
			Image full;                                           // kept from frame to frame
			bool active = false;
		};
		std::map<std::pair<uint32, uint32>, Promoted> s_promoted;  // by surface key; the TV surface's among them
		std::pair<uint32, uint32> s_tvKey{};
		bool s_tvKnown = false, s_hudActive = false;
		struct { uint32 split = 0, atScan = 0, promoted = 0; } s_hudStats;   // a line every 600 frames (HudEnds)

		std::pair<uint32, uint32> KeyOf(const Image& img)
		{
			for (auto& [key, other] : s.surfaces)
				if (&other == &img)
					return key;
			return {};
		}

		// the full-size image for `img` (made or kept), its picture by FSR 1 (the TV) or a blit, swapped into the map
		bool Promote(const std::pair<uint32, uint32>& key, Image& img, bool fsr)
		{
			if (img.scale >= 1.0f || img.layers != 1)
				return false;
			Promoted& p = s_promoted[key];
			if (p.active)
				return true;
			const bool depth = img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
			if (!p.full.image || p.full.width != img.gw || p.full.height != img.gh || p.full.format != img.format)
			{
				if (p.full.image)
				{
					WaitPending();
					DestroyImage(p.full);
				}
				p.full = CreateImage(img.format, img.aspect, img.gw, img.gh, VK_IMAGE_USAGE_SAMPLED_BIT |
					(depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
					(fsr && fsr3::On() ? VK_IMAGE_USAGE_STORAGE_BIT : 0));    // FSR 3 writes it from a compute pass
			}
			EndRendering();
			if (fsr)
			{
				// FSR 3 with this frame's motion and the scene's depth (both at the scene's scale), else FSR 1
				Image* motionTarget = fsr3::On() ? motion::TargetImage() : nullptr;
				Image* sceneDepth = motionTarget ? motion::SceneDepth() : nullptr;
				float jx = 0.0f, jy = 0.0f;
				if (motion::JitterOn())
					motion::Jitter(s.frame, jx, jy);
				const bool done = sceneDepth && sceneDepth->image && sceneDepth->scale == img.scale &&
					fsr3::Upscale(img, *sceneDepth, *motionTarget, p.full, jx, jy, false);
				if (!done && !fsr1::Upscale(img, p.full))
					return false;
			}
			else
			{
				Transition(p.full, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				if (CanBlit(img.format, !depth))
				{
					Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
					VkImageBlit b{};
					b.srcSubresource = b.dstSubresource = { img.aspect, 0, 0, 1 };
					b.srcOffsets[1] = { (sint32)img.width, (sint32)img.height, 1 };
					b.dstOffsets[1] = { (sint32)p.full.width, (sint32)p.full.height, 1 };
					vkCmdBlitImage(s.cmd, img.image, img.layout, p.full.image, p.full.layout, 1, &b, depth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
				}
				else
				{
					VkImageSubresourceRange all{ img.aspect, 0, 1, 0, 1 };
					VkClearDepthStencilValue zd{};
					VkClearColorValue zc{};
					if (depth)
						vkCmdClearDepthStencilImage(s.cmd, p.full.image, p.full.layout, &zd, 1, &all);
					else
						vkCmdClearColorImage(s.cmd, p.full.image, p.full.layout, &zc, 1, &all);
				}
			}
			Image full = p.full;
			full.gw = img.gw, full.gh = img.gh, full.scaled = img.scaled, full.scale = 1.0f;
			full.bytes = img.bytes, full.written = img.written, full.resetFor = img.resetFor;
			full.readH = img.readH, full.fitH = img.fitH, full.readSince = img.readSince, full.noFit = img.noFit;
			p.scaled = img;
			img = full;
			p.active = true;
			return true;
		}

		// at the swap: every stand-in out of the map, its picture blitted down (not the TV's: the next frame draws it
		// again), the scaled image back in
		void Demote()
		{
			for (auto& [key, p] : s_promoted)
			{
				if (!p.active)
					continue;
				p.active = false;
				auto it = s.surfaces.find(key);
				if (it == s.surfaces.end() || it->second.image != p.full.image)
				{
					WaitPending();
					DestroyImage(p.scaled);                           // the surface was made again meanwhile
					p.full = Image{};
					continue;
				}
				Image& img = it->second;
				Image scaled = p.scaled;
				const bool depth = img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
				if (!(s_tvKnown && key == s_tvKey) && CanBlit(img.format, !depth))
				{
					EndRendering();
					Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
					Transition(scaled, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
					VkImageBlit b{};
					b.srcSubresource = b.dstSubresource = { img.aspect, 0, 0, 1 };
					b.srcOffsets[1] = { (sint32)img.width, (sint32)img.height, 1 };
					b.dstOffsets[1] = { (sint32)scaled.width, (sint32)scaled.height, 1 };
					vkCmdBlitImage(s.cmd, img.image, img.layout, scaled.image, scaled.layout, 1, &b, depth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
				}
				scaled.bytes = img.bytes, scaled.written = img.written, scaled.resetFor = img.resetFor;
				scaled.readH = img.readH, scaled.readSince = img.readSince, scaled.noFit = img.noFit;
				p.full = img;                                         // its layout as it was left
				img = scaled;
			}
		}
	}

	Image* TvSurface()
	{
		if (!s_tvKnown || !(fsr1::On() || fsr3::On()))
			return nullptr;
		auto it = s.surfaces.find(s_tvKey);
		return it == s.surfaces.end() || !it->second.image ? nullptr : &it->second;
	}

	bool HudActive()
	{
		return s_hudActive;
	}

	void HudBegins(bool atScanCopy)
	{
		Image* tv = TvSurface();
		if (!tv || s_hudActive)
			return;
		if (!Promote(s_tvKey, *tv, true))
			return;
		s_hudActive = true;
		(atScanCopy ? s_hudStats.atScan : s_hudStats.split)++;
	}

	void HudPromote(Image& img)
	{
		if (s_hudActive && Promote(KeyOf(img), img, false))
			s_hudStats.promoted++;
	}

	namespace
	{
		void HudEnds()
		{
			if ((fsr1::On() || fsr3::On()) && s.frame % 600 == 0 && (s_hudStats.split || s_hudStats.atScan))
			{
				Log(fmt::format("fsr1: frames {}-{}: {} upscaled where the HUD begins, {} at the scan copy (HUD included); {} "
					"targets given full-size stand-ins after the split", s.frame - 599, s.frame, s_hudStats.split, s_hudStats.atScan,
					s_hudStats.promoted));
				s_hudStats = {};
			}
			s_hudActive = false;
			Demote();
		}
	}

	// ---- dynamic resolution (WWHD_RENDER_SCALE=auto, above) ---------------------------------------
	namespace
	{
		bool CanBlit(VkFormat f, bool linear)
		{
			VkFormatProperties fp;
			vkGetPhysicalDeviceFormatProperties(s.physical, f, &fp);
			const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
				(linear ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0);
			return (fp.optimalTilingFeatures & need) == need;
		}

		// every scaled surface made again at `scale`, its picture blitted over (depth, and formats that can't be blitted,
		// start from zero, as a grown surface does: the next frame draws them again); its copies, views and descriptor
		// sets go with the old image (DestroyImage), mip chains are made again at their next use (ChainOf)
		void RescaleSurfaces(float scale)
		{
			s_scale = scale;                                          // surfaces made from now on
			bool any = false;
			for (auto& [key, img] : s.surfaces)
				any |= img.image && img.scaled && img.scale != scale;
			if (!any)
				return;
			EndRendering();
			WaitPending();                                            // the other frame in flight may still use them
			std::vector<std::pair<Image*, Image>> made;
			for (auto& [key, img] : s.surfaces)
			{
				if (!img.image || !img.scaled || img.scale == scale)
					continue;
				const bool depth = img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
				Image next = CreateImage(img.format, img.aspect, ScaledBy(img.gw, scale), ScaledBy(img.gh, scale), VK_IMAGE_USAGE_SAMPLED_BIT |
					(depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), img.layers);
				next.gw = img.gw, next.gh = img.gh, next.scaled = true, next.scale = scale;
				Transition(next, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				VkImageSubresourceRange all{ img.aspect, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
				const bool linear = !depth && CanBlit(img.format, true);
				if (img.clearPending)                                  // cleared anyway: the clear moves over
				{
					next.clearPending = true, next.clearValue = img.clearValue;
					img.clearPending = false;
				}
				else if (linear || (depth && CanBlit(img.format, false)))
				{
					Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
					VkImageBlit b{};
					b.srcSubresource = b.dstSubresource = { img.aspect, 0, 0, img.layers };
					b.srcOffsets[1] = { (sint32)img.width, (sint32)img.height, 1 };
					b.dstOffsets[1] = { (sint32)next.width, (sint32)next.height, 1 };
					vkCmdBlitImage(s.cmd, img.image, img.layout, next.image, next.layout, 1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
				}
				else if (depth)
				{
					VkClearDepthStencilValue zero{};
					vkCmdClearDepthStencilImage(s.cmd, next.image, next.layout, &zero, 1, &all);
				}
				else
				{
					VkClearColorValue zero{};
					vkCmdClearColorImage(s.cmd, next.image, next.layout, &zero, 1, &all);
				}
				made.emplace_back(&img, next);
			}
			SubmitAndWait();                                          // then the old images can go
			for (auto& [img, next] : made)
			{
				Image& old = *img;
				next.bytes = old.bytes, next.written = old.written, next.resetFor = old.resetFor;
				next.readH = old.readH, next.fitH = old.fitH, next.readSince = old.readSince, next.noFit = old.noFit;
				DestroyImage(old);
				old = next;
			}
		}

		// the controller (Config's comment, above). The GPU's time alone can't decide: an iGPU with time to spare clocks
		// itself down, so its time a frame rises again at a lower scale (the worker at 50%: 804 MHz instead of 1150).
		// So it goes down only while frames miss the frame rate and the GPU's time is near the budget (the GPU, not
		// the CPU, is behind), and up by trying: after two seconds of frames on time one step up; if frames then miss,
		// back down, and the next try waits twice as long (up to 32 s; back to 2 s once a step up held 10 s)
		void ChooseScale()
		{
			if (!DynamicScale())
				return;
			constexpr uint32 kWindow = 30;                        // half a second at 60
			constexpr float kStep = 0.125f;
			using Clock = std::chrono::steady_clock;
			static Clock::time_point s_since = Clock::now();
			static uint32 s_onTime = 0, s_wait = 4, s_heldUp = 0;
			static bool s_tried = false;                          // the last change was a try upwards
			double ms;
			if (!timing::TakeWindow(kWindow, ms))
				return;
			const auto now = Clock::now();
			const double period = std::chrono::duration<double, std::milli>(now - s_since).count() / kWindow;
			s_since = now;
			const ScaleConfig& c = Config();
			const double target = c.budget / 0.92;                // a frame at the frame rate
			const bool missing = period > target * 1.04;
			const float scale = RenderScale();
			float next = scale;
			if (missing)
			{
				s_onTime = 0;
				if (s_tried)                                          // the try failed: back, and wait longer next time
				{
					next = std::max(c.min, scale - kStep);
					s_wait = std::min(s_wait * 2, 64u);
				}
				else if (ms > c.budget * 0.8 && scale > c.min)
				{
					// the step from the estimate that all of the GPU's time scales with the area (at least one step)
					const float want = scale * (float)std::sqrt(c.budget / ms);
					next = std::min(scale - kStep, std::max(c.min, std::floor(want / kStep + 0.001f) * kStep));
				}
				s_tried = false, s_heldUp = 0;
			}
			else
			{
				s_onTime++;
				if (s_tried && ++s_heldUp >= 20)                     // a try held ten seconds: tries come quickly again
					s_tried = false, s_wait = 4;
				if (scale < c.max && s_onTime >= s_wait)
					next = std::min(c.max, scale + kStep), s_tried = true, s_heldUp = 0;
			}
			if (next == scale)
				return;
			s_onTime = 0;
			Log(fmt::format("render scale: {:.3f} -> {:.3f} at frame {} (frames {:.2f} ms apart, the GPU {:.2f} ms a frame, budget {:.2f})",
				scale, next, s.frame, period, ms, c.budget));
			RescaleSurfaces(next);
			s_since = Clock::now();                                   // the rescale's own time not counted
		}
	}

	void SurfaceRead(Image& img, uint32 rows)
	{
		if (rows > img.readH)
			img.readH = rows, img.readSince = s.frame;
		if (rows > img.gh && !img.fitH)
			img.noFit = true;                                       // a taller texture at its address
		if (img.fitH && rows > img.fitH)
		{
			if (s_fitReadsPast++ < 50)
				for (auto& [key, other] : s.surfaces)
					if (&other == &img)
						Log(fmt::format("surface fit: read past: {:08x} fmt {:x}: {} rows read, fitted to {} (frame {}){}", key.first,
							key.second & 0x7FFFFFFF, rows, img.fitH, s.frame + 1, SurfaceFitMode() == 1 ? "; back to its full height" : " (proof)"));
			img.fitH = 0;                                          // Surface grows it back at its next use
			img.noFit = true;
		}
	}

	Image& Surface(uint32 addr, uint32 gx2, bool depth, uint32 w, uint32 h, uint32 layers)
	{
		Image& img = s.surfaces[{ addr, gx2 | (depth ? 0x80000000u : 0) }];
		img.bytes = std::max(img.bytes, w * h * layers * Latte::GetFormatBits((Latte::E_GX2SURFFMT)gx2) / 8);
		if (img.fitH && SurfaceFitMode() == 1)
			h = std::min(h, img.fitH);
		if (img.image && img.gw >= w && img.gh >= h && img.layers >= layers)
			return img;
		EndRendering();                                             // clears and copies follow
		timing::Scope span(timing::Kind::Grow);
		Format f = depth ? DepthFormat(gx2) : ColorFormat(gx2);
		VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
			(depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		const uint32 gw = std::max(w, img.gw), gh = std::max(h, img.gh);
		const bool scaled = ScaledSurface(gw, gh);
		Image grown = CreateImage(f.vk, f.aspect, Scaled(gw, scaled), Scaled(gh, scaled), usage, std::max(layers, img.layers));
		grown.gw = gw, grown.gh = gh, grown.scaled = scaled, grown.scale = scaled ? RenderScale() : 1.0f;
		Transition(grown, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);   // start from zero, not undefined contents
		VkImageSubresourceRange all{ f.aspect, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
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
			if (img.scale == grown.scale)
			{
				VkImageCopy c{};
				c.srcSubresource = c.dstSubresource = { img.aspect, 0, 0, img.layers };
				c.extent = { img.width, img.height, 1 };
				vkCmdCopyImage(s.cmd, img.image, img.layout, grown.image, grown.layout, 1, &c);
			}
			else                                                    // grown to the screen's size (now scaled), or another scale
			{
				VkImageBlit b{};
				b.srcSubresource = b.dstSubresource = { img.aspect, 0, 0, img.layers };
				b.srcOffsets[1] = { (sint32)img.width, (sint32)img.height, 1 };
				b.dstOffsets[1] = { (sint32)ScaledBy(img.gw, grown.scale), (sint32)ScaledBy(img.gh, grown.scale), 1 };
				vkCmdBlitImage(s.cmd, img.image, img.layout, grown.image, grown.layout, 1, &b, VK_FILTER_NEAREST);
			}
			SubmitAndWait();                                        // then the old image can go
		}
		uint32 bytes = img.bytes, readH = img.readH, readSince = img.readSince;
		uint64 written = img.written, resetFor = img.resetFor;
		bool noFit = img.noFit;
		if (img.image)
			DestroyImage(img);
		img = grown;
		img.bytes = bytes;
		img.written = written;
		img.resetFor = resetFor;
		img.readH = readH, img.readSince = readSince, img.noFit = noFit;
		return img;
	}

	// Guest memory is shared by every surface in it; here each surface has its own image. When a
	// surface was written after another it overlaps, the older one's data is gone on the real GPU.
	// The reference's texture cache then deletes the older texture and reloads it from guest memory
	// (LatteTC_CleanupCheckTexture with LatteTC_IsTextureDataOverwritten; the GPU never writes guest
	// memory, so it reloads zeros), at a swap once the texture hasn't been used for 100 ms. That makes
	// its timing depend on the host; here it happens at every swap: overwritten surfaces read zero.
	static void ResetOverwrittenSurfaces()
	{
		for (auto& [key, img] : s.surfaces)
		{
			if (!img.image)
				continue;
			uint64 newest = 0;
			for (auto& [okey, other] : s.surfaces)
				if (&other != &img && other.image && okey.first < key.first + img.bytes && key.first < okey.first + other.bytes)
					newest = std::max(newest, other.written);
			if (newest <= img.written || newest <= img.resetFor)
				continue;
			img.resetFor = newest;
			timing::Scope span(timing::Kind::Reset, !timing::On() ? std::string() : fmt::format("reset {}x{} f{}{}", img.gw, img.gh,
				(int)img.format, img.layers > 1 ? fmt::format(" L{}", img.layers) : ""));
			if (DeferClear(img, VkClearValue{}))
				continue;
			Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkImageSubresourceRange all{ img.aspect, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS };
			if (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
			{
				VkClearDepthStencilValue zero{};
				vkCmdClearDepthStencilImage(s.cmd, img.image, img.layout, &zero, 1, &all);
			}
			else
			{
				VkClearColorValue zero{};
				vkCmdClearColorImage(s.cmd, img.image, img.layout, &zero, 1, &all);
			}
		}
	}

	// ---- debugging: WWHD_RENDER_DUMP=N writes every surface (every array layer) at frame N, into
	// CEMU_SHOT_DIR: colour as 8-bit PPM, depth and single-channel float as 16-bit PGM ------------
	static float HalfToFloat(uint16 h)
	{
		uint32 e = (h >> 10) & 0x1F, m = h & 0x3FF;
		float v = e == 0 ? m / 1024.0f / 16384.0f : std::ldexp(1.0f + m / 1024.0f, (int)e - 15);
		return (h & 0x8000) ? -v : v;
	}

	static float SmallFloat(uint32 bits, uint32 mantissaBits)      // unsigned 5-bit-exponent floats (B10G11R11)
	{
		uint32 e = bits >> mantissaBits, m = bits & ((1u << mantissaBits) - 1);
		float f = (float)m / (float)(1u << mantissaBits);
		return e == 0 ? std::ldexp(f, -14) : std::ldexp(1.0f + f, (int)e - 15);
	}

	static void DumpSurfaces(uint32 frame)
	{
		EndRendering();
		for (auto& [key, img] : s.surfaces)
		for (uint32 layer = 0; img.image && layer < img.layers; layer++)
		{
			bool depth = img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
			uint32 bpp;
			switch (img.format)
			{
			case VK_FORMAT_R8_UNORM: bpp = 1; break;
			case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_D16_UNORM: bpp = 2; break;
			case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break;
			default: bpp = 4; break;                                    // RGBA8, RGB10A2, B10G11R11, R32F, D32 (and D24S8/D32S8's depth)
			}
			VkDeviceSize size = (VkDeviceSize)img.width * img.height * bpp;
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
			Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			VkBufferImageCopy r{};
			r.imageSubresource = { depth ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : img.aspect, 0, layer, 1 };
			r.imageExtent = { img.width, img.height, 1 };
			vkCmdCopyImageToBuffer(s.cmd, img.image, img.layout, buf, 1, &r);
			HostReadBarrier();
			SubmitAndWait();
			uint8* px;
			vkMapMemory(s.device, mem, 0, size, 0, (void**)&px);
			size_t n = (size_t)img.width * img.height;
			std::vector<float> rgb(n * 3);
			for (size_t i = 0; i < n; i++)
			{
				const uint8* p = px + i * bpp;
				uint32 w = 0;
				memcpy(&w, p, std::min(bpp, 4u));
				float* o = &rgb[i * 3];
				switch (img.format)
				{
				case VK_FORMAT_R8_UNORM: o[0] = o[1] = o[2] = p[0] / 255.0f; break;
				case VK_FORMAT_R8G8_UNORM: o[0] = p[0] / 255.0f; o[1] = p[1] / 255.0f; o[2] = 0; break;
				case VK_FORMAT_R16_SFLOAT: o[0] = o[1] = o[2] = HalfToFloat((uint16)w); break;
				case VK_FORMAT_D16_UNORM: o[0] = o[1] = o[2] = (w & 0xFFFF) / 65535.0f; break;
				case VK_FORMAT_R16G16B16A16_SFLOAT:
					for (int c = 0; c < 3; c++)
						o[c] = HalfToFloat((uint16)(p[c * 2] | (p[c * 2 + 1] << 8)));
					break;
				case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
					o[0] = (w & 0x3FF) / 1023.0f; o[1] = ((w >> 10) & 0x3FF) / 1023.0f; o[2] = ((w >> 20) & 0x3FF) / 1023.0f; break;
				case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
					o[0] = SmallFloat(w & 0x7FF, 6); o[1] = SmallFloat((w >> 11) & 0x7FF, 6); o[2] = SmallFloat(w >> 22, 5); break;
				case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
					memcpy(&o[0], &w, 4); o[1] = o[2] = o[0]; break;
				case VK_FORMAT_D24_UNORM_S8_UINT: o[0] = o[1] = o[2] = (w & 0xFFFFFF) / 16777215.0f; break;
				default: o[0] = p[0] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[2] / 255.0f; break;
				}
			}
			// single-channel data (depth, R16/R32 float) as exact 16-bit grey (PGM, 0..1), the rest as
			// 8-bit RGB: the reference's texture dump does the same (cemu-patches/0012)
			bool grey = depth || img.format == VK_FORMAT_R32_SFLOAT || img.format == VK_FORMAT_R16_SFLOAT;
			std::vector<uint8> out(n * (grey ? 2 : 3));
			for (size_t i = 0; i < n; i++)
			{
				if (grey)
				{
					uint16 g = (uint16)std::clamp(rgb[i * 3] * 65535.0f + 0.5f, 0.0f, 65535.0f);
					out[i * 2] = (uint8)(g >> 8);
					out[i * 2 + 1] = (uint8)g;
				}
				else
					for (int c = 0; c < 3; c++)
						out[i * 3 + c] = (uint8)std::clamp(rgb[i * 3 + c] * 255.0f + 0.5f, 0.0f, 255.0f);
			}
			std::string path = fmt::format("{}/dump{:06}_{:08x}_{:x}{}_{}x{}_s{}.{}", s.shotDir, frame, key.first, key.second & 0xFFFF,
				depth ? "d" : "", img.width, img.height, layer, grey ? "pgm" : "ppm");
			if (FILE* f = fopen(path.c_str(), "wb"))
			{
				fprintf(f, grey ? "P5\n%u %u\n65535\n" : "P6\n%u %u\n255\n", img.width, img.height);
				fwrite(out.data(), 1, out.size(), f);
				fclose(f);
			}
			vkUnmapMemory(s.device, mem);
			vkDestroyBuffer(s.device, buf, nullptr);
			vkFreeMemory(s.device, mem, nullptr);
		}
		Log(fmt::format("dumped {} surfaces at frame {}", s.surfaces.size(), frame));
	}

	// ---- frame capture (as the reference's screenshots: an RGBA8 copy of the TV image) --------
	// WWHD_SHOT_DRC=1 writes the GamePad's image too (fNNNNNN.drc.ppm), though nothing shows it
	static void WritePPM(const Image& tv, uint32 frame, const char* screen = "tv")
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
		HostReadBarrier();
		SubmitAndWait();
		uint8* px;
		vkMapMemory(s.device, mem, 0, size, 0, (void**)&px);
		std::string path = fmt::format("{}/f{:06}.{}.ppm", s.shotDir, frame, screen);
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
		EndRendering();
		uint32 mask = p[0];
		// the span names what's cleared (WWHD_GPU_TIMING's top list): colour and/or depth, the sizes and layers
		timing::Scope span(timing::Kind::Clear, !timing::On() ? std::string() : fmt::format("clear{}{}{}",
			(mask & 1) && (uint32)p[1] ? fmt::format(" colour {}x{} f{:x} L{}", std::max<uint32>(p[4], p[6]), (uint32)p[5], (uint32)p[2],
				std::max<uint32>(p[8], 1)) : "",
			(mask & 6) && (uint32)p[9] ? fmt::format(" depth{} {}x{} L{}", (mask & 4) ? "+stencil" : "", std::max<uint32>(p[12], p[14]),
				(uint32)p[13], std::max<uint32>(p[16], 1)) : "",
			(mask & 2) && !(mask & 4) && (uint32)p[9] ? " (+colour aliases)" : ""));
		if ((mask & 1) && (uint32)p[1])
		{
			uint32 w = std::max<uint32>(p[4], p[6]), h = p[5], first = p[7], count = std::max<uint32>(p[8], 1);
			Image& img = Surface(p[1], p[2], false, w, h, first + count);
			img.written = ++s.writes;
			VkClearValue cv{};
			VkClearColorValue& c = cv.color;
			c.float32[0] = (float)(uint32)p[17] / 255.0f;
			c.float32[1] = (float)(uint32)p[18] / 255.0f;
			c.float32[2] = (float)(uint32)p[19] / 255.0f;
			c.float32[3] = (float)(uint32)p[20] / 255.0f;
			if (first != 0 || count != 1 || !DeferClear(img, cv))
			{
				Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, first, count };
				vkCmdClearColorImage(s.cmd, img.image, img.layout, &c, 1, &range);
			}
		}
		if ((mask & 6) && (uint32)p[9])
		{
			uint32 w = std::max<uint32>(p[12], p[14]), h = p[13], first = p[15], count = std::max<uint32>(p[16], 1);
			Image& img = Surface(p[9], p[10], true, w, h, first + count);
			img.written = ++s.writes;
			VkClearDepthStencilValue v;
			uint32 depthBits = p[21];
			memcpy(&v.depth, &depthBits, 4);
			v.stencil = p[22];
			VkImageAspectFlags aspect = ((mask & 2) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
				((mask & 4) && (img.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
			VkClearValue dv{};
			dv.depthStencil = v;
			if (aspect == img.aspect && first == 0 && count == 1 && DeferClear(img, dv))
				aspect = 0;                                        // the next pass's load op
			else
				Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			if (aspect)
			{
				VkImageSubresourceRange range{ aspect, 0, 1, first, count };
				vkCmdClearDepthStencilImage(s.cmd, img.image, img.layout, &v, 1, &range);
			}
			// the reference's depth clear also clears the color textures that start at the address and are
			// no wider, to the depth value (LatteRenderTarget_applyTextureDepthClear), unless it clears stencil
			if ((mask & 2) && !(mask & 4))
				for (auto it = s.surfaces.lower_bound({ (uint32)p[9], 0 }); it != s.surfaces.end() && it->first.first == (uint32)p[9]; ++it)
				{
					Image& c = it->second;
					if (!c.image || (c.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || c.gw > w || first >= c.layers)
						continue;
					c.written = ++s.writes;
					VkClearValue value{};
					value.color = { { v.depth, v.depth, v.depth, v.depth } };
					if (first == 0 && DeferClear(c, value))
						continue;
					Transition(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
					VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, first, std::min(count, c.layers - first) };
					vkCmdClearColorImage(s.cmd, c.image, c.layout, &value.color, 1, &range);
				}
		}
	}

	// IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER: address, width, height, pitch, tile mode, swizzle, slice,
	// format, scan target (1 TV, 4 DRC)
	void RendererCopyToScanBuffer(const uint32be* p, uint32 nWords)
	{
		if (nWords < 9)
			return;
		EndRendering();
		timing::Scope span(timing::Kind::Scan);
		uint32 addr = p[0], w = p[1], h = p[2], pitch = p[3], gx2 = p[7], target = p[8];
		static const bool trace = getenv("WWHD_RENDER_TRACE") != nullptr;
		if (trace)
			Log(fmt::format("copy to scan buffer {} in frame {}: {:08x} {}x{} fmt {:x}", target, s.frame + 1, addr, w, h, gx2));
		Image& src = Surface(addr, gx2, false, std::max(w, pitch), h);
		SurfaceRead(src, h);
		if (target == 1 && motion::Debug())                         // WWHD_MOTION=debug: the motion over the TV image
			motion::DrawDebug(src);
		if (target == 1 && (fsr1::On() || fsr3::On()))             // the TV surface (the upscalers' HUD split)
		{
			s_tvKey = KeyOf(src), s_tvKnown = true;
			if (!s_hudActive)
				HudBegins(true);                                    // no HUD split this frame: upscaled here
		}
		Image& dst = s.scan[target == 1 ? 0 : 1];
		// the reference's screenshot is an RGBA8 blit of this buffer, sRGB if the scan buffer is
		VkFormat f = (target == 1 ? LatteGPUState.tvBufferUsesSRGB : LatteGPUState.drcBufferUsesSRGB)
			? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		if (!dst.image || dst.gw != w || dst.gh != h || dst.format != f || dst.scale != src.scale)   // at its surface's scale too
		{
			if (dst.image)
			{
				WaitPending();                                      // the other frame may still present from it
				DestroyImage(dst);
			}
			dst = CreateImage(f, VK_IMAGE_ASPECT_COLOR_BIT, ScaledBy(w, src.scale), ScaledBy(h, src.scale), 0);
			dst.gw = w, dst.gh = h, dst.scaled = src.scaled, dst.scale = src.scale;
		}
		Transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		Transition(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageBlit b{};
		b.srcSubresource = b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		b.srcOffsets[1] = { (sint32)dst.width, (sint32)dst.height, 1 };
		b.dstOffsets[1] = { (sint32)dst.width, (sint32)dst.height, 1 };
		vkCmdBlitImage(s.cmd, src.image, src.layout, dst.image, dst.layout, 1, &b, VK_FILTER_NEAREST);
	}

	// IT_HLE_TRIGGER_SCANBUFFER_SWAP: one per GX2SwapScanBuffers, so frame N is the Nth swap. Shot N
	// is the image the next swap presents, as in the reference (renderer.h).
	void RendererSwap()
	{
		EndRendering();
		s.frame++;
		DrawStats(s.frame);
		static const uint32 dumpFrame = [] { const char* e = getenv("WWHD_RENDER_DUMP"); return e ? (uint32)atoi(e) : 0u; }();
		if (dumpFrame && s.frame == dumpFrame)
			DumpSurfaces(s.frame);
		if (s.frame > 1 && s.shotFrames.count(s.frame - 1) && s.scan[0].image)
			WritePPM(s.scan[0], s.frame - 1);
		static const bool shotDrc = getenv("WWHD_SHOT_DRC") && atoi(getenv("WWHD_SHOT_DRC")) != 0;
		if (shotDrc && s.frame > 1 && s.shotFrames.count(s.frame - 1) && s.scan[1].image)
			WritePPM(s.scan[1], s.frame - 1, "drc");
		HudEnds();
		if (motion::On())
			motion::FrameEnd(s.frame);
		FitSurfaces();
		ChooseScale();
		ResetOverwrittenSurfaces();
		{
			timing::Scope span(timing::Kind::Present);
			PresentRecord(s.scan[0]);
		}
		timing::Frame();
		SubmitFrame();
		PresentQueue();
		cache::SaveNowAndThen();
	}

	void SaveShaderCache()
	{
		if (RendererOn())
			cache::Save();
	}
}
