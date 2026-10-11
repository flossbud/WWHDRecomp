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
// Frame dips without slowdown (the owner's ask, docs/research/gpu-plan.md; always on in real time at 60 since t-fpscap,
// the owner's choice: no setting; WWHD_60FPS_KEEPSPEED=0 turns it off for A/Bs and tests only, logged):
// the game's ticks keep a schedule (one each 33.3 ms, resynced after a hitch of over 250 ms), and a half tick's frame
// that would end after the next tick is due (its expected work, the last half frames' average, from now) is dropped:
// its logic and its actor draws run (the converted processes' half steps, the half tick's late rules; the draws take
// numbers from the game's random stream, so they stay: WWHD_60FPS_DROPDRAWS=1 drops them too, a test), the game's
// render jobs (RenderDisplay draw and calcGPU, which build the lists of its picture) don't. The game draws a frame's
// picture in the next frame (fw_procDraw, sead's first step, from the lists built in the frame before) and presents it
// at that frame's end, so the dropped frame itself still draws and presents the frame before's picture, and the frame
// after it draws and presents nothing (s_noPicture: game_procPresent skipped, its swap still counted, so the frame
// numbers and the whole/half rhythm stay). So a slow stretch shows fewer frames instead of slow motion, each picture at
// its own time. (Until q-deckblack the dropped frame skipped its own present and the frame after drew from lists never
// built: a black frame for every dropped one, the Steam Deck's flicker.) WWHD_60FPS_DROPTEST=k (a test, the virtual
// clock too): every k-th half tick's frame dropped, so the checks can show that a dropped frame changes no game state
// (tools/sixty/tests/droptest.sh) and leaves no black picture (blacktest.sh).
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
	uint64 __OSCoreCpuNanoseconds(uint32 core);    // (three host threads: cores 0 and 2's host threads)
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
	bool GX2DrawDone();
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

	// 30 fps in real time paced as 60 is (f_0274C874 below; t-30fps): WWHD_30_PACING=0 keeps the game's own wait
	bool Paced30()
	{
		static const bool on = [] { const char* e = getenv("WWHD_30_PACING"); return !(e && atoi(e) == 0); }();
		return on && !PPCTimer_isVirtualClock() && wwhd::rt::SixtyFrom() == ~0u;
	}

	uint64 s_dropped = 0, s_halves = 0;            // half frames dropped, and all half frames (the real-time log)
	bool s_capDrop = false;                        // this frame was dropped by the frame rate cap (WWHD_FPS_CAP), not late
	bool s_noPicture = false;                      // this frame draws and presents nothing: the frame before was dropped
	bool s_noPictureCap = false;                   //   (its picture's lists were never built), by the cap (on time)
	Clock::time_point s_tickDue{}, s_frameStart{};
	double s_halfWorkMs = 8.0;                     // the half frames' work, a running average
	uint64 s_frameVsync = 0;                       // the vsync count when this frame's work began
	uint64 s_pairVsync = 0;                        // and when its tick's whole frame's did

	// ---- the frame log ----
	struct Frame
	{
		uint32 swap;
		bool half, dropped = false;
		float beganMs, workMs, drawDoneMs, cpuMs, idleMs, gpuMs, waitMs, fenceMs;
		float core0Ms, core2Ms;                    // three host threads: cores 0 and 2's host CPU over the frame (0 with one)
		uint32 vsyncs;
		uint32 storesSeen, storesSaved;            // the half tick's journal (thousands)
	};
	std::vector<Frame> s_frames;
	const char* s_logPath = nullptr;
	Clock::time_point s_first, s_began;
	uint64 s_cpuAtBegin = 0, s_idleAtBegin = 0, s_core0AtBegin = 0, s_core2AtBegin = 0;

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
		fprintf(f, "# swap tick began_ms work_ms drawdone_ms cpu_ms idle_ms gpu_ms vsyncs wait_ms stores_k saved_k fence_ms core0_ms core2_ms\n");
		for (const Frame& r : s_frames)
			fprintf(f, "%u %c %.2f %.2f %.2f %.2f %.2f %.2f %u %.2f %u %u %.2f %.2f %.2f\n", r.swap, r.dropped ? 'd' : r.half ? 'h' : 'w', r.beganMs, r.workMs,
				r.drawDoneMs, r.cpuMs, r.idleMs, r.gpuMs, r.vsyncs, r.waitMs, r.storesSeen, r.storesSaved, r.fenceMs, r.core0Ms, r.core2Ms);
		fclose(f);
	}

	bool Logging()
	{
		static const bool on = [] {
			s_logPath = getenv("WWHD_FRAME_LOG");
			if (!s_logPath || !*s_logPath)
				return false;
			s_frames.reserve(1 << 18);                 // an hour at 60 (tools/sixty/tests/soak.sh)
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
		s_core0AtBegin = coreinit::__OSCoreCpuNanoseconds(0);
		s_core2AtBegin = coreinit::__OSCoreCpuNanoseconds(2);
		GX2::wwhd_TakeDrawDoneWaitNs();
	}
}

