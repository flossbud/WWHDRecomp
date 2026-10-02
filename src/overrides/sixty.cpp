// 60 fps (docs/recompiler-design.md D21, M6): which frames are the game's ticks, and a state probe
// that compares a 60-tick run with a 30-tick run at equal game times.
//
// f_025F172C is m_Do_main's frame body as the HD port has it: sead's calc of the game's task
// (f_0203593C) calls it once a frame. It counts frames (a heap check every n), calls f_025E15E0, then
// fapGm_Execute (f_025D42EC): fpcM_Management (f_025DF948: process deletion, priorities and creation,
// every process's execute, every process's draw, then fapGm_After's scene, overlap and camera
// managers) and cCt_Counter (f_0200E6EC, g_Counter at 101FF558).
//
// WWHD_60FPS=1: the game presents every vsync (GX2SetSwapInterval's 2 becomes 1), so its frame runs
// 60 times a second; with WWHD_60FPS_FROM=N, from swap N on (the route reaches play at 30 fps first:
// loading and menus take game time, so a route's inputs would land elsewhere). A frame an even
// number of swaps from there is a whole tick; one between is a half tick (g_rtHalfTick, set here
// for the whole frame by sead's fw_procFrame, f_0274C264). On a half tick the game's frame runs, and
// the instructions listed in config/US_v0/tick_rules.txt (the process manager's create, delete and
// execute passes, the play scene's simulation, the counters, the HD per-frame logic) don't: the
// game's logic runs 30 times a second, as at 30 fps, and each of its states is drawn twice. Systems
// move to 60 from there, one at a time (D21). WWHD_60FPS_HALF=none skips the frame body on half
// ticks instead, nothing of the game's frame at all, to compare.
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
	std::vector<uint32> s_processes;               // every process that executed this whole tick
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
	PPCInterpreter_t* s_frameThread = nullptr;      // the main thread: the one running fw_procFrame
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

	// The guest call chain of the storing thread: each frame's saved return address along the stack's
	// back chain, up to `depth` callers, as text
	std::string GuestChain(int depth)
	{
		PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
		if (!cpu)
			return "";
		char chain[16 * 9 + 1];
		int n = 0;
		uint32 frame = cpu->gpr[1];
		for (int d = 0; d < depth && frame && frame < 0xF0000000u; d++)
		{
			const uint32 caller = rd32(frame);         // the caller's frame (the back chain)
			if (!caller || caller <= frame)
				break;
			n += snprintf(chain + n, sizeof(chain) - n, " %08x", rd32(caller + 4));
			frame = caller;
		}
		return std::string(chain, n);
	}

	// WWHD_STATE_CENSUS_TRACE=addr: for stores to that address, the guest call chain (8 callers);
	// WWHD_STATE_CENSUS_CHAINS=1: for every store into an actor or g_dComIfG_gameInfo, the target
	// and the first 4 callers (dir/census_chains.txt), to find the call sites to put rules on
	std::unordered_map<std::string, uint64> s_traces, s_chains;
	constexpr uint32 kGameInfo = 0x1046F0B0u, kGameInfoSize = 0x6000u;   // f_025200D4's singleton

	void CensusTrace(uint32 ea)
	{
		static const uint32 watched = [] { const char* e = getenv("WWHD_STATE_CENSUS_TRACE"); return e ? (uint32)strtoul(e, nullptr, 16) : 0u; }();
		if (ea != watched || watched == 0)
			return;
		s_traces[GuestChain(8)]++;
	}

	// WWHD_STATE_CENSUS_HEAP=1: the main thread's stores anywhere else too (heap objects: layouts,
	// effects, sound), counted by the first 4 callers, as "heap" in census_chains.txt
	void CensusHeap(uint32 ea)
	{
		static const bool on = [] { const char* e = getenv("WWHD_STATE_CENSUS_HEAP"); return e && atoi(e) == 1; }();
		PPCInterpreter_t* cpu = on ? PPCInterpreter_getCurrentInstance() : nullptr;
		if (!cpu || cpu != s_frameThread)
			return;
		const uint32 sp = cpu->gpr[1];
		if (ea + 0x10000 > sp && ea < sp + 0x10000)  // the stack (near its pointer)
			return;
		s_chains["heap" + GuestChain(4)]++;
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
			{
				CensusHeap(ea);
				return;
			}
			k.name = it->name + 1u;
			k.where = ea - it->low;
		}
		s_census[k]++;
		static const bool chains = [] { const char* e = getenv("WWHD_STATE_CENSUS_CHAINS"); return e && atoi(e) == 1; }();
		if (chains && (k.name || (ea >= kGameInfo && ea < kGameInfo + kGameInfoSize)))
		{
			char target[48];
			if (k.name)
				snprintf(target, sizeof(target), "actor %u +%x", k.name - 1, k.where & ~3u);
			else
				snprintf(target, sizeof(target), "gameInfo +%x", (ea - kGameInfo) & ~3u);
			s_chains[std::string(target) + GuestChain(4)]++;
		}
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
		for (const auto& [name, map] : { std::pair{ "/census_traces.txt", &s_traces }, std::pair{ "/census_chains.txt", &s_chains } })
			if (!map->empty())
				if (FILE* t = fopen((dir + name).c_str(), "w"))
				{
					for (const auto& [chain, n] : *map)
						fprintf(t, "%llu %s\n", (unsigned long long)n, chain.c_str());
					fclose(t);
				}
	}

	// ---- half-tick draws leave no trace in actors (WWHD_60FPS_ROLLBACK, default on) -----------------
	// A half tick runs every process's draw. Draws change their process too: the lighting's blend
	// state (settingTevStruct), culling flags, display-list pointers, and for some logic (the raft's
	// light flicker, f_02363374, picks new random targets from its draw; the HUD's draw moves
	// counters in g_dComIfG_gameInfo). A process whose logic runs at 30 must reach its next tick as it
	// left the last one, so every store the main thread makes into a process that executed in the last
	// whole tick, or into g_dComIfG_gameInfo, is journaled (its old bytes) and put back when the half
	// tick's frame ends: the draw shows its frame and leaves nothing behind.
	bool Rollback()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_ROLLBACK"); return !e || atoi(e) != 0; }();
		return on;
	}

	struct Saved { uint32 ea, size; uint8 bytes[32]; };
	std::vector<Saved> s_saved;

	void RollbackStore(uint32 ea, uint32 size)
	{
		if (PPCInterpreter_getCurrentInstance() != s_frameThread || size > 32)
			return;
		// WWHD_60FPS_ROLLBACK=2: all of the game's .data and .bss too, not only g_dComIfG_gameInfo (the
		// HUD's draw and dMenu's helpers keep menu state in statics beside the menu flag, 0x101EA069)
		static const bool statics = [] { const char* e = getenv("WWHD_60FPS_ROLLBACK"); return e && atoi(e) == 2; }();
		const bool global = statics ? (ea >= kGlobalsLow && ea < kGlobalsHigh) : (ea >= kGameInfo && ea < kGameInfo + kGameInfoSize);
		if (!global)
		{
			auto it = std::upper_bound(s_ranges.begin(), s_ranges.end(), ea, [](uint32 v, const ActorRange& r) { return v < r.low; });
			if (it == s_ranges.begin() || ea >= (--it)->high)
				return;
		}
		Saved& e = s_saved.emplace_back();
		e.ea = ea;
		e.size = size;
		memcpy(e.bytes, memory_base + ea, size);
	}

	void RollbackRestore()
	{
		for (auto it = s_saved.rbegin(); it != s_saved.rend(); ++it)
			memcpy(memory_base + it->ea, it->bytes, it->size);
		s_saved.clear();
	}

	void HalfTickStore(uint32 ea, uint32 size, void* from);

	bool Census()
	{
		static const bool on = [] { const char* e = getenv("WWHD_STATE_CENSUS"); return Probe() && e && atoi(e) == 1; }();
		return on;
	}

	void HalfTickStore(uint32 ea, uint32 size, void* from)
	{
		if (Census())
			CensusStore(ea, size, from);
		if (Rollback())
			RollbackStore(ea, size);
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

		auto put32 = [](FILE* f, uint32 v) { fwrite(&v, 4, 1, f); };
		auto putBytes = [&](uint32 tag, uint32 a, uint32 b, uint32 ea, uint32 size) {
			put32(state, tag); put32(state, tick); put32(state, a); put32(state, b); put32(state, ea); put32(state, size);
			fwrite(memory_base + ea, 1, size, state);
		};
		// WWHD_STATE_DUMP_GLOBALS=t1,t2,...: all of .data and .bss (3.3 MB), and every actor's bytes, at
		// these ticks
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
		const bool chosen = std::find(globalTicks.begin(), globalTicks.end(), tick) != globalTicks.end();
		if (chosen)
			putBytes(kGlobal, 100, 0, kGlobalsLow, kGlobalsHigh - kGlobalsLow);
		const bool full = tick % every == 0 || chosen;   // the chosen ticks get the actors' bytes too
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
}

