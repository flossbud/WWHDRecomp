// Runtime loading of Vulkan (see vk.h).
#include "vk.h"
#include <dlfcn.h>

namespace wwhd::vk
{
#define WWHD_VK_DEFINE(f) PFN_##f f = nullptr;
	WWHD_VK_GLOBAL(WWHD_VK_DEFINE)
	WWHD_VK_INSTANCE(WWHD_VK_DEFINE)
	WWHD_VK_DEVICE(WWHD_VK_DEFINE)
#undef WWHD_VK_DEFINE

	static PFN_vkGetInstanceProcAddr s_getInstanceProcAddr = nullptr;

	bool Load()
	{
		void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
		if (!lib)
			lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);   // Android's name
		if (!lib)
			return false;
		s_getInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
		if (!s_getInstanceProcAddr)
			return false;
#define WWHD_VK_LOAD(f) f = (PFN_##f)s_getInstanceProcAddr(nullptr, #f);
		WWHD_VK_GLOBAL(WWHD_VK_LOAD)
#undef WWHD_VK_LOAD
		return vkCreateInstance != nullptr;
	}

	void LoadInstance(VkInstance instance)
	{
#define WWHD_VK_LOAD(f) f = (PFN_##f)s_getInstanceProcAddr(instance, #f);
		WWHD_VK_INSTANCE(WWHD_VK_LOAD)
#undef WWHD_VK_LOAD
	}

	void LoadDevice(VkDevice device, bool renderingKHR)
	{
#define WWHD_VK_LOAD(f) f = (PFN_##f)vkGetDeviceProcAddr(device, #f);
		WWHD_VK_DEVICE(WWHD_VK_LOAD)
#undef WWHD_VK_LOAD
		if (renderingKHR)                          // VK_KHR_dynamic_rendering: the same functions, the extension's names
		{
			vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR");
			vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR");
		}
	}
}
