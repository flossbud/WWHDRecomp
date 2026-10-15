// passes.cpp: dynamic rendering on a device without it (m-android, the Retroid Pocket 4 Pro: Mali-G77's r32 driver is
// Vulkan 1.1 with no VK_KHR_dynamic_rendering). The renderer records vkCmdBeginRendering/vkCmdEndRendering and builds
// its pipelines with VkPipelineRenderingCreateInfo everywhere; EmulateRendering wraps those, so that each rendering is
// a render pass of one subpass and each pipeline is built against the render pass of its formats:
// - a render pass per set of attachments (slot, format, load and store ops, layout), its layouts the ones the renderer
//   gave (initial = subpass = final: no transitions of its own, so the renderer's barriers still order everything, as
//   they do around dynamic rendering);
// - a framebuffer per set of views and size, made with the render pass of their formats alone (compatible with every
//   pass of those formats, whatever the ops); the views' formats are taken at vkCreateImageView, and a view's
//   framebuffers go with it at vkDestroyImageView (the renderer destroys a view only once the GPU is done with it);
// - a pipeline's VkPipelineRenderingCreateInfo becomes the render pass of its formats (subpass 0).
// Every image is single-sampled (the renderer makes no other). WWHD_VK_MASK=rendering takes this path on any device.
#include "vk.h"
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace wwhd::vk
{
	namespace
	{
		PFN_vkCreateRenderPass s_createRenderPass;
		PFN_vkCreateFramebuffer s_createFramebuffer;
		PFN_vkDestroyFramebuffer s_destroyFramebuffer;
		PFN_vkCmdBeginRenderPass s_beginRenderPass;
		PFN_vkCmdEndRenderPass s_endRenderPass;
		PFN_vkCreateImageView s_createImageView;
		PFN_vkDestroyImageView s_destroyImageView;
		PFN_vkCreateGraphicsPipelines s_createGraphicsPipelines;
		VkDevice s_device = VK_NULL_HANDLE;
		std::mutex s_lock;                                    // pipelines are built on the compile threads too

		size_t Fnv(const void* p, size_t n)
		{
			uint64_t h = 0xcbf29ce484222325ull;
			for (size_t i = 0; i < n; i++)
				h = (h ^ ((const uint8_t*)p)[i]) * 0x100000001b3ull;
			return (size_t)h;
		}

		// slots 0-7 the colours, 8 the depth (and stencil); format UNDEFINED: unused
		struct Attachment
		{
			VkFormat format;
			VkAttachmentLoadOp load;
			VkAttachmentStoreOp store;
			VkImageLayout layout;
		};
		struct PassKey
		{
			Attachment a[9];
			uint32_t colors;                                  // the subpass's colour count (unused slots included)
			bool operator==(const PassKey& o) const { return !memcmp(this, &o, sizeof(*this)); }
		};
		struct FramebufferKey
		{
			VkRenderPass formats;                             // the render pass of the attachments' formats alone
			VkImageView views[9];                             // in attachment order
			uint32_t width, height, layers;
			bool operator==(const FramebufferKey& o) const { return !memcmp(this, &o, sizeof(*this)); }
		};
		struct Hash
		{
			size_t operator()(const PassKey& k) const { return Fnv(&k, sizeof(k)); }
			size_t operator()(const FramebufferKey& k) const { return Fnv(&k, sizeof(k)); }
		};
		std::unordered_map<PassKey, VkRenderPass, Hash> s_passes;
		std::unordered_map<FramebufferKey, VkFramebuffer, Hash> s_framebuffers;
		std::unordered_map<VkImageView, VkFormat> s_viewFormats;
		std::unordered_map<VkImageView, std::vector<FramebufferKey>> s_viewFramebuffers;

		const Attachment kFormatOnly{ VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		Attachment FormatOnly(VkFormat format, bool depth)
		{
			Attachment a = kFormatOnly;
			a.format = format;
			if (depth)
				a.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			return a;
		}

		VkRenderPass Pass(const PassKey& k)                   // s_lock held
		{
			if (auto it = s_passes.find(k); it != s_passes.end())
				return it->second;
			VkAttachmentDescription att[9]{};
			VkAttachmentReference colors[8], depth{};
			uint32_t n = 0;
			auto describe = [&](const Attachment& a) {
				VkAttachmentDescription& d = att[n];
				d.format = a.format;
				d.samples = VK_SAMPLE_COUNT_1_BIT;
				d.loadOp = d.stencilLoadOp = a.load;
				d.storeOp = d.stencilStoreOp = a.store;
				d.initialLayout = d.finalLayout = a.layout;
				return VkAttachmentReference{ n++, a.layout };
			};
			for (uint32_t i = 0; i < 8; i++)
				colors[i] = k.a[i].format != VK_FORMAT_UNDEFINED ? describe(k.a[i])
					: VkAttachmentReference{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
			const bool hasDepth = k.a[8].format != VK_FORMAT_UNDEFINED;
			if (hasDepth)
				depth = describe(k.a[8]);
			VkSubpassDescription sub{};
			sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			sub.colorAttachmentCount = k.colors;
			sub.pColorAttachments = colors;
			sub.pDepthStencilAttachment = hasDepth ? &depth : nullptr;
			VkRenderPassCreateInfo ci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
			ci.attachmentCount = n;
			ci.pAttachments = att;
			ci.subpassCount = 1;
			ci.pSubpasses = &sub;
			VkRenderPass pass = VK_NULL_HANDLE;
			s_createRenderPass(s_device, &ci, nullptr, &pass);
			return s_passes[k] = pass;
		}

		VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* a,
			VkImageView* view)
		{
			const VkResult r = s_createImageView(device, info, a, view);
			if (r == VK_SUCCESS)
			{
				std::lock_guard lock(s_lock);
				s_viewFormats[*view] = info->format;
			}
			return r;
		}

		void VKAPI_CALL DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* a)
		{
			{
				std::lock_guard lock(s_lock);
				s_viewFormats.erase(view);
				if (auto users = s_viewFramebuffers.find(view); users != s_viewFramebuffers.end())
				{
					for (const FramebufferKey& k : users->second)
						if (auto it = s_framebuffers.find(k); it != s_framebuffers.end())
						{
							s_destroyFramebuffer(device, it->second, nullptr);
							s_framebuffers.erase(it);
						}
					s_viewFramebuffers.erase(users);
				}
			}
			s_destroyImageView(device, view, a);
		}

		VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
			const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* a, VkPipeline* pipelines)
		{
			std::vector<VkGraphicsPipelineCreateInfo> copy(infos, infos + count);
			for (VkGraphicsPipelineCreateInfo& ci : copy)
			{
				auto* r = (const VkPipelineRenderingCreateInfo*)ci.pNext;
				if (ci.renderPass != VK_NULL_HANDLE || !r || r->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
					continue;                                 // the renderer chains it first, alone
				PassKey k{};
				k.colors = r->colorAttachmentCount;
				for (uint32_t i = 0; i < r->colorAttachmentCount && i < 8; i++)
					if (r->pColorAttachmentFormats[i] != VK_FORMAT_UNDEFINED)
						k.a[i] = FormatOnly(r->pColorAttachmentFormats[i], false);
				const VkFormat depth = r->depthAttachmentFormat != VK_FORMAT_UNDEFINED ? r->depthAttachmentFormat
					: r->stencilAttachmentFormat;
				if (depth != VK_FORMAT_UNDEFINED)
					k.a[8] = FormatOnly(depth, true);
				{
					std::lock_guard lock(s_lock);
					ci.renderPass = Pass(k);
				}
				ci.subpass = 0;
				ci.pNext = r->pNext;
			}
			return s_createGraphicsPipelines(device, cache, count, copy.data(), a, pipelines);
		}

		void VKAPI_CALL BeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info)
		{
			PassKey ops{}, formats{};
			FramebufferKey fk{};
			VkClearValue clears[9]{};
			uint32_t n = 0;
			std::lock_guard lock(s_lock);
			auto add = [&](uint32_t slot, const VkRenderingAttachmentInfo& a, bool depth) {
				const auto f = s_viewFormats.find(a.imageView);
				if (f == s_viewFormats.end())
					return;                                   // not a view made through vkCreateImageView: left unused
				ops.a[slot] = { f->second, a.loadOp, a.storeOp, a.imageLayout };
				formats.a[slot] = FormatOnly(f->second, depth);
				fk.views[n] = a.imageView;
				clears[n++] = a.clearValue;
			};
			ops.colors = formats.colors = info->colorAttachmentCount;
			for (uint32_t i = 0; i < info->colorAttachmentCount && i < 8; i++)
				if (info->pColorAttachments[i].imageView != VK_NULL_HANDLE)
					add(i, info->pColorAttachments[i], false);
			const VkRenderingAttachmentInfo* depth = info->pDepthAttachment ? info->pDepthAttachment : info->pStencilAttachment;
			if (depth && depth->imageView != VK_NULL_HANDLE)
				add(8, *depth, true);
			const VkRenderPass pass = Pass(ops);
			fk.formats = Pass(formats);
			fk.width = (uint32_t)info->renderArea.offset.x + info->renderArea.extent.width;
			fk.height = (uint32_t)info->renderArea.offset.y + info->renderArea.extent.height;
			fk.layers = info->layerCount ? info->layerCount : 1;
			VkFramebuffer& fb = s_framebuffers[fk];
			if (fb == VK_NULL_HANDLE)
			{
				VkFramebufferCreateInfo ci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
				ci.renderPass = fk.formats;
				ci.attachmentCount = n;
				ci.pAttachments = fk.views;
				ci.width = fk.width;
				ci.height = fk.height;
				ci.layers = fk.layers;
				s_createFramebuffer(s_device, &ci, nullptr, &fb);
				for (uint32_t i = 0; i < n; i++)
				{
					std::vector<FramebufferKey>& users = s_viewFramebuffers[fk.views[i]];
					bool known = false;
					for (const FramebufferKey& u : users)
						known |= u == fk;
					if (!known)
						users.push_back(fk);
				}
			}
			VkRenderPassBeginInfo bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
			bi.renderPass = pass;
			bi.framebuffer = fb;
			bi.renderArea = info->renderArea;
			bi.clearValueCount = n;
			bi.pClearValues = clears;
			s_beginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
		}

		void VKAPI_CALL EndRendering(VkCommandBuffer cmd)
		{
			s_endRenderPass(cmd);
		}
	}

	void EmulateRendering(VkDevice device)
	{
		std::lock_guard lock(s_lock);
		s_device = device;
		s_passes.clear();                                     // a new device (the renderer's Init again): the old ones went with it
		s_framebuffers.clear();
		s_viewFormats.clear();
		s_viewFramebuffers.clear();
		s_createRenderPass = (PFN_vkCreateRenderPass)vkGetDeviceProcAddr(device, "vkCreateRenderPass");
		s_createFramebuffer = (PFN_vkCreateFramebuffer)vkGetDeviceProcAddr(device, "vkCreateFramebuffer");
		s_destroyFramebuffer = (PFN_vkDestroyFramebuffer)vkGetDeviceProcAddr(device, "vkDestroyFramebuffer");
		s_beginRenderPass = (PFN_vkCmdBeginRenderPass)vkGetDeviceProcAddr(device, "vkCmdBeginRenderPass");
		s_endRenderPass = (PFN_vkCmdEndRenderPass)vkGetDeviceProcAddr(device, "vkCmdEndRenderPass");
		s_createImageView = vkCreateImageView;
		s_destroyImageView = vkDestroyImageView;
		s_createGraphicsPipelines = vkCreateGraphicsPipelines;
		vkCreateImageView = CreateImageView;
		vkDestroyImageView = DestroyImageView;
		vkCreateGraphicsPipelines = CreateGraphicsPipelines;
		vkCmdBeginRendering = BeginRendering;
		vkCmdEndRendering = EndRendering;
	}
}
