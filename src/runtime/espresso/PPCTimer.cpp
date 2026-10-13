// PPCTimer.cpp (docs/recompiler-design.md D18, D19): the timer: host time, or the virtual clock.
// Cemu's src/Cafe/HW/Espresso/PPCTimer.cpp (Mozilla Public License 2.0), forked: wwhd-null links
// this in place of Cemu's object, under Cemu's names (so Cemu's code that calls it reaches ours).
// Route traces, GPU commands and sound must stay as Cemu's (tools/reference/stream_check.sh).
#include "Cafe/HW/Espresso/Const.h"
#include "config/ActiveSettings.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "../../os/savestate.h"
#include <chrono>
#include <cstdio>
#include <unistd.h>

// wwhd: host time from std::chrono::steady_clock (design D19) in place of rdtsc: portable, and no
// 3-second measurement of the TSC's frequency at start. The "TSC" of this interface is steady-clock
// nanoseconds.
static uint64 HostNanoseconds()
{
	return (uint64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::atomic<uint64> s_startNanoseconds{ HostNanoseconds() };

void PPCTimer_init()
{
	s_startNanoseconds = HostNanoseconds();
}

void PPCTimer_start()
{
	s_startNanoseconds = HostNanoseconds();
}

uint64 PPCTimer_getRawTsc()
{
	return HostNanoseconds();
}

uint64 PPCTimer_microsecondsToTsc(uint64 us)
{
	return us * 1000;
}

uint64 PPCTimer_tscToMicroseconds(uint64 ns)
{
	return ns / 1000;
}

bool PPCTimer_isReady()
{
	return true;
}

void PPCTimer_waitForInit()
{
}

// wwhd-reference: deterministic virtual clock (CEMU_VIRTUAL_CLOCK=1).
// Guest time advances only with executed guest instructions (1 instruction = 1 core cycle) and,
// when every guest thread is idle, jumps straight to the next timed event. Nothing reads the
// host clock, so two runs with the same input see the same guest time.
// wwhd-play (src/runtime/build_kind.cpp; weak: a build without the recompiled program has none) can't yield on the exact
// instruction, so it refuses the virtual clock rather than run a check that can't match
[[gnu::weak]] extern const bool g_rtLooseBuild;
static const bool s_virtualClockEnabled = [] {
	if (!getenv("CEMU_VIRTUAL_CLOCK"))
		return false;
	if (&g_rtLooseBuild && g_rtLooseBuild)
	{
		fprintf(stderr, "wwhd-play is the real-time build: the virtual clock (CEMU_VIRTUAL_CLOCK) needs wwhd-null\n");
		_exit(2);
	}
	return true;
}();
static std::atomic<uint64> s_virtualCycles{0};
// wwhd: WWHD_VIRTUAL_SPEED=n (default 1) runs n instructions per guest cycle: an emulated CPU n times
// as fast. For the 60 fps measurements (D21, tools/sixty): at 60 frames a second each frame has to
// fit in one vsync of guest time, as it does on a PC in real time; both runs of a comparison use the
// same n. Every check uses 1. At N frames a tick (WWHD_STEPS, U4) the vsync is each 1/(30 N) s from the 60 fps switch on
// (U5, LatteTiming_setVsyncSteps), so from there the speed is n N/2, rounded up (n 3: 5 at 3 steps, 6 at 4): a step's
// frame fits its vsync as a frame at 60 fits n's. Before the switch, and at 2, n
static const uint64 s_virtualSpeed = [] {
	const char* e = getenv("WWHD_VIRTUAL_SPEED");
	return e && atoi(e) > 1 ? (uint64)atoi(e) : 1ull;
}();
static uint64 s_virtualRemainder = 0;    // instructions not yet a whole cycle (the scheduler's thread)
uint32 LatteTiming_getVsyncSteps();      // gpu/null_gpu.cpp: frames a tick, 2 until the switch

bool PPCTimer_isVirtualClock()
{
	return s_virtualClockEnabled;
}

void PPCTimer_advanceVirtualClock(uint64 cycles)
{
	const uint64 steps = LatteTiming_getVsyncSteps();
	const uint64 speed = steps == 2 ? s_virtualSpeed : (s_virtualSpeed * steps + 1) / 2;
	if (speed > 1)
	{
		cycles += s_virtualRemainder;
		s_virtualRemainder = cycles % speed;
		cycles /= speed;
	}
	s_virtualCycles.fetch_add(cycles);
}

void PPCTimer_advanceVirtualClockTo(uint64 cycles)
{
	uint64 cur = s_virtualCycles.load();
	while (cur < cycles && !s_virtualCycles.compare_exchange_weak(cur, cycles)) {}
}

// thread safe
uint64 PPCTimer_getFromRDTSC()
{
	if (s_virtualClockEnabled)
		return s_virtualCycles.load();
	// core cycles since PPCTimer_start (ns * CORE_CLOCK / 1e9 without overflowing), then Cemu's
	// timer scaling (x8, shifted down by the configured factor: 3 is real speed)
	uint64 ns = HostNanoseconds() - s_startNanoseconds.load(std::memory_order_relaxed);
	uint64 cycles = ns / 1000000000ULL * Espresso::CORE_CLOCK + ns % 1000000000ULL * Espresso::CORE_CLOCK / 1000000000ULL;
	return (cycles << 3) >> ActiveSettings::GetTimerShiftFactor();
}

// wwhd: save states (os/savestate.h): guest time goes on from the state's, whichever clock either process runs on (a
// state saved in real time on the Steam Deck loads under the virtual clock on a worker: guest time is the timer's value
// either way), and the calendar is the state's
static void StateRecord(wwhd::state::Archive& ar)
{
	uint64 now = PPCTimer_getFromRDTSC(), remainder = s_virtualRemainder;
	ar.Pod(now);
	ar.Pod(remainder);
	ar.Pod(ppcCyclesSince2000);
	ar.Pod(ppcCyclesSince2000TimerClock);
	ar.Pod(ppcCyclesSince2000_UTC);
	if (!ar.Loading() || !ar.Ok())
		return;
	if (s_virtualClockEnabled)
	{
		s_virtualCycles = now;
		s_virtualRemainder = remainder;
		return;
	}
	// real time: the start such that PPCTimer_getFromRDTSC reads `now` at this moment
	const uint64 cycles = (now << ActiveSettings::GetTimerShiftFactor()) >> 3;
	const uint64 ns = cycles / Espresso::CORE_CLOCK * 1000000000ULL + cycles % Espresso::CORE_CLOCK * 1000000000ULL / Espresso::CORE_CLOCK;
	s_startNanoseconds = HostNanoseconds() - ns;
}
WWHD_STATE_RECORD("timer", StateRecord);