// m_Do_main's frame body (see the top)
void f_025F172C(PPCInterpreter_t* __restrict ctx)
{
	if (g_rtHalfTick)
	{
		static const bool skip = [] { const char* e = getenv("WWHD_60FPS_HALF"); return e && strcmp(e, "none") == 0; }();
		if (skip)
			return;
		[[clang::musttail]] return orig_f_025F172C(ctx);   // the tick rules hold its logic to whole ticks
	}
	if (!Probe() && wwhd::rt::SixtyFrom() == ~0u)
		[[clang::musttail]] return orig_f_025F172C(ctx);
	const uint32 swap = wwhd::os::SwapCount();
	s_actors.clear();
	s_processes.clear();
	orig_f_025F172C(ctx);
	if (Probe())
		Dump(wwhd::rt::GameFrame(swap));
	if (Census() || Rollback())
	{
		s_ranges.clear();
		for (uint32 proc : s_processes)
			if (const uint32 profile = rd32(proc + 0x10); profile && rd32(profile + 0x10) <= 0x40000)
				s_ranges.push_back({ proc, proc + rd32(profile + 0x10), rd16(proc + 0x08) });
		std::sort(s_ranges.begin(), s_ranges.end(), [](const ActorRange& a, const ActorRange& b) { return a.low < b.low; });
	}
}

