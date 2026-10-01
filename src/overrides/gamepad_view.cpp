// f_027D6BB0: renders one view of a scene (docs/recompiler-design.md D9; the GamePad's screen, D17).
//
// The game draws two screens every frame, the TV's and the GamePad's, which shows the ITEMS menu
// (3D item icons, tabs and buttons: about 108 of the save route's 1,187 draws a frame). The GamePad
// is absent (D17) and nothing shows its image. The engine's render jobs (f_027D74AC, run by the job
// system's worker task f_0276AA34 and by the main thread) render each scene's views through this
// function: f_027C424C(view, scene + 4, the scene's render target *(scene + 0xc), ...), which sets
// the viewport from the target's rectangle (floats, read by f_0274F964) and draws the view's layers.
//
// In real time a view whose target is the GamePad's 854x480 screen is skipped: nothing is drawn into
// the buffer that gets copied to the GamePad. With the virtual clock (every check) the game's code
// runs, orig_f_027D6BB0. WWHD_SKIP_GAMEPAD=1 skips with the virtual clock too (to check that the
// TV's picture doesn't change), WWHD_SKIP_GAMEPAD=0 never skips.
#include "override.h"
#include <bit>
#include <mutex>
#include <set>

namespace
{
	float rdf32(uint32 ea)
	{
		return std::bit_cast<float>(rd32(ea));
	}

	int Setting()
	{
		static const int setting = [] {
			const char* e = getenv("WWHD_SKIP_GAMEPAD");
			return e ? atoi(e) : -1;
		}();
		return setting;
	}

	// each render target once, with its rectangle, in the log
	void Note(uint32 target)
	{
		static std::mutex lock;
		static std::set<uint32> seen;
		std::lock_guard guard(lock);
		if (seen.insert(target).second)
			cemuLog_log(LogType::Force, "wwhd: render target {:08x}: {} {} {} {}", target, rdf32(target + 0x8), rdf32(target + 0xc),
				rdf32(target + 0x10), rdf32(target + 0x14));
	}
}

void f_027D6BB0(PPCInterpreter_t* __restrict ctx)
{
	const int setting = Setting();
	if (setting == 1 || (setting != 0 && wwhd::rt::FastPaths()))
	{
		const uint32 target = rd32(GPR(3) + 0xc);
		if (target != 0)
		{
			Note(target);
			if (rdf32(target + 0x10) - rdf32(target + 0x8) == 854.0f && rdf32(target + 0x14) - rdf32(target + 0xc) == 480.0f)
				return;                                            // the GamePad's screen: nothing shows it
		}
	}
	[[clang::musttail]] return orig_f_027D6BB0(ctx);
}
