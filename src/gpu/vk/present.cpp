// Presenting the TV image to the frontend's window (renderer.h, Window).
//
// At every swap the scan buffer is blitted, scaled to fit and centred, into the next swapchain image,
// inside the swap's command buffer; the image is presented once that has finished. The swapchain
// takes the scan buffer's encoding (sRGB or not) so the blit keeps its bytes, which are the bytes the
// reference's screenshot has. It is rebuilt when the window's size or that encoding changes, or
// when the presentation engine says it is out of date. Mailbox presentation when the device has it
// (never waits, never tears), FIFO otherwise; WWHD_VSYNC=1 asks for FIFO, and so does WWHD_VRR=1 (variable refresh:
// the display's refresh follows the presents; window_system.cpp asks the display system for it). The overlay (renderer.h)
// is blitted over it, uploaded again whenever the frontend changes it, and the FPS overlay's panel at the picture's
// bottom left (after the upscaler and frame generation, in the window's own pixels).
#include "renderer_internal.h"
#include "../../os/settings.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>

namespace wwhd::gpu
{
	namespace
	{
		Window s_window;
		bool s_hasWindow = false;
		std::atomic<bool> s_rebuild{ false };                    // RebuildSwapchain: at the next acquire

		struct Swapchain
		{
			VkSurfaceKHR surface = VK_NULL_HANDLE;
			VkSwapchainKHR chain = VK_NULL_HANDLE;
			std::vector<VkImage> images;
			VkFormat format = VK_FORMAT_UNDEFINED;
			bool srgb = false;
			uint32 width = 0, height = 0;
			VkFence acquired = VK_NULL_HANDLE;
			uint32 index = 0;
			bool pending = false;                                // an image was acquired and drawn this swap
			std::vector<VkSemaphore> rendered;                   // per image: the lazy path's swap submit signals it
			bool signalled = false;                              // this swap's present waits for rendered[index]
			uint32 first = UINT32_MAX;                           // frame generation: the frame between, presented before index
		} w;

		// a panel laid over the picture: the frontend's pixels, uploaded to its image when they change
		struct Panel
		{
			std::mutex lock;
			sint32 x = 0, y = 0;
			uint32 w = 0, h = 0;
			std::vector<uint32> rgba;
			uint32 version = 0;
			Image image;
			uint32 uploaded = 0;                // the version in image
		};
		Panel s_overlay;                        // the system's screens, in the TV image's 1920x1080 frame (SetOverlay)
		Panel s_fps;                            // the FPS overlay, in the window's pixels (SetFpsPanel)
		std::atomic<double> s_frameX{ 0 }, s_frameY{ 0 }, s_frameScale{ 0 };   // where the 1920x1080 frame was last drawn

		std::atomic<uint64> s_shown{ 0 };       // frames presented (ShownFrames)
		std::atomic<uint64> s_longestNs{ 0 };   // the longest interval between two since TakeLongestShownInterval
		std::chrono::steady_clock::time_point s_lastShown;

		bool IsSrgb(VkFormat f)
		{
			return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB;
		}

		// WWHD_PRESENT_PROBE=1 (a test, q-deckblack; tools/sixty/tests/blacktest.sh): at every present, with a window or
		// without, an 8x8 grid of the TV image's pixels (the scan buffer) copied out and read back two swaps later (its submit
		// done: the lazy path's slot waited for); a black one logged with its frame, the game's swap it presents (gx2's
		// NoteGuestSwap) and the time since the present before, and a summary every 600 presents.
		// WWHD_PRESENT_PROBE_OUT=path: a line a present there, "SWAP FRAME BLACK" (1 black, 0 not). =2: the window image's
		// grid too, after the blit and the overlay (the swapchain then made with TRANSFER_SRC)
		namespace probe
		{
			constexpr uint32 kGrid = 8, kPoints = kGrid * kGrid, kSlots = 8, kStride = 16;
			constexpr VkDeviceSize kSlotBytes = 2 * kPoints * kStride;
			int Mode()
			{
				static const int m = [] { const char* e = getenv("WWHD_PRESENT_PROBE"); return e ? atoi(e) : 0; }();
				return m;
			}
			VkBuffer s_buf = VK_NULL_HANDLE;
			VkDeviceMemory s_mem = VK_NULL_HANDLE;
			const uint8* s_data = nullptr;
			FILE* s_out = nullptr;
			struct Rec { uint32 frame = 0, guest = 0, image = 0; bool window = false; double since = 0; };
			Rec s_recs[kSlots];
			uint32 s_presents = 0, s_scanBlack = 0, s_windowBlack = 0, s_logged = 0, s_from = 0, s_alphaMin = 255, s_alphaMax = 0;
			std::chrono::steady_clock::time_point s_last{};
			bool s_swapSrc = false;                                  // the swapchain was made with TRANSFER_SRC
			std::mutex s_guestLock;
			std::deque<uint32> s_guest;                              // the swaps gx2 sent, not presented yet

