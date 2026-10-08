// Frame pacing at 60 fps in real time, and the frame log (docs/recompiler-design.md D21).
//
// sead's frame (fw_procFrame, f_0274C264) ends in fw_waitForVsync (f_0274C874): it waits for the
// next vsync, then until every swap has flipped. At 30 fps (swap interval 2) a frame gets two
// vsyncs; at 60 one, and a game tick takes two frames: a whole tick's (the game's logic and the
// draw) and a half tick's (the draw, and the converted processes' half steps). The whole tick's
// frame costs the most. When it overruns its vsync the game's wait gives it a second one, and as
// the game's tick is frame-locked, the game slows down (the owner's slow motion facing Outset:
// 41-48 fps). Paired instead: a whole tick's frame that has missed its vsync doesn't wait for the
// next one, and a half tick's frame waits for the pair's second vsync. A tick keeps its two vsyncs
// whenever its two frames fit in them together, whichever of the two overruns; a pair that
// overruns both starts the next at once (the game slows by the overrun, not by a vsync). Flips
// aren't waited for: gfx_EndFrame's GX2DrawDone has had the GPU finish the frame by then. Real
// time only: with the virtual clock (measurement runs) a frame always fits its vsync, and the
// game's own wait runs. WWHD_60FPS_PACING=vsync keeps the game's wait in real time too.
//
// WWHD_FRAME_LOG=path writes a line per frame at exit, for tools/sixty/frames.py: the swap, whole
// (w) or half (h) tick, when its work began (ms from the first), the work until the wait (ms), of
// which GX2DrawDone waited for the GPU, the scheduler thread's CPU time and idle time during it,
// the GPU thread's CPU time for the frame's swap, the vsyncs that came during the work, the wait
// (ms), and the stores the half tick's journal saw and saved (thousands; sixty.cpp).
#include "override.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

bool PPCTimer_isVirtualClock();                    // src/runtime/espresso/PPCTimer.cpp
namespace coreinit
{
	uint64 __OSIdleNanoseconds();                  // src/os/coreinit/coreinit_Thread.cpp
}
namespace wwhd::gpu
{
	uint64 VsyncCount();                           // src/gpu/null_gpu.cpp
	uint64 GpuFrameCpuNs();
	uint64 TakeFenceWaitNs();                      // src/gpu/vk/renderer.cpp
}
namespace GX2
{
	void wwhd_WaitForVsyncCount(uint64 count);     // src/os/gx2/core/GX2_Event.cpp
	uint64 wwhd_TakeDrawDoneWaitNs();
}
namespace wwhd::sixty
{
	void TakeJournalCounts(uint64& seen, uint64& saved);   // sixty.cpp
}

namespace
{
	using Clock = std::chrono::steady_clock;

	bool Paced()
	{
		static const bool paced = [] {
			const char* e = getenv("WWHD_60FPS_PACING");
			return !(e && strcmp(e, "vsync") == 0);
		}();
		return paced && !PPCTimer_isVirtualClock() && wwhd::os::SwapCount() >= wwhd::rt::SixtyFrom();   // ~0 when off
	}

	uint64 s_frameVsync = 0;                       // the vsync count when this frame's work began
	uint64 s_pairVsync = 0;                        // and when its tick's whole frame's did

	// ---- the frame log ----
	struct Frame
	{
		uint32 swap;
		bool half;
		float beganMs, workMs, drawDoneMs, cpuMs, idleMs, gpuMs, waitMs, fenceMs;
		uint32 vsyncs;
		uint32 storesSeen, storesSaved;            // the half tick's journal (thousands)
	};
	std::vector<Frame> s_frames;
	const char* s_logPath = nullptr;
	Clock::time_point s_first, s_began;
	uint64 s_cpuAtBegin = 0, s_idleAtBegin = 0;

	uint64 ThreadCpuNs()
	{
#if defined(CLOCK_THREAD_CPUTIME_ID)
		timespec ts;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
		return (uint64)ts.tv_sec * 1000000000ull + (uint64)ts.tv_nsec;
#else
		return 0;
#endif
	}