// fw_waitForVsync, sead's end of the frame (vtable slot 0xe4 of fw_procFrame's object): the pairs
// above in real time at 60 fps, the game's wait otherwise; and the frame log
void f_0274C874(PPCInterpreter_t* __restrict ctx)
{
	const bool logging = Logging();
	if (!logging && !Paced() && !Paced30())
		[[clang::musttail]] return orig_f_0274C874(ctx);
	const Clock::time_point waitFrom = logging ? Clock::now() : Clock::time_point{};
	const uint64 vsync = wwhd::gpu::VsyncCount();
	Frame r{};
	if (logging && s_began != Clock::time_point{} && s_frames.size() < s_frames.capacity())
	{
		r.swap = wwhd::os::SwapCount();
		r.half = g_rtHalfTick;
		r.dropped = s_noPicture;                   // the frame log's d: a frame that presented nothing
		r.beganMs = Ms(s_began - s_first);
		r.workMs = Ms(waitFrom - s_began);
		r.drawDoneMs = GX2::wwhd_TakeDrawDoneWaitNs() / 1e6f;
		r.cpuMs = (ThreadCpuNs() - s_cpuAtBegin) / 1e6f;
		r.idleMs = (coreinit::__OSIdleNanoseconds() - s_idleAtBegin) / 1e6f;
		r.gpuMs = wwhd::gpu::GpuFrameCpuNs() / 1e6f;
		r.fenceMs = wwhd::gpu::TakeFenceWaitNs() / 1e6f;
		r.core0Ms = (coreinit::__OSCoreCpuNanoseconds(0) - s_core0AtBegin) / 1e6f;
		r.core2Ms = (coreinit::__OSCoreCpuNanoseconds(2) - s_core2AtBegin) / 1e6f;
		r.vsyncs = (uint32)(vsync - s_frameVsync);
		uint64 seen, saved;
		wwhd::sixty::TakeJournalCounts(seen, saved);
		r.storesSeen = (uint32)(seen / 1000);
		r.storesSaved = (uint32)(saved / 1000);
	}
	if (g_rtHalfTick && !wwhd::pacing::g_dropFrame && !s_noPicture && s_frameStart != Clock::time_point{})
		s_halfWorkMs += (std::chrono::duration<double, std::milli>(Clock::now() - s_frameStart).count() - s_halfWorkMs) / 8;
	// at 30 in real time (t-30fps): the game's wait (orig: GX2WaitForVsync until GX2GetSwapStatus shows no swap pending)
	// slipped a whole vsync whenever the GPU thread hadn't flipped the last frame yet, so on the worker 30 ran at 27-28.7
	// with 14.5 ms of work a frame; as at 60, the frame's own slot instead: two vsyncs after the last frame's, or none
	// when it's late already (or dropped: the frame skip). WWHD_30_PACING=0: the game's wait
	static uint64 s_slot30 = 0;
	if (Paced30())
	{
		if (!wwhd::pacing::g_dropFrame && !s_noPicture && s_slot30 && vsync < s_slot30 + 2)
		{
			GX2::wwhd_WaitForVsyncCount(s_slot30 + 2);
			s_slot30 += 2;
		}
		else
			s_slot30 = vsync;                          // late, dropped or the first: the schedule from here
	}
	else if (Paced() && ((wwhd::pacing::g_dropFrame && !s_capDrop) || (s_noPicture && !s_noPictureCap)))
		;                                          // dropped late, or the frame after: behind already, the next tick at once
	                                               // (the frame rate cap's are on time: they wait as shown ones, below)
	else if (!Paced())
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

namespace wwhd::pacing
{
	bool g_dropFrame = false;

	bool DropDraws()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_DROPDRAWS"); return e && atoi(e) == 1; }();
		return g_dropFrame && on;
	}

	// ---- frame skip below 30 (t-frameskip, the owner's ask): a whole tick's frame that starts more than a tick behind
	// its schedule is dropped as keep-speed drops half frames (its logic and actor draws run, its render jobs don't, and
	// the frame after it draws and presents nothing; swaps counted), so a machine that renders 20 frames a second still
	// plays at full speed, only choppier.
	// A floor: never more than 100 ms without a shown frame. A hitch over 250 ms (a load) resyncs instead. Nothing here
	// renders into guest memory (renderer.cpp: rendering stays in host images; no gameplay GX2CopySurface on the routes),
	// so a dropped frame leaves no read-back value behind. It only helps when rendering is what's slow: when the logic
	// itself can't make 30 ticks a second the game slows as before, and the log says which. Real time only (the virtual
	// clock is never behind). WWHD_FRAMESKIP=0 turns it off (A/Bs and tests); WWHD_FRAMESKIP_LATE=ms the lateness (a
	// tick); WWHD_FRAMESKIP_TEST=k drops every k-th whole frame, -k all but every k-th (a test, the virtual clock too:
	// droptest.sh). Only in play (scene 7, from fopScnM_ChangeReq): with the title's frames dropped its own Link came 5
	// ticks later (its opening progresses on its rendering). At 30 as at 60 every frame makes one swap: the dropped
	// one's present, the frame after it SkipSwap (q-deckblack; gfx_EndFrame, under game_procPresent, is GX2SwapScanBuffers'
	// only caller, so the next real swap that gx2's GX2_SuppressNextSwap left out after a dropped frame, until then, was
	// the next frame's present)
	Clock::time_point s_lastShown{}, s_skipPeriod{};
	uint32 s_scene = ~0u;                          // the last scene asked for: 8 the opening (title), 9 the file select, 7 play

	uint32 s_shownSinceScene = 0;                  // frames shown since the last scene change was asked for

	void SceneRequested(uint32 proc)
	{
		s_scene = proc;
		s_shownSinceScene = 0;
	}
	uint32 s_wholeSkipped = 0, s_wholeFrames = 0, s_periodTicks = 0;
	Clock::time_point s_wholeStart{};              // the last whole frame's start, and whether it was skipped
	bool s_wholeWasSkipped = false;
	double s_skippedMs = 0;                        // the skipped whole frames' length this period (to the next whole frame)

	bool WholeSkip(uint32 tick, Clock::time_point now, Clock::time_point due)
	{
		static const int test = [] { const char* e = getenv("WWHD_FRAMESKIP_TEST"); return e ? atoi(e) : 0; }();   // -k: k-1 of every k
		static const bool on = [] {
			const char* e = getenv("WWHD_FRAMESKIP");
			if (e && atoi(e) == 0)
				cemuLog_log(LogType::Force, "wwhd pacing: WARNING: WWHD_FRAMESKIP=0, a test override: below 30 frames a second the game slows down");
			return !(e && atoi(e) == 0);
		}();
		if (s_scene != 7)                               // only in play: the logo, the opening and the file select
			return false;                               // go on as authored (the opening's progress waits on its rendering)
		// nor in a scene's first 60 shown frames (a precaution: its first frames' render jobs set up its rendering)
		if (s_shownSinceScene < 60)
			return false;
		if (test > 0)
			return tick % (uint32)test == 0;
		if (test < 0)
			return tick % (uint32)-test != 0;
		if (!on || PPCTimer_isVirtualClock())
			return false;
		static const auto behind = std::chrono::microseconds([] { const char* e = getenv("WWHD_FRAMESKIP_LATE"); return e ? atoi(e) * 1000 : 33333; }());
		const auto late = now - due;
		return late > behind && late < std::chrono::milliseconds(250) &&
			s_lastShown != Clock::time_point{} && now - s_lastShown < std::chrono::milliseconds(100);
	}

	// every 10 s while frames are being skipped: how many, and the game's speed (ticks against 30 a second): a speed
	// under 95% with frames skipped means the logic itself is the limit
	void SkipLog(Clock::time_point now)
	{
		s_periodTicks++;
		if (s_skipPeriod == Clock::time_point{})
			s_skipPeriod = now;
		const double secs = std::chrono::duration<double>(now - s_skipPeriod).count();
		if (secs < 10.0)
			return;
		if (s_wholeSkipped)
		{
			// a skipped frame (its logic, no render jobs) longer than a tick: that, not rendering, is the limit
			const double speed = 100.0 * s_periodTicks / secs / 30.0, skippedMs = s_skippedMs / s_wholeSkipped;
			cemuLog_log(LogType::Force, "wwhd pacing: frame skip: {} of {} whole frames skipped in {:.0f} s, game speed {:.0f}%; a skipped frame "
				"took {:.1f} ms{}", s_wholeSkipped, s_wholeFrames, secs, speed, skippedMs, speed >= 95.0 ? " (rendering was the limit)" :
				skippedMs > 33.3 ? ": even without rendering a tick takes longer than 33.3 ms here (the game's logic, or a wait for the GPU)" :
				": the shown frames are too slow for the floor of one every 100 ms");
		}
		s_wholeSkipped = s_wholeFrames = s_periodTicks = 0;
		s_skippedMs = 0;
		s_skipPeriod = now;
	}

	// a whole tick's frame (30 fps, or 60's whole frames from FrameStart): the schedule and the skip
	void WholeFrame(uint32 tick, Clock::time_point now)
	{
		if (s_tickDue == Clock::time_point{} || now - s_tickDue > std::chrono::milliseconds(250))
			s_tickDue = now;
		if (s_wholeWasSkipped && s_wholeStart != Clock::time_point{})
			s_skippedMs += std::chrono::duration<double, std::milli>(now - s_wholeStart).count();
		g_dropFrame = WholeSkip(tick, now, s_tickDue);
		s_wholeStart = now;
		s_wholeWasSkipped = g_dropFrame;
		s_wholeFrames++;
		if (g_dropFrame)
			s_wholeSkipped++;
		SkipLog(now);
		s_tickDue += std::chrono::microseconds(33333);
	}

	// at 30 fps (sixty.cpp's frame body, real time and WWHD_FRAMESKIP_TEST)
	void FrameStart30(uint32 swap)
	{
		const Clock::time_point now = Clock::now();
		s_noPicture = g_dropFrame, s_noPictureCap = false;
		g_dropFrame = false;
		WholeFrame(swap, now);
	}

	void FrameShown()
	{
		s_lastShown = Clock::now();
		s_shownSinceScene++;
	}

	void FrameStart(uint32 swap, uint32 from, bool half)
	{
		static const uint32 test = [] { const char* e = getenv("WWHD_60FPS_DROPTEST"); return e ? (uint32)atoi(e) : 0u; }();
		static const bool keep = [] {
			const char* e = getenv("WWHD_60FPS_KEEPSPEED");
			if (e && atoi(e) == 0)
				cemuLog_log(LogType::Force, "wwhd pacing: WARNING: WWHD_60FPS_KEEPSPEED=0, a test override: frames that dip slow the game down");
			return !(e && atoi(e) == 0);
		}();
		const Clock::time_point now = Clock::now();
		s_frameStart = now;
		s_noPicture = g_dropFrame, s_noPictureCap = s_capDrop;
		g_dropFrame = false;
		s_capDrop = false;
		if (!half)
		{
			// the tick's whole frame: it was due at s_tickDue; the next one 33.3 ms after (resynced after a long hitch:
			// a load or a pause plays on from where it is, it doesn't race to catch up); more than a tick behind, it's
			// skipped (above)
			WholeFrame((swap - from) / 2, now);
			return;
		}
		s_halves++;
		// WWHD_FPS_CAP=40|50 (the settings page's frame rate; t-fpscap): 60 ticks' logic, every whole frame shown, and of
		// every three ticks' half frames one shown (40: four frames in six) or two (50: five in six), the rest dropped as
		// keep-speed drops them (their logic runs, their render jobs and picture don't). An even pattern, not even time:
		// motion is smoother at 30 or 60. Its frames wait as shown ones in real time (f_0274C874)
		static const uint32 cap = [] { const char* e = getenv("WWHD_FPS_CAP"); const int v = e ? atoi(e) : 60; return v == 40 || v == 50 ? (uint32)v : 0u; }();
		const uint32 tick = (swap - from) / 2;
		if (test)
			g_dropFrame = tick % test == 0;
		else
		{
			if (cap)                                   // (under the virtual clock too: droptest.sh; no check sets it)
				g_dropFrame = s_capDrop = cap == 40 ? tick % 3 != 0 : tick % 3 == 0;
			if (!g_dropFrame && keep && Paced())       // keep-speed: a half frame that would end after the next tick is due
				g_dropFrame = now + std::chrono::microseconds((long long)(s_halfWorkMs * 1000)) > s_tickDue;
		}
		if (g_dropFrame)
			s_dropped++;
		if (s_halves % 600 == 0 && (keep || test || cap))
			cemuLog_log(LogType::Force, "wwhd pacing: {} of the last 600 half frames dropped (half frame work ~{:.1f} ms)",
				s_dropped, s_halfWorkMs);
		if (s_halves % 600 == 0)
			s_dropped = 0;
	}
}

