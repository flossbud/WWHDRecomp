// 60 fps (docs/recompiler-design.md D21, M6), the first step: the game's own frame held to 30 ticks a
// second while the rest of the frame runs 60 times a second, and a state probe that compares a
// 60-tick run with a 30-tick run at equal game times.
//
// f_025F172C is m_Do_main's frame body as the HD port has it: sead's calc of the game's task
// (f_0203593C) calls it once a frame. It counts frames (a heap check every n), calls f_025E15E0, then
// fapGm_Execute (f_025D42EC): fpcM_Management (f_025DF948: process deletion, priorities and creation,
// every process's execute, every process's draw, then fapGm_After's scene, overlap and camera
// managers) and cCt_Counter (f_0200E6EC, g_Counter at 101FF558).
//
// WWHD_60FPS=1: the game presents every vsync (GX2SetSwapInterval's 2 becomes 1), so its frame runs
// 60 times a second; with WWHD_60FPS_FROM=N, from swap N on (the route reaches play at 30 fps first:
// loading and menus take game time, so a route's inputs would land elsewhere). On a whole tick (an
// even number of swaps from there) the body runs as the game's; on the half tick between, only
// fpcM_Management's draw pass: MtxInit, f_025F2B08, fpcDw_Handler (every
// process's draw) and the HD renderer's hook. The game's logic then runs 30 times a second, as at
// 30 fps, and each of its states is drawn twice; subsystems move to 60 from here (D21).
//
// The state probe (WWHD_STATE_DUMP=dir, at 30 or 60 fps): after each whole tick, dir/ticks.txt gets
// the tick, the swap count and the guest clock, every actor that executed (fopAc_Execute, f_025D475C,
// notes each) is hashed into dir/hashes.txt (whole tick, process name, address, size, FNV-1a of its
// bytes), and every WWHD_STATE_DUMP_EVERY whole ticks (default 30)
// its bytes go to dir/state.bin, with a few globals. Ticks are counted in game frames
// (wwhd::rt::GameFrame: swap N at 30 fps, swap FROM + 2(N - FROM) at 60), as the route's input is.
// The dumps are game memory: they stay on the worker.
#include "override.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <unordered_map>
#include <string>
#include <vector>

void wwhd_SetSwapInterval(uint32 interval);       // os/gx2/core/GX2_Misc.cpp

namespace
{
	bool Probe()
	{
		static const bool on = [] { const char* e = getenv("WWHD_STATE_DUMP"); return e && *e; }();
		return on;
	}

	std::vector<uint32> s_actors;                  // the actors that executed this whole tick, in order
	constexpr uint32 kGlobal = 0x474C4F42u;         // state.bin record tags: "GLOB", "ACTR"
	constexpr uint32 kActor = 0x41435452u;
	constexpr uint32 kGlobalsLow = 0x1018C0C0u, kGlobalsHigh = 0x104DA1C8u;   // .data and .bss

	// ---- the store census (WWHD_STATE_CENSUS=1 with the probe, at 60 fps) ----------------------------
	// During every half-tick frame (fw_procFrame, f_0274C264), every store generated code makes into an
	// actor that executed in the last whole tick, or into the game's .data/.bss, is counted by the host
	// address it was made from (a place in a recompiled function f_X) and what it hit: an actor's
	// process name and offset, or a global's address. dir/census.txt lists them at exit; tools/sixty/
	// census.py names the host addresses. That is what the frame changes outside the game's tick.
	struct ActorRange { uint32 low, high; uint16 name; };
	std::vector<ActorRange> s_ranges;               // the last whole tick's actors, by address
	struct CensusKey
	{
		uintptr_t from;
		uint32 name;                                // process name + 1, or 0 for a global
		uint32 where;                               // offset in the actor, or the global's address
		bool operator==(const CensusKey&) const = default;
	};
	struct CensusHash
	{
		size_t operator()(const CensusKey& k) const { return k.from * 0x9E3779B97F4A7C15ull ^ ((uint64)k.name << 32 | k.where); }
	};
	std::unordered_map<CensusKey, uint64, CensusHash> s_census;

	// WWHD_STATE_CENSUS_TRACE=addr: for stores to that address, the guest call chain too (the back
	// chain of the storing thread's stack: each frame's saved return address), up to 8 callers
	std::unordered_map<std::string, uint64> s_traces;

	void CensusTrace(uint32 ea)
	{
		static const uint32 watched = [] { const char* e = getenv("WWHD_STATE_CENSUS_TRACE"); return e ? (uint32)strtoul(e, nullptr, 16) : 0u; }();
		if (ea != watched || watched == 0)
			return;
		PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
		if (!cpu)
			return;
		char chain[160];
		int n = 0;
		uint32 frame = cpu->gpr[1];
		for (int depth = 0; depth < 8 && frame && frame < 0xF0000000u; depth++)
		{
			const uint32 caller = rd32(frame);         // the caller's frame (the back chain)
			if (!caller || caller <= frame)
				break;
			n += snprintf(chain + n, sizeof(chain) - n, " %08x", rd32(caller + 4));
			frame = caller;
		}
		s_traces[chain]++;
	}

