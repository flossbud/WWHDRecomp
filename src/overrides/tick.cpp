// f_02746790: the game's tick (docs/recompiler-design.md D21): f_02747C6C(root + 0x40), once a frame,
// first thing in game_procFrameBody, before RenderDisplay_draw.
//
// The 60 fps prototype (M6; WWHD_60FPS=1, real time only): the game presents every vsync (our
// GX2SetSwapInterval turns its 2 into 1) and this runs every other frame, so the game still ticks 30
// times a second and the frames between draw the same state again. Next: interpolate the camera and
// the actors on those frames. Everywhere else (and in every check) the game's code runs.
#include "override.h"

void f_02746790(PPCInterpreter_t* __restrict ctx)
{
	if (wwhd::rt::SixtyFps())
	{
		// WWHD_60FPS_AFTER=n: tick every frame for the first n ticks (experiment: boot, then skip)
		static const uint32 after = [] { const char* e = getenv("WWHD_60FPS_AFTER"); return e ? (uint32)atoi(e) : 0u; }();
		static uint32 calls = 0;
		static bool tickNext = true;          // the main thread's, the only caller during play
		if (++calls > after)
		{
			const bool tick = tickNext;
			tickNext = !tickNext;
			if (!tick)
				return;
		}
	}
	[[clang::musttail]] return orig_f_02746790(ctx);
}
