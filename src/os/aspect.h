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
	float Latch(float a);                          // the renderer, at a swap: the aspect from here on (Requested's, once
	                                               // the window has held its size a moment: a drag resizes continuously)
	float Current();                               // the latched aspect (the game's hooks, the renderer)
	bool Recent(float a);                          // one the game may still hold (the last few latched, and 16:9)
	// the widening of the 16:9 picture: kx = a / (16/9) wider, ky = (16/9) / a taller (one of them 1)
	void Factors(float a, float& kx, float& ky);
	inline bool Same(float a, float b) { return a == b || (a - b < 1e-4f && b - a < 1e-4f); }

	// The HUD (stage 2): the HD layouts (nw::lyt) drawn into the wider picture keep their proportions, centred, and their
	// edge panes move out to its edges (src/overrides/aspect.cpp). Which screen a layout is drawn to is known only where
	// it is drawn (the renderer): its projection's upload is marked in the command stream while the game loads it
	// (TagProjection, around DrawInfo::LoadProjectionMtx and the text draw: gx2's uniform upload asks Tagged), and each
	// layout root's draw is marked; the renderer reports whether that root went to a widened target (SetRootTv)
	void TagProjection(int delta);                 // the calling thread's tag, +1 entering, -1 leaving
	bool Tagged();
	void SetRootTv(uint32_t root, bool tv);        // the renderer: a layout root drawn into a widened target, or not
	bool RootOnTv(uint32_t root);                  // true until reported otherwise
}
