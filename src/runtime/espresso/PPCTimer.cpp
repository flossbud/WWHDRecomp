// PPCTimer.cpp (docs/recompiler-design.md D18, D19): the timer: host time, or the virtual clock. Cemu's
// src/Cafe/HW/Espresso/PPCTimer.cpp (Mozilla Public License 2.0), forked: wwhd-null links this in
// place of Cemu's object, at its position in the link (so its guest-memory slots keep their
// addresses) and under Cemu's names (so Cemu's code that calls it reaches ours). Route traces, GPU
// commands and sound must stay as Cemu's (tools/reference/stream_check.sh).
#include "Cafe/HW/Espresso/Const.h"
#include "config/ActiveSettings.h"
#include <chrono>

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
static const bool s_virtualClockEnabled = getenv("CEMU_VIRTUAL_CLOCK") != nullptr;
static std::atomic<uint64> s_virtualCycles{0};

bool PPCTimer_isVirtualClock()
{
	return s_virtualClockEnabled;
}

void PPCTimer_advanceVirtualClock(uint64 cycles)
{
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
