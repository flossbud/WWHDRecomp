// Vulkan for the renderer (docs/recompiler-design.md D13), loaded at runtime with dlopen so that
// wwhd-null runs without a Vulkan driver when rendering is off. Functions are pointers in
// wwhd::vk, loaded by Load() (global and instance level) and LoadDevice() (device level).
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

namespace wwhd::vk
{
#define WWHD_VK_GLOBAL(X) \
	X(vkCreateInstance) X(vkEnumerateInstanceVersion)
#define WWHD_VK_INSTANCE(X) \
	X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFeatures2) \
	X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroyInstance) X(vkEnumerateDeviceExtensionProperties) \
	X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define WWHD_VK_DEVICE(X) \
	X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
	X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
	X(vkResetCommandBuffer) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) \
	X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
	X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
	X(vkCreateImageView) X(vkDestroyImageView) \
	X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
	X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdBlitImage) \
	X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) \
	X(vkCreateShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) \
	X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
	X(vkCreateSampler) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
	X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) \
	X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetDepthBias) \
	X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
	X(vkQueuePresentKHR)

#define WWHD_VK_DECLARE(f) extern PFN_##f f;
	WWHD_VK_GLOBAL(WWHD_VK_DECLARE)
	WWHD_VK_INSTANCE(WWHD_VK_DECLARE)
	WWHD_VK_DEVICE(WWHD_VK_DECLARE)
#undef WWHD_VK_DECLARE

	bool Load();                                   // libvulkan and the global functions
	void LoadInstance(VkInstance instance);
	void LoadDevice(VkDevice device);
}
