// Runtime loading of Vulkan (see vk.h).
#include "vk.h"
#include <dlfcn.h>
#if defined(WWHD_ADRENOTOOLS)
#include <adrenotools/driver.h>
#include <android/log.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#endif

namespace wwhd::vk
{
#define WWHD_VK_DEFINE(f) PFN_##f f = nullptr;
	WWHD_VK_GLOBAL(WWHD_VK_DEFINE)
	WWHD_VK_INSTANCE(WWHD_VK_DEFINE)
	WWHD_VK_DEVICE(WWHD_VK_DEFINE)
#undef WWHD_VK_DEFINE

	static PFN_vkGetInstanceProcAddr s_getInstanceProcAddr = nullptr;
	static bool s_imported = false;

	bool ImportedDriver() { return s_imported; }

#if defined(WWHD_ADRENOTOOLS)
	// a4-drivers: WWHD_GPU_DRIVER=DIR, a GPU driver the player imported (Turnip, Qualcomm's own: a package's meta.json and
	// its library, unpacked in the app's internal files, where dlopen may load from), loaded by libadrenotools in place
	// of the system's (its hooks in the app's native library dir, WWHD_NATIVE_LIB_DIR); the system's when it fails
	static void* OpenImportedDriver()
	{
		const char* chosen = getenv("WWHD_GPU_DRIVER");         // "system", a driver's folder in WWHD_DRIVERS_DIR, or a path
		const char* hooks = getenv("WWHD_NATIVE_LIB_DIR");
		if (!chosen || !*chosen || strcmp(chosen, "system") == 0 || !hooks || !*hooks)
			return nullptr;
		const char* drivers = getenv("WWHD_DRIVERS_DIR");
		const std::string path = chosen[0] == '/' ? std::string(chosen) : std::string(drivers ? drivers : "") + "/" + chosen;
		const char* dir = path.c_str();
		auto note = [](const std::string& line) { __android_log_print(ANDROID_LOG_INFO, "wwhd", "vk: %s", line.c_str()); };
		std::ifstream meta(std::string(dir) + "/meta.json");
		std::stringstream text;
		text << meta.rdbuf();
		const std::string json = text.str();
		// "libraryName": "vulkan.ad07XX.so" (the packages' one field we need; no JSON parser here)
		std::string name;
		if (size_t k = json.find("\"libraryName\""); k != std::string::npos)
			if (size_t q0 = json.find('"', json.find(':', k) + 1); q0 != std::string::npos)
				if (size_t q1 = json.find('"', q0 + 1); q1 != std::string::npos)
					name = json.substr(q0 + 1, q1 - q0 - 1);
		if (name.empty())
		{
			note(std::string("imported driver: no libraryName in ") + dir + "/meta.json: the system's driver");
			return nullptr;
		}
		const std::string driverDir = std::string(dir) + "/";
		void* lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hooks, driverDir.c_str(),
			name.c_str(), nullptr, nullptr);
		note(lib ? "imported driver " + driverDir + name : "imported driver " + driverDir + name + " didn't load: the system's driver");
		return lib;
	}
#endif

	bool Load()
	{
		void* lib = nullptr;
#if defined(WWHD_ADRENOTOOLS)
		lib = OpenImportedDriver();
#endif
		s_imported = lib != nullptr;
		if (!lib)
			lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
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
