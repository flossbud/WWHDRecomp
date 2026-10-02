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
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <tuple>
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

	// ---- converted processes (WWHD_60FPS_CONVERT=n,m,...: process names) ---------------------------
	// A converted process runs its logic every frame at 60 fps, with g_rtStep the part of a 30 Hz
	// tick a frame is (0.5; the helpers in sixty_step.cpp read it), and its writes stand; every other
	// process runs on whole ticks with a step of 1 (D21 step 3).
	bool Converted(uint16 name)
	{
		static const std::vector<uint16> names = [] {
			std::vector<uint16> v;
			if (const char* e = getenv("WWHD_60FPS_CONVERT"))
				for (const char* p = e; *p;)
				{
					char* end;
					const unsigned long n = strtoul(p, &end, 10);
					if (end == p)
						break;
					v.push_back((uint16)n);
					for (p = end; *p == ','; p++)
						;
				}
			return v;
		}();
		return !names.empty() && std::find(names.begin(), names.end(), name) != names.end();
	}
	bool AnyConverted()
	{
		static const bool any = [] { const char* e = getenv("WWHD_60FPS_CONVERT"); return e && *e; }();
		return any;
	}
	int s_converting = 0;                           // a converted process's execute is running

	// The HD input object (*0x101F5088, something_button_related): +0x18 pad 0's buttons pressed this
	// frame (f_02007814 and its neighbours test its bits), +0x124 held, +0x130/+0x134 the stick. The
	// input manager runs on whole ticks (a tick rule), so on a half tick last tick's presses would read
	// as pressed again: a converted process (Link's setStickData) would act on them twice (on the warp
	// route he took the Wind Waker out and put it away in the same tick). They read as not pressed on
	// a half tick and are put back after it.
	constexpr uint32 kInputObject = 0x101F5088u, kPressed = 0x18u;

	// A converted process steps at 60 only while no event runs (dComIfGp_event_runCheck: the byte at
	// g_dComIfG_gameInfo +0x5292, as Link's code tests it): events and cutscenes drive Link and the
	// camera through code not converted yet, which then runs on whole ticks as at 30. Decided at each
	// whole tick for it and the half tick after, so a switch falls between ticks; an event that starts
	// during a whole tick (after the process ran: the Wind Waker's song starts one from Link's own
	// tick) also cancels the half tick's step, which would run event code half a tick early (it took
	// Link out of the song on the warp route). The process then moves half a tick less at that start.
	bool EventRunning()
	{
		return rd8(0x1046F0B0u + 0x5292u) != 0;
	}

	// An event ordered this tick (dEvt_control_c's order count, two bytes before its mode, as in the
	// decomp's layout): the event manager starts it on the next whole tick, so a half tick between
	// runs the orderer before the event is set up (Link's Wind Waker wait cancelled itself)
	bool EventOrdered()
	{
		return (sint8)rd8(0x1046F0B0u + 0x5290u) > 0;
	}
	std::unordered_map<uint32, bool> s_stepping;    // process -> stepping at 60 this tick
	uint64 s_halfSteps = 0, s_eventStops = 0, s_orderStops = 0;
	void StepStats()
	{
		cemuLog_log(LogType::Force, "wwhd sixty: {} half steps of converted processes; {} stopped by a running event, {} by an ordered one",
			s_halfSteps, s_eventStops, s_orderStops);
	}

	// WWHD_STATE_TRACK=n,m,...: with the probe, these processes' bytes after every whole tick and, at
	// 60 fps, every half tick (dir/track.bin: tick x2 (+1 on a half tick), name, address, size, bytes;
	// up to 0x8000 bytes of each: all of Link), for tools/sixty/compare.py --track
	bool Tracked(uint16 name)
	{
		static const std::vector<uint16> names = [] {
			std::vector<uint16> v;
			if (const char* e = getenv("WWHD_STATE_TRACK"))
				for (const char* p = e; *p;)
				{
					char* end;
					const unsigned long n = strtoul(p, &end, 10);
					if (end == p)
						break;
					v.push_back((uint16)n);
					for (p = end; *p == ','; p++)
						;
				}
			return v;
		}();
		return !names.empty() && std::find(names.begin(), names.end(), name) != names.end();
	}
	std::vector<uint32> s_tracked;                  // tracked processes that executed this frame
	std::vector<uint32> s_processes;               // every process that executed this whole tick
	constexpr uint32 kGlobal = 0x474C4F42u;         // state.bin record tags: "GLOB", "ACTR"
	constexpr uint32 kActor = 0x41435452u;
	constexpr uint32 kGlobalsLow = 0x1018C0C0u, kGlobalsHigh = 0x104DA1C8u;   // .data and .bss

	// ---- the store census (WWHD_STATE_CENSUS=1 with the probe, at 60 fps) ----------------------------
	// During every half-tick frame (fw_procFrame, f_0274C264), every store generated code makes into an
	// actor that executed in the last whole tick, or into the game's .data/.bss, is counted by the
	// guest instruction that made it and what it hit: an actor's process name and offset, or a
	// global's address. dir/census.txt lists them at exit; tools/sixty/census.py names the functions.
	// That is what the frame changes outside the game's tick.
	struct ActorRange { uint32 low, high; uint16 name; };
	std::vector<ActorRange> s_ranges;               // the last whole tick's actors, by address
	PPCInterpreter_t* s_frameThread = nullptr;      // the main thread: the one running fw_procFrame
	struct CensusKey
	{
		uint32 pc;                                  // the storing instruction (0: hand-written code)
		uint32 name;                                // process name + 1, or 0 for a global
		uint32 where;                               // offset in the actor, or the global's address
		bool operator==(const CensusKey&) const = default;
	};
	struct CensusHash
	{
		size_t operator()(const CensusKey& k) const { return k.pc * 0x9E3779B97F4A7C15ull ^ ((uint64)k.name << 32 | k.where); }
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

	uint32 s_drawing = 0;                            // the process whose draw is running (fpcM_Draw), or 0

	// WWHD_STATE_CENSUS_HEAP=1: the main thread's stores anywhere else too (heap objects: layouts,
	// effects, sound), counted by the process being drawn and the first 4 callers, as "heap" in
	// census_chains.txt
	void CensusHeap(uint32 ea)
	{
		static const bool on = [] { const char* e = getenv("WWHD_STATE_CENSUS_HEAP"); return e && atoi(e) == 1; }();
		PPCInterpreter_t* cpu = on ? PPCInterpreter_getCurrentInstance() : nullptr;
		if (!cpu || cpu != s_frameThread)
			return;
		const uint32 sp = cpu->gpr[1];
		if (ea + 0x10000 > sp && ea < sp + 0x10000)  // the stack (near its pointer)
			return;
		char who[24];
		snprintf(who, sizeof(who), "heap %d", s_drawing ? (int)rd16(s_drawing + 0x08) : -1);
		s_chains[who + GuestChain(4)]++;
	}

	void CensusStore(uint32 ea, uint32 size, uint32 pc)
	{
		CensusTrace(ea);
		CensusKey k{ pc, 0, ea };
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
		for (const auto& [k, n] : s_census)
			fprintf(f, "%08x %u %x %llu\n", k.pc, k.name, k.where, (unsigned long long)n);
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

	// ---- half ticks' draws leave no trace (WWHD_60FPS_ROLLBACK, default 2) ----------------------------
	// A half tick runs every process's draw. Draws change their process too: the lighting's blend
	// state (settingTevStruct), culling flags, display-list pointers, and some run logic (the raft's
	// light flicker, f_02363374, picks new random targets from its draw; the HUD's draw moves counters
	// in g_dComIfG_gameInfo and statics beside the menu flag; the environment's draw blends light
	// transitions in g_env_light). A process whose logic runs at 30 must reach its next tick as it left
	// the last one, so every store the main thread makes during a half tick's frame into a process
	// that executed in the last whole tick, or into the game's .data and .bss, is journaled (its old
	// bytes) and put back when the frame ends (gfx_EndFrame's GX2DrawDone has let the GPU finish with
	// it by then): the draw shows its frame and leaves nothing behind. Level 3 also undid what draws
	// write to the heap; that broke state that has to persist (the warp route diverged), so it stays
	// a probe.
	// WWHD_60FPS_ROLLBACK: 0 off; 1 processes and g_dComIfG_gameInfo; 2 (default) processes and all of
	// the game's .data and .bss; 3 also every store a process's draw makes anywhere but the stack
	// (heap objects: layouts, cloth, effects)
	int RollbackLevel()
	{
		static const int level = [] { const char* e = getenv("WWHD_60FPS_ROLLBACK"); return e ? atoi(e) : 2; }();
		return level;
	}

	bool Rollback()
	{
		return RollbackLevel() != 0;
	}

	struct Saved { uint32 ea, size; uint8 bytes[32]; };
	std::vector<Saved> s_saved;

	void RollbackStore(uint32 ea, uint32 size)
	{
		if (s_converting || PPCInterpreter_getCurrentInstance() != s_frameThread || size > 32)
			return;
		// level 2: all of the game's .data and .bss, not only g_dComIfG_gameInfo (the environment's draw
		// blends its light transitions in g_env_light, the HUD's draw and dMenu's helpers keep state in
		// statics beside the menu flag, 0x101EA069); level 3: anything a process's draw writes but its
		// stack
		const int level = RollbackLevel();
		const bool global = level >= 2 ? (ea >= kGlobalsLow && ea < kGlobalsHigh) : (ea >= kGameInfo && ea < kGameInfo + kGameInfoSize);
		const uint32 sp = s_frameThread->gpr[1];
		const bool drawn = level >= 3 && s_drawing != 0 && !(ea + 0x10000 > sp && ea < sp + 0x10000);
		if (!global && !drawn)
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

	// The journal's filter. Every store the half tick's frame makes comes here (in real time a fifth
	// of the main thread's time went to RollbackStore's checks and its saves, on the sail route), and
	// most are into memory that isn't journaled (the heap: models' matrices, display lists, packets)
	// or into bytes the frame has already saved (the matrix stack, a draw's fields, many times a
	// frame). So which 4 KB pages hold journaled memory is worked out once a frame (all of the page,
	// or part of it: then RollbackStore's own check), and each page keeps a bit per byte saved this
	// frame: a store is saved once. Saves of bytes saved before (a store over some new bytes is saved
	// whole) are harmless, as RollbackRestore puts them back last to first.
	constexpr uint32 kPartPage = 0x80000000u;
	std::vector<uint32> s_page;                     // per page of the 32-bit space: 0, or its bits' index + 1, | kPartPage
	std::vector<uint32> s_pagesSet;                 // the pages set this frame
	std::vector<std::array<uint64, 64>> s_savedBits;   // per journaled page: the bytes saved this frame
	uint64 s_storesSeen = 0, s_storesSaved = 0;     // the frame log's counts (src/overrides/pacing.cpp)

	void JournalPages()
	{
		if (s_page.empty())
			s_page.assign(1u << 20, 0);
		auto mark = [](uint32 low, uint32 high) {
			for (uint32 p = low >> 12; p <= (high - 1) >> 12; p++)
			{
				uint32& e = s_page[p];
				if (!e)
				{
					s_savedBits.emplace_back();
					s_savedBits.back().fill(0);
					e = (uint32)s_savedBits.size() | kPartPage;
					s_pagesSet.push_back(p);
				}
				if (low <= p << 12 && high - (p << 12) >= 0x1000)
					e &= ~kPartPage;
			}
		};
		mark(kGlobalsLow, kGlobalsHigh);
		for (const ActorRange& r : s_ranges)
			if (r.high > r.low)
				mark(r.low, r.high);
	}

	void JournalPagesClear()
	{
		for (uint32 p : s_pagesSet)
			s_page[p] = 0;
		s_pagesSet.clear();
		s_savedBits.clear();
	}

	// RollbackStore through the filter: nothing if the store's page isn't journaled or its bytes are
	// saved already this frame
	inline void FilteredRollbackStore(uint32 ea, uint32 size)
	{
		s_storesSeen++;
		const uint32 e = s_page[ea >> 12];
		if (!e)
			return;
		const uint32 off = ea & 0xFFF;
		if (off + size > 0x1000 || size > 32)       // across two pages (rare): unfiltered
		{
			RollbackStore(ea, size);
			return;
		}
		uint64* bits = s_savedBits[(e & ~kPartPage) - 1].data();
		const uint32 i0 = off >> 6, i1 = (off + size - 1) >> 6;
		uint64 m0, m1 = 0;                           // the store's bytes in bits[i0] and bits[i1]
		if (i0 == i1)
			m0 = ((1ull << size) - 1) << (off & 63);
		else
		{
			m0 = ~0ull << (off & 63);
			const uint32 end = (off + size) & 63;
			m1 = end ? (1ull << end) - 1 : ~0ull;
		}
		if ((bits[i0] & m0) == m0 && (bits[i1] & m1) == m1)
			return;
		const size_t before = s_saved.size();
		RollbackStore(ea, size);                    // its own checks: the thread, part pages' ranges
		if (s_saved.size() != before)
		{
			bits[i0] |= m0;
			bits[i1] |= m1;
			s_storesSaved++;
		}
	}

	void HalfTickStore(uint32 ea, uint32 size, uint32 pc);

	bool Census()
	{
		static const bool on = [] { const char* e = getenv("WWHD_STATE_CENSUS"); return Probe() && e && atoi(e) == 1; }();
		return on;
	}

	// ---- a half tick's draws see the last whole tick's shared state -----------------------------------
	// A converted process's half step writes the game's globals too (Link sets the action the HUD shows,
	// gameInfo's do and A statuses, every step). The draws of unconverted processes then saw a state
	// their own update (whole ticks) hadn't prepared for, which the 30 Hz game never reaches (once, in
	// real time, the HUD's draw crashed in its text code). So what converted processes write to .data
	// and .bss during a half tick's execute pass is hidden from its draw pass (put back to the whole
	// tick's values before the first draw) and handed back after the frame's rollback: the converted
	// processes go on from their own state, and draws see the whole tick's.
	std::vector<Saved> s_convGlobals;               // converted executes' stores into .data/.bss: old bytes
	std::vector<Saved> s_convGlobalsNew;            // and their bytes after the execute pass
	bool s_convGlobalsHidden = false;

	// with the probe: which globals were hidden, and how often (dir/hidden.txt at exit)
	std::unordered_map<uint32, uint64> s_hiddenCounts;
	void HiddenWrite()
	{
		static const std::string dir = getenv("WWHD_STATE_DUMP");
		if (FILE* f = fopen((dir + "/hidden.txt").c_str(), "w"))
		{
			for (const auto& [ea, n] : s_hiddenCounts)
				fprintf(f, "%08x %llu\n", ea, (unsigned long long)n);
			fclose(f);
		}
	}

	void HideConvertedGlobals()
	{
		static const bool hide = [] { const char* e = getenv("WWHD_60FPS_HIDE"); return !(e && atoi(e) == 0); }();   // a probe
		if (s_convGlobalsHidden || s_convGlobals.empty() || !hide)
			return;
		s_convGlobalsHidden = true;
		if (Probe())
		{
			static bool once = [] { atexit(HiddenWrite); at_quick_exit(HiddenWrite); return true; }();
			(void)once;
			std::set<uint32> words;
			for (const Saved& e : s_convGlobals)
				words.insert(e.ea & ~3u);
			for (uint32 w : words)
				s_hiddenCounts[w]++;
		}
		s_convGlobalsNew.clear();
		for (const Saved& e : s_convGlobals)
		{
			Saved& n = s_convGlobalsNew.emplace_back();
			n.ea = e.ea;
			n.size = e.size;
			memcpy(n.bytes, memory_base + e.ea, e.size);
		}
		for (auto it = s_convGlobals.rbegin(); it != s_convGlobals.rend(); ++it)
			memcpy(memory_base + it->ea, it->bytes, it->size);
	}

	void ShowConvertedGlobals()
	{
		if (s_convGlobalsHidden)
			for (const Saved& n : s_convGlobalsNew)
				memcpy(memory_base + n.ea, n.bytes, n.size);
		s_convGlobals.clear();
		s_convGlobalsNew.clear();
		s_convGlobalsHidden = false;
	}

	void HalfTickStore(uint32 ea, uint32 size, uint32 pc)
	{
		if (Census())
			CensusStore(ea, size, pc);
		if (s_converting && !s_convGlobalsHidden && size <= 32 && ea >= kGlobalsLow && ea < kGlobalsHigh &&
			PPCInterpreter_getCurrentInstance() == s_frameThread)
		{
			Saved& e = s_convGlobals.emplace_back();
			e.ea = ea;
			e.size = size;
			memcpy(e.bytes, memory_base + ea, size);
		}
		if (!Rollback())
			return;
		if (!s_pagesSet.empty() && RollbackLevel() == 2)
			FilteredRollbackStore(ea, size);
		else
			RollbackStore(ea, size);
	}

	// ---- the step-doubling trial (WWHD_60FPS_TRIAL=n,m,... with the probe, at 30 fps) -------------
	// Finds the per-tick steps of a process that its conversion hasn't reached yet, one tick at a
	// time and without the run drifting. At every tick each listed process's execute runs as two half
	// steps (g_rtStep 0.5, the second as a half tick, as at 60 fps) with every store the main thread
	// makes journaled; the values they leave are noted and the stores put back; then the execute runs
	// once as the game's, and the run goes on from that. Every store (outside the stack) whose bytes
	// after the two half steps differ from theirs after the game's step is counted by the instruction
	// that made it last in the half steps (or, if only the game's step made it, by that one), with
	// the value before, after the half steps and after the game's step: dir/trial.txt at exit, which
	// tools/sixty/trial.py reports. Those are the open-coded steps, and the helpers' inexact cases, to
	// give rules or overrides (D21). WWHD_60FPS_TRIAL_DRAW=1 also runs the process's draw between the
	// half steps, as a 60 fps frame does (its stores are put back with the rest).
	bool Trial(uint16 name)
	{
		static const std::vector<uint16> names = [] {
			std::vector<uint16> v;
			if (const char* e = getenv("WWHD_60FPS_TRIAL"))
				for (const char* p = e; *p;)
				{
					char* end;
					const unsigned long n = strtoul(p, &end, 10);
					if (end == p)
						break;
					v.push_back((uint16)n);
					for (p = end; *p == ','; p++)
						;
				}
			return v;
		}();
		return !names.empty() && std::find(names.begin(), names.end(), name) != names.end();
	}

	struct StoreUnit { uint32 pc, ea, size; };      // a store: its instruction, address and size
	int s_trialPhase = 0;                           // 1: the two half steps, 2: the game's step
	uint32 s_trialSp = 0;                           // the stack pointer under the execute
	std::vector<Saved> s_trialSaved;                // the half steps' stores' old bytes, to put back
	std::unordered_map<uint32, StoreUnit> s_halfLast, s_refLast;   // byte -> the last store to it
	std::unordered_map<uint32, uint8> s_initial, s_halfValue;      // byte -> before; after the half steps

	void TrialStore(uint32 ea, uint32 size, uint32 pc)
	{
		if (PPCInterpreter_getCurrentInstance() != s_frameThread || size > 32)
			return;
		if (ea + 0x10000 > s_trialSp && ea < s_trialSp + 0x100)   // the execute's stack frames
			return;
		if (s_trialPhase == 1)
		{
			Saved& e = s_trialSaved.emplace_back();
			e.ea = ea;
			e.size = size;
			memcpy(e.bytes, memory_base + ea, size);
		}
		auto& last = s_trialPhase == 1 ? s_halfLast : s_refLast;
		for (uint32 b = ea; b < ea + size; b++)
		{
			s_initial.try_emplace(b, rd8(b));
			last[b] = { pc, ea, size };
		}
	}

	struct TrialKey
	{
		uint32 pc;
		uint32 refOnly;                             // 1: only the game's step made this store
		uint32 name;                                // process name + 1; 0 a global; ~0 the heap
		uint32 where;                               // offset in the process, the global's address, 0
		bool operator==(const TrialKey&) const = default;
	};
	struct TrialHash
	{
		size_t operator()(const TrialKey& k) const { return (k.pc * 0x9E3779B97F4A7C15ull) ^ ((uint64)k.name << 32 | k.where) ^ k.refOnly; }
	};
	struct TrialStat
	{
		uint64 ticks = 0;                           // ticks it differed
		uint32 size = 0;
		char kind = 'i';                            // f a single, d a double, i an integer
		double worst = -1;                          // the largest difference, at:
		uint32 tick = 0;
		uint64 initial = 0, half = 0, ref = 0;
	};
	std::unordered_map<TrialKey, TrialStat, TrialHash> s_trial;
	uint64 s_trialTicks = 0;

	// What an instruction stores: f a single (stfs, stfsu, stfsx, stfsux, psq_st of 4 bytes),
	// d a double (stfd...), i an integer
	char StoreKind(uint32 pc, uint32 size)
	{
		if (pc == 0)
			return 'i';
		const uint32 insn = rd32(pc), op = insn >> 26, xo = (insn >> 1) & 0x3FF;
		if (op == 52 || op == 53 || (op == 31 && (xo == 663 || xo == 695)))
			return 'f';
		if (op == 54 || op == 55 || (op == 31 && (xo == 727 || xo == 759)))
			return 'd';
		if ((op == 60 || op == 61 || (op == 4 && ((xo & 0x3F) == 7 || (xo & 0x3F) == 39))) && size == 4)
			return 'f';
		return 'i';
	}

	void TrialWrite()
	{
		static const std::string dir = getenv("WWHD_STATE_DUMP");
		FILE* f = fopen((dir + "/trial.txt").c_str(), "w");
		if (!f)
			return;
		fprintf(f, "# ticks %llu\n", (unsigned long long)s_trialTicks);
		fprintf(f, "# pc refOnly name where size kind ticks worst tick initial half ref\n");
		for (const auto& [k, t] : s_trial)
			fprintf(f, "%08x %u %d %x %u %c %llu %.9g %u %llx %llx %llx\n", k.pc, k.refOnly, (int)k.name - 1, k.where, t.size, t.kind,
				(unsigned long long)t.ticks, t.worst, t.tick, (unsigned long long)t.initial, (unsigned long long)t.half, (unsigned long long)t.ref);
		fclose(f);
	}

	// The two half steps' result against the game's step: each store that left different bytes
	void TrialCompare(uint32 tick)
	{
		std::set<std::pair<uint32, uint32>> units;   // (byte address of the store, 1 if the game's step only)
		std::vector<std::tuple<StoreUnit, uint32>> differing;
		auto valueAfterHalf = [](uint32 b) {
			auto it = s_halfValue.find(b);
			return it != s_halfValue.end() ? it->second : s_initial.at(b);
		};
		auto consider = [&](uint32 b) {
			if (valueAfterHalf(b) == rd8(b))
				return;
			auto h = s_halfLast.find(b);
			const bool refOnly = h == s_halfLast.end();
			const StoreUnit u = refOnly ? s_refLast.at(b) : h->second;
			if (units.insert({ u.ea, refOnly }).second)
				differing.push_back({ u, refOnly });
		};
		for (const auto& [b, u] : s_halfLast)
			consider(b);
		for (const auto& [b, u] : s_refLast)
			if (!s_halfLast.count(b))
				consider(b);
		for (const auto& [u, refOnly] : differing)
		{
			const uint32 size = std::min(u.size, 8u);
			uint64 initial = 0, half = 0, ref = 0;
			for (uint32 i = 0; i < size; i++)
			{
				const uint32 b = u.ea + i;
				const auto init = s_initial.find(b);
				const uint8 i0 = init != s_initial.end() ? init->second : rd8(b);
				initial = initial << 8 | i0;
				const auto hv = s_halfValue.find(b);
				half = half << 8 | (hv != s_halfValue.end() ? hv->second : i0);
				ref = ref << 8 | rd8(b);
			}
			const char kind = StoreKind(u.pc, u.size);
			double diff;
			if (kind == 'f')
				diff = std::fabs((double)std::bit_cast<float>((uint32)half) - (double)std::bit_cast<float>((uint32)ref));
			else if (kind == 'd')
				diff = std::fabs(std::bit_cast<double>(half) - std::bit_cast<double>(ref));
			else
			{
				const int shift = 64 - 8 * (int)size;
				diff = std::fabs((double)((sint64)(half << shift) >> shift) - (double)((sint64)(ref << shift) >> shift));
			}
			if (!(diff == diff))
				diff = 1e30;                                  // a NaN on one side
			TrialKey k{ u.pc, refOnly ? 1u : 0u, 0, u.ea };
			if (!(u.ea >= kGlobalsLow && u.ea < kGlobalsHigh))
			{
				auto it = std::upper_bound(s_ranges.begin(), s_ranges.end(), u.ea, [](uint32 v, const ActorRange& r) { return v < r.low; });
				if (it != s_ranges.begin() && u.ea < (--it)->high)
				{
					k.name = it->name + 1u;
					k.where = u.ea - it->low;
				}
				else
				{
					k.name = ~0u;                             // the heap: by instruction only
					k.where = 0;
				}
			}
			TrialStat& t = s_trial[k];
			t.ticks++;
			t.size = u.size;
			t.kind = kind;
			if (diff > t.worst)
			{
				t.worst = diff;
				t.tick = tick;
				t.initial = initial;
				t.half = half;
				t.ref = ref;
			}
		}
	}

	struct Registers
	{
		uint32 gpr[32];
		FPR_t fpr[32];
		uint32 fpscr;
		uint8 cr[32];
		uint8 xer_ca, xer_so, xer_ov;
		uint32 LR, CTR, XER, UGQR[8];
		uint32 resAddr, resValue;

		void Save(const PPCInterpreter_t* c)
		{
			memcpy(gpr, c->gpr, sizeof(gpr));
			memcpy(fpr, c->fpr, sizeof(fpr));
			fpscr = c->fpscr;
			memcpy(cr, c->cr, sizeof(cr));
			xer_ca = c->xer_ca; xer_so = c->xer_so; xer_ov = c->xer_ov;
			LR = c->spr.LR; CTR = c->spr.CTR; XER = c->spr.XER;
			memcpy(UGQR, c->spr.UGQR, sizeof(UGQR));
			resAddr = c->reservedMemAddr; resValue = c->reservedMemValue;
		}

		void Restore(PPCInterpreter_t* c) const
		{
			memcpy(c->gpr, gpr, sizeof(gpr));
			memcpy(c->fpr, fpr, sizeof(fpr));
			c->fpscr = fpscr;
			memcpy(c->cr, cr, sizeof(cr));
			c->xer_ca = xer_ca; c->xer_so = xer_so; c->xer_ov = xer_ov;
			c->spr.LR = LR; c->spr.CTR = CTR; c->spr.XER = XER;
			memcpy(c->spr.UGQR, UGQR, sizeof(UGQR));
			c->reservedMemAddr = resAddr; c->reservedMemValue = resValue;
		}
	};

	// One process's execute as a trial (see above), or with fn another per-tick function (the particle
	// calc); the game's step stands
	void TrialExecute(PPCInterpreter_t* ctx, void (*fn)(PPCInterpreter_t*) = orig_f_025DE58C)
	{
		static bool once = [] { atexit(TrialWrite); at_quick_exit(TrialWrite); return true; }();
		(void)once;
		const uint32 tick = wwhd::rt::GameFrame(wwhd::os::SwapCount());
		Registers regs;
		regs.Save(ctx);
		s_frameThread = PPCInterpreter_getCurrentInstance();
		s_trialSp = ctx->gpr[1];
		s_trialSaved.clear();
		s_halfLast.clear();
		s_refLast.clear();
		s_initial.clear();
		s_halfValue.clear();
		g_rtStoreCensus = TrialStore;
		g_rtJournalOn = true;

		s_trialPhase = 1;                               // two half steps, as at 60 fps
		g_rtStep = 0.5f;
		s_converting++;
		fn(ctx);
		regs.Restore(ctx);
		static const bool draw = [] { const char* e = getenv("WWHD_60FPS_TRIAL_DRAW"); return e && atoi(e) == 1; }();
		if (draw && fn == orig_f_025DE58C)
		{
			s_converting--;                             // the draw as at 60 fps: a step of 1, not converting
			g_rtStep = 1.0f;
			orig_f_025DE2CC(ctx);                       // fpcM_Draw(the same process)
			regs.Restore(ctx);
			g_rtStep = 0.5f;
			s_converting++;
		}
		g_rtHalfTick = true;
		fn(ctx);
		g_rtHalfTick = false;
		s_converting--;
		g_rtStep = 1.0f;
		for (const auto& [b, u] : s_halfLast)
			s_halfValue[b] = rd8(b);
		for (auto it = s_trialSaved.rbegin(); it != s_trialSaved.rend(); ++it)
			memcpy(memory_base + it->ea, it->bytes, it->size);

		regs.Restore(ctx);
		s_trialPhase = 2;                               // the game's step: the run goes on from it
		fn(ctx);
		g_rtJournalOn = wwhd::rt::QuietWatching();   // a fast path's watch may still need it
		g_rtStoreCensus = nullptr;
		s_trialPhase = 0;
		TrialCompare(tick);
		s_trialTicks++;
	}

	uint64 Hash(uint32 ea, uint32 size)
	{
		uint64 h = 0xcbf29ce484222325ull;
		for (uint32 i = 0; i < size; i++)
			h = (h ^ rd8(ea + i)) * 0x100000001b3ull;
		return h;
	}

	void DumpTracked(uint32 tick2)
	{
		static const std::string dir = getenv("WWHD_STATE_DUMP");
		static FILE* f = fopen((dir + "/track.bin").c_str(), "wb");
		if (f)
		{
			for (uint32 proc : s_tracked)
			{
				const uint32 profile = rd32(proc + 0x10);
				uint32 size = profile ? rd32(profile + 0x10) : 0;
				if (size == 0 || size > 0x40000)
					continue;
				size = std::min(size, 0x8000u);
				const uint32 head[4] = { tick2, rd16(proc + 0x08), proc, size };
				fwrite(head, 4, 4, f);
				fwrite(memory_base + proc, 1, size, f);
			}
			fflush(f);
		}
		s_tracked.clear();
	}

	// With the probe: the 3D particles after every frame's particle calc (dir/particles.txt: game frame
	// times two, plus one on half ticks; emitters, particles, children, and the particles' positions
	// summed), to compare the particle system at 60 with the 30-tick run's. JPAEmitterManager at
	// *0x1047B2D4: a list per group (first link at +0x50 + 12 g); an emitter's particles at +0x1AC
	// (count +0x1B4) and children at +0x1B8 (count +0x1C0); a particle's global position at +0x28.
	void ParticleCensus(uint32 tick2)
	{
		static const std::string dir = getenv("WWHD_STATE_DUMP");
		static FILE* f = fopen((dir + "/particles.txt").c_str(), "w");
		const uint32 mgr = rd32(0x1047B2D4u);
		if (!f || !mgr)
			return;
		uint32 emitters = 0, particles = 0, children = 0;
		double sum[3] = {};
		for (uint32 g = 0; g < 7; g++)
			for (uint32 link = rd32(mgr + 0x50 + 12 * g); link; link = rd32(link + 12))
			{
				const uint32 e = rd32(link);
				emitters++;
				particles += rd32(e + 0x1B4);
				children += rd32(e + 0x1C0);
				for (uint32 list : { 0x1ACu, 0x1B8u })
					for (uint32 pl = rd32(e + list); pl; pl = rd32(pl + 12))
						for (int i = 0; i < 3; i++)
						{
							const uint32 v = rd32(rd32(pl) + 0x28 + 4 * i);
							float x;
							memcpy(&x, &v, 4);
							sum[i] += x;
						}
			}
		fprintf(f, "%u %u %u %u %.1f %.1f %.1f\n", tick2, emitters, particles, children, sum[0], sum[1], sum[2]);
		fflush(f);
		// WWHD_PARTICLE_LIST=t: at that census point, each emitter: its callbacks' vtables (emitter
		// +0x1E4, particles +0x1E8), rate (+0x34), rate step (+0x27), dynamics flags (+0x84; 2 a fixed
		// interval), volume type (+0x26), divisions (+0x66), particles, and the first one's age and life
		static const uint32 list = [] { const char* e = getenv("WWHD_PARTICLE_LIST"); return e ? (uint32)atoi(e) : 0u; }();
		if (tick2 != list)
			return;
		for (uint32 g = 0; g < 7; g++)
			for (uint32 link = rd32(mgr + 0x50 + 12 * g); link; link = rd32(link + 12))
			{
				const uint32 e = rd32(link), cb = rd32(e + 0x1E4), pcb = rd32(e + 0x1E8), first = rd32(e + 0x1AC);
				const uint32 rate = rd32(e + 0x34);
				float r, age = 0, life = 0;
				memcpy(&r, &rate, 4);
				if (first)
				{
					const uint32 a = rd32(rd32(first) + 0x78), l = rd32(rd32(first) + 0x7C);
					memcpy(&age, &a, 4);
					memcpy(&life, &l, 4);
				}
				fprintf(f, "  emitter %08x group %u cb %08x pcb %08x rate %.3f step %u dyn %08x volume %u div %u particles %u age %.1f life %.1f\n",
					e, g, cb ? rd32(cb) : 0, pcb ? rd32(pcb) : 0, r, rd8(e + 0x27), rd32(e + 0x84), rd8(e + 0x26), rd16(e + 0x66),
					rd32(e + 0x1B4), age, life);
			}
		fflush(f);
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
					char* end;
					const unsigned long n = strtoul(p, &end, 10);
					if (end == p)
						break;
					v.push_back((uint32)n);
					for (p = end; *p == ','; p++)
						;
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

namespace wwhd::sixty
{
	// the half tick's journal since the last call: stores seen and saved (the frame log, pacing.cpp)
	void TakeJournalCounts(uint64& seen, uint64& saved)
	{
		seen = s_storesSeen;
		saved = s_storesSaved;
		s_storesSeen = s_storesSaved = 0;
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
		if (!Probe())
			[[clang::musttail]] return orig_f_025F172C(ctx);   // the tick rules hold its logic to whole ticks
		const uint32 swap = wwhd::os::SwapCount();
		s_tracked.clear();
		orig_f_025F172C(ctx);
		DumpTracked(wwhd::rt::GameFrame(swap) * 2 + 1);
		ParticleCensus(wwhd::rt::GameFrame(swap) * 2 + 1);
		return;
	}
	if (!Probe() && wwhd::rt::SixtyFrom() == ~0u)
		[[clang::musttail]] return orig_f_025F172C(ctx);
	const uint32 swap = wwhd::os::SwapCount();
	s_actors.clear();
	s_processes.clear();
	s_tracked.clear();
	orig_f_025F172C(ctx);
	if (Probe())
	{
		Dump(wwhd::rt::GameFrame(swap));
		DumpTracked(wwhd::rt::GameFrame(swap) * 2);
		ParticleCensus(wwhd::rt::GameFrame(swap) * 2);
	}
	if (Census() || Rollback())
	{
		s_ranges.clear();
		for (uint32 proc : s_processes)
			if (const uint32 profile = rd32(proc + 0x10); profile && rd32(profile + 0x10) <= 0x40000)
				s_ranges.push_back({ proc, proc + rd32(profile + 0x10), rd16(proc + 0x08) });
		// a converted process's draws are part of its frame, at 60 as its execute is: they stand (while
		// it steps at 60: in an event it runs on whole ticks, and its draws are put back as all others')
		if (wwhd::rt::SixtyFrom() != ~0u)
			std::erase_if(s_ranges, [](const ActorRange& r) { auto it = s_stepping.find(r.low); return Converted(r.name) && it != s_stepping.end() && it->second; });
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
	// WWHD_STATE_CENSUS=2 watches whole ticks' frames too (from the switch on), for the trace
	static const bool censusAll = [] { const char* e = getenv("WWHD_STATE_CENSUS"); return Probe() && e && atoi(e) == 2; }();
	if (!g_rtHalfTick && censusAll && swap >= from)
	{
		static bool once = [] { atexit(CensusWrite); at_quick_exit(CensusWrite); return true; }();
		(void)once;
		g_rtStoreCensus = CensusStore;
		g_rtJournalOn = true;
		orig_f_0274C264(ctx);
		g_rtJournalOn = wwhd::rt::QuietWatching();   // a fast path's watch may still need it
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
	const uint32 input = AnyConverted() ? rd32(kInputObject) : 0;
	const uint32 pressed = input ? rd32(input + kPressed) : 0;
	if (input)
		wr32(input + kPressed, 0);
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
		if (RollbackLevel() == 2)
			JournalPages();
	}
	orig_f_0274C264(ctx);
	if (watch)
	{
		g_rtJournalOn = wwhd::rt::QuietWatching();   // a fast path's watch may still need it
		g_rtStoreCensus = nullptr;
		RollbackRestore();
		JournalPagesClear();
		ShowConvertedGlobals();                         // the converted processes' half-step globals, back
	}
	wr32(0x101FF9D4u, rng[0]);
	wr32(0x101FF9D8u, rng[1]);
	wr32(0x101FF9DCu, rng[2]);
	if (input)
		wr32(input + kPressed, pressed);
}

// fpcM_Execute: every process's execute goes through it (fpcM_Management's execute pass, f_025DE788):
// actors, the camera, the environment, the HUD and menus, scenes. Noted for the half ticks' rollback.
void f_025DE58C(PPCInterpreter_t* __restrict ctx)
{
	const uint32 from = wwhd::rt::SixtyFrom();      // ~0 when 60 fps is off
	if (from == ~0u && !Probe())
		[[clang::musttail]] return orig_f_025DE58C(ctx);
	const uint32 proc = GPR(3);
	if (from == ~0u && s_trialPhase == 0 && Trial(rd16(proc + 0x08)))
	{
		s_processes.push_back(proc);
		TrialExecute(ctx);
		return;
	}
	bool converted = from != ~0u && wwhd::os::SwapCount() >= from && Converted(rd16(proc + 0x08));
	if (converted)
	{
		if (!g_rtHalfTick)
			s_stepping[proc] = !EventRunning();
		else if (s_stepping[proc])
		{
			static bool once = [] { atexit(StepStats); at_quick_exit(StepStats); return true; }();
			(void)once;
			const bool running = EventRunning(), ordered = !running && EventOrdered();
			s_eventStops += running;
			s_orderStops += ordered;
			if (running || ordered)
				s_stepping[proc] = false;
			else
				s_halfSteps++;
		}
		converted = s_stepping[proc];
	}
	if (Probe() && Tracked(rd16(proc + 0x08)) && (!g_rtHalfTick || converted))
		s_tracked.push_back(proc);
	if (!g_rtHalfTick)
		s_processes.push_back(proc);
	else if (!converted)
	{
		GPR(3) = 1;                                   // a half tick: only converted processes run
		return;
	}
	if (!converted)
		[[clang::musttail]] return orig_f_025DE58C(ctx);
	const float step = g_rtStep;
	g_rtStep = 0.5f;
	s_converting++;
	orig_f_025DE58C(ctx);
	s_converting--;
	g_rtStep = step;
}

// dPa_control_c::calc3D (f_025A81A0; the play scene's draw calls it, f_025B019C): every 3D particle
// emitter's calc, JPAEmitterManager::calc (f_0282167C) on groups 0 to 6: emission, the particles'
// motion and ageing, the emitters' callbacks (the ship's wake and splashes). At 60 fps it runs on
// whole ticks only, as the game's logic does, unless WWHD_60FPS_PARTICLES=1: then every frame, with
// a time step of half a tick (config/US_v0/tick_rules.txt has JParticle's steps) and its stores
// standing, as a converted process's do. At 30 fps with the probe, WWHD_60FPS_TRIAL_PARTICLES=1 runs
// it as the step-doubling trial (TrialExecute).
void f_025A81A0(PPCInterpreter_t* __restrict ctx)
{
	static const bool converted = [] { const char* e = getenv("WWHD_60FPS_PARTICLES"); return e && atoi(e) == 1; }();
	const uint32 from = wwhd::rt::SixtyFrom();      // ~0 when 60 fps is off
	if (from == ~0u)
	{
		static const bool trial = [] { const char* e = getenv("WWHD_60FPS_TRIAL_PARTICLES"); return Probe() && e && atoi(e) == 1; }();
		if (trial && s_trialPhase == 0)
		{
			TrialExecute(ctx, orig_f_025A81A0);
			return;
		}
		[[clang::musttail]] return orig_f_025A81A0(ctx);
	}
	if (!converted || wwhd::os::SwapCount() < from)
	{
		if (g_rtHalfTick)
			return;                                     // whole ticks only, as the game's logic
		[[clang::musttail]] return orig_f_025A81A0(ctx);
	}
	const float step = g_rtStep;
	g_rtStep = 0.5f;
	s_converting++;
	orig_f_025A81A0(ctx);
	s_converting--;
	g_rtStep = step;
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

// fpcM_Draw: every process's draw goes through it (fpcDw_Handler's iterator, and the play scene's
// draw for its actors). On half ticks the HUD (METER, process 481) isn't drawn: its draw rebuilds its
// text there when what it shows has changed, and the rollback put back its pointers but not the heap
// they pointed into (entering play at 60 from boot, its next draw crashed in the text code every
// time); the HD port renders the HUD's layouts every frame anyway, so half ticks still show it.
// WWHD_60FPS_SKIPDRAW=n,m,... (or "all") replaces that list of processes not drawn on half ticks
// (a probe: which draws move state the rollback doesn't reach); WWHD_60FPS_SKIPDRAW= (empty) draws all.
void f_025DE2CC(PPCInterpreter_t* __restrict ctx)
{
	if (g_rtHalfTick)
	{
		HideConvertedGlobals();                         // the draw pass sees the whole tick's globals
		static const std::vector<uint16> skip = [] {
			std::vector<uint16> v;
			const char* e = getenv("WWHD_60FPS_SKIPDRAW");
			if (!e)
				return std::vector<uint16>{ 481 };            // the HUD (see above)
			for (const char* p = e; *p;)
			{
				char* end;
				const unsigned long n = strtoul(p, &end, 10);
				if (end == p)
					break;                                     // "all", or the end of the numbers
				v.push_back((uint16)n);
				for (p = end; *p == ','; p++)
					;
			}
			return v;
		}();
		static const bool all = [] { const char* e = getenv("WWHD_60FPS_SKIPDRAW"); return e && strcmp(e, "all") == 0; }();
		static const bool probing = getenv("WWHD_60FPS_SKIPDRAW") != nullptr;
		const uint16 name = rd16(GPR(3) + 0x08);
		static std::set<uint16> drawn;                 // which process names draw on half ticks, logged once each
		if (probing && drawn.insert(name).second)
			cemuLog_log(LogType::Force, "wwhd sixty: process {} draws on half ticks", name);
		if (all || (!skip.empty() && std::find(skip.begin(), skip.end(), name) != skip.end()))
		{
			GPR(3) = 1;
			return;
		}
	}
	const uint32 outer = s_drawing;
	s_drawing = GPR(3);
	orig_f_025DE2CC(ctx);
	s_drawing = outer;
}

// fopAc_Execute: every actor's execute goes through it (from fpcM_Management's execute pass)
void f_025D475C(PPCInterpreter_t* __restrict ctx)
{
	if ((Probe() || wwhd::rt::SixtyFrom() != ~0u) && !g_rtHalfTick && s_trialPhase != 1)
		s_actors.push_back(GPR(3));
	[[clang::musttail]] return orig_f_025D475C(ctx);
}
