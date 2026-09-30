// Presenting the TV image to the frontend's window (renderer.h, Window).
//
// At every swap the scan buffer is blitted, scaled to fit and centred, into the next swapchain image,
// inside the swap's command buffer; the image is presented once that has finished. The swapchain
// takes the scan buffer's encoding (sRGB or not) so the blit keeps its bytes, which are the bytes the
// reference's screenshot has. It is rebuilt when the window's size or that encoding changes, or
// when the presentation engine says it is out of date. Mailbox presentation when the device has it
// (never waits, never tears), FIFO otherwise; WWHD_VSYNC=1 asks for FIFO. The overlay (renderer.h)
// is blitted over it, uploaded again whenever the frontend changes it.
#include "renderer_internal.h"

namespace wwhd::gpu
{
	namespace
	{
		Window s_window;
		bool s_hasWindow = false;

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
		} w;

		struct OverlayPixels
		{
			sint32 x = 0, y = 0;
			uint32 w = 0, h = 0;
			std::vector<uint32> rgba;
			uint32 version = 0;
		};
		std::mutex s_overlayLock;
		OverlayPixels s_overlay;
		Image s_overlayImage;
		uint32 s_overlayUploaded = 0;           // the version in s_overlayImage

		bool IsSrgb(VkFormat f)
		{
			return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB;
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
			if (!(vsync && *vsync == '1'))
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
			ci.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount) : caps.minImageCount + 1;
			ci.imageFormat = chosen.format;
			ci.imageColorSpace = chosen.colorSpace;
			ci.imageExtent = extent;
			ci.imageArrayLayers = 1;
			ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			ci.preTransform = caps.currentTransform;
			ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
				: (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
			ci.presentMode = PresentMode();
			ci.clipped = VK_TRUE;
			ci.oldSwapchain = w.chain;
			VkSwapchainKHR chain;
			Check(vkCreateSwapchainKHR(s.device, &ci, nullptr, &chain), "vkCreateSwapchainKHR");
			if (w.chain)
				vkDestroySwapchainKHR(s.device, w.chain, nullptr);  // nothing uses it: every swap waits for its submit
			w.chain = chain;
			vkGetSwapchainImagesKHR(s.device, w.chain, &n, nullptr);
			w.images.resize(n);
			vkGetSwapchainImagesKHR(s.device, w.chain, &n, w.images.data());
			w.format = chosen.format;
			w.srgb = srgb;
			w.width = extent.width;
			w.height = extent.height;
			Log(fmt::format("window: {}x{}, {} images, format {}, {}", w.width, w.height, n, (int)w.format,
				ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo"));
			return true;
		}
	}

	void SetOverlay(sint32 x, sint32 y, uint32 w, uint32 h, std::vector<uint32> rgba)
	{
		std::lock_guard lock(s_overlayLock);
		s_overlay.x = x;
		s_overlay.y = y;
		s_overlay.w = rgba.empty() ? 0 : w;
		s_overlay.h = rgba.empty() ? 0 : h;
		s_overlay.rgba = std::move(rgba);
		s_overlay.version++;
	}

	// the overlay over the TV image's rectangle in the swapchain image (in TRANSFER_DST layout)
	static void DrawOverlay(VkImage target, sint32 tvX, sint32 tvY, double scale)
	{
		std::unique_lock lock(s_overlayLock);
		if (!s_overlay.w)
			return;
		VkFormat format = w.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;   // keep its bytes, like the TV image
		Image& img = s_overlayImage;
		if (!img.image || img.width != s_overlay.w || img.height != s_overlay.h || img.format != format)
		{
			if (img.image)
				DestroyImage(img);
			img = CreateImage(format, VK_IMAGE_ASPECT_COLOR_BIT, s_overlay.w, s_overlay.h, 0);
			s_overlayUploaded = 0;
		}
		if (s_overlayUploaded != s_overlay.version)
		{
			VkDeviceSize bytes = (VkDeviceSize)s_overlay.w * s_overlay.h * 4;
			VkDeviceSize at = RingAlloc(bytes, 16);
			memcpy(s.ring.data + at, s_overlay.rgba.data(), bytes);
			Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkBufferImageCopy c{};
			c.bufferOffset = at;
			c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			c.imageExtent = { s_overlay.w, s_overlay.h, 1 };
			vkCmdCopyBufferToImage(s.cmd, s.ring.buffer, img.image, img.layout, 1, &c);
			s_overlayUploaded = s_overlay.version;
		}
		sint32 x0 = tvX + (sint32)(s_overlay.x * scale), y0 = tvY + (sint32)(s_overlay.y * scale);
		sint32 x1 = tvX + (sint32)((s_overlay.x + (sint32)s_overlay.w) * scale), y1 = tvY + (sint32)((s_overlay.y + (sint32)s_overlay.h) * scale);
		lock.unlock();
		if (x1 <= x0 || y1 <= y0)
			return;
		Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkImageBlit b{};
		b.srcSubresource = b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		b.srcOffsets[1] = { (sint32)img.width, (sint32)img.height, 1 };
		b.dstOffsets[0] = { x0, y0, 0 };
		b.dstOffsets[1] = { x1, y1, 1 };
		vkCmdBlitImage(s.cmd, img.image, img.layout, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
	}

	void SetWindow(Window window)
	{
		s_window = std::move(window);
		s_hasWindow = true;
	}

	bool HasWindow() { return s_hasWindow; }

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
	static bool Acquire(bool srgb)
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
			if (!w.chain || width != w.width || height != w.height || srgb != w.srgb || attempt > 0)
				if (!Build(width, height, srgb))
					return false;
			VkResult r = vkAcquireNextImageKHR(s.device, w.chain, UINT64_MAX, VK_NULL_HANDLE, w.acquired, &w.index);
			if (r == VK_ERROR_OUT_OF_DATE_KHR)
				continue;
			if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
				Check(r, "vkAcquireNextImageKHR");
			Check(vkWaitForFences(s.device, 1, &w.acquired, VK_TRUE, UINT64_MAX), "vkWaitForFences");
			vkResetFences(s.device, 1, &w.acquired);
			w.pending = true;
			break;
		}
		return w.pending;
	}

	void PresentRecord(Image& scan)
	{
		w.pending = false;
		if (!s_hasWindow || !scan.image || !Acquire(IsSrgb(scan.format)))
			return;

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
		DrawOverlay(target, x, y, dw / 1920.0);
		Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
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

	void PresentQueue()
	{
		if (!w.pending)
			return;
		w.pending = false;
		VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		pi.swapchainCount = 1;
		pi.pSwapchains = &w.chain;
		pi.pImageIndices = &w.index;
		VkResult r = vkQueuePresentKHR(s.queue, &pi);             // the submit has finished: nothing to wait for
		if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
			w.width = 0;                                          // rebuild at the next swap
		else
			Check(r, "vkQueuePresentKHR");
	}
}