			bool Eight(VkFormat f)
			{
				return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
			}

			void Copy(VkImage image, VkImageLayout layout, sint32 x0, sint32 y0, sint32 cw, sint32 ch, VkDeviceSize at)
			{
				VkBufferImageCopy r[kPoints];
				for (uint32 i = 0; i < kPoints; i++)
				{
					r[i] = {};
					r[i].bufferOffset = at + i * kStride;
					r[i].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
					r[i].imageOffset = { x0 + (sint32)(((i % kGrid) * 2 + 1) * cw / (2 * kGrid)), y0 + (sint32)(((i / kGrid) * 2 + 1) * ch / (2 * kGrid)), 0 };
					r[i].imageExtent = { 1, 1, 1 };
				}
				vkCmdCopyImageToBuffer(s.cmd, image, layout, s_buf, kPoints, r);
			}

			// black: the grid's middle 4x4 (the scene; the HUD at its edges can stand over a black world, B80)
			bool Black(const uint8* p)
			{
				for (uint32 i = 0; i < kPoints; i++)
				{
					const uint32 x = i % kGrid, y = i / kGrid;
					if (x < 2 || x > 5 || y < 2 || y > 5)
						continue;
					if (p[i * kStride] > 4 || p[i * kStride + 1] > 4 || p[i * kStride + 2] > 4)
						return false;
				}
				return true;
			}

			// the records whose submit is done (two swaps back), counted and the black ones logged
			void Read()
			{
				for (Rec& r : s_recs)
				{
					if (!r.frame || r.frame + 2 > s.frame)
						continue;
					const uint8* p = s_data + (r.frame % kSlots) * kSlotBytes;
					const bool scanBlack = Black(p), windowBlack = r.window && Black(p + kPoints * kStride);
					if (r.window)
						for (uint32 i = 0; i < kPoints; i++)
							s_alphaMin = std::min<uint32>(s_alphaMin, p[kPoints * kStride + i * kStride + 3]),
							s_alphaMax = std::max<uint32>(s_alphaMax, p[kPoints * kStride + i * kStride + 3]);
					if (s_out)
						fprintf(s_out, "%u %u %d\n", r.guest, r.frame, scanBlack ? 1 : 0);
					if (!s_presents)
						s_from = r.frame;
					s_presents++, s_scanBlack += scanBlack, s_windowBlack += windowBlack;
					if ((scanBlack || windowBlack) && s_logged < 300)
					{
						s_logged++;
						Log(fmt::format("present probe: frame {} (swap {}) black: the scan buffer {}, the window image {}; image {}, {:.1f} ms after "
							"the present before", r.frame, r.guest, scanBlack ? "black" : "not", r.window ? (windowBlack ? "black" : "not") : "not sampled",
							r.image, r.since));
					}
					if (s_presents >= 600)
					{
						Log(fmt::format("present probe: frames {}-{}: {} presents, {} with a black scan buffer, {} with a black window image{}", s_from,
							r.frame, s_presents, s_scanBlack, s_windowBlack, s_swapSrc ? fmt::format(" (its alpha {}..{})", s_alphaMin, s_alphaMax) : ""));
						s_presents = s_scanBlack = s_windowBlack = 0, s_alphaMin = 255, s_alphaMax = 0;
					}
					r.frame = 0;
				}
			}