	void CensusStore(uint32 ea, uint32 size, void* from)
	{
		CensusTrace(ea);
		CensusKey k{ (uintptr_t)from, 0, ea };
		if (ea >= kGlobalsLow && ea < kGlobalsHigh)
			;
		else
		{
			auto it = std::upper_bound(s_ranges.begin(), s_ranges.end(), ea, [](uint32 v, const ActorRange& r) { return v < r.low; });
			if (it == s_ranges.begin() || ea >= (--it)->high)
				return;
			k.name = it->name + 1u;
			k.where = ea - it->low;
		}
		s_census[k]++;
	}

	void CensusWrite()
	{
		static const std::string dir = getenv("WWHD_STATE_DUMP");
		FILE* f = fopen((dir + "/census.txt").c_str(), "w");
		if (!f)
			return;
		Dl_info info{};
		dladdr((void*)&CensusWrite, &info);
		fprintf(f, "base %lx\n", (unsigned long)(uintptr_t)info.dli_fbase);
		for (const auto& [k, n] : s_census)
			fprintf(f, "%lx %u %x %llu\n", (unsigned long)k.from, k.name, k.where, (unsigned long long)n);
		fclose(f);
		if (!s_traces.empty())
			if (FILE* t = fopen((dir + "/census_traces.txt").c_str(), "w"))
			{
				for (const auto& [chain, n] : s_traces)
					fprintf(t, "%llu%s\n", (unsigned long long)n, chain.c_str());
				fclose(t);
			}
	}

	bool Census()
	{
		static const bool on = [] { const char* e = getenv("WWHD_STATE_CENSUS"); return Probe() && e && atoi(e) == 1; }();
		return on;
	}

	uint64 Hash(uint32 ea, uint32 size)
	{
		uint64 h = 0xcbf29ce484222325ull;
		for (uint32 i = 0; i < size; i++)
			h = (h ^ rd8(ea + i)) * 0x100000001b3ull;
		return h;
	}

	void Dump(uint32 tick)
	{
		static const std::string dir = getenv("WWHD_STATE_DUMP");
		static const uint32 every = [] { const char* e = getenv("WWHD_STATE_DUMP_EVERY"); return e && atoi(e) > 0 ? (uint32)atoi(e) : 30u; }();
		static FILE* hashes = fopen((dir + "/hashes.txt").c_str(), "w");
		static FILE* state = fopen((dir + "/state.bin").c_str(), "wb");
		static FILE* ticks = fopen((dir + "/ticks.txt").c_str(), "w");
		static bool warned = false;
		if (!hashes || !state || !ticks)
			return;
		fprintf(ticks, "%u %u %llu\n", tick, wwhd::os::SwapCount(), (unsigned long long)wwhd::os::Timebase());
		const bool full = tick % every == 0;
		auto put32 = [](FILE* f, uint32 v) { fwrite(&v, 4, 1, f); };
		auto putBytes = [&](uint32 tag, uint32 a, uint32 b, uint32 ea, uint32 size) {
			put32(state, tag); put32(state, tick); put32(state, a); put32(state, b); put32(state, ea); put32(state, size);
			fwrite(memory_base + ea, 1, size, state);
		};
		// WWHD_STATE_DUMP_GLOBALS=t1,t2,...: all of .data and .bss at these ticks (3.3 MB each)
		static const std::vector<uint32> globalTicks = [] {
			std::vector<uint32> v;
			if (const char* e = getenv("WWHD_STATE_DUMP_GLOBALS"))
				for (const char* p = e; *p;)
				{
					v.push_back((uint32)strtoul(p, (char**)&p, 10));
					while (*p == ',')
						p++;
				}
			return v;
		}();
		if (std::find(globalTicks.begin(), globalTicks.end(), tick) != globalTicks.end())
			putBytes(kGlobal, 100, 0, kGlobalsLow, kGlobalsHigh - kGlobalsLow);
		if (full)
		{
			putBytes(kGlobal, 0, 0, 0x101FF558u, 12);      // g_Counter: mCounter0, mCounter1, mTimer
			putBytes(kGlobal, 1, 0, 0x1048D0A8u, 4);       // m_Do_main's frame counter
			putBytes(kGlobal, 2, 0, 0x104B45F8u, 48);      // the camera matrix RenderDisplay_draw copied last
			putBytes(kGlobal, 3, 0, 0x101FF9D4u, 12);      // cM_rnd's state (f_02019788: Wichmann-Hill r0, r1, r2)
		}
		for (uint32 actor : s_actors)
		{
			const uint32 profile = rd32(actor + 0x10);
			const uint16 name = rd16(actor + 0x08);
			const uint32 size = profile ? rd32(profile + 0x10) : 0;
			if (!warned && (!profile || rd16(profile + 0x08) != name || size > 0x40000))
			{
				warned = true;
				cemuLog_log(LogType::Force, "wwhd sixty: actor {:08x} has process name {} but profile {:08x} says {} (size {:x})",
					actor, name, profile, profile ? rd16(profile + 0x08) : 0, size);
			}
			if (size == 0 || size > 0x40000)
				continue;
			fprintf(hashes, "%u %u %08x %x %016llx\n", tick, name, actor, size, (unsigned long long)Hash(actor, size));
			if (full)
				putBytes(kActor, name, 0, actor, size);
		}
		fflush(hashes);
		fflush(ticks);
		if (full)
			fflush(state);
	}