// game_procPresent (gfx_EndFrame: the outputs' copies, GX2DrawDone, the swap, ProcUI): the frame after a dropped one
// presents nothing (it has no picture: fw_procDraw, below), but keeps the GX2DrawDone (q-lazyring). The game submits
// display lists in buffers of its own (GX2DirectCallDisplayList) and writes them again in a later frame; that is safe
// only because each frame's present waits in GX2DrawDone until the GPU thread has read everything submitted. Without it
// the next frames overwrote lists the GPU thread hadn't reached (up to 13 submissions behind), which it then parsed as
// garbage: "unknown PM4 packet 00" in most real-time runs since keep-speed became the default (27d276e), and a crash in
// null_gpu.cpp's packet() when the render thread fell behind (FSR 3 on the worker's iGPU, with the lazy DrawDone: a third
// of the runs). WWHD_60FPS_DROPDRAWDONE=0: left out (a test)
void f_020350C4(PPCInterpreter_t* __restrict ctx)
{
	if (!s_noPicture)
	{
		wwhd::pacing::FrameShown();
		[[clang::musttail]] return orig_f_020350C4(ctx);
	}
	static const bool drawDone = [] { const char* e = getenv("WWHD_60FPS_DROPDRAWDONE"); return !(e && atoi(e) == 0); }();
	if (drawDone)
		GX2::GX2DrawDone();
	wwhd::os::SkipSwap();
}

// fw_procDraw (sead's frame, vtable slot 0xd4, its first step): the TV's and the GamePad's draw passes and the copies to
// the scan buffers, from the lists RenderDisplay draw and calcGPU built in the frame before: the picture this frame
// presents. The frame after a dropped one leaves it out (q-deckblack): its lists were never built. Before, it drew
// them anyway and presented black: one black frame for every dropped one (keep-speed, the 40/50 caps and the frame skip
// alike), the flicker on the Steam Deck, which drops often (never seen on a PC that keeps 60)
void f_0274C67C(PPCInterpreter_t* __restrict ctx)
{
	if (!s_noPicture)
		[[clang::musttail]] return orig_f_0274C67C(ctx);
}

// RenderDisplay draw and calcGPU (the game's render job lists: the next frame's picture): not for a dropped frame
void f_0272A8C4(PPCInterpreter_t* __restrict ctx)
{
	if (!wwhd::pacing::g_dropFrame)
		[[clang::musttail]] return orig_f_0272A8C4(ctx);
}

void f_0272AD80(PPCInterpreter_t* __restrict ctx)
{
	if (!wwhd::pacing::g_dropFrame)
		[[clang::musttail]] return orig_f_0272AD80(ctx);
}