			// at each present, before the window (headless too): the scan buffer's grid; false: not probing
			bool Begin(Image& scan)
			{
				if (!Mode() || !scan.image)
					return false;
				if (!s_buf)
				{
					if (!Eight(scan.format))
					{
						static bool once = (Log(fmt::format("present probe: the scan buffer's format {} isn't 8 bits a channel: off", (int)scan.format)), true);
						(void)once;
						return false;
					}
					VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
					bci.size = kSlots * kSlotBytes;
					bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
					Check(vkCreateBuffer(s.device, &bci, nullptr, &s_buf), "vkCreateBuffer");
					VkMemoryRequirements req;
					vkGetBufferMemoryRequirements(s.device, s_buf, &req);
					VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
					ai.allocationSize = req.size;
					ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
					Check(vkAllocateMemory(s.device, &ai, nullptr, &s_mem), "vkAllocateMemory");
					vkBindBufferMemory(s.device, s_buf, s_mem, 0);
					void* mapped = nullptr;
					Check(vkMapMemory(s.device, s_mem, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
					s_data = (const uint8*)mapped;
					if (const char* out = getenv("WWHD_PRESENT_PROBE_OUT"); out && *out && (s_out = fopen(out, "w")))
					{
						atexit([] { fflush(s_out); });
						at_quick_exit([] { fflush(s_out); });
					}
					Log(fmt::format("present probe: on (WWHD_PRESENT_PROBE={}), the scan buffer {}x{} format {}{}", Mode(), scan.width, scan.height,
						(int)scan.format, s_swapSrc ? ", the window image too" : ""));
				}
				Read();
				const auto now = std::chrono::steady_clock::now();
				Rec& r = s_recs[s.frame % kSlots];
				r = {};
				r.frame = s.frame;
				{
					std::lock_guard lock(s_guestLock);
					if (!s_guest.empty())
						r.guest = s_guest.front(), s_guest.pop_front();
				}
				r.since = s_last == std::chrono::steady_clock::time_point{} ? 0.0 : std::chrono::duration<double, std::milli>(now - s_last).count();
				s_last = now;
				Transition(scan, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				Copy(scan.image, scan.layout, 0, 0, (sint32)scan.width, (sint32)scan.height, (s.frame % kSlots) * kSlotBytes);
				HostReadBarrier();
				return true;
			}

			// =2, after the blit and the overlay (the window image in TRANSFER_DST): its grid within the picture
			void End(VkImage target, sint32 x, sint32 y, sint32 dw, sint32 dh)
			{
				s_recs[s.frame % kSlots].image = w.index;
				if (!s_swapSrc)
					return;
				VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
				b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
				b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = target;
				b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
				Copy(target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, x, y, dw, dh, (s.frame % kSlots) * kSlotBytes + kPoints * kStride);
				std::swap(b.oldLayout, b.newLayout);
				vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
				s_recs[s.frame % kSlots].window = true;
				HostReadBarrier();
			}
		}

		void Barrier(VkImage image, VkImageLayout from, VkImageLayout to)
		{
			VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			b.oldLayout = from;
			b.newLayout = to;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = image;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
				0, nullptr, 1, &b);
		}

		VkPresentModeKHR PresentMode()
		{
			uint32 n = 0;
			vkGetPhysicalDeviceSurfacePresentModesKHR(s.physical, w.surface, &n, nullptr);
			std::vector<VkPresentModeKHR> modes(n);
			vkGetPhysicalDeviceSurfacePresentModesKHR(s.physical, w.surface, &n, modes.data());
			const char* vsync = getenv("WWHD_VSYNC");
			const char* vrr = getenv("WWHD_VRR");                  // variable refresh: FIFO, the refresh follows the presents
			// frame generation: FIFO paces its two presents a swap
			if (!(vsync && *vsync == '1') && !(vrr && *vrr == '1') && !FrameGenPresents())
				for (VkPresentModeKHR m : modes)
					if (m == VK_PRESENT_MODE_MAILBOX_KHR)
						return m;
			return VK_PRESENT_MODE_FIFO_KHR;                       // every device has it
		}

		// (Re)build the swapchain for the window's current size and the given encoding. False when the
		// window has no area (minimised).
		bool Build(uint32 width, uint32 height, bool srgb)
		{
			VkSurfaceCapabilitiesKHR caps;
			Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.physical, w.surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
			VkExtent2D extent = caps.currentExtent;
			if (extent.width == UINT32_MAX)
				extent = { std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
					std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height) };
			if (extent.width == 0 || extent.height == 0)
				return false;

			uint32 n = 0;
			vkGetPhysicalDeviceSurfaceFormatsKHR(s.physical, w.surface, &n, nullptr);
			std::vector<VkSurfaceFormatKHR> formats(n);
			vkGetPhysicalDeviceSurfaceFormatsKHR(s.physical, w.surface, &n, formats.data());
			VkSurfaceFormatKHR chosen = formats.at(0);
			for (const VkSurfaceFormatKHR& f : formats)
				if (IsSrgb(f.format) == srgb && (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM ||
					IsSrgb(f.format)))
				{
					chosen = f;
					break;
				}

			VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
			ci.surface = w.surface;
			const uint32 extra = FrameGenPresents() ? 2 : 1;        // frame generation holds two images at once
			ci.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + extra, caps.maxImageCount) : caps.minImageCount + extra;
			ci.imageFormat = chosen.format;
			ci.imageColorSpace = chosen.colorSpace;
			ci.imageExtent = extent;
			ci.imageArrayLayers = 1;
			probe::s_swapSrc = probe::Mode() >= 2 && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (probe::s_swapSrc ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
			ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			ci.preTransform = caps.currentTransform;
			ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
				: (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
			ci.presentMode = PresentMode();
			ci.clipped = VK_TRUE;
			ci.oldSwapchain = w.chain;
			VkSwapchainKHR chain;
			Check(vkCreateSwapchainKHR(s.device, &ci, nullptr, &chain), "vkCreateSwapchainKHR");
			// the lazy path's frames in flight may still draw into the old images and its presents wait for their
			// semaphores: the queue idle first (a rebuild is rare: a resize)
			vkQueueWaitIdle(s.queue);
			WaitPending();
			for (VkSemaphore sem : w.rendered)
				vkDestroySemaphore(s.device, sem, nullptr);
			w.rendered.clear();
			if (w.chain)
				vkDestroySwapchainKHR(s.device, w.chain, nullptr);  // nothing uses it now
			w.chain = chain;
			vkGetSwapchainImagesKHR(s.device, w.chain, &n, nullptr);
			w.images.resize(n);
			vkGetSwapchainImagesKHR(s.device, w.chain, &n, w.images.data());
			w.format = chosen.format;
			w.srgb = srgb;
			w.width = extent.width;
			w.height = extent.height;
			Log(fmt::format("window: {}x{}, {} images, format {}, {}", w.width, w.height, n, (int)w.format,
				ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : getenv("WWHD_VRR") && *getenv("WWHD_VRR") == '1' ?
				"fifo (variable refresh)" : "fifo"));
			return true;
		}
	}

