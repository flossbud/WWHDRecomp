// lsfg.cpp: Lossless Scaling frame generation (b-lsfg, session bottom; the owner's ask). Frame generation "Lossless
// Scaling x2/x3/x4" (WWHD_FRAMEGEN=lsfg2/3/4) runs lsfg-vk, the Vulkan layer that runs Lossless Scaling's frame
// generation shaders on Linux, with the player's own copy of Lossless Scaling (its DLL: never shipped or committed).
//
// The layer isn't ours to ship (lsfg-vk 2 is CC BY-NC-ND 4.0), so an installed one is found among the loader's layers
// (both implicit: v1 VK_LAYER_LS_frame_generation, which reads Lossless.dll; v2 VK_LAYER_LSFGVK_frame_generation, which
// reads lsfg-vk.dll from Lossless Scaling's "lsfg-vk" branch) and driven by its environment, set before the instance:
// v1 LSFG_LEGACY=1 and LSFG_*, v2 LSFGVK_ENV=1 and LSFGVK_* (a launcher's own values win). The DLL: WWHD_LSFG_DLL, else
// portable/, else Steam's Lossless Scaling folder (the layers' own search). With both layers installed, v2 when its
// DLL is there. Without a layer or a DLL: off, the settings page says why. The other choices: FSR 3's frame generation
// disables an installed layer (DISABLE_LSFG, DISABLE_LSFGVK) so the two don't stack; off leaves it to the player's own
// lsfg-vk profiles. The layer interpolates the whole picture (the HUD too) and presents FIFO unless told otherwise.
#include "renderer_internal.h"
#include "../../os/settings.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace wwhd::gpu::lsfg
{
	namespace
	{
		namespace fs = std::filesystem;

		// a DLL by name: WWHD_LSFG_DLL (the file, or the folder holding it), portable/, Steam's Lossless Scaling folder
		std::string FindDll(const char* name)
		{
			std::error_code ec;
			if (const char* e = getenv("WWHD_LSFG_DLL"); e && *e)
			{
				const fs::path p(e);
				if (fs::is_directory(p, ec))
					return fs::exists(p / name, ec) ? (p / name).string() : "";
				return p.filename() == name && fs::exists(p, ec) ? p.string() : "";
			}
			std::vector<fs::path> dirs;
			const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
			if (!ec)
				dirs.push_back(exe.parent_path() / "portable");
			for (const char* var : { "XDG_DATA_HOME", "HOME" })
			{
				const char* root = getenv(var);
				if (!root || !*root)
					continue;
				for (const char* steam : { ".local/share/Steam/steamapps/common", ".steam/steam/steamapps/common",
						".steam/debian-installation/steamapps/common",
						".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common",
						"snap/steam/common/.local/share/Steam/steamapps/common" })
					dirs.push_back(fs::path(root) / steam / "Lossless Scaling");
			}
			for (const fs::path& d : dirs)
				if (fs::exists(d / name, ec))
					return (d / name).string();
			return "";
		}

		bool s_active = false, s_disabled = false;

		void Off(const std::string& why)
		{
			s_active = false;
			s_disabled = true;
			setenv("DISABLE_LSFG", "1", 1);
			setenv("DISABLE_LSFGVK", "1", 1);
			os::settings::SetNote("WWHD_FRAMEGEN", "off: " + why);
			Log("lsfg: Lossless Scaling frame generation off: " + why);
		}
	}

	void Prepare()
	{
		const char* e = getenv("WWHD_FRAMEGEN");
		if (!e || s_disabled)
			return;
		if (strcmp(e, "1") == 0 || strcmp(e, "force") == 0)        // FSR 3's: not both
		{
			setenv("DISABLE_LSFG", "1", 1);
			setenv("DISABLE_LSFGVK", "1", 1);
			return;
		}
		if (strncmp(e, "lsfg", 4) != 0)
			return;
		const int multiplier = std::clamp(atoi(e + 4), 2, 4);
		bool v1 = false, v2 = false;
		uint32 n = 0;
		if (vk::vkEnumerateInstanceLayerProperties && vk::vkEnumerateInstanceLayerProperties(&n, nullptr) == VK_SUCCESS)
		{
			std::vector<VkLayerProperties> layers(n);
			vk::vkEnumerateInstanceLayerProperties(&n, layers.data());
			for (const VkLayerProperties& l : layers)
			{
				v1 |= strcmp(l.layerName, "VK_LAYER_LS_frame_generation") == 0;
				v2 |= strcmp(l.layerName, "VK_LAYER_LSFGVK_frame_generation") == 0;
			}
		}
		if (!v1 && !v2)
			return Off("lsfg-vk isn't installed (lsfg-vk.dev)");
		const std::string dll2 = v2 ? FindDll("lsfg-vk.dll") : "";
		const std::string dll1 = v1 && dll2.empty() ? FindDll("Lossless.dll") : "";
		if (dll1.empty() && dll2.empty())
			return Off(v2 ? "lsfg-vk.dll not found (Lossless Scaling's lsfg-vk branch; WWHD_LSFG_DLL or portable/)"
				: "Lossless.dll not found (WWHD_LSFG_DLL or portable/)");
		const std::string m = std::to_string(multiplier);
		if (!dll2.empty())
		{
			setenv("DISABLE_LSFG", "1", 1);
			setenv("LSFGVK_ENV", "1", 0);
			setenv("LSFGVK_DLL_PATH", dll2.c_str(), 0);
			setenv("LSFGVK_MULTIPLIER", m.c_str(), 0);
		}
		else
		{
			setenv("DISABLE_LSFGVK", "1", 1);
			setenv("LSFG_LEGACY", "1", 0);
			setenv("LSFG_DLL_PATH", dll1.c_str(), 0);
			setenv("LSFG_MULTIPLIER", m.c_str(), 0);
		}
		s_active = true;
		os::settings::SetFrameGen(multiplier);
		os::settings::SetNote("WWHD_FRAMEGEN", "");
		Log(fmt::format("lsfg: Lossless Scaling frame generation x{} through lsfg-vk {} ({})", multiplier,
			dll2.empty() ? 1 : 2, dll2.empty() ? "Lossless.dll" : "lsfg-vk.dll"));
	}

	bool Active() { return s_active; }

	void Disable(const std::string& why)
	{
		Off(why);
		os::settings::SetFrameGen(1);
	}
}