// fw_procFrame, sead's procFrame_: one whole frame (the tick, the draw, the present, the vsync wait).
// At 60 fps it decides whether the frame is a whole or a half tick (see the top); with the store
// census on, a half tick's frame is watched from end to end.
void f_0274C264(PPCInterpreter_t* __restrict ctx)
{
	const uint32 from = wwhd::rt::SixtyFrom();      // ~0 when 60 fps is off
	if (from == ~0u)
		[[clang::musttail]] return orig_f_0274C264(ctx);
	const uint32 swap = wwhd::os::SwapCount();
	if (swap == from && from != 0)
	{
		wwhd_SetSwapInterval(1);                       // from here on the game presents every vsync
		cemuLog_log(LogType::Force, "wwhd sixty: 60 fps from swap {}", swap);
	}
	g_rtHalfTick = swap >= from && (swap - from) % 2 != 0;
	g_rtStep = swap >= from ? 0.5f : 1.0f;
	// WWHD_STATE_CENSUS=2 watches whole ticks' frames too (from the switch on), for the trace
	static const bool censusAll = [] { const char* e = getenv("WWHD_STATE_CENSUS"); return Probe() && e && atoi(e) == 2; }();
	if (!g_rtHalfTick && censusAll && swap >= from)
	{
		static bool once = [] { atexit(CensusWrite); at_quick_exit(CensusWrite); return true; }();
		(void)once;
		g_rtStoreCensus = CensusStore;
		g_rtJournalOn = true;
		orig_f_0274C264(ctx);
		g_rtJournalOn = false;
		g_rtStoreCensus = nullptr;
		return;
	}
	if (!g_rtHalfTick)
		[[clang::musttail]] return orig_f_0274C264(ctx);
	// The game's random stream (cM_rnd, f_02019788: Wichmann-Hill state at 101FF9D4) belongs to its
	// ticks: actors' draws take numbers from it too (the lighting's flicker, f_025615B8, through
	// settingTevStruct), so a half tick's frame would move it on. Drawing it gets the numbers the next
	// whole tick's draws will get, and the stream is put back.
	uint32 rng[3] = { rd32(0x101FF9D4u), rd32(0x101FF9D8u), rd32(0x101FF9DCu) };
	const bool watch = Census() || Rollback();
	if (Census())
	{
		static bool once = [] { atexit(CensusWrite); at_quick_exit(CensusWrite); return true; }();
		(void)once;
	}
	if (watch)
	{
		s_frameThread = PPCInterpreter_getCurrentInstance();
		g_rtStoreCensus = HalfTickStore;
		g_rtJournalOn = true;
	}
	orig_f_0274C264(ctx);
	if (watch)
	{
		g_rtJournalOn = false;
		g_rtStoreCensus = nullptr;
		RollbackRestore();
	}
	wr32(0x101FF9D4u, rng[0]);
	wr32(0x101FF9D8u, rng[1]);
	wr32(0x101FF9DCu, rng[2]);
}