	static void SetPanel(Panel& p, sint32 x, sint32 y, uint32 w, uint32 h, std::vector<uint32> rgba)
	{
		std::lock_guard lock(p.lock);
		p.x = x;
		p.y = y;
		p.w = rgba.empty() ? 0 : w;
		p.h = rgba.empty() ? 0 : h;
		p.rgba = std::move(rgba);
		p.version++;
	}

	void SetOverlay(sint32 x, sint32 y, uint32 w, uint32 h, std::vector<uint32> rgba)
	{
		SetPanel(s_overlay, x, y, w, h, std::move(rgba));
	}

	void SetFpsPanel(uint32 w, uint32 h, std::vector<uint32> rgba)
	{
		SetPanel(s_fps, 0, 0, w, h, std::move(rgba));
	}

	uint64 ShownFrames() { return s_shown.load(std::memory_order_relaxed); }

	double TakeLongestShownInterval() { return s_longestNs.exchange(0, std::memory_order_relaxed) / 1e6; }

	// a panel into the swapchain image (in TRANSFER_DST layout): place(w, h, x0, y0, x1, y1) gives its rectangle there
	template<typename Place>
	static void DrawPanel(Panel& p, VkImage target, VkFilter filter, Place place)
	{
		std::unique_lock lock(p.lock);
		if (!p.w)
			return;
		VkFormat format = w.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;   // keep its bytes, like the TV image
		Image& img = p.image;
		if (!img.image || img.width != p.w || img.height != p.h || img.format != format)
		{
			if (img.image)
			{
				WaitPending();                                   // the lazy path: the last frame's submit may still blit it
				DestroyImage(img);
			}
			img = CreateImage(format, VK_IMAGE_ASPECT_COLOR_BIT, p.w, p.h, 0);
			p.uploaded = 0;
		}
		if (p.uploaded != p.version)
		{
			VkDeviceSize bytes = (VkDeviceSize)p.w * p.h * 4;
			VkDeviceSize at = RingAlloc(bytes, 16);
			memcpy(s.ring.data + at, p.rgba.data(), bytes);
			Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkBufferImageCopy c{};
			c.bufferOffset = at;
			c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			c.imageExtent = { p.w, p.h, 1 };
			vkCmdCopyBufferToImage(s.cmd, s.ring.buffer, img.image, img.layout, 1, &c);
			p.uploaded = p.version;
		}
		sint32 x0, y0, x1, y1;
		place(p, x0, y0, x1, y1);
		lock.unlock();
		if (x1 <= x0 || y1 <= y0)
			return;
		Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkImageBlit b{};
		b.srcSubresource = b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		b.srcOffsets[1] = { (sint32)img.width, (sint32)img.height, 1 };
		b.dstOffsets[0] = { x0, y0, 0 };
		b.dstOffsets[1] = { x1, y1, 1 };
		vkCmdBlitImage(s.cmd, img.image, img.layout, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, filter);
	}

