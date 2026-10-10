// The TV picture's aspect ratio (b-wide; docs/research/gpu-plan.md "Aspect ratio"): WWHD_ASPECT=16:9 (the game's own,
// the default), auto (the window's shape) or 4:3. The game keeps its 16:9 guest screen; with another aspect its camera
// sees more to the sides (Hor+: wider than 16:9, the vertical field of view kept) or above and below (Vert+: narrower,
// the horizontal one kept), the culling frustum with it (src/overrides/aspect.cpp, hooks on the game's camera), and the
// renderer makes its screen-shaped targets that much wider or taller (gpu/vk). Auto without a window is 16:9; the
// checks never set it, so none sees a difference.
#pragma once
#include <cstdint>

namespace wwhd::os::aspect
{
	constexpr float kBase = 16.0f / 9.0f;
	void SetWindow(uint32_t width, uint32_t height);   // the window's drawable size (the frontend, whenever it changes)
	float Requested();                             // what the setting asks for now (Auto without a window: 16:9)
	float Latch();                                 // at each swap (the renderer): the aspect from here on, Requested's
	float Current();                               // the latched aspect (the game's hooks, the renderer)
	bool Recent(float a);                          // one the game may still hold (the last few latched, and 16:9)
	// the widening of the 16:9 picture: kx = a / (16/9) wider, ky = (16/9) / a taller (one of them 1)
	void Factors(float a, float& kx, float& ky);
	inline bool Same(float a, float b) { return a == b || (a - b < 1e-4f && b - a < 1e-4f); }
}
