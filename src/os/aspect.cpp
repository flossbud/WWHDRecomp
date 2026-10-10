// The TV picture's aspect ratio (aspect.h).
#include "aspect.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace wwhd::os::aspect
{
	namespace
	{
		enum class Mode { Original, Window, Fixed };
		struct Setting { Mode mode; float value; };
		Setting Read()
		{
			const char* e = getenv("WWHD_ASPECT");
			if (!e || !*e || !strcmp(e, "16:9"))
				return { Mode::Original, kBase };
			if (!strcmp(e, "auto") || !strcmp(e, "window"))
				return { Mode::Window, 0.0f };
			float a = 0.0f;
			if (const char* c = strchr(e, ':'))
				a = (float)atof(e) / (float)atof(c + 1);       // 4:3, 21:9, 16:10...
			else
				a = (float)atof(e);
			return a >= 1.0f && a <= 4.0f ? Setting{ Mode::Fixed, a } : Setting{ Mode::Original, kBase };
		}
		std::atomic<float> s_window{ 0.0f };                    // 0: no window
		std::atomic<float> s_current{ kBase };
		std::mutex s_lock;
		float s_recent[4] = {};                                  // aspects latched before (the game may hold one a frame)
	}

	void SetWindow(uint32_t width, uint32_t height)
	{
		if (!width || !height)
			return;
		// resizing is continuous: steps of 0.5% keep the targets from being made again every frame, and a window within
		// half a step of 16:9 (rounded pixel sizes) is 16:9
		float a = std::round((float)width / (float)height * 200.0f) / 200.0f;
		if (std::fabs(a - kBase) < 0.006f)
			a = kBase;
		s_window = a;
	}

	float Requested()
	{
		static const Setting setting = Read();
		const float window = s_window.load();
		if (setting.mode == Mode::Original || (setting.mode == Mode::Window && window == 0.0f))
			return kBase;                                        // Auto without a window (every check): 16:9
		const float a = setting.mode == Mode::Window ? window : setting.value;
		return std::fmin(std::fmax(a, 1.0f), 4.0f);
	}

	float Latch()
	{
		const float a = Requested(), was = s_current.load();
		if (!Same(a, was))
		{
			std::lock_guard guard(s_lock);
			memmove(s_recent + 1, s_recent, sizeof s_recent - sizeof *s_recent);
			s_recent[0] = was;
			s_current = a;
		}
		return a;
	}

	float Current()
	{
		return s_current.load(std::memory_order_relaxed);
	}

	bool Recent(float a)
	{
		if (Same(a, kBase) || Same(a, Current()))
			return true;
		std::lock_guard guard(s_lock);
		for (float r : s_recent)
			if (r != 0.0f && Same(a, r))
				return true;
		return false;
	}

	void Factors(float a, float& kx, float& ky)
	{
		kx = a > kBase ? a / kBase : 1.0f;
		ky = a < kBase ? kBase / a : 1.0f;
	}
}