	bool OverlayFrame(double& x, double& y, double& scale)
	{
		if (s_frameScale <= 0)
			return false;
		x = s_frameX, y = s_frameY, scale = s_frameScale;
		return true;
	}

	// the overlay in the 1920x1080 frame at (tvX, tvY) and that scale in the swapchain image
	static void DrawOverlay(VkImage target, sint32 tvX, sint32 tvY, double scale)
	{
		s_frameX = tvX, s_frameY = tvY, s_frameScale = scale;
		DrawPanel(s_overlay, target, VK_FILTER_LINEAR, [&](const Panel& p, sint32& x0, sint32& y0, sint32& x1, sint32& y1) {
			x0 = tvX + (sint32)(p.x * scale), y0 = tvY + (sint32)(p.y * scale);
			x1 = tvX + (sint32)((p.x + (sint32)p.w) * scale), y1 = tvY + (sint32)((p.y + (sint32)p.h) * scale);
		});
	}

	// the FPS overlay at the bottom left of the picture (x, y, dw x dh in the swapchain image), its pixels scaled by a
	// whole number that follows the picture's height (8-pixel glyphs: 24 pixels high at 1080, 48 at 2160), so they stay sharp
	static void DrawFps(VkImage target, sint32 x, sint32 y, sint32 dw, sint32 dh)
	{
		DrawPanel(s_fps, target, VK_FILTER_NEAREST, [&](const Panel& p, sint32& x0, sint32& y0, sint32& x1, sint32& y1) {
			const sint32 k = std::max(1, (sint32)std::lround(dh / 360.0)), margin = std::max(4, dh / 60);
			x0 = x + margin, y1 = y + dh - margin;
			x1 = std::min(x + dw, x0 + (sint32)p.w * k), y0 = std::max(y, y1 - (sint32)p.h * k);
		});
	}

	void SetWindow(Window window)
	{
		s_window = std::move(window);
		s_hasWindow = true;
	}

	bool HasWindow() { return s_hasWindow; }

	void RebuildSwapchain() { s_rebuild = true; }

	std::vector<const char*> WindowInstanceExtensions()
	{
		return s_hasWindow ? s_window.instanceExtensions : std::vector<const char*>{};
	}

	void CreateWindowSurface()
	{
		if (!s_hasWindow)
			return;
		w.surface = (VkSurfaceKHR)s_window.createSurface(s.instance);
		if (!w.surface)
			Fail("cannot create a Vulkan surface for the window");
	}

	bool CanPresent(uint32 queueFamily)
	{
		if (!s_hasWindow)
			return true;
		VkBool32 ok = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(s.physical, queueFamily, w.surface, &ok);
		return ok;
	}