// fpcM_Execute: every process's execute goes through it (fpcM_Management's execute pass, f_025DE788):
// actors, the camera, the environment, the HUD and menus, scenes. Noted for the half ticks' rollback.
void f_025DE58C(PPCInterpreter_t* __restrict ctx)
{
	if (wwhd::rt::SixtyFrom() != ~0u || Census())
		s_processes.push_back(GPR(3));
	[[clang::musttail]] return orig_f_025DE58C(ctx);
}

// sead's method tree runs other nodes beside the game's every frame (f_02747BDC; their share of the
// tour route's tick: the game's f_0203593C 94%, these 2.1, 2.1 and 0.5%). WWHD_60FPS_NODES=mask
// says which run only on whole ticks (default all): 1 f_0260C74C, a request queue (a ring of
// 0xa0-byte entries) beside the HD UI's code; 2 f_027618B8, a manager that calls each of its objects;
// 4 f_0273CBD0, another such manager. (A probe: which of them drives the HD menus' state machines and
// layout animations, which ran every frame.)
namespace
{
	bool NodeOnHalfTick(uint32 bit)
	{
		static const uint32 mask = [] { const char* e = getenv("WWHD_60FPS_NODES"); return e ? (uint32)strtoul(e, nullptr, 0) : 7u; }();
		return !g_rtHalfTick || !(mask & bit);
	}
}

void f_0260C74C(PPCInterpreter_t* __restrict ctx)
{
	if (!NodeOnHalfTick(1))
		return;
	[[clang::musttail]] return orig_f_0260C74C(ctx);
}

void f_027618B8(PPCInterpreter_t* __restrict ctx)
{
	if (!NodeOnHalfTick(2))
		return;
	[[clang::musttail]] return orig_f_027618B8(ctx);
}

void f_0273CBD0(PPCInterpreter_t* __restrict ctx)
{
	if (!NodeOnHalfTick(4))
		return;
	[[clang::musttail]] return orig_f_0273CBD0(ctx);
}

// fopAc_Execute: every actor's execute goes through it (from fpcM_Management's execute pass)
void f_025D475C(PPCInterpreter_t* __restrict ctx)
{
	if (Probe() || wwhd::rt::SixtyFrom() != ~0u)
		s_actors.push_back(GPR(3));
	[[clang::musttail]] return orig_f_025D475C(ctx);
}