	// the draw pass of fpcM_Management (f_025DF948), as it calls it: MtxInit, f_025F2B08,
	// fpcDw_Handler(fpcM_DrawIterater, fpcM_Draw), and f_02715310(some_gfx_ptr) when that is set
	void DrawPass(PPCInterpreter_t* __restrict ctx)
	{
		ctx->spr.LR = 0x025DF970u;
		f_0200FAC4(ctx);
		ctx->spr.LR = 0x025DF974u;
		f_025F2B08(ctx);
		GPR(3) = 0x025DF908u;
		GPR(4) = 0x025DF904u;
		ctx->spr.LR = 0x025DFA38u;
		f_025DE37C(ctx);
		GPR(3) = rd32(0x101F8344u);                    // some_gfx_ptr, the hook's argument
		if (GPR(3) != 0)
		{
			ctx->spr.LR = 0x025DFA5Cu;
			f_02715310(ctx);
		}
	}
}

// m_Do_main's frame body (see the top)
void f_025F172C(PPCInterpreter_t* __restrict ctx)
{
	const uint32 from = wwhd::rt::SixtyFrom();      // ~0 when 60 fps is off
	if (from == ~0u && !Probe())
		[[clang::musttail]] return orig_f_025F172C(ctx);
	const uint32 swap = wwhd::os::SwapCount();
	const uint32 lr = ctx->spr.LR;
	static uint32 lastSwap = ~0u;
	if (swap == lastSwap)
	{
		static bool warned = false;
		if (!warned)
			cemuLog_log(LogType::Force, "wwhd sixty: the frame body ran twice at swap {}: whole and half ticks are out of step", swap);
		warned = true;
	}
	lastSwap = swap;
	if (swap == from && from != 0)
	{
		wwhd_SetSwapInterval(1);                       // from here on the game presents every vsync
		cemuLog_log(LogType::Force, "wwhd sixty: 60 fps from swap {}", swap);
	}
	if (swap >= from && (swap - from) % 2 != 0)
	{
		// a half tick: WWHD_60FPS_HALF=draw (default) runs the draw pass, =none nothing of this frame
		static const bool draw = [] { const char* e = getenv("WWHD_60FPS_HALF"); return !e || strcmp(e, "none") != 0; }();
		if (draw)
			DrawPass(ctx);
		ctx->spr.LR = lr;
		return;
	}
	s_actors.clear();
	orig_f_025F172C(ctx);
	if (Probe())
		Dump(wwhd::rt::GameFrame(swap));
	if (Census())
	{
		s_ranges.clear();
		for (uint32 actor : s_actors)
			if (const uint32 profile = rd32(actor + 0x10); profile && rd32(profile + 0x10) <= 0x40000)
				s_ranges.push_back({ actor, actor + rd32(profile + 0x10), rd16(actor + 0x08) });
		std::sort(s_ranges.begin(), s_ranges.end(), [](const ActorRange& a, const ActorRange& b) { return a.low < b.low; });
	}
}

// fw_procFrame, sead's procFrame_: one whole frame (the tick, the draw, the present, the vsync wait).
// With the store census on, a half tick's frame is watched from end to end.
void f_0274C264(PPCInterpreter_t* __restrict ctx)
{
	const uint32 from = wwhd::rt::SixtyFrom();
	const uint32 swap = wwhd::os::SwapCount();
	if (!Census() || swap < from || (swap - from) % 2 == 0)
		[[clang::musttail]] return orig_f_0274C264(ctx);
	static bool once = [] { atexit(CensusWrite); at_quick_exit(CensusWrite); return true; }();
	(void)once;
	g_rtStoreCensus = CensusStore;
	g_rtJournalOn = true;
	orig_f_0274C264(ctx);
	g_rtJournalOn = false;
	g_rtStoreCensus = nullptr;
}

// fopAc_Execute: every actor's execute goes through it (from fpcM_Management's execute pass)
void f_025D475C(PPCInterpreter_t* __restrict ctx)
{
	if (Probe())
		s_actors.push_back(GPR(3));
	[[clang::musttail]] return orig_f_025D475C(ctx);
}