	// the next swapchain image (w.index, w.pending), the swapchain rebuilt first if needed
	// rebuild: false for frame generation's second image of a swap: a rebuild would destroy its first, which the swap's
	// commands already draw into (the swapchain is then rebuilt at the next swap)
	static bool Acquire(bool srgb, bool rebuild = true)
	{
		w.pending = false;
		if (!w.acquired)
		{
			VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			Check(vkCreateFence(s.device, &fci, nullptr, &w.acquired), "vkCreateFence");
		}
		uint32 width = 0, height = 0;
		s_window.size(width, height);
		for (int attempt = 0; attempt < 2; attempt++)
		{
			if (!w.chain || width != w.width || height != w.height || srgb != w.srgb || attempt > 0 || (rebuild && s_rebuild.exchange(false)))
			{
				if (!rebuild && w.chain)
				{
					w.width = 0;                                    // at the next swap
					return false;
				}
				if (!Build(width, height, srgb))
					return false;
			}
			VkResult r = vkAcquireNextImageKHR(s.device, w.chain, UINT64_MAX, VK_NULL_HANDLE, w.acquired, &w.index);
			if (r == VK_ERROR_OUT_OF_DATE_KHR)
			{
				if (!rebuild)
				{
					w.width = 0;
					return false;
				}
				continue;
			}
			if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
				Check(r, "vkAcquireNextImageKHR");
			Check(vkWaitForFences(s.device, 1, &w.acquired, VK_TRUE, UINT64_MAX), "vkWaitForFences");
			vkResetFences(s.device, 1, &w.acquired);
			w.pending = true;
			break;
		}
		return w.pending;
	}