	float Ms(Clock::duration d) { return std::chrono::duration<float, std::milli>(d).count(); }

	void LogWrite()
	{
		FILE* f = fopen(s_logPath, "w");
		if (!f)
			return;
		fprintf(f, "# swap tick began_ms work_ms drawdone_ms cpu_ms idle_ms gpu_ms vsyncs wait_ms stores_k saved_k fence_ms\n");
		for (const Frame& r : s_frames)
			fprintf(f, "%u %c %.2f %.2f %.2f %.2f %.2f %.2f %u %.2f %u %u %.2f\n", r.swap, r.half ? 'h' : 'w', r.beganMs, r.workMs,
				r.drawDoneMs, r.cpuMs, r.idleMs, r.gpuMs, r.vsyncs, r.waitMs, r.storesSeen, r.storesSaved, r.fenceMs);
		fclose(f);
	}

	bool Logging()
	{
		static const bool on = [] {
			s_logPath = getenv("WWHD_FRAME_LOG");
			if (!s_logPath || !*s_logPath)
				return false;
			s_frames.reserve(1 << 16);
			atexit(LogWrite);
			at_quick_exit(LogWrite);
			return true;
		}();
		return on;
	}

	void FrameBegins()
	{
		s_frameVsync = wwhd::gpu::VsyncCount();
		if (!Logging())
			return;
		s_began = Clock::now();
		if (s_first == Clock::time_point{})
			s_first = s_began;
		s_cpuAtBegin = ThreadCpuNs();
		s_idleAtBegin = coreinit::__OSIdleNanoseconds();
		GX2::wwhd_TakeDrawDoneWaitNs();
	}
}

// fw_waitForVsync, sead's end of the frame (vtable slot 0xe4 of fw_procFrame's object): the pairs
// above in real time at 60 fps, the game's wait otherwise; and the frame log
void f_0274C874(PPCInterpreter_t* __restrict ctx)
{
	const bool logging = Logging();
	if (!logging && !Paced())
		[[clang::musttail]] return orig_f_0274C874(ctx);
	const Clock::time_point waitFrom = logging ? Clock::now() : Clock::time_point{};
	const uint64 vsync = wwhd::gpu::VsyncCount();
	Frame r{};
	if (logging && s_began != Clock::time_point{} && s_frames.size() < s_frames.capacity())
	{
		r.swap = wwhd::os::SwapCount();
		r.half = g_rtHalfTick;
		r.beganMs = Ms(s_began - s_first);
		r.workMs = Ms(waitFrom - s_began);
		r.drawDoneMs = GX2::wwhd_TakeDrawDoneWaitNs() / 1e6f;
		r.cpuMs = (ThreadCpuNs() - s_cpuAtBegin) / 1e6f;
		r.idleMs = (coreinit::__OSIdleNanoseconds() - s_idleAtBegin) / 1e6f;
		r.gpuMs = wwhd::gpu::GpuFrameCpuNs() / 1e6f;
		r.fenceMs = wwhd::gpu::TakeFenceWaitNs() / 1e6f;
		r.vsyncs = (uint32)(vsync - s_frameVsync);
		uint64 seen, saved;
		wwhd::sixty::TakeJournalCounts(seen, saved);
		r.storesSeen = (uint32)(seen / 1000);
		r.storesSaved = (uint32)(saved / 1000);
	}
	if (!Paced())
		orig_f_0274C874(ctx);
	else if (!g_rtHalfTick)
	{
		s_pairVsync = s_frameVsync;
		if (vsync == s_frameVsync)                 // in time: to the next vsync, as the game's wait
			GX2::wwhd_WaitForVsyncCount(vsync + 1);
	}
	else
		GX2::wwhd_WaitForVsyncCount(s_pairVsync + 2);
	if (logging && r.swap)
	{
		r.waitMs = Ms(Clock::now() - waitFrom);
		s_frames.push_back(r);
	}
	FrameBegins();
}