	// the image scaled to fit and centred in the acquired window image, the overlay over it, ready to present
	static void Draw(Image& scan, bool probing = false)
	{
		VkImage target = w.images[w.index];
		Barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkClearColorValue black{};
		VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(s.cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		// the largest centred rectangle with the scan buffer's aspect ratio
		double scale = std::min((double)w.width / scan.width, (double)w.height / scan.height);
		sint32 dw = std::max(1, (sint32)(scan.width * scale)), dh = std::max(1, (sint32)(scan.height * scale));
		sint32 x = ((sint32)w.width - dw) / 2, y = ((sint32)w.height - dh) / 2;
		Transition(scan, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkImageBlit b{};
		b.srcSubresource = b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		b.srcOffsets[1] = { (sint32)scan.width, (sint32)scan.height, 1 };
		b.dstOffsets[0] = { x, y, 0 };
		b.dstOffsets[1] = { x + dw, y + dh, 1 };
		vkCmdBlitImage(s.cmd, scan.image, scan.layout, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
		// the overlay's 1920x1080 frame fitted and centred in the picture (another aspect's picture is wider or taller)
		const double fs = std::min(dw / 1920.0, dh / 1080.0);
		DrawOverlay(target, x + (dw - (sint32)(1920 * fs)) / 2, y + (dh - (sint32)(1080 * fs)) / 2, fs);
		DrawFps(target, x, y, dw, dh);
		if (probing)
			probe::End(target, x, y, dw, dh);
		Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}

	void NoteGuestSwap(uint32 swap)
	{
		if (!probe::Mode())
			return;
		std::lock_guard lock(probe::s_guestLock);
		if (probe::s_guest.size() < 4096)                         // nothing takes them without the renderer
			probe::s_guest.push_back(swap);
	}

	void PresentRecord(Image& scan, Image* between)
	{
		w.pending = false;
		w.first = UINT32_MAX;
		const bool probing = probe::Begin(scan);                   // WWHD_PRESENT_PROBE: the scan buffer's grid, headless too
		if (!s_hasWindow || !scan.image)
			return;
		if (between && Acquire(IsSrgb(scan.format)))              // frame generation: the frame between, presented first
		{
			Draw(*between);
			w.first = w.index;
			if (!Acquire(IsSrgb(scan.format), false))               // no second image this swap (a resize): the first alone
			{
				w.index = w.first, w.first = UINT32_MAX, w.pending = true;
				return;
			}
		}
		else if (!Acquire(IsSrgb(scan.format)))
			return;
		Draw(scan, probing);
	}

	bool WindowExtent(uint32& width, uint32& height)
	{
		if (!s_hasWindow)
			return false;
		s_window.size(width, height);
		return width && height;
	}

	// Frame generation presents twice a swap, so FIFO shows its frame and the game's one refresh apart: it takes a
	// display at (about) twice the game's frame rate, or the game would wait for it. WWHD_FRAMEGEN=force skips the check
	// (tests). Decided once, at the first present with a window.
	bool FrameGenPresents()
	{
		static int s_decided = -1;
		if (s_decided >= 0)
			return s_decided != 0;
		if (!s_hasWindow || !fsr3::FrameGenWanted())
		{
			const char* up = getenv("WWHD_FRAMEGEN");
			if (s_hasWindow && up && *up == '1')                       // asked for without the FSR 3 upscaler
				os::settings::SetNote("WWHD_FRAMEGEN", "off: it needs the FSR 3 upscaler");
			return s_decided = 0, false;
		}
		const char* e = getenv("WWHD_FRAMEGEN");
		const char* cap = getenv("WWHD_FPS_CAP");
		const char* sixty = getenv("WWHD_60FPS");
		const float rate = cap && atoi(cap) > 0 ? (float)atoi(cap) : sixty && *sixty == '1' ? 60.0f : 30.0f;
		const float hz = s_window.refresh ? s_window.refresh() : 0.0f;
		const bool force = e && strcmp(e, "force") == 0;
		s_decided = force || hz >= 2 * rate * 0.95f;
		if (!s_decided)                                           // the settings page says why, not only the log
			os::settings::SetNote("WWHD_FRAMEGEN", hz > 0 ? fmt::format("off: the display runs at {:.0f} Hz, under twice the game's {:.0f}",
				hz, rate) : "off: the display's refresh rate is unknown");
		Log(s_decided ? fmt::format("window: frame generation: {} frames shown a second from the game's {} (the display at {:.0f} Hz){}",
				2 * rate, rate, hz, force ? ", forced" : "")
			: fmt::format("window: frame generation off: the display runs at {:.0f} Hz, under twice the game's {} frames a second", hz, rate));
		return s_decided != 0;
	}

	// before the game starts (design D20): the overlay on black, in a 1920x1080 frame fitted to the
	// window as the TV image is
	void PresentOverlayOnly()
	{
		if (!RendererOn() || !s_hasWindow || !Acquire(w.chain ? w.srgb : false))
			return;
		VkImage target = w.images[w.index];
		Barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkClearColorValue black{};
		VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(s.cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		double scale = std::min(w.width / 1920.0, w.height / 1080.0);
		DrawOverlay(target, ((sint32)w.width - (sint32)(1920 * scale)) / 2, ((sint32)w.height - (sint32)(1080 * scale)) / 2, scale);
		Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		SubmitAndWait();
		PresentQueue();
	}

	// the semaphore the swap's submit signals and its present waits for (the lazy path: the submit isn't waited for);
	// one per image: an image is acquired again (its fence waited for) only once its last present is done with it
	uint32 PresentSemaphores(VkSemaphore (&out)[2])
	{
		if (!w.pending)
			return 0;
		if (w.rendered.size() != w.images.size())
			w.rendered.resize(w.images.size(), VK_NULL_HANDLE);
		uint32 n = 0;
		for (uint32 index : { w.first, w.index })
		{
			if (index == UINT32_MAX)
				continue;
			VkSemaphore& sem = w.rendered[index];
			if (!sem)
			{
				VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
				Check(vkCreateSemaphore(s.device, &sci, nullptr, &sem), "vkCreateSemaphore");
			}
			out[n++] = sem;
		}
		w.signalled = true;
		return n;
	}

	void PresentQueue()
	{
		if (!w.pending)
			return;
		w.pending = false;
		const bool signalled = w.signalled;
		w.signalled = false;
		for (uint32 index : { w.first, w.index })                 // frame generation's frame first
		{
			if (index == UINT32_MAX)
				continue;
			VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
			pi.swapchainCount = 1;
			pi.pSwapchains = &w.chain;
			pi.pImageIndices = &index;
			if (signalled)                                        // the lazy path: the submit may not have finished
			{
				pi.waitSemaphoreCount = 1;
				pi.pWaitSemaphores = &w.rendered[index];
			}
			VkResult r = vkQueuePresentKHR(s.queue, &pi);         // otherwise the submit has finished: nothing to wait for
			if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
				w.width = 0;                                      // rebuild at the next swap
			else
				Check(r, "vkQueuePresentKHR");
			// the FPS overlay's count: every frame shown, frame generation's too
			const auto now = std::chrono::steady_clock::now();
			if (s_shown.fetch_add(1, std::memory_order_relaxed))
			{
				const uint64 ns = (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(now - s_lastShown).count();
				if (ns > s_longestNs.load(std::memory_order_relaxed))
					s_longestNs.store(ns, std::memory_order_relaxed);
			}
			s_lastShown = now;
		}
		w.first = UINT32_MAX;
	}
}
