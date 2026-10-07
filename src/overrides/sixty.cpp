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
#include "../os/input.h"
#include "../os/debug_menu.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
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
	// The processes converted and checked so far (D21), when WWHD_60FPS_CONVERT isn't set: the
	// camera, Link, the ship, its sail (GRID), the seagulls, Bokoblins and their sticks (BOKO), Dragon
	// Roost's lava geysers (Obj_Ygush00), lava (Obj_Eayogn), Obj_Gryw00, flags (Tori_Flag), bomb flowers
	// (296, d_a_bflower.cpp) and Obj_Ebomzo; push blocks (Obj_Movebox), chests (TBOX), doors (DOOR10) and the
	// sky (VRBOX, VRBOX2), which match the 30-tick run with no rules (tools/sixty/actor_types.py); Chuchus
	// (CC), Keese (KI), Moblins (MO2), Darknuts (TN), Kargarocs (BB), ReDeads (RD), Gohma (BTD) and
	// Valoo's tail in its room (DR2), Magtails (MT), Peahats (PH), Boko Babas (BO), and Outset's
	// NPC_YM2 and NPC_YW1; Hyrule's flags (MAJUU_FLAG), the capes of Darknuts and Phantom Ganon (MANT),
	// the Moblins' lanterns (KANTERA), Puppet Ganon's three forms (BGN, BGN2, BGN3), Ganondorf (GND),
	// Miniblins (PT) and Bubbles (BL), the Wind Temple's fans (WINDMILL), blade traps (Obj_Trap),
	// Wizzrobes (WZ), moving platforms (MACHINE), 252, Armos (AM, AM2) and Molgera (BWD, BWDG), the Earth
	// Temple's Stalfos (ST), Poes (PW), Floormasters (FM) and Jalhalla (BPW), the Forsaken Fortress's
	// anchors (IKARI), barrels (Obj_Barrel), ropes and their lanterns' moths (HIMO3) and Tetra (NPC_ZL1),
	// the Tower of the Gods' Beamos (Bemos, Beam), lifts (Hmlif), balance lifts, statues (Obj_Try), floor
	// switches (Obj_Swflat), its water (Obj_Tide), Obj_Hha, Obj_Htetu1, stakes (KUI), Obj_Hcbh and Gohdan
	// (BST), rats (NZ), Helmaroc King (BDK), pots (TSUBO), stones (STONE), and Link's arrows (ARROW, ARROW_ICEEFF,
	// ARROW_LIGHTEFF), boomerang (BOOMERANG), hookshot (HOOKSHOT), grappling hook (HIMO2), the Ballad of Gales'
	// cyclone (TORNADO), bait (ESA) and houses' doors (KNOB00); the Wind Temple's grates (Obj_Hami2-4),
	// lift (Obj_Hbrf1), seed soil (Obj_Vmc), spikes (TOGE), masks (Obj_Homen), hooks (Obj_Hfuck1) and
	// spring pads (Obj_Jump 166), the Earth Temple's tapestries (Obj_Tapestry), coffins (Obj_Kanoke) and
	// mirrors (Obj_Mmrr), fire jets (Obj_Flame), Ganon's Tower's waterfalls (Obj_Gtaki, Obj_Gnnbtaki,
	// Obj_Gnntakis, Obj_Gnntakie), Asoko's lifts (Obj_Hlift), the Earth Temple's light statues (Obj_Mkie)
	// and light tags (469), the Wind Temple's propeller grates (AMI_PROP) and breakable floors (FLOOR),
	// the weather's KYEFF and KYEFF2 (478, 479: tick_rules/weather.txt), the cutscenes' puppets DEMO00, Jabun, the
	// Master Sword chamber's knight statues and Tetra's ship (406, 358, 396, 57: tick_rules/cutscenes.txt), the
	// Hyoi seagull (195: tick_rules/companions.txt), the Wind Temple's fans, the boulders
	// Link lifts, the ladders that drop, the life ball, Tetra's gong, the fire walls, the goddess statues and
	// the barriers (299, 455, 85, 399, 284, 286, 265, 285: tick_rules/objects_left.txt), Dragon Roost Cavern's
	// falling rocks (FallRock 422: tick_rules/notes_bottom.txt)
	// (session bottom); Windfall's
	// windmill wheel (Obj_Ferris), pigs (KB),
	// townsfolk (NPC_PEOPLE, NPC_KK1, NPC_MK, NPC_UK, NPC_GK1, NPC_TT, NPC_RSH1), market stalls (Obj_Roten),
	// its distant models (445) and stands (DAI); Outset's fishman (NPC_SO), Beedle's ship (OBJ_IKADA), palms
	// (Obj_Lpalm), crabs (KN), items (ITEM), the grotto (OBJ_HOLE), the mailbox (OBJ_TORIPOST), and indoors
	// Grandma (NPC_BA1), Joel, Zill and Rose (NPC_KO1, NPC_KO2, NPC_OB1), dishes and shelves; the sea's
	// cannons (OBJ_CANON), warships (OSHIP), Gyorg spawners (GY_CTRLB), Obj_Coming, lookout platforms
	// (Obj_Aygr), wind tags, the ships' flags (Sie_Flag) and bombs (BOMB); Forest Haven's fireflies (FF), forest
	// fireflies (NH), lily pads (LEAF_LIFT), baba buds (JBO), trees (Lwood), Obj_Ojtree, WARPFOUT, the Deku
	// Tree (NPC_DE1), KYTAG00, TAG_HINT, BG and Tag_Attention (KUI is in session bottom's line); the Forbidden Woods' vines (SK, SK2),
	// fences (SAKU), acorn leaves (ACORN_LEAF), warp pots (OBJ_WARPT), KDDOOR, leaf piles (Obj_Leaves),
	// Obj_Mtest, ANDSW0, the propeller switch's 430, ceiling tentacles (SHAND), small vines (SSK), Mothulas
	// (GM) and Kalle Demos (BMD, BMDHAND, BMDFOOT); torches (EP) and wall lamps (LAMP); Octoroks (OQ), hanging
	// flower platforms (KITA), the hanging house (KOKIIE), signs (KANBAN), Big Octo (DAIOCTA, DAIOCTA_EYE) and
	// Gyorgs (GY), Morths (KS), floating barrels (Obj_Barrel2), the heat haze (Ykgr), floor switches (Obj_Swpush),
	// fires (Fire), warp lights (WARPLIGHT), light bridges and stairs (LIGHTBRIDGE, LIGHTSTAIR),
	// the hot floor (Hot_Floor), eye switches (Hys), mailboxes (OBJ_TORIPOST), item stands (STANDITEM),
	// bomb flowers' bombs (BOMB2), steam vents (SteamTag), flame lifts (MFLFT), swinging platforms (MSW); the Rito
	// Aerie's post boxes (Obj_Ospbox), Rito (NPC_BM1-5), mail sorter (NPC_BMSW), Komali (NPC_ZK1), Valoo (DR); the
	// Great Fairy (BIGELF), Fire Mountain's magma rocks (42), Cave03's Kryu00 (33), rat holes (199), Sturgeon
	// (NPC_AJ1) and the islands' still objects (143, 274, 262, 287, 87, 460, 64), the Tower of the Gods' water
	// level (Tag_Waterlevel 471: its water, Obj_Tide, reads it every frame) (session top).
	// One line each, so the parallel sessions' additions never meet (docs/handoff.md "Two parallel
	// sessions"); keep a comma after each line's last.
	// WWHD_60FPS_CONVERT= (empty) converts none.
	constexpr const char* kConvertedByDefault =
		"476,168,165,171,194,189,463,151,154,142,175,296,162,43,292,300,437,438,206,215,188,191,181,224,234,223,216,209,214,316,317,"
		"174,192,193,243,244,245,246,247,207,114,135,208,254,252,202,203,217,219,190,212,119,211,431,456,447,426,233,232,40,111,458,29,39,136,137,250,150,240,198,238,453,454,472,473,474,432,169,446,443,221,305,47,48,49,50,52,122,129,148,166,289,159,272,267,275,140,138,139,46,75,469,94,96,478,479,406,358,396,57,195,299,455,85,399,284,286,265,285,241,334,167,379,370,141,251,260,422,279,278,283,312,386,392,413,417,448,444,393,350,429,425,"   // session bottom
		// (session main's line, between comment lines so neighbours' edits don't conflict)
		"51,276,367,301,302,303,113,112,314,361,382,380,321,30,92,104,107,145,157,273,323,451,153,377,124,"   // session main
		// (session top's line)
		"123,220,353,170,368,374,364,373,352,121,445,309,118,68,73,200,255,71,67,335,319,320,331,461,45,63,179,230,269,164,391,176,294,187,313,120,213,407,83,105,116,383,410,439,470,101,102,398,297,65,304,144,74,307,430,99,103,204,235,236,237,185,186,227,97,98,182,225,226,228,205,457,397,27,424,106,427,428,231,450,67,462,295,423,91,90,84,326,327,328,329,330,348,371,222,369,42,33,199,143,274,262,332,287,87,460,64,89,172,173,318,218,210,342,394,355,357,359,362,375,366,322,347,372,365,341,344,340,360,376,325,363,381,346,345,351,324,336,337,338,339,343,280,277,291,271,93,56,55,459,152,128,131,38,401,125,127,60,61,465,268,402,471,26,35,53,58,59,78,80,108,109,110,126,130,132,146,147,155,156,161,163,180,197,256,20,115,183,201,117,160,158,86,133,28,31,79,81,72,76,70,54,77,69,32,34,177,95,36,37,41,248,229,184,178,88,25,249,134,82,257,258,259,62,44,100,149,196,242,66";   // session top
	const char* ConvertList()
	{
		const char* e = getenv("WWHD_60FPS_CONVERT");
		return e ? e : kConvertedByDefault;
	}
	bool Converted(uint16 name)
	{
		static const std::vector<uint16> names = [] {
			std::vector<uint16> v;
			if (const char* e = ConvertList())
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
	// WWHD_60FPS_CONVERT=all[,-n,-m...] (a test mode): every actor (a process seen in fopAc_Execute)
	// but those named is converted, to find with a 30-against-60 track which types need no rules of
	// their own (tools/sixty/actor_types.py, D21); -168 keeps Link to whole ticks, as at 30, so that
	// actors reading him see the 30-tick run's Link
	std::unordered_map<uint32, bool> s_knownActors;
	bool ConvertAll()
	{
		static const bool all = [] { const char* e = getenv("WWHD_60FPS_CONVERT"); return e && strncmp(e, "all", 3) == 0; }();
		return all;
	}
	bool ExcludedFromAll(uint16 name)
	{
		static const std::vector<uint16> out = [] {
			std::vector<uint16> v;
			if (const char* e = getenv("WWHD_60FPS_CONVERT"))
				for (const char* p = strchr(e, '-'); p; p = strchr(p + 1, '-'))
					v.push_back((uint16)atoi(p + 1));
			return v;
		}();
		return std::find(out.begin(), out.end(), name) != out.end();
	}
	bool AnyConverted()
	{
		static const bool any = [] { const char* e = ConvertList(); return e && *e; }();
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
	// Link out of the song on the warp route). A process the event doesn't concern finishes its tick there
	// (InEvent, below); the event's own move half a tick less at that start.
	bool EventRunning()
	{
		return rd8(0x1046F0B0u + 0x5292u) != 0;
	}

	// An event ordered this tick (dEvt_control_c's order count, two bytes before its mode, as in the
	// decomp's layout): the event manager starts it on the next whole tick, so a half tick between
	// runs the orderer before the event is set up (Link's Wind Waker wait cancelled itself). While an
	// event runs, the control takes only a change (an order of type 0xD, as fopAcM_orderChangeEventId
	// f_025D7874 makes it: dEvt_control_c::check ends the event for it; an ending event is EventEnding's)
	// and drops the others at the tick's end: an actor that orders its event every tick through a
	// cutscene (the Master Sword's) held every converted process to whole ticks
	bool EventOrdered()
	{
		constexpr uint32 kEvtControl = 0x1046F0B0u + 0x51D0u;
		const int orders = (sint8)rd8(kEvtControl + 0xC0u);
		if (orders <= 0)
			return false;
		if (rd8(kEvtControl + 0xC2u) == 0)          // no event runs (EventRunning, below)
			return true;
		for (int i = 0; i < orders && i < 8; i++)
			if (rd16(kEvtControl + i * 0x18u) == 0xD)
				return true;
		return false;
	}
	// An event whose end was asked this tick (dComIfGp_event_reset: bit 8 of dEvt_control_c's event flags,
	// g_dComIfG_gameInfo +0x52B8, as DOOR10's demo action sets it, f_02127408): the control ends it on the
	// next whole tick (its check: the partners' event commands cleared, the event's state NONE). At 30 no
	// actor runs between its reset and that. A half tick between ran the actor with its command still set:
	// a converted door, back in its wait action, took the command for a new door event and stayed in its
	// demo action for good (the owner's doors that wouldn't open again; and a door stuck there answered the
	// next door's event too, setting Link's goal along its own direction: Link turning away at a door).
	// WWHD_60FPS_EVENTEND=0 (a probe): as before, and getIsAddvance (f_025447C8, below) as the game's.
	bool EventEndRules()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_EVENTEND"); return !(e && atoi(e) == 0); }();
		return on;
	}
	bool EventEnding()
	{
		return EventEndRules() && (rd16(0x1046F0B0u + 0x52B8u) & 8) != 0;
	}
	std::unordered_map<uint32, bool> s_stepping;    // process -> stepping at 60 this tick
	std::unordered_map<uint32, bool> s_eventAtWhole;   // process -> an event ran at its whole step
	std::unordered_map<uint32, bool> s_stepInEventAtWhole;   // process -> Link's action stepped in events at it

	// The `late` stores a stepping process's whole step passed over (they wait for its half step, the
	// tick's end; rt_late_note, ppc_ops.h), noted with their values, by process, in order, wherever
	// they go. An edge that holds its half step (below) makes them there, finishing the tick's
	// once-a-tick stores; before, they were lost: at the Wind's Requiem's start the camera's call count
	// (m11C) fell a tick behind for the whole song, the song camera's first call came again on the next
	// tick and saved the view it had just set as the one to return to (the view after the song faced
	// Link). A half step that runs makes them itself, so its notes are dropped.
	struct LateNote { uint32 ea, size; uint64 value; };
	std::vector<LateNote> s_lateNotes;              // the running whole step's
	std::unordered_map<uint32, std::vector<LateNote>> s_lateNoted;   // process -> its whole step's
	uint64 s_lateMade = 0;

	void MakeLateNotes(uint32 proc)
	{
		const auto it = s_lateNoted.find(proc);
		if (it == s_lateNoted.end())
			return;
		s_converting++;                             // as its step's stores (HalfTickStore: globals stand)
		for (const LateNote& n : it->second)
		{
			if (n.size == 1)
				wr8(n.ea, (uint8)n.value);
			else if (n.size == 2)
				wr16(n.ea, (uint16)n.value);
			else if (n.size == 4)
				wr32(n.ea, (uint32)n.value);
			else
				wr64(n.ea, n.value);
		}
		s_converting--;
		s_lateMade += it->second.size();
		s_lateNoted.erase(it);
	}
	// Converted processes step at 60 in an event too while Link's action is one checked in events
	// (D21): his procedure's index (daPy_lk_c +0x65F0, the function at +0x65F8) in this list: 4 wait,
	// 6 move (an entrance's walk out matches the 30-tick run, the camera too), 0x88 and 0x89 steering
	// and sitting in the boat (the Ballad of Gales' flight; its camera ruled), 0x9A conducting, 0x9B
	// and 0x9C playing a song back and its end (his melody's countdown and the song camera ruled), 0xAA
	// talking (bug B18; the talk camera ruled), 0xAD opening a chest and 0xAE holding up an item he got
	// (their cameras ruled), 0xB8, 0xCE and 0xCF startled, frozen and held by a ReDead (its two-actor
	// camera ruled), 0xC1 opening a house's door (KNOB00's event, tick_rules/doors.txt), 0xC4 the Wind's
	// Requiem's change of wind (its camera ruled), 0xD2 rising in a warp light (bug B20; his rise and the
	// rolling event camera ruled). In the boss fights' camera events (N8, session bottom: Gohma's fight, the
	// grappling hook's own scene, d_a_himo2.cpp, and Gohma's death, d_a_btd.cpp, each a potential event whose
	// camera the actor sets each step): 7, 8 and 9 the Z-targeted walk, wait and move (as 4 and 6), 0x24,
	// 0x25 and 0x27 a jump off a rope, its landing and a fall, 0x76-0x7F the rope's (the grappling hook's catch
	// is an event: its coil count once a tick, tick_rules/notes_bottom.txt, without which predeploy's grapple
	// ended 29.7 units from 30's against 7.2, gtgrapple 86.8 against 7.1), 0xB9 turning back (a boss door's
	// event). Others hold the event to
	// whole ticks as before; so does the half tick after an event starts, is ordered or is asked to end.
	// WWHD_60FPS_EVENTS=0: no stepping in events at all; WWHD_60FPS_EVENTS=1: not for N8's actions.
	uint32 s_link = 0;                              // Link (168) as he last executed
	// Cutscenes at 60 (below, "cutscenes at 60"; WWHD_60FPS_DEMOS=0 turns it off): Link's cutscene action
	// (0xA9, dProcTool f_0241FD7C: his place, angle and animation's frame from the cutscene's data) steps
	// in events, and the cutscene's values are evaluated for both of the tick's steps
	bool DemoSixty()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_DEMOS"); return !(e && atoi(e) == 0); }();
		return on;
	}
	bool StepInEvents()
	{
		static const int level = [] { const char* e = getenv("WWHD_60FPS_EVENTS"); return e ? atoi(e) : 2; }();
		if (level == 0 || !s_link || rd16(s_link + 0x08) != 168)
			return false;
		const uint32 action = rd32(s_link + 0x65F0);
		if (level >= 2 && ((action >= 7 && action <= 9) || action == 0x24 || action == 0x25 || action == 0x27 ||
			(action >= 0x76 && action <= 0x7F) || action == 0xB9))
			return true;
		return action == 4 || action == 6 || action == 0x88 || action == 0x89 || action == 0x9A || action == 0x9B ||
			action == 0x9C || action == 0xAA || action == 0xAD || action == 0xAE || action == 0xB8 || action == 0xC1 || action == 0xC4 ||
			action == 0xCE || action == 0xCF || action == 0xD2 || (action == 0xA9 && DemoSixty());
	}
	uint64 s_halfSteps = 0, s_eventStops = 0, s_orderStops = 0, s_endStops = 0, s_edgeFinishes = 0, s_actionStops = 0, s_landSnaps = 0;
	void StepStats()
	{
		cemuLog_log(LogType::Force, "wwhd sixty: {} half steps of converted processes; {} stopped by a running event, {} by an ordered one, {} by an ending one; {} of Link's left his new action's call out; {} finished their tick at an event's edge; {} late stores made for stopped ones; {} of Link's landings finished in the half step",
			s_halfSteps, s_eventStops, s_orderStops, s_endStops, s_actionStops, s_edgeFinishes, s_lateMade, s_landSnaps);
	}

	// Link's action call (daPy_lk_c::execute's (this->*mCurProcFunc)(); `hold` rules on it, link_actions.txt)
	// set up a turn in place in his whole step: at 30 that tick only sets the turn up (no turn) and it turns
	// from the next tick, so his half step leaves the call out (g_rtHold) while the rest of it (the common
	// move, collision, animation) runs, and the turn's first step comes in the next tick's whole step. Taking
	// the call there turned half a tick ahead, and the turn's end and the walk after it followed (route door2:
	// walking a tick and a half early, 25 units ahead; now within 3.2). Only for the turn: other set-ups in the
	// call (a walk from waiting, a roll from a roll) have their action's step, or its animation's, in the same
	// tick at 30 in ways the call's edge doesn't show, and holding them lost ground (handoff: Link's actions).
	// WWHD_60FPS_ACTIONHOLD=0 turns it off.
	bool ActionHold()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_ACTIONHOLD"); return !(e && atoi(e) == 0); }();
		return on;
	}
	// the actions whose set-up in his action call holds the half step's call: the turn in place (0x17,
	// procWaitTurn, set up from waiting or moving by checkNextMode with no turn that tick at 30) and waiting
	// (4, set up when a turn or a move ends, its own step, the stick's check, coming the next tick at 30)
	// and the slash while moving (0x42, procCutF: predeploy's plants 45.5 -> 37.0 units, the rest unchanged), and the
	// Deku Leaf's glide (0x93, procFanGlide: set up from a jump with X, its first step sets its lift, at 30 the next
	// tick: route fwbud's glide 10-15 units low, missing the ledge 30 catches; now within 0.3 units, session top's
	// dungeons2), and pushing and pulling a block (0x33 procPushMove, 0x34 procPullMove, set up from 0x32 with the stick:
	// their first step calls the block's push-pull callback, whose count starts the block's walk, at 30 the next tick;
	// session qa, B48: route crate's crate a tick early, now its slide within 0.75 units of 30's).
	// Tried and not held: 0x24 (procAutoJump: no change) and 0x36 (procSwimWait: swing 70 -> 137).
	// WWHD_60FPS_HOLDEXTRA=a,b,... (hex action numbers): more of them, to try (the drifts item)
	bool HoldsAfter(uint32 action)
	{
		static const std::vector<uint32> extra = [] {
			std::vector<uint32> v;
			if (const char* e = getenv("WWHD_60FPS_HOLDEXTRA"))
				for (const char* p = e; *p;)
				{
					char* end;
					v.push_back(uint32(strtoul(p, &end, 16)));
					p = *end == ',' ? end + 1 : end;
					if (end == p && *p) break;
				}
			return v;
		}();
		return action == 0x17 || action == 4 || action == 0x42 || action == 0x93 || action == 0x33 || action == 0x34 ||
			std::find(extra.begin(), extra.end(), action) != extra.end();
	}
	bool s_linkActionChanged = false;                 // by his action call in the last whole step
	uint32 s_linkActionAtCall = 0;

	// His half step after a whole step whose move landed him or took the ground from under him (his ground check's
	// flags, mAcch at +0x80C, m_flags at +0x834: 0x20 the ground hit) leaves his action call out too (session bottom's
	// "landings", from session top's look; B39, top's knockback rule in link_actions.txt): at 30 the tick after it
	// reacts (a landing: changeLandProc's land, land damage or roll, the other actions' own landings; the ground lost
	// under one of the ground's actions: changeAutoJumpProc's auto jump, ledge hang 0x2C or fall, whole in
	// link_actions.txt), and his half step reacted half a tick early (a drop from 400 units: fall and land a tick
	// early). For the ground lost the half step doesn't move him either (sixty_step.cpp's posMoveFromFootPos, by
	// g_rtLinkGroundLost): his walk's foot-driven speed in the air made 17 where 30 had 14.4 and the dock's auto jump
	// went higher; left still, his next whole step starts where 30's next tick does. WWHD_60FPS_GROUNDHOLD=0 turns it off.
	bool GroundHold()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_GROUNDHOLD"); return !(e && atoi(e) == 0); }();
		return on;
	}
	uint32 s_linkGroundAtWhole = 0;                   // his ground hit bit as his whole step began
	uint64 s_groundStops = 0;
	// The half step after a whole step whose move landed him finishes the tick's landing (session qa, bug B46): an air
	// action (his auto jump) coming down on a slope with forward speed landed in the whole step's move (its speed.y then
	// 0), and the half step's move, with half a tick's gravity, ended 0.02 units above the slope further on: no ground
	// hit, and autoGroundHit (the GameCube's daPy_lk_c::autoGroundHit) puts a Link on ground within 30.1
	// units below only when he isn't flying. His next whole step's action call saw no ground, and it went on for 20
	// ticks down the owner's slope in Dragon Roost Cavern (each whole step landing, each half step losing it). At 30 the
	// tick's one move went below the slope and landed at its end. So that half step's lost ground within 30.1 units
	// below is his ground, as autoGroundHit's: his height, the ground check's landing bits and speed.y 0 (the next whole
	// step's action call then lands him, where 30's tick does). WWHD_60FPS_LANDSNAP=0 turns it off.
	bool LandSnap()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_LANDSNAP"); return !(e && atoi(e) == 0); }();
		return on;
	}
	void LandSnapAfter(uint32 link, uint32 landingBits)
	{
		const uint32 flags = rd32(link + 0x834);           // mAcch (+0x80C)'s m_flags: 0x20 the ground hit
		const float y = std::bit_cast<float>(rd32(link + 0x318));
		const float groundH = std::bit_cast<float>(rd32(link + 0x8A0));   // mAcch's m_ground_h (GameCube +0x94)
		const float speedY = std::bit_cast<float>(rd32(link + 0x340));
		if ((flags & 0x20) || !(groundH <= y && groundH >= y - 30.1f) || speedY > 0.0f)
			return;
		wr32(link + 0x318, std::bit_cast<uint32>(groundH));
		wr32(link + 0x834, flags | landingBits);
		wr32(link + 0x340, 0);
		s_landSnaps++;
	}
	// the ground's actions whose lost ground changeAutoJumpProc reacts to: waiting, moving (and targeting), the turns,
	// the rolls, a landing's
	bool GroundAction(uint32 a)
	{
		switch (a)
		{
		case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x17: case 0x18: case 0x1E: case 0x21:
		case 0x25:
			return true;
		default:
			return false;
		}
	}

	// A process outside the event finishes its tick at an event's edge (D21). The half tick's stops above
	// (an event that began, was ordered or was asked to end during the whole tick, after the process took
	// its whole-tick step) cancelled every converted process's half step there, so each moved half a tick
	// less at every event's start (the Tower of the Gods' light stairs fell half a frame behind 30's at
	// each). The stops are for the event's own: Link, the camera, an actor the event controls (its event
	// command, dEvt_info_c at +0xF8, set; the staff status fopAcStts_FORCEMOVE, 0x8000 of actor_status
	// at +0x2E0) and the actors of a pending order (dEvt_control_c at g_dComIfG_gameInfo +0x51D0: eight
	// orders of 0x18 bytes, the actors at +0x08 and +0x0C, the count at +0xC0, as its order, f_0253EC0C,
	// writes them); anything that isn't an actor stays held too. The others take their half step, as
	// at 30 they finished the tick before the event. WWHD_60FPS_EVENTEDGE=0 (a probe): as before.
	bool EventEdgeFinish()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_EVENTEDGE"); return !(e && atoi(e) == 0); }();
		return on;
	}
	bool InEvent(uint32 proc)
	{
		const uint16 name = rd16(proc + 0x08);
		if (name == 168 || name == 476 || !s_knownActors.count(proc))
			return true;
		if (rd16(proc + 0xF8) != 0 || (rd32(proc + 0x2E0) & 0x8000u) != 0)
			return true;
		constexpr uint32 kEvtControl = 0x1046F0B0u + 0x51D0u;
		const int orders = (sint8)rd8(kEvtControl + 0xC0u);
		for (int i = 0; i < orders && i < 8; i++)
			if (rd32(kEvtControl + i * 0x18u + 0x08u) == proc || rd32(kEvtControl + i * 0x18u + 0x0Cu) == proc)
				return true;
		return false;
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
		static const bool all = [] { const char* e = getenv("WWHD_STATE_TRACK"); return e && strcmp(e, "all") == 0; }();
		return all || (!names.empty() && std::find(names.begin(), names.end(), name) != names.end());
	}
	std::vector<uint32> s_tracked;                  // tracked processes that executed this frame
	bool s_firstDraw = true;                        // a frame's first draw is still to come (f_025DE2CC)

	// ---- the flight recorder (WWHD_FLIGHT=path with WWHD_STATE_TRACK=n,m,...: real-time play) ------
	// The tracked processes' bytes after every frame (whole and half ticks), and the controller as the
	// game last read it, for the last WWHD_FLIGHT_FRAMES frames (default 1200: 20 s at 60 fps), kept
	// in memory without the probe. F9 in the window, closing it, and an exit write them to path-TIME-N.bin as
	// track.bin is written (tools/sixty/compare.py's load_track; the controller as process 0xFFFF, 28
	// bytes: the swap, the buttons (os/input.h's), the four sticks as floats, the host clock in
	// microseconds (its low word), all host order). The owner plays, sees something wrong, presses F9: the frames that led to
	// it. Game memory: it goes off the desktop to the worker.
	std::deque<std::vector<uint8>> s_flight;
	std::atomic<int> s_flightAsk{ 0 };              // 1: write at the next frame; 2: written
	uint32 s_flightDumps = 0;
	void FlightWrite();
	bool Flight()
	{
		static const bool on = [] {
			const char* e = getenv("WWHD_FLIGHT");
			if (!e || !*e)
				return false;
			atexit(FlightWrite);                       // headless runs end by exit() on the game's thread
			at_quick_exit(FlightWrite);
			return true;
		}();
		return on;
	}

	void FlightWrite()
	{
		static const std::string path = getenv("WWHD_FLIGHT");
		static const long long started = (long long)time(nullptr);
		const std::string name = path + "-" + std::to_string(started) + "-" + std::to_string(++s_flightDumps) + ".bin";
		if (FILE* f = fopen(name.c_str(), "wb"))
		{
			for (const auto& frame : s_flight)
				fwrite(frame.data(), 1, frame.size(), f);
			fclose(f);
			cemuLog_log(LogType::Force, "wwhd flight: {} frames to {}", s_flight.size(), name);
		}
	}

	void FlightRecord(uint32 tick2)
	{
		static const size_t frames = [] { const char* e = getenv("WWHD_FLIGHT_FRAMES"); return e ? (size_t)atoi(e) : 1200u; }();
		std::vector<uint8> rec;
		auto put = [&](const void* p, size_t n) { rec.insert(rec.end(), (const uint8*)p, (const uint8*)p + n); };
		for (uint32 proc : s_tracked)
		{
			const uint32 profile = rd32(proc + 0x10);
			uint32 size = profile ? rd32(profile + 0x10) : 0;
			if (size == 0 || size > 0x40000)
				continue;
			size = std::min(size, 0x8000u);
			const uint32 head[4] = { tick2, rd16(proc + 0x08), proc, size };
			put(head, sizeof(head));
			put(memory_base + proc, size);
		}
		const wwhd::os::input::Pad pad = wwhd::os::input::LastRead();
		const uint64 clock = (uint64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		const uint32 head[4] = { tick2, 0xFFFFu, 0, 28 };
		const uint32 body[7] = { wwhd::os::SwapCount(), pad.buttons, std::bit_cast<uint32>(pad.lx), std::bit_cast<uint32>(pad.ly),
			std::bit_cast<uint32>(pad.rx), std::bit_cast<uint32>(pad.ry), (uint32)clock };   // the host clock (us), low word
		put(head, sizeof(head));
		put(body, sizeof(body));
		s_flight.push_back(std::move(rec));
		while (s_flight.size() > frames)
			s_flight.pop_front();
		if (s_flightAsk.load() == 1)
		{
			FlightWrite();
			s_flightAsk.store(2);
		}
	}
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

	// WWHD_STATE_CENSUS_TRACE=addr: for stores to that address, the storing instruction and the guest
	// call chain (8 callers);
	// WWHD_STATE_CENSUS_CHAINS=1: for every store into an actor or g_dComIfG_gameInfo, the target
	// and the first 4 callers (dir/census_chains.txt), to find the call sites to put rules on
	std::unordered_map<std::string, uint64> s_traces, s_chains;
	constexpr uint32 kGameInfo = 0x1046F0B0u, kGameInfoSize = 0x6000u;   // f_025200D4's singleton

	void CensusTrace(uint32 ea, uint32 pc)
	{
		static const uint32 watched = [] { const char* e = getenv("WWHD_STATE_CENSUS_TRACE"); return e ? (uint32)strtoul(e, nullptr, 16) : 0u; }();
		if (ea != watched || watched == 0)
			return;
		char at[16];
		snprintf(at, sizeof(at), " at %08x:", pc);
		s_traces[at + GuestChain(8)]++;
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
		CensusTrace(ea, pc);
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
	bool s_attached = false;                         // an actor Link holds runs for a half tick's draw

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
		const bool drawn = ((level >= 3 && s_drawing != 0) || s_attached) && !(ea + 0x10000 > sp && ea < sp + 0x10000);
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

	// Live structures: lists whose nodes are on the heap (which a half tick doesn't put back) and whose
	// heads or links can be in .data and .bss. What their own code writes is neither put back after a
	// half tick nor hidden from its draw pass, or heads and nodes part:
	// - J3DDrawBuffer's lists (J3DDrawBuffer.cpp): frameInit (f_027F093C) takes every packet off its
	//   buffer's list, the entries (f_027F0C14 to f_027F1088: entryImm and the sorted ones) put one on
	//   and J3DPacket::clear (f_027F1508) unlinks it: a packet's slot (+0x94) and next (+0x10), the
	//   lists' heads. A static packet (Link's eye packets, in .bss) that a half tick's frameInit took
	//   off its list got its slot back from the rollback, and the next entryImm asserted
	//   (J3DDrawBuffer.cpp 243; in the Forbidden Woods, 4 OSPanics at every warp there).
	// - JAudio's JAI layer (f_02801444 to f_0280E03C: JAIAnimation.cpp to JAISoundTable.cpp): its
	//   sound handles move between lists headed in .bss (JAISeMgr's at 0x104B5008, f_0280593C). A
	//   converted Link's footstep started on a half tick had its heads hidden from the draw pass and
	//   handed back after it: in the Forbidden Woods the next sound segfaulted (f_0280593C).
	bool LiveStore(uint32 pc)
	{
		return (pc >= 0x027F093Cu && pc < 0x027F0A0Cu) || (pc >= 0x027F0C14u && pc < 0x027F1088u) ||
			(pc >= 0x027F1508u && pc < 0x027F1518u) || (pc >= 0x02801444u && pc < 0x0280E03Cu);
	}

	void HalfTickStore(uint32 ea, uint32 size, uint32 pc)
	{
		if (Census())
			CensusStore(ea, size, pc);
		if (LiveStore(pc))
			return;
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
		if (!s_pagesSet.empty() && RollbackLevel() == 2 && !s_attached)
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

	// What Link holds. A converted Link's half step moves his hands, but what he holds (his four
	// daPy_actorKeep_c at +0x6590, an ID and the actor: the equipped item (the arrow on the bow), the
	// thrown, the grabbed (a pot, a bomb, a rock) and the rope) runs its execute on whole ticks, so a
	// half tick drew it where his hands had been (the owner's flickering pots, bombs, rocks and arrow).
	// Link's half step notes them; on the half tick each one's execute runs again as at 30, a step of
	// 1, just before its own draw, for that draw only: every store it makes but on the stack is
	// journaled and put back after the frame. Before its draw rather than after Link's step: Link's
	// draw (a converted process's, standing) reads what he holds and would keep what it saw (the bow's
	// charge, Link +0x46B0, gained a tick).
	// What Link lets go of in a throw (B29). While he carries something, his steps set its position to his
	// hands' (setGrabItemPos), so his half step puts it where his hands are half a tick on; at 30 a throw
	// (procGrabThrow, his action 0x71) lets go of it where the last tick put it. Thrown from a half step's
	// place, a carried Medli started past the wall she meets at 30 and fell out of the world. So after
	// each whole step the grabbed actor (keep +0x65A0) and its place are noted, and a whole step whose
	// throw let go of it puts it back there (current and old position), as at 30. WWHD_60FPS_THROWPOS=0
	// turns it off.
	uint32 s_grabbed = 0, s_grabbedId = 0;
	float s_grabbedPos[3];

	void ThrowPlace(uint32 link)
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_THROWPOS"); return !(e && atoi(e) == 0); }();
		const uint32 id = rd32(link + 0x65A0), actor = rd32(link + 0x65A4);
		const bool holding = id != ~0u && actor >= 0x10000000u && actor < 0x50000000u && rd32(actor + 4) == id;
		if (on && s_grabbed && (!holding || actor != s_grabbed) && rd32(link + 0x65F0) == 0x71 &&
			rd32(s_grabbed + 4) == s_grabbedId)
		{
			for (int i = 0; i < 3; i++)
			{
				wr32(s_grabbed + 0x314 + 4 * i, std::bit_cast<uint32>(s_grabbedPos[i]));
				wr32(s_grabbed + 0x300 + 4 * i, std::bit_cast<uint32>(s_grabbedPos[i]));
			}
		}
		s_grabbed = holding ? actor : 0;
		s_grabbedId = holding ? id : 0;
		if (holding)
			for (int i = 0; i < 3; i++)
				s_grabbedPos[i] = std::bit_cast<float>(rd32(actor + 0x314 + 4 * i));
	}

	std::vector<uint32> s_held;                     // the actors Link holds, this half tick

	void NoteHeld(uint32 link)
	{
		s_held.clear();
		for (uint32 o = 0x6590; o <= 0x65A8; o += 8)
		{
			const uint32 id = rd32(link + o), actor = rd32(link + o + 4);
			if (id == ~0u || actor < 0x10000000u || actor >= 0x50000000u || rd32(actor + 4) != id || Converted(rd16(actor + 8)))
				continue;
			if (std::find(s_processes.begin(), s_processes.end(), actor) == s_processes.end())
				continue;                                  // not in the last whole tick (no rollback for it)
			s_held.push_back(actor);
		}
	}

	// A half tick's draw of the process r3 (fn, fpcM_Draw): if Link holds it, its execute runs first,
	// and what that execute changed is put back right after the draw, so no later draw sees it. The
	// draw's stores are all journaled too (the arrow's draw sets the bow's charge in Link, whose
	// memory, a converted process's, the journal otherwise leaves alone), and its saves of the
	// execute's bytes get the bytes from before the execute: the frame's rollback puts back the tick's.
	bool HeldDraw(PPCInterpreter_t* ctx, void (*fn)(PPCInterpreter_t*))
	{
		const uint32 proc = GPR(3);
		auto it = std::find(s_held.begin(), s_held.end(), proc);
		if (it == s_held.end())
			return false;
		s_held.erase(it);
		Registers regs;
		regs.Save(ctx);
		const size_t mark = s_saved.size();
		const int converting = s_converting;            // Link draws what he holds within his own draw,
		s_converting = 0;                               // a converted process's (no journal there)
		const float step = g_rtStep;
		g_rtStep = 1.0f;
		s_attached = true;
		const uint32* pages = g_rtStorePages;
		g_rtStorePages = nullptr;                   // every store of its execute and draw is journaled
		orig_f_025DE58C(ctx);
		regs.Restore(ctx);
		const size_t executed = s_saved.size();
		fn(ctx);                                    // journaled whole too: its draw writes into Link
		g_rtStorePages = pages;
		s_attached = false;                         // (the bow's charge, from the arrow's state)
		g_rtStep = step;
		s_converting = converting;
		std::unordered_map<uint32, uint8> before;      // byte -> its value before the execute
		for (size_t i = mark; i < executed; i++)
			for (uint32 b = 0; b < s_saved[i].size; b++)
				before.try_emplace(s_saved[i].ea + b, s_saved[i].bytes[b]);
		for (size_t i = executed; i < s_saved.size(); i++)
			for (uint32 b = 0; b < s_saved[i].size; b++)
				if (auto f = before.find(s_saved[i].ea + b); f != before.end())
					s_saved[i].bytes[b] = f->second;
		for (size_t i = executed; i-- > mark;)
			memcpy(memory_base + s_saved[i].ea, s_saved[i].bytes, s_saved[i].size);
		s_saved.erase(s_saved.begin() + mark, s_saved.begin() + executed);
		return true;
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
				// and over all its particles: the oldest's age and the lives' mean
				float oldest = 0, lives = 0;
				uint32 n = 0;
				for (uint32 pl = first; pl; pl = rd32(pl + 12), n++)
				{
					oldest = std::max(oldest, std::bit_cast<float>(rd32(rd32(pl) + 0x78)));
					lives += std::bit_cast<float>(rd32(rd32(pl) + 0x7C));
				}
				fprintf(f, "  emitter %08x group %u cb %08x pcb %08x rate %.3f step %u dyn %08x volume %u div %u particles %u age %.1f life %.1f oldest %.1f lives %.2f\n",
					e, g, cb ? rd32(cb) : 0, pcb ? rd32(pcb) : 0, r, rd8(e + 0x27), rd32(e + 0x84), rd8(e + 0x26), rd16(e + 0x66),
					rd32(e + 0x1B4), age, life, oldest, n ? lives / n : 0.0f);
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
	// F9 in the window: the flight recorder's frames to a file, at the game's next frame
	void FlightDump()
	{
		if (Flight())
			s_flightAsk.store(1);
	}

	// the window closing: the same, waiting up to half a second for the game's thread to write them
	void FlightFinal()
	{
		if (!Flight())
			return;
		s_flightAsk.store(1);
		for (int i = 0; i < 50 && s_flightAsk.load() != 2; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	// the half tick's journal since the last call: stores seen and saved (the frame log, pacing.cpp)
	void TakeJournalCounts(uint64& seen, uint64& saved)
	{
		seen = s_storesSeen + g_rtStoresPassed;
		saved = s_storesSaved;
		s_storesSeen = s_storesSaved = 0;
		g_rtStoresPassed = 0;
	}
}

// WWHD_DEBUG_STAGE=tick:NAME,point,room,layer[;tick:...] (a test aid): at those game frames, a stage
// change as an exit asks for one: g_dComIfG_gameInfo's next stage (+0x5140: the name, 8 bytes; the
// spawn point, s16; the room and layer, s8; then enabled and the wipe, as dStage_nextStage_c's), so a
// route can start anywhere (e.g. 920:M_NewD2,0,0,-1: Dragon Roost Cavern's entrance)
namespace
{
	struct StageWarp { int tick, point, room, layer; char name[9]; };
	// the debug menu's warp (wwhd::debug::RequestStage, src/os/debug_menu.h): one waiting for the next
	// game frame
	std::mutex s_warpLock;
	bool s_warpWaiting = false;
	StageWarp s_warp{};
	void NextStage(const StageWarp& w)
	{
		constexpr uint32 kNext = 0x1046F0B0u + 0x5140u;
		for (uint32 i = 0; i < 8; i++)
			wr8(kNext + i, (uint8)w.name[i]);
		wr16(kNext + 8, (uint16)w.point);
		wr8(kNext + 0xA, (uint8)w.room);
		wr8(kNext + 0xB, (uint8)w.layer);
		wr8(kNext + 0xC, 1);
		wr8(kNext + 0xD, 0);
		cemuLog_log(LogType::Force, "wwhd debug: next stage {} point {} room {} layer {}", w.name, w.point, w.room, w.layer);
	}
	// the boss rush's turn (below): the game's own stage change, its wipe as it asked, to the next boss
	void RedirectStage(const StageWarp& w)
	{
		constexpr uint32 kNext = 0x1046F0B0u + 0x5140u;
		for (uint32 i = 0; i < 8; i++)
			wr8(kNext + i, (uint8)w.name[i]);
		wr16(kNext + 8, (uint16)w.point);
		wr8(kNext + 0xA, (uint8)w.room);
		wr8(kNext + 0xB, (uint8)w.layer);
		cemuLog_log(LogType::Force, "wwhd debug: boss rush: on to {} point {} room {} layer {}", w.name, w.point, w.room, w.layer);
	}
	void DebugStage()
	{
		wwhd::debug::BossBeatenHere();                  // the refights' boss beaten here sees a stage change's request
		if (!g_rtHalfTick)
		{
			std::lock_guard lock(s_warpLock);
			if (s_warpWaiting)
			{
				s_warpWaiting = false;
				NextStage(s_warp);
			}
			// the boss rush (debug_menu.cpp): the stage change the game asks for once a boss is beaten (its
			// warp out) goes to the next boss instead
			else if (rd8(0x1046F0B0u + 0x5140u + 0xC) != 0)
			{
				const char* name;
				StageWarp w{ 0, 0, 0, -1, {} };
				if (wwhd::debug::RushNextStage(name, w.point, w.room, w.layer))
				{
					strncpy(w.name, name, 8);
					RedirectStage(w);
				}
			}
			// WWHD_DEBUG_RUSHBEATEN=tick (a test aid): the boss rush's boss beaten at that game frame
			static const int beaten = [] { const char* e = getenv("WWHD_DEBUG_RUSHBEATEN"); return e ? atoi(e) : -1; }();
			if (beaten >= 0 && (int)wwhd::rt::GameFrame(wwhd::os::SwapCount()) == beaten)
				wwhd::debug::RushBossBeaten();
		}
		static const std::vector<StageWarp> warps = [] {
			std::vector<StageWarp> v;
			if (const char* e = getenv("WWHD_DEBUG_STAGE"))
				for (const char* p = e; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : nullptr)
				{
					StageWarp w{ -1, 0, 0, -1, {} };
					if (sscanf(p, "%d:%8[^,],%d,%d,%d", &w.tick, w.name, &w.point, &w.room, &w.layer) == 5)
						v.push_back(w);
				}
			return v;
		}();
		if (warps.empty() || g_rtHalfTick)
			return;
		const int now = (int)wwhd::rt::GameFrame(wwhd::os::SwapCount());
		for (const StageWarp& w : warps)
			if (w.tick == now)
				NextStage(w);
	}
}

namespace wwhd::debug
{
	void RequestStage(const char* name, int point, int room, int layer)
	{
		std::lock_guard lock(s_warpLock);
		s_warp = StageWarp{ 0, point, room, layer, {} };
		strncpy(s_warp.name, name, 8);
		s_warpWaiting = true;
	}
}

// WWHD_DEBUG_SPAWN=tick:process[/subtype],param,x,y,z[,anglex[,angley[,anglez]]][;...] (a test aid): at those game
// frames an actor of that process name (actor_names.tsv's numbers), subtype (the name table's argument, default 0:
// the sea's Octorok "Oqw" is 227/1, the pools' "Oq" 227/0) and parameters (hex) is created at that position
// in Link's room, as fopAcM_create does: the creation record (f_025D5678: parameters, position, room,
// angle, scale, subtype, parent) and fpcM_Create (f_025E14A8: the layer, *0x101F3AE8, the process
// name, no create function, the record). anglex (hex), the angle's x, is more parameters for some
// actors (a Darknut's equipment is (anglex >> 5) & 7: 0x80 a shield and a cape); angley (hex) is its
// heading (a grappling hook's stake, KUI, takes the hook only from across its axis); anglez (hex) is more
// parameters again (a chest's item, TBOX, is anglez >> 8)
void f_025D5678(PPCInterpreter_t* __restrict ctx);
void f_025E14A8(PPCInterpreter_t* __restrict ctx);
namespace
{
	struct Spawn { int tick, proc, subtype; uint32 param; float x, y, z; uint32 anglex, angley, anglez; };
	// creates the actor as fopAcM_create does (the creation record, then fpcM_Create)
	void SpawnNow(PPCInterpreter_t* ctx, const Spawn& w)
	{
		Registers regs;
		regs.Save(ctx);
		const uint32 sp = (ctx->gpr[1] - 0x200) & ~0xFu;
		const uint32 pos = sp + 0x100;
		wr32(pos, std::bit_cast<uint32>(w.x));
		wr32(pos + 4, std::bit_cast<uint32>(w.y));
		wr32(pos + 8, std::bit_cast<uint32>(w.z));
		const uint32 angle = sp + 0x110;           // csXyz: x, y, z
		wr16(angle, (uint16)w.anglex);
		wr16(angle + 2, (uint16)w.angley);
		wr16(angle + 4, (uint16)w.anglez);
		wr32(sp, ctx->gpr[1]);                     // a back chain
		ctx->gpr[1] = sp;
		ctx->gpr[3] = w.param;
		ctx->gpr[4] = pos;
		ctx->gpr[5] = (uint32)(sint32)(sint8)rd8(s_link + 0x326);   // Link's room
		ctx->gpr[6] = w.anglex || w.angley || w.anglez ? angle : 0;
		ctx->gpr[7] = 0;
		ctx->gpr[8] = (uint32)(sint32)w.subtype;
		ctx->gpr[9] = ~0u;
		f_025D5678(ctx);
		const uint32 append = ctx->gpr[3];
		uint32 id = ~0u;
		if (append)
		{
			ctx->gpr[1] = sp;
			ctx->gpr[3] = rd32(0x101F3AE8u);
			ctx->gpr[4] = (uint32)w.proc;
			ctx->gpr[5] = 0;
			ctx->gpr[6] = 0;
			ctx->gpr[7] = append;
			f_025E14A8(ctx);
			id = ctx->gpr[3];
		}
		regs.Restore(ctx);
		cemuLog_log(LogType::Force, "wwhd debug: spawned process {}/{} param {:08x} at {} {} {}: id {:x}", w.proc, w.subtype, w.param, w.x, w.y, w.z, id);
	}

	// the debug menu's spawn (wwhd::debug::RequestSpawn): one waiting for the next game frame, placed
	// 150 units ahead of Link and facing him
	std::mutex s_spawnLock;
	bool s_spawnWaiting = false;
	Spawn s_spawnAsked{};

	void DebugSpawn(PPCInterpreter_t* ctx)
	{
		static const std::vector<Spawn> spawns = [] {
			std::vector<Spawn> v;
			if (const char* e = getenv("WWHD_DEBUG_SPAWN"))
				for (const char* p = e; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : nullptr)
				{
					Spawn w{ -1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
					int n = 0;
					if (sscanf(p, "%d:%d%n", &w.tick, &w.proc, &n) != 2)
						continue;
					const char* q = p + n;
					if (*q == '/')
					{
						int c = 0;
						if (sscanf(q + 1, "%d%n", &w.subtype, &c) != 1)
							continue;
						q += 1 + c;
					}
					if (sscanf(q, ",%x,%f,%f,%f,%x,%x,%x", &w.param, &w.x, &w.y, &w.z, &w.anglex, &w.angley, &w.anglez) >= 4)
						v.push_back(w);
				}
			return v;
		}();
		if (g_rtHalfTick || !s_link || rd16(s_link + 0x08) != 168)
			return;
		{
			std::lock_guard lock(s_spawnLock);
			if (s_spawnWaiting)
			{
				s_spawnWaiting = false;
				Spawn w = s_spawnAsked;
				const sint16 heading = (sint16)rd16(s_link + 0x32A);   // shape_angle.y
				const double a = heading * (3.14159265358979 / 32768.0);
				w.x = std::bit_cast<float>(rd32(s_link + 0x314)) + 150.0f * (float)std::sin(a);
				w.y = std::bit_cast<float>(rd32(s_link + 0x318));
				w.z = std::bit_cast<float>(rd32(s_link + 0x31C)) + 150.0f * (float)std::cos(a);
				w.angley = (uint16)(heading + 0x8000);
				SpawnNow(ctx, w);
			}
		}
		if (spawns.empty())
			return;
		const int now = (int)wwhd::rt::GameFrame(wwhd::os::SwapCount());
		for (const Spawn& w : spawns)
			if (w.tick == now)
				SpawnNow(ctx, w);
	}
}

// WWHD_DEBUG_FLAGS=ev:XXYY=V[,...][;sw:N=V[,...]][;it:XX=V[,...]][;ac:N|*=V][;sy:N=V][;im:N=V][;er:XXYY=N] (a test aid, session bottom's census-left): the
// save's event bits (dSv_event_c::isEventBit f_025B8B94: XXYY hex, its byte XX and mask YY, as the decomp's
// dSv_event_flag_c names them), switches (dSv_info_c::isSwitch f_025BA0C0: N decimal, in any room) and items got
// (dComIfGs_checkGetItem f_02520C0C: XX hex, d_item_data.h's numbers) and placed actors done (dSv_info_c::isActor
// f_025BA6A4, the stage loader's check of a placement's set ID: N decimal, or * for all, so beaten enemies are placed
// again), the symbols got (dSv_player_collect_c::isSymbol f_025B7D90: N decimal, the bit, 0-7) and the stage's items
// taken (dSv_info_c::isItem f_025BA494: N decimal, in any room) read as V (0 or 1), and event registers
// (dSv_event_c::getEventReg f_025B8BB0: XXYY hex as the decomp's names, e.g. C203) read as N (decimal)
// by every caller, the save left as it is: an actor whose create returns cPhs_ERROR_e on the finished save's story
// state (Phantom Ganon beaten, Makar's types, Co1 before symbol 1...) is made by forcing what it checks.
namespace
{
	struct ForcedFlags { std::unordered_map<uint32, bool> ev, sw, it, ac, sy, im; std::unordered_map<uint32, uint32> er; int acAll = -1; bool any = false; };
	const ForcedFlags& Forced()
	{
		static const ForcedFlags f = [] {
			ForcedFlags r;
			const char* e = getenv("WWHD_DEBUG_FLAGS");
			if (!e)
				return r;
			std::string all(e);
			for (size_t at = 0; at < all.size();)
			{
				size_t end = all.find(';', at);
				if (end == std::string::npos)
					end = all.size();
				const std::string part = all.substr(at, end - at);
				at = end + 1;
				const bool isEv = part.rfind("ev:", 0) == 0, isSw = part.rfind("sw:", 0) == 0, isIt = part.rfind("it:", 0) == 0,
					isAc = part.rfind("ac:", 0) == 0, isSy = part.rfind("sy:", 0) == 0, isIm = part.rfind("im:", 0) == 0,
					isEr = part.rfind("er:", 0) == 0;
				if (!isEv && !isSw && !isIt && !isAc && !isSy && !isIm && !isEr)
					continue;
				for (size_t p = 3; p < part.size();)
				{
					size_t q = part.find(',', p);
					if (q == std::string::npos)
						q = part.size();
					const std::string item = part.substr(p, q - p);
					p = q + 1;
					const size_t eq = item.find('=');
					if (eq == std::string::npos)
						continue;
					const bool v = atoi(item.substr(eq + 1).c_str()) != 0;
					r.any = true;
					if (isEr)
					{
						r.er[(uint32)strtoul(item.substr(0, eq).c_str(), nullptr, 16)] = (uint32)atoi(item.substr(eq + 1).c_str());
						continue;
					}
					if (isAc && item.substr(0, eq) == "*")
					{
						r.acAll = v;
						continue;
					}
					const uint32 key = (uint32)strtoul(item.substr(0, eq).c_str(), nullptr, isSw || isAc || isSy || isIm ? 10 : 16);
					(isEv ? r.ev : isSw ? r.sw : isAc ? r.ac : isSy ? r.sy : isIm ? r.im : r.it)[key] = v;
					r.any = true;
				}
			}
			return r;
		}();
		return f;
	}
}

void orig_f_025B8B94(PPCInterpreter_t* __restrict ctx);
// dSv_event_c::getEventReg(event r3, register r4) -> r3: forced under WWHD_DEBUG_FLAGS's er: (above)
void f_025B8BB0(PPCInterpreter_t* __restrict ctx)
{
	const ForcedFlags& f = Forced();
	if (f.any)
		if (const auto it = f.er.find(GPR(4) & 0xFFFF); it != f.er.end())
		{
			GPR(3) = it->second;
			return;
		}
	[[clang::musttail]] return orig_f_025B8BB0(ctx);
}
void orig_f_025BA0C0(PPCInterpreter_t* __restrict ctx);
// dSv_event_c::isEventBit(event r3, number r4) -> r3: forced under WWHD_DEBUG_FLAGS (above); 3F10 (Puppet
// Ganon beaten) "no" under the refights as a dungeon's boss bit is (f_025B9100)
void f_025B8B94(PPCInterpreter_t* __restrict ctx)
{
	const ForcedFlags& f = Forced();
	if (f.any)
		if (const auto it = f.ev.find(GPR(4) & 0xFFFF); it != f.ev.end())
		{
			GPR(3) = it->second ? 1 : 0;
			return;
		}
	if ((GPR(4) & 0xFFFF) == 0x3F10 && wwhd::debug::BossRefight() && !wwhd::debug::BossBeatenHere())
	{
		GPR(3) = 0;
		return;
	}
	[[clang::musttail]] return orig_f_025B8B94(ctx);
}

void orig_f_025B7D90(PPCInterpreter_t* __restrict ctx);
// dSv_player_collect_c::isSymbol(collect r3, number r4) -> r3: forced under WWHD_DEBUG_FLAGS (above)
void f_025B7D90(PPCInterpreter_t* __restrict ctx)
{
	const ForcedFlags& f = Forced();
	if (!f.any)
		[[clang::musttail]] return orig_f_025B7D90(ctx);
	if (const auto it = f.sy.find(GPR(4) & 0xFF); it != f.sy.end())
	{
		GPR(3) = it->second ? 1 : 0;
		return;
	}
	[[clang::musttail]] return orig_f_025B7D90(ctx);
}

void orig_f_025BA494(PPCInterpreter_t* __restrict ctx);
// dSv_info_c::isItem(info r3, number r4, room r5) -> r3: forced under WWHD_DEBUG_FLAGS (above)
void f_025BA494(PPCInterpreter_t* __restrict ctx)
{
	const ForcedFlags& f = Forced();
	if (!f.any)
		[[clang::musttail]] return orig_f_025BA494(ctx);
	if (const auto it = f.im.find(GPR(4)); it != f.im.end())
	{
		GPR(3) = it->second ? 1 : 0;
		return;
	}
	[[clang::musttail]] return orig_f_025BA494(ctx);
}

void orig_f_025BA6A4(PPCInterpreter_t* __restrict ctx);
// dSv_info_c::isActor(info r3, set ID r4, room r5) -> r3: forced under WWHD_DEBUG_FLAGS (above)
void f_025BA6A4(PPCInterpreter_t* __restrict ctx)
{
	const ForcedFlags& f = Forced();
	if (!f.any)
		[[clang::musttail]] return orig_f_025BA6A4(ctx);
	if (const auto it = f.ac.find(GPR(4)); it != f.ac.end())
	{
		GPR(3) = it->second ? 1 : 0;
		return;
	}
	if (f.acAll >= 0)
	{
		GPR(3) = (uint32)f.acAll;
		return;
	}
	[[clang::musttail]] return orig_f_025BA6A4(ctx);
}

// dSv_info_c::isSwitch(info r3, number r4, room r5) -> r3: forced under WWHD_DEBUG_FLAGS (above)
void f_025BA0C0(PPCInterpreter_t* __restrict ctx)
{
	const ForcedFlags& f = Forced();
	if (!f.any)
		[[clang::musttail]] return orig_f_025BA0C0(ctx);
	if (const auto it = f.sw.find(GPR(4)); it != f.sw.end())
	{
		GPR(3) = it->second ? 1 : 0;
		return;
	}
	[[clang::musttail]] return orig_f_025BA0C0(ctx);
}

// WWHD_DEBUG_EVENT=tick:NAME[;...] (a test aid): from those game frames, once no event runs (EventRunning: a
// stage's arrival is an event), the stage's event NAME (its event list's name: a JStudio cutscene's event has
// the staff PACKAGE and CAMERA) is ordered for Link as an actor orders one: its index by
// dEvent_manager_c::getEventIdx (f_02543F10, the manager at g_dComIfG_gameInfo + 0x52C4) and
// fopAcM_orderOtherEventId (f_025D7A58: an "other" event, the list's priority, dEvtFlag_NOPARTNER), so a
// finished save can replay the story's cutscenes
void f_02543F10(PPCInterpreter_t* __restrict ctx);
void f_025D7A58(PPCInterpreter_t* __restrict ctx);
namespace
{
	void DebugEvent(PPCInterpreter_t* ctx)
	{
		struct Order { int tick; std::string name; bool done; };
		static std::vector<Order> events = [] {
			std::vector<Order> v;
			if (const char* e = getenv("WWHD_DEBUG_EVENT"))
				for (const char* p = e; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : nullptr)
				{
					int tick = 0, n = 0;
					if (sscanf(p, "%d:%n", &tick, &n) != 1 || !n)
						continue;
					const char* end = strchr(p + n, ';');
					std::string name(p + n, end ? (size_t)(end - (p + n)) : strlen(p + n));
					if (!name.empty() && name.size() < 0x40)
						v.push_back({ tick, name, false });
				}
			return v;
		}();
		if (events.empty() || g_rtHalfTick || !s_link || rd16(s_link + 0x08) != 168 || EventRunning())
			return;
		const int now = (int)wwhd::rt::GameFrame(wwhd::os::SwapCount());
		for (Order& o : events)
		{
			if (o.done || now < o.tick)
				continue;
			o.done = true;
			Registers regs;
			regs.Save(ctx);
			const uint32 sp = (ctx->gpr[1] - 0x200) & ~0xFu;
			const uint32 str = sp + 0x100;
			for (size_t i = 0; i <= o.name.size(); i++)
				wr8(str + (uint32)i, i < o.name.size() ? (uint8)o.name[i] : 0);
			wr32(sp, ctx->gpr[1]);                     // a back chain
			ctx->gpr[1] = sp;
			ctx->gpr[3] = 0x1046F0B0u + 0x52C4u;
			ctx->gpr[4] = str;
			ctx->gpr[5] = 0xFF;
			f_02543F10(ctx);
			const sint16 index = (sint16)ctx->gpr[3];
			uint32 ordered = 0;
			if (index >= 0)
			{
				ctx->gpr[1] = sp;
				ctx->gpr[3] = s_link;
				ctx->gpr[4] = (uint32)(sint32)index;
				ctx->gpr[5] = 0xFF;                    // no map tool id
				ctx->gpr[6] = 0xFFFF;                  // no hind
				ctx->gpr[7] = 0;                       // the list's priority
				ctx->gpr[8] = 1;                       // dEvtFlag_NOPARTNER_e
				f_025D7A58(ctx);
				ordered = ctx->gpr[3];
			}
			regs.Restore(ctx);
			cemuLog_log(LogType::Force, "wwhd debug: event {} (index {}) ordered at frame {}: {}", o.name, index, now, ordered);
			break;                                     // one order a tick
		}
	}
}

namespace wwhd::debug
{
	void RequestSpawn(int process, uint32 param, uint32 anglex)
	{
		std::lock_guard lock(s_spawnLock);
		s_spawnAsked = Spawn{ 0, process, 0, param, 0, 0, 0, anglex, 0, 0 };
		s_spawnWaiting = true;
	}
}

// The test arena (the owner's idea N28): Gohma's room (M_DragB) on a finished save with no warp in the middle, a
// big open floor for movement, physics and enemy tests (spawn them there). WWHD_DEBUG_ARENA=1, or the debug menu's
// "Test arena" (a warp there, refights off): while it is on, in M_DragB the boss's warp out (the warp flower,
// WARPFLOWER 104, d_a_warpf.cpp) isn't created; any other stage, or the arena off, the game's own. Off unless set.
namespace
{
	std::atomic<int> s_arena{ -1 };                  // -1: as WWHD_DEBUG_ARENA says
}
namespace wwhd::debug
{
	bool Arena()
	{
		static const bool env = [] { const char* e = getenv("WWHD_DEBUG_ARENA"); return e && *e == '1'; }();
		const int v = s_arena.load();
		return v < 0 ? env : v != 0;
	}
	void SetArena(bool on)
	{
		s_arena.store(on ? 1 : 0);
	}
}

// dSv_memBit_c::isDungeonItem(mem, item): item 3 is the stage's "boss beaten" (isStageBossEnemy and 44
// call sites test it). A test aid: WWHD_DEBUG_BOSS=1 answers "no" for it, so a boss appears again in
// its room on a finished save (with WWHD_DEBUG_STAGE to get there); nothing is written to the save.
// The debug menu turns it on and off (wwhd::debug::SetBossRefight, src/os/debug_menu.h).
// Once the boss is beaten there (the game sets the bit: f_025B9098 below) the save's own answer, until
// the game takes its next stage change: the boss's warp out is made then and is created only with the
// bit set (the warp flower, daWarpf_c::CreateInit; B47: with "no" to the end, no warp after Gohma).
// Puppet Ganon reads event bit 3F10 instead (d_a_bgn.cpp: set by his death, read by his create): the
// same for it (f_025B8B94, f_025B8B68).
namespace
{
	std::atomic<int> s_bossRefight{ -1 };            // -1: as WWHD_DEBUG_BOSS says
	// a boss beaten in this stage, and the game's stage change asked for since (the new stage not yet in)
	std::atomic<bool> s_beatenHere{ false }, s_beatenLeaving{ false };
}
namespace wwhd::debug
{
	bool BossRefight()
	{
		static const bool env = [] { const char* e = getenv("WWHD_DEBUG_BOSS"); return e && *e == '1'; }();
		const int v = s_bossRefight.load();
		return v < 0 ? env : v != 0;
	}
	void SetBossRefight(bool on)
	{
		s_bossRefight.store(on ? 1 : 0);
	}
	void BossBeaten()
	{
		s_beatenLeaving = false;
		s_beatenHere = true;
		RushBossBeaten();
	}
	// dStage_nextStage_c's enable (play +0x5140 +0xC): set by a stage change's request, cleared by the new
	// play scene's creation (d_s_play.cpp's phase_1, before phase_4's dStage_Create makes its rooms and
	// their actors); also called each frame (DebugStage), so a request is seen even if no one asks
	bool BossBeatenHere()
	{
		if (!s_beatenHere)
			return false;
		if (rd8(0x1046F0B0u + 0x5140u + 0xC) != 0)
			s_beatenLeaving = true;
		else if (s_beatenLeaving)
			s_beatenHere = s_beatenLeaving = false;
		return s_beatenHere;
	}
}
void orig_f_025B9100(PPCInterpreter_t* __restrict ctx);
void f_025B9100(PPCInterpreter_t* __restrict ctx)
{
	if (wwhd::debug::BossRefight() && ctx->gpr[4] == 3 && !wwhd::debug::BossBeatenHere())
	{
		ctx->gpr[3] = 0;
		return;
	}
	orig_f_025B9100(ctx);
}

// dSv_player_collect_c::isCollect(collect, field, bit): field 0's bits are the Master Sword's powers (1 the
// Earth Temple's, 2... as d_menu_collect.cpp reads them). Medli (NPC_MD 367) is created only in her boss
// room once bit 2 is set, so a finished save never has her with Link. A test aid: WWHD_DEBUG_COMPANION=1
// answers "no" for field 0 to her code alone (the caller's address in her functions, 02280508-02295870),
// so she is created as the Earth Temple's companion (M_Dai); Link's sword and the menus still read the
// save. Nothing is written to the save.
void orig_f_025B7A2C(PPCInterpreter_t* __restrict ctx);
void f_025B7A2C(PPCInterpreter_t* __restrict ctx)
{
	static const bool companion = [] { const char* e = getenv("WWHD_DEBUG_COMPANION"); return e && *e == '1'; }();
	const uint32 lr = ctx->spr.LR;
	if (companion && ctx->gpr[4] == 0 && lr >= 0x02280508u && lr < 0x02295870u)
	{
		ctx->gpr[3] = 0;
		return;
	}
	orig_f_025B7A2C(ctx);
}

// dComIfGs_checkGetItem(item r3): Makar (NPC_CB1 334) is created only in Molgera's room once the Master Sword
// has its full power (0x3E). WWHD_DEBUG_COMPANION=1 answers "no" for that item to his code alone (the caller in
// his functions, 0221CD78-022273A8), so a finished save creates him as the Wind Temple's companion (kaze).
void orig_f_02520C0C(PPCInterpreter_t* __restrict ctx);
void f_02520C0C(PPCInterpreter_t* __restrict ctx)
{
	static const bool companion = [] { const char* e = getenv("WWHD_DEBUG_COMPANION"); return e && *e == '1'; }();
	if (const ForcedFlags& f = Forced(); f.any)    // WWHD_DEBUG_FLAGS's it: (above)
		if (const auto it = f.it.find(ctx->gpr[3] & 0xFF); it != f.it.end())
		{
			ctx->gpr[3] = it->second ? 1 : 0;
			return;
		}
	const uint32 lr = ctx->spr.LR;
	if (companion && ctx->gpr[3] == 0x3E && lr >= 0x0221CD78u && lr < 0x022273A8u)
	{
		ctx->gpr[3] = 0;
		return;
	}
	orig_f_02520C0C(ctx);
}

// dSv_memBit_c::onDungeonItem(mem, item): item 3 set is a boss beaten (onStageBossEnemy), which the debug
// menu's boss rush waits for (wwhd::debug::RushBossBeaten) and the refights answer from then (above)
void f_025B9098(PPCInterpreter_t* __restrict ctx)
{
	if (ctx->gpr[4] == 3)
	{
		cemuLog_log(LogType::Force, "wwhd debug: a boss beaten (its dungeon's bit set)");
		wwhd::debug::BossBeaten();
	}
	[[clang::musttail]] return orig_f_025B9098(ctx);
}

// dSv_event_c::onEventBit(event r3, number r4): 3F10 is Puppet Ganon beaten (d_a_bgn.cpp sets it just after
// asking for GanonK point 4 layer 9, the scene after his fight): for the boss rush and the refights as a
// dungeon's bit is (above)
void f_025B8B68(PPCInterpreter_t* __restrict ctx)
{
	if ((GPR(4) & 0xFFFF) == 0x3F10)
	{
		cemuLog_log(LogType::Force, "wwhd debug: a boss beaten (Puppet Ganon's event bit set)");
		wwhd::debug::BossBeaten();
	}
	[[clang::musttail]] return orig_f_025B8B68(ctx);
}

// fopAcM_createItemForBoss(pos, unused, room, angle, scale, kind) -> process ID: a beaten boss's heart
// container (the decomp's: fopAcM_createItem, f_025D8870, item 8 and type 3, as daItemAct_BOSS 0xC for
// kind 1, else 0x5; Gohdan and Molgera call it, the others through their disappearing body, d_a_disappear).
// In the boss rush none (the owner's wish, B47): no process and the ID -1 (fpcM_ERROR_PROCESS_ID_e), by
// which the bosses that hold it (Gohdan, Molgera) find nothing and go on with their scene.
void f_025D8A5C(PPCInterpreter_t* __restrict ctx)
{
	if (wwhd::debug::RushRunning())
	{
		cemuLog_log(LogType::Force, "wwhd debug: boss rush: no heart container");
		GPR(3) = 0xFFFFFFFFu;
		return;
	}
	[[clang::musttail]] return orig_f_025D8A5C(ctx);
}

// daFm_c::modeProc(proc, newMode) (Floormasters, FM 119; d_a_fm.cpp; mMode at +0x3C8): proc 0 sets mMode and calls the
// new mode's init, proc 1 calls its run (each a member function pointer, tail-called). At 30 a mode set in a tick
// runs from the next tick; at 60 its run came in the same tick's half step, so each "set, then run next call" in its
// chain took half a tick where 30 takes a tick: its grab came 1.5 ticks early (notice, rise, grab: f1100.5, 1101.5,
// 1121.5 against 30's 1101, 1103, 1123). As Link's action call (ActionHold): a mode set in its whole step holds the
// half step's run; a mode set in its half step (a change 30 makes in the next tick) holds both steps of the next
// tick. Then its modes change on 30's ticks or half a tick before. WWHD_60FPS_MODEHOLD=0 turns it off.
namespace
{
	bool ModeHold()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_MODEHOLD"); return !(e && atoi(e) == 0); }();
		return on;
	}
	// a process, the steps (2 a tick: whole 2t, half 2t + 1) its last mode was set in and held until
	struct ModeSet { uint32 at, until; };
	std::unordered_map<uint32, ModeSet> s_modeSet;
}
void f_021412BC(PPCInterpreter_t* __restrict ctx)
{
	if (!g_rtSixty || !ModeHold())
		[[clang::musttail]] return orig_f_021412BC(ctx);
	const uint32 actor = GPR(3), step = 2 * wwhd::rt::GameFrame(wwhd::os::SwapCount()) + (g_rtHalfTick ? 1 : 0);
	if (GPR(4) == 0)
		s_modeSet[actor] = { step, g_rtHalfTick ? step + 2 : step + 1 };
	else if (GPR(4) == 1)
		if (const auto it = s_modeSet.find(actor); it != s_modeSet.end() && step > it->second.at && step <= it->second.until)
			return;
	[[clang::musttail]] return orig_f_021412BC(ctx);
}

// fpcM_Create(layer, process name, create function, ?, append) -> process ID. In the boss rush, Kalle Demos's death
// makes no Makar (NPC_CB1, 334: d_a_bmd.cpp's death scene creates him, then his rescue event thanks Link; the
// owner's N21): no request, the ID -1, which the boss doesn't keep. The append (0x40 bytes, made by fopAcM_create
// before this call) isn't freed: once a rush.
void f_025E14A8(PPCInterpreter_t* __restrict ctx)
{
	if (GPR(4) == 334 && wwhd::debug::RushBoss() == 1)
	{
		cemuLog_log(LogType::Force, "wwhd debug: boss rush: no Makar after Kalle Demos");
		GPR(3) = 0xFFFFFFFFu;
		return;
	}
	// the test arena (N28, above): no warp flower (WARPFLOWER 104) in Gohma's room
	if ((GPR(4) & 0xFFFF) == 104 && wwhd::debug::Arena())
	{
		char stage[9] = {};
		for (int i = 0; i < 8; i++)
			stage[i] = (char)rd8(0x1046F0B0u + 0x5134u + i);   // the current stage's name
		if (strcmp(stage, "M_DragB") == 0)
		{
			GPR(3) = 0xFFFFFFFFu;
			return;
		}
	}
	[[clang::musttail]] return orig_f_025E14A8(ctx);
}

// mDoAud_setSceneName(name, room, layer) (session qa's find, symbols.csv): the sound's next scene, which picks its
// music. In the boss rush the log says which (the warp out's turn, below, comes before it)
void f_025E17CC(PPCInterpreter_t* __restrict ctx)
{
	if (wwhd::debug::RushRunning() && GPR(3) != 0)
	{
		char name[9] = {};
		for (uint32 i = 0; i < 8; i++)
			name[i] = (char)rd8(GPR(3) + i);
		static char last[9] = {};                     // called each frame of the wipe: logged when it changes
		if (strcmp(name, last) != 0)
			cemuLog_log(LogType::Force, "wwhd debug: boss rush: the sound's next scene {} room {}", name, (int)(sint32)GPR(4));
		memcpy(last, name, sizeof(last));
	}
	[[clang::musttail]] return orig_f_025E17CC(ctx);
}

// dComIfGp_setNextStage(name, point, room, layer, lastSpeed, lastMode, setPoint, wipe): the next stage. In the boss
// rush the warp out of a beaten boss goes to the next boss here, as it's asked for: the play scene's draw hands the
// next stage's name to the audio (mDoAud_setSceneName, d_s_play.cpp) the frame the change starts, so the redirect
// at the next whole tick (DebugStage) came after the music was picked: the warp's own destination's (the Great
// Sea's after Gohma, the Forest Haven's after Kalle Demos: B63, B64)
void f_0252012C(PPCInterpreter_t* __restrict ctx)
{
	if (!wwhd::debug::RushRunning())
		[[clang::musttail]] return orig_f_0252012C(ctx);
	orig_f_0252012C(ctx);
	const char* name;
	StageWarp w{ 0, 0, 0, -1, {} };
	if (wwhd::debug::RushNextStage(name, w.point, w.room, w.layer))
	{
		strncpy(w.name, name, 8);
		cemuLog_log(LogType::Force, "wwhd debug: boss rush: the warp out turned as it's asked for");
		RedirectStage(w);
	}
}

// ---- cutscenes at 60 (D21) ------------------------------------------------------------------------
// A cutscene (dDemo_manager_c: its update, f_025291C8, calls JStudio's stb::TControl::forward(1),
// f_0283D514, on whole ticks) moves its cast and camera by values JStudio evaluates once a tick: each
// TVariableValue a curve or a rate of the time, its age (whole frames, +0x4) times the control's
// seconds a frame, handed by the objects' adaptors to the cast's demo actors (dDemo_actor_c: place,
// angle, animation frame) and the camera's. While the cast steps at 60 in the event (Link's cutscene
// action, StepInEvents), the whole tick's update is followed by forward(0) (as the manager's start calls
// it: no frame passes, so the sequence's waits and commands stay on whole ticks) evaluating them at age
// - 1/2 (update_time_ f_02839DB4, update_functionValue_ f_02839DF4, below: (2 age - 1) x spf / 2), and
// the half tick begins with them at age again: the cast reads half way on its whole step and the tick's
// own values on its half step, as stepped processes are (Ganondorf's arrival in GTower: Link's frame
// 13.5 and 14 against 30's 14, his own stepped animations' halves alike). The camera shows a step late
// what it read, so its whole frames equal 30's ticks and its half frames lie half way. Made as a step's
// stores (s_converting), so the half tick's rollback keeps them.
void f_0283D514(PPCInterpreter_t* __restrict ctx);
namespace
{
	bool s_demoHalf = false;                         // the values being evaluated at age - 1/2
	bool s_demoBehind = false;                       // the whole tick's cast got them at age - 1/2

	// the cutscene's values evaluated again (forward(0)), at age - 1/2 if half; true if a cutscene runs
	bool DemoEvaluate(PPCInterpreter_t* ctx, bool half)
	{
		const uint32 control = rd32(0x101D5FE8u);    // dDemo_manager_c's m_control (its create, f_025286E0)
		if (!control || !rd32(0x101D6004u))          // no cutscene loaded (its current file)
			return false;
		Registers regs;
		regs.Save(ctx);
		const uint32 sp = (ctx->gpr[1] - 0x200) & ~0xFu;
		wr32(sp, ctx->gpr[1]);                     // a back chain
		ctx->gpr[1] = sp;
		ctx->gpr[3] = control;
		ctx->gpr[4] = 0;
		s_demoHalf = half;
		s_converting++;
		f_0283D514(ctx);
		s_converting--;
		s_demoHalf = false;
		regs.Restore(ctx);
		return true;
	}

	// a half tick's start: the values the whole tick's cast got at age - 1/2 are the tick's own again
	void DemoHalfTick(PPCInterpreter_t* ctx)
	{
		if (!s_demoBehind)
			return;
		s_demoBehind = false;
		DemoEvaluate(ctx, false);
	}

	// a TVariableValue's update at age - 1/2: (2 age - 1) x spf / 2 (at age 0, age 0's)
	void HalfAge(PPCInterpreter_t* ctx, void (*orig)(PPCInterpreter_t*))
	{
		const uint32 v = GPR(3);
		const uint32 age = rd32(v + 4);
		wr32(v + 4, age ? age * 2 - 1 : 0);
		FPR(1).fp0 *= 0.5;
		FPR(1).fp1 = FPR(1).fp0;
		orig(ctx);
		wr32(v + 4, age);
	}
}

// dDemo_manager_c::update (a cutscene's tick: JStudio's forward(1)); while the cast steps at 60 in its
// event, the values at age - 1/2 for the whole tick's step (above)
void f_025291C8(PPCInterpreter_t* __restrict ctx)
{
	orig_f_025291C8(ctx);
	if (DemoSixty() && wwhd::rt::SixtyFrom() != ~0u && wwhd::os::SwapCount() >= wwhd::rt::SixtyFrom() && !g_rtHalfTick &&
		StepInEvents())
		s_demoBehind = DemoEvaluate(ctx, true);
}

// JStudio::TVariableValue::update_time_ (its value: age x spf x its rate)
void f_02839DB4(PPCInterpreter_t* __restrict ctx)
{
	if (!s_demoHalf)
		[[clang::musttail]] return orig_f_02839DB4(ctx);
	HalfAge(ctx, orig_f_02839DB4);
}

// JStudio::TVariableValue::update_functionValue_ (its value: its function at age x spf)
void f_02839DF4(PPCInterpreter_t* __restrict ctx)
{
	if (!s_demoHalf)
		[[clang::musttail]] return orig_f_02839DF4(ctx);
	HalfAge(ctx, orig_f_02839DF4);
}

// a late store that a stepping process's whole step passed over (ppc_ops.h's late_wr32 and on; while
// g_rtLateNotes is set, by f_025DE58C below): noted for its half step (s_lateNoted)
void rt_late_note(uint32 ea, uint32 size, uint64 value)
{
	s_lateNotes.push_back({ ea, size, value });
}

// m_Do_main's frame body (see the top)
void f_025F172C(PPCInterpreter_t* __restrict ctx)
{
	DebugStage();
	DebugSpawn(ctx);
	DebugEvent(ctx);
	s_firstDraw = true;
	s_held.clear();
	if (!g_rtHalfTick)
		s_lateNoted.clear();                        // only a tick's own notes are made at its half step
	if (g_rtHalfTick)
	{
		DemoHalfTick(ctx);                          // a cutscene's values the tick's own again, before the cast steps
		static const bool skip = [] { const char* e = getenv("WWHD_60FPS_HALF"); return e && strcmp(e, "none") == 0; }();
		if (skip)
			return;
		if (!Probe() && !Flight())
			[[clang::musttail]] return orig_f_025F172C(ctx);   // the tick rules hold its logic to whole ticks
		const uint32 swap = wwhd::os::SwapCount();
		s_tracked.clear();
		orig_f_025F172C(ctx);
		if (Flight())
			FlightRecord(wwhd::rt::GameFrame(swap) * 2 + 1);
		if (Probe())
		{
			DumpTracked(wwhd::rt::GameFrame(swap) * 2 + 1);
			ParticleCensus(wwhd::rt::GameFrame(swap) * 2 + 1);
		}
		s_tracked.clear();
		return;
	}
	if (!Probe() && !Flight() && wwhd::rt::SixtyFrom() == ~0u)
		[[clang::musttail]] return orig_f_025F172C(ctx);
	const uint32 swap = wwhd::os::SwapCount();
	s_actors.clear();
	s_processes.clear();
	s_tracked.clear();
	orig_f_025F172C(ctx);
	if (Flight())
		FlightRecord(wwhd::rt::GameFrame(swap) * 2);
	if (Probe())
	{
		Dump(wwhd::rt::GameFrame(swap));
		DumpTracked(wwhd::rt::GameFrame(swap) * 2);
		ParticleCensus(wwhd::rt::GameFrame(swap) * 2);
	}
	s_tracked.clear();
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

// ---- the HD UI's layout animations at 60 (the work queue's "ui30") --------------------------------------
// The HD UI's layouts (the HUD, the message box, the menus) animate with NintendoWare's AnimTransform. Once a
// tick (the play scene's execute, whole ticks) each screen's update (f_02002C90, for a screen whose root pane's
// +0xC is 0) puts its animations' frames on their panes (its vtable's +0x4C: each AnimTransform's Animate,
// vtable +0x14 slot +0x2C, f_028714D0, hands its frame at +0xC to each bound pane: scale, colours,
// visibility...), works out the panes' matrices (+0x64: f_0287FBFC, the draw info at the update's +0xC), and
// then steps the frames for the next tick (+0x54: f_02872DFC, frame += rate +0x24, then wrapped, clamped or
// turned back by its mode +0x28, its events in +0x2C). The HD renderer draws those matrices every frame, so
// every layout animation moved at 30 Hz (the HUD's bow gems pulse in scale, P_ArrowFire/Ice/Light_00: the
// corner block every hz30 scan shows). On a half tick, before its frame, each one that played in the last
// whole tick (its rate not 0) is put back to the frame that tick drew, stepped half its rate on by its own
// controller, put on its panes by its own Animate, and given back its frame, rate and events; then each screen
// that tick updated works out its panes' matrices again (all of them). The half tick's frame draws them half a
// rate on, and the next whole tick puts and steps them as at 30. An animation or screen gone since (list links,
// vtable or root changed) is skipped. WWHD_60FPS_UIANIM=0: off.
namespace
{
	bool UiAnims()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_UIANIM"); return !(e && atoi(e) == 0); }();
		return on;
	}

	struct UiAnim { uint32 anim, vtable, frame; };   // its frame before the whole tick's step
	std::vector<UiAnim> s_uiAnims;               // the AnimTransforms that played in the last whole tick
	struct UiScreen { uint32 update, screen, vtable, root; };
	std::vector<UiScreen> s_uiScreens;           // the screens it updated (f_02002C90)

	void UiAnimsHalfStep(PPCInterpreter_t* ctx)
	{
		if (s_uiAnims.empty() || s_uiScreens.empty())
			return;
		Registers regs;
		regs.Save(ctx);
		const uint32 sp = (regs.gpr[1] - 0x200) & ~0xFu;
		for (const UiAnim& a : s_uiAnims)
		{
			const uint32 t = a.anim, next = rd32(t), prev = rd32(t + 4);
			if (rd32(t + 0x14) != a.vtable || !next || !prev || rd32(next + 4) != t || rd32(prev) != t)
				continue;
			uint8 saved[0x30];
			memcpy(saved, memory_base + t, sizeof(saved));
			wr32(t + 0xC, a.frame);                      // from the frame the whole tick drew (its step came after)
			wr32(t + 0x24, std::bit_cast<uint32>(std::bit_cast<float>(rd32(t + 0x24)) * 0.5f));
			wr32(sp, regs.gpr[1]);                       // a back chain
			ctx->gpr[1] = sp;
			ctx->gpr[3] = t;
			orig_f_02872DFC(ctx);                        // half its rate on
			memcpy(memory_base + t + 0x24, saved + 0x24, sizeof(saved) - 0x24);   // its rate, mode and events
			ctx->gpr[1] = sp;
			ctx->gpr[3] = t;
			ctx->spr.CTR = rd32(a.vtable + 0x2C);        // Animate
			RT_CALL_CTR();
			memcpy(memory_base + t, saved, sizeof(saved));   // its frame as the whole tick left it
		}
		// and the panes' matrices from their new values: each screen the whole tick updated (f_02002C90:
		// animate, its vtable's +0x4C; calculate, +0x64, f_0287FBFC with the draw info at the update's +0xC;
		// then +0x54), its calculate again
		for (const UiScreen& u : s_uiScreens)
		{
			const uint32 screen = rd32(u.update + 4);
			if (screen != u.screen || rd32(screen + 0x30) != u.vtable || rd32(screen + 0xC) != u.root)
				continue;
			wr32(sp, regs.gpr[1]);
			ctx->gpr[1] = sp;
			ctx->gpr[3] = screen;
			ctx->gpr[4] = u.update + 0xC;
			ctx->gpr[5] = 1;                             // every pane (with 0 a pane is redone only under a changed one)
			ctx->spr.CTR = rd32(u.vtable + 0x64);
			RT_CALL_CTR();
		}
		regs.Restore(ctx);
	}
}

void f_02872DFC(PPCInterpreter_t* __restrict ctx)
{
	if (g_rtHalfTick || !g_rtSixty || !UiAnims())
		[[clang::musttail]] return orig_f_02872DFC(ctx);
	const uint32 t = (uint32)GPR(3), frame = rd32(t + 0xC);
	orig_f_02872DFC(ctx);
	if (rd32(t + 0x24) & 0x7FFFFFFFu &&            // playing: its rate isn't 0 (and its first step this tick)
		std::none_of(s_uiAnims.begin(), s_uiAnims.end(), [t](const UiAnim& a) { return a.anim == t; }))
		s_uiAnims.push_back({ t, rd32(t + 0x14), frame });
}

void f_02002C90(PPCInterpreter_t* __restrict ctx)
{
	if (!g_rtHalfTick && g_rtSixty && UiAnims())
		if (const uint32 screen = rd32((uint32)GPR(3) + 4), root = screen ? rd32(screen + 0xC) : 0; root && !rd32(root + 0xC))
			s_uiScreens.push_back({ (uint32)GPR(3), screen, rd32(screen + 0x30), root });   // one it updates (as it checks)
	[[clang::musttail]] return orig_f_02002C90(ctx);
}

// ---- the camera's tick (session top, the work queue's "camera") ------------------------------------------------
// The camera (process 476: dCamera_c is inside it) approaches targets that move every frame: Link's heading, his
// place. Two half steps toward a moving target don't land where one tick's step lands (the target the first half
// step chases is half a tick old), so in a sustained turn the camera's control angle ran ~130 behind 30's (0.7
// degrees), and Link, whose heading target is the stick's angle + the camera's (setStickData, every step), read
// the half-stepped camera in his half step where 30's tick reads the tick's start: the camera-relative walk
// drifted (route sidle 53 units). So the camera's whole-tick half step is for show (its frame draws it): its bytes
// before and after that step are kept, and as the half tick's frame begins, every byte the step changed that
// nothing has changed since goes back. Every half step then reads the tick-start camera, and the camera's own
// half step runs as a 30 fps tick (step 1, the whole-tick rules on), which ends where 30's tick ends for 30's
// inputs (route tour: its half ticks equal 30's ticks to the bit while the camera settles after the load).
// On by default since B57 (the owner's N4: the camera drifting 5-17 degrees behind 30's in long turns; session qa
// on session top's measures, ww-4 d18f043 with Link's plain-walk turn once a tick): movecircle's first circle 0.13
// units from 30's (heading 0.02 degrees), moveangle 0.07/0.9 at 0/45 degrees. WWHD_60FPS_CAMTICK=0 turns it off.
// Predeploy with it: tour and land FAIL (271, 306 units; 20 and 99 without), both a bifurcation at Outset's corner
// (f1473-1477: Link a few units aside slides past where 30's stops), so predeploy walks them steered 2 degrees
// (routes tour2, land2: 51 and 45 with it, 228 and 359 without); sidle 9.7 -> 111 (WARN), ladder 11.
namespace
{
	bool CamTick()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_CAMTICK"); return !(e && atoi(e) == 0); }();
		return on;
	}
	struct CamSave { uint32 proc = 0, id = 0, size = 0; std::vector<uint8> before, after; bool restored = false; };
	CamSave s_cam;
	// the camera's whole step is about to run (proc, the camera process): its bytes before it
	void CamTickBefore(uint32 proc)
	{
		const uint32 profile = rd32(proc + 0x10);
		const uint32 size = profile ? rd32(profile + 0x10) : 0;
		s_cam = CamSave{};
		if (size == 0 || size > 0x4000)
			return;
		s_cam.proc = proc;
		s_cam.id = rd32(proc + 0x04);
		s_cam.size = size;
		s_cam.before.assign(memory_base + proc, memory_base + proc + size);
	}
	// ... and after it
	void CamTickAfter()
	{
		if (s_cam.proc)
			s_cam.after.assign(memory_base + s_cam.proc, memory_base + s_cam.proc + s_cam.size);
	}
	// the half tick's frame begins: what the camera's whole step changed goes back to the tick's start
	void CamTickRestore()
	{
		if (!s_cam.proc || s_cam.after.size() != s_cam.size || rd32(s_cam.proc + 0x04) != s_cam.id || rd16(s_cam.proc + 0x08) != 476)
		{
			s_cam = CamSave{};
			return;
		}
		uint8* const p = memory_base + s_cam.proc;
		for (uint32 i = 0; i < s_cam.size; i++)
			if (p[i] == s_cam.after[i] && s_cam.after[i] != s_cam.before[i])
				p[i] = s_cam.before[i];
		s_cam.restored = true;
	}
}

// ---- collision pushes at 60 (session bottom's "pushes") ---------------------------------------------
// The collision resolution (cCcS::Move, f_0200E558: whole ticks only) clears each Co collider's push
// (its Stts's m_cc_move, +0: ClrCoHitInf) and sets it again from this tick's overlaps; the actor moves by
// it in its next execute (fopAcM_posMove(this, GetCCMoveP()), Link's posMove), and most actors never clear
// it: it stays until the next resolution. A converted process steps twice before that (its half step,
// then the next whole one), so it was pushed twice a tick (a pig Link walks into slid sideways half as far
// again and fell off the dock elsewhere). Now each whole tick's Co colliders are noted after the
// resolution; on the half tick a stepping process's push is half of it, and after the half tick the other
// half is left for its whole step (all of it, if it didn't step), so the two steps move it one tick's
// push, half a step each. WWHD_60FPS_PUSHSPLIT=0 (a probe) turns it off.
// With the resolution after the half step (session top's "resolve", below) the next tick's whole step comes
// first: a stepping process's push is halved when the resolution has made it, the other half is put back for
// its half step when that frame starts, and if its half step didn't run that half moves it at the frame's
// end, before the next resolution (pos += it), so it is still one tick's push.
namespace
{
	bool ResolveLate();                             // (below)
	bool PushSplit()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_PUSHSPLIT"); return !(e && atoi(e) == 0); }();
		return on;
	}
	uint32 s_ccPass = 0;                            // collision resolutions so far
	struct CoPush { uint32 stts, actor, id; uint16 name; uint32 move[3]; bool halved, second; };
	std::vector<CoPush> s_coPushes;                // the last whole tick's Co colliders' Stts
	uint32 s_coPushesSwap = ~0u;                    // the swap whose resolution noted them

	bool LiveActor(const CoPush& c)
	{
		return c.actor >= 0x10000000u && c.actor < 0x50000000u && rd32(c.stts + 0xC) == c.actor &&
			rd32(c.actor + 4) == c.id && rd16(c.actor + 8) == c.name;
	}

	// the resolution after the half step: a stepping process's push, half of it for the next whole step
	void PushesResolved()
	{
		for (CoPush& c : s_coPushes)
		{
			c.halved = c.second = false;
			if (!LiveActor(c) || !Converted(c.name))
				continue;
			const auto it = s_stepping.find(c.actor);   // (the half tick that just ended)
			if (it == s_stepping.end() || !it->second)
				continue;
			for (int i = 0; i < 3; i++)
			{
				c.move[i] = rd32(c.stts + 4 * i);
				wr32(c.stts + 4 * i, std::bit_cast<uint32>(std::bit_cast<float>(c.move[i]) * 0.5f));
			}
			c.halved = true;
		}
	}

	// a half tick's start: a stepping process's push, half of it
	void PushesHalfTickBegin()
	{
		if (ResolveLate())
		{
			for (CoPush& c : s_coPushes)               // the other half, for the half step
				if (c.halved && LiveActor(c))
				{
					for (int i = 0; i < 3; i++)
						wr32(c.stts + 4 * i, std::bit_cast<uint32>(std::bit_cast<float>(c.move[i]) * 0.5f));
					c.second = true;
				}
			return;
		}
		const bool fresh = s_coPushesSwap + 1 == wwhd::os::SwapCount();   // this tick's resolution's (not a pause's)
		for (CoPush& c : s_coPushes)
		{
			c.halved = false;
			if (!fresh)
				continue;
			if (!LiveActor(c) || !Converted(c.name))
				continue;
			const auto it = s_stepping.find(c.actor);
			if (it == s_stepping.end() || !it->second)
				continue;
			for (int i = 0; i < 3; i++)
			{
				c.move[i] = rd32(c.stts + 4 * i);
				wr32(c.stts + 4 * i, std::bit_cast<uint32>(std::bit_cast<float>(c.move[i]) * 0.5f));
			}
			c.halved = true;
		}
	}

	// a half tick's end: the other half for its whole step (all of it if the half step didn't run)
	void PushesHalfTickEnd()
	{
		if (ResolveLate())
		{
			for (CoPush& c : s_coPushes)               // a half step that didn't run: its half moves it now
			{
				if (c.second && LiveActor(c))
				{
					const auto it = s_stepping.find(c.actor);
					if (it == s_stepping.end() || !it->second)
						for (int i = 0; i < 3; i++)
							wr32(c.actor + 0x314 + 4 * i, std::bit_cast<uint32>(std::bit_cast<float>(rd32(c.actor + 0x314 + 4 * i)) +
								std::bit_cast<float>(c.move[i]) * 0.5f));
				}
				c.halved = c.second = false;
			}
			return;
		}
		for (const CoPush& c : s_coPushes)
		{
			if (!c.halved || !LiveActor(c))
				continue;
			const auto it = s_stepping.find(c.actor);
			const bool stepped = it != s_stepping.end() && it->second;
			for (int i = 0; i < 3; i++)
				wr32(c.stts + 4 * i, std::bit_cast<uint32>(std::bit_cast<float>(c.move[i]) * (stepped ? 0.5f : 1.0f)));
		}
	}
}

// ---- the collision resolution after the half step (session top's "resolve") ---------------------------
// The play scene's draw (dScnPly_Draw, f_025AF8A0) first resolves the tick's collisions (dCcS::Move, below:
// cCcS::Move's CalcArea, ChkAtTg, ChkCo, MoveAfterCheck, its lists emptied; on whole ticks only, 025AF938).
// At 60 that came after the whole step, where a converted process stands half a tick short of 30's tick
// end (since animstart Link's pose too: his sword's capsule held a pose 30 never has, and Kalle Demos' core
// took a cut 30 misses). Now the whole tick's resolution waits for its half tick and runs when that frame
// ends (its draws undone, the converted processes' globals handed back), before the next whole tick, as
// 30's tick k resolves before tick k+1: unconverted processes registered their colliders in the whole step,
// converted ones in both (the lists are deduped first: a collider object holds its half step's values, the
// tick end's), and both steps of the next tick read its hits and pushes, as 30's next tick does. One still
// waiting at a whole tick (a half tick's frame that didn't come) runs then. WWHD_60FPS_RESOLVE=0: off.
namespace
{
	bool ResolveLate()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_RESOLVE"); return !(e && atoi(e) == 0); }();
		return on;
	}
	uint32 s_resolveCcs = 0;                          // the manager whose tick's resolution waits for its half tick
	uint64 s_resolvesLate = 0;
	void ResolveStats()
	{
		cemuLog_log(LogType::Force, "wwhd sixty: {} collision resolutions after the half step", s_resolvesLate);
	}
	// WWHD_DEBUG_RESOLVE=path (a test aid): each resolution waited for and made (swap, half tick, where, the
	// lists' counts before and after the dedupe)
	FILE* ResolveLog()
	{
		static FILE* f = [] { const char* e = getenv("WWHD_DEBUG_RESOLVE"); return e ? fopen(e, "w") : nullptr; }();
		return f;
	}
	void ResolvePending(PPCInterpreter_t* ctx, const char* where)
	{
		const uint32 ccs = s_resolveCcs;
		s_resolveCcs = 0;
		if (FILE* f = ResolveLog())
		{
			fprintf(f, "%u %d %s %08x at %u tg %u co %u all %u\n", wwhd::os::SwapCount(), g_rtHalfTick ? 1 : 0, where, ccs,
				rd32(ccs + 0x2800), rd32(ccs + 0x2804), rd32(ccs + 0x2808), rd32(ccs + 0x280C));
			fflush(f);
		}
		Registers regs;
		regs.Save(ctx);
		const uint32 sp = (ctx->gpr[1] - 0x100) & ~0xFu;
		wr32(sp, ctx->gpr[1]);                         // a back chain
		ctx->gpr[1] = sp;
		ctx->gpr[3] = ccs;
		const bool half = g_rtHalfTick;
		g_rtHalfTick = false;                          // the tick's own resolution
		orig_f_02518798(ctx);
		ctx->gpr[1] = sp;
		ctx->gpr[3] = ccs;
		orig_f_0251879C(ctx);                          // then the lists' clear, as the scene's draw makes it
		g_rtHalfTick = half;
		regs.Restore(ctx);
		s_resolvesLate++;
	}
}

// dCcS::Draw(manager r3): in WWHD only cCcS::DrawClear (f_0200E5BC: the lists emptied), from the scene's draw
// after the resolution, on both ticks' frames: while a resolution waits for its half tick (above) it waits
// too, and comes after it
void f_0251879C(PPCInterpreter_t* __restrict ctx)
{
	if (s_resolveCcs)
		return;
	[[clang::musttail]] return orig_f_0251879C(ctx);
}

// dCcS::Move(manager r3): cCcS::Move, the tick's collision resolution (above: at 60 after its half tick)
void f_02518798(PPCInterpreter_t* __restrict ctx)
{
	if (!g_rtSixty || g_rtHalfTick || !ResolveLate())
	{
		if (FILE* f = ResolveLog())                    // (the test aid at 30: the lists as they are resolved)
		{
			const uint32 ccs = GPR(3), n = std::min(rd32(ccs + 0x280C), 0x500u);
			std::vector<uint32> seen;
			uint32 dups = 0;
			for (uint32 i = 0; i < n; i++)
			{
				const uint32 obj = rd32(ccs + 0x1400 + 4 * i);
				dups += obj && std::find(seen.begin(), seen.end(), obj) != seen.end();
				seen.push_back(obj);
			}
			fprintf(f, "%u %d now %08x at %u tg %u co %u all %u (%u twice)\n", wwhd::os::SwapCount(), g_rtHalfTick ? 1 : 0, ccs,
				rd32(ccs + 0x2800), rd32(ccs + 0x2804), rd32(ccs + 0x2808), n, dups);
			fflush(f);
		}
		[[clang::musttail]] return orig_f_02518798(ctx);
	}
	static bool once = [] { atexit(ResolveStats); at_quick_exit(ResolveStats); return true; }();
	(void)once;
	if (s_resolveCcs)
		ResolvePending(ctx, "again");                  // the last tick's, still waiting
	s_resolveCcs = GPR(3);
	if (FILE* f = ResolveLog())
	{
		fprintf(f, "%u %d wait %08x at %u tg %u co %u all %u\n", wwhd::os::SwapCount(), g_rtHalfTick ? 1 : 0, GPR(3),
			rd32(GPR(3) + 0x2800), rd32(GPR(3) + 0x2804), rd32(GPR(3) + 0x2808), rd32(GPR(3) + 0x280C));
		fflush(f);
	}
}

// cCcS::Move(manager r3): the tick's collision resolution (CalcArea, ChkAtTg, ChkCo, MoveAfterCheck),
// then the lists emptied (their counts, +0x2800 At, +0x2804 Tg, +0x2808 Co, +0x280C all; the Co list at
// +0x1000, a collider's Stts at +0x44). At 60 the Co colliders are noted for the half tick (above).
void f_0200E558(PPCInterpreter_t* __restrict ctx)
{
	s_ccPass++;                                      // for the hits probe (below)
	// WWHD_DEBUG_PUSHLOG=path (a test aid, at 30 too): after each resolution, the Co colliders pushed (swap,
	// process name, Stts, the push)
	static FILE* dbg = [] { const char* e = getenv("WWHD_DEBUG_PUSHLOG"); return e ? fopen(e, "w") : nullptr; }();
	if (!dbg && (!g_rtSixty || g_rtHalfTick || !PushSplit() || !AnyConverted()))
		[[clang::musttail]] return orig_f_0200E558(ctx);
	const uint32 ccs = GPR(3);
	s_coPushes.clear();                              // (noted for the log at 30 too; the half tick needs 60)
	s_coPushesSwap = wwhd::os::SwapCount();
	const uint32 count = std::min(rd32(ccs + 0x2808), 0x100u);
	for (uint32 i = 0; i < count; i++)
	{
		const uint32 obj = rd32(ccs + 0x1000 + 4 * i);
		const uint32 stts = obj ? rd32(obj + 0x44) : 0;
		if (!stts)
			continue;
		const uint32 actor = rd32(stts + 0xC);
		if (actor < 0x10000000u || actor >= 0x50000000u)
			continue;
		bool seen = false;
		for (const CoPush& c : s_coPushes)
			seen = seen || c.stts == stts;
		if (!seen)
			s_coPushes.push_back({ stts, actor, rd32(actor + 4), rd16(actor + 8), {}, false, false });
	}
	orig_f_0200E558(ctx);
	if (g_rtSixty && PushSplit() && ResolveLate() && AnyConverted())
		PushesResolved();                           // (it runs after the half step now: above)
	if (dbg)
	{
		bool any = false;
		for (const CoPush& c : s_coPushes)
			if (rd32(c.stts) || rd32(c.stts + 4) || rd32(c.stts + 8))
			{
				if (!any)
					fprintf(dbg, "%u:", wwhd::os::SwapCount());
				any = true;
				fprintf(dbg, " [%u %08x %g,%g,%g]", c.name, c.stts, std::bit_cast<float>(rd32(c.stts)),
					std::bit_cast<float>(rd32(c.stts + 4)), std::bit_cast<float>(rd32(c.stts + 8)));
			}
		if (any)
		{
			fprintf(dbg, "\n");
			fflush(dbg);
		}
	}
}

// ---- hits read on both steps (session bottom's "hits", a probe) ------------------------------------
// A resolution's hits (a collider's At and Tg hit flags) stay until the next resolution, as its pushes do, so
// a converted process's half step and its next whole step both read them. WWHD_DEBUG_HITS=path logs each
// collider whose hit from one resolution reads true on both steps (game frame, half tick, at|tg, the
// collider's actor's process name; the Stts at +0x44, its actor at +0xC): a hit acted on twice unless a
// guard (a hit timer counted on whole ticks, the hit cleared) stops it.
namespace
{
	FILE* HitsLog()
	{
		static FILE* f = [] { const char* e = getenv("WWHD_DEBUG_HITS"); return e ? fopen(e, "w") : nullptr; }();
		return f;
	}
	struct HitSeen { uint32 pass; bool half; };
	std::unordered_map<uint32, HitSeen> s_hitSeen;  // collider (x2, + Tg) -> the last step its hit read true
	void HitRead(uint32 obj, uint32 tg, bool hit)
	{
		static const bool all = getenv("WWHD_DEBUG_HITS_ALL") != nullptr;   // every hit read, "1" if on both steps
		if (!hit || (!g_rtSixty && !all))                                    // (all: at 30 too, to compare)
			return;
		const uint32 key = obj * 2 + tg;
		const auto it = s_hitSeen.find(key);
		const bool twice = it != s_hitSeen.end() && it->second.pass == s_ccPass && it->second.half != g_rtHalfTick;
		if (twice || all)
		{
			const uint32 stts = rd32(obj + 0x44), actor = stts ? rd32(stts + 0xC) : 0;
			const int name = actor >= 0x10000000u && actor < 0x50000000u ? (int)rd16(actor + 8) : -1;
			fprintf(HitsLog(), "%u %d %s %d %08x %d\n", wwhd::rt::GameFrame(wwhd::os::SwapCount()), g_rtHalfTick ? 1 : 0,
				tg ? "tg" : "at", name, obj, twice ? 1 : 0);
			fflush(HitsLog());
		}
		s_hitSeen[key] = { s_ccPass, g_rtHalfTick };
	}
}

// dCcD_GObjInf::ChkAtHit(collider r3) -> its At hit this resolution (and its actor, or none needed)
void f_025160DC(PPCInterpreter_t* __restrict ctx)
{
	if (!HitsLog())
		[[clang::musttail]] return orig_f_025160DC(ctx);
	const uint32 obj = GPR(3);
	orig_f_025160DC(ctx);
	HitRead(obj, 0, GPR(3) != 0);
}

// dCcD_GObjInf::ChkTgHit(collider r3) -> its Tg hit this resolution (and its actor, or none needed)
void f_025162A4(PPCInterpreter_t* __restrict ctx)
{
	if (!HitsLog())
		[[clang::musttail]] return orig_f_025162A4(ctx);
	const uint32 obj = GPR(3);
	orig_f_025162A4(ctx);
	HitRead(obj, 1, GPR(3) != 0);
}

// ---- the random stream's draws (session bottom's "testaids") --------------------------------------------
// WWHD_DEBUG_RNDLOG=path (a probe): every cM_rnd draw (f_02019788; cM_rndF and cM_rndFX call it), at 30 too:
// game frame, half tick, the process's name (the one drawing in a draw pass, else the last to execute), the
// caller, the state before it (Wichmann-Hill's three seeds at 0x101FF9D4); two runs' logs side by side show
// where 60's stream parts from 30's.
// A half tick's draws are put back after its frame (f_0274C264, below), so a converted process's draw on its
// half step takes the number the next whole tick will draw again, and the stream moves only on whole ticks.
// What still parts it from 30's: a draw that moved to the half step (a state changed there and drew; at 30 it
// draws in the next tick, at 60 not again in the whole step). WWHD_60FPS_RNDSYNC=0 turns off what follows: such a
// draw is made up for, the stream moved one on right after that process's whole step, where 30 draws it, once
// for each draw of its half step's execute (by caller) that its whole step's execute didn't make again (one it
// did is a draw each step), so the processes after it draw 30's numbers (session top found a seagull's (194)
// trigger firing in its half step: made up at the frame's end, the processes after it drew a number early,
// a Bokoblin wandered off). Only a converted process's own execute counts: its draw pass's draws (with
// cM_rndF's, f_02019788's caller is the same for most draws) were taken for repeats and the make-up lost.
extern uint32 g_rtActor;
namespace
{
	bool RndSync()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_RNDSYNC"); return !(e && atoi(e) == 0); }();
		return on;
	}
	std::unordered_map<uint32, std::vector<uint32>> s_rndHalf;   // a stepping process's half-step draws (callers)
	uint32 s_rndExec = 0;                           // the converted process whose execute runs (f_025DE58C), or 0
	uint64 s_rndMadeUp = 0;
	FILE* RndLog()
	{
		static FILE* f = [] { const char* e = getenv("WWHD_DEBUG_RNDLOG"); return e ? fopen(e, "w") : nullptr; }();
		return f;
	}
	void RndAdvance(uint32 n)
	{
		uint32 r0 = rd32(0x101FF9D4u), r1 = rd32(0x101FF9D8u), r2 = rd32(0x101FF9DCu);
		for (uint32 i = 0; i < n; i++)
		{
			r0 = (uint32)(((int)r0 * 171) % 30269);
			r1 = (uint32)(((int)r1 * 172) % 30307);
			r2 = (uint32)(((int)r2 * 170) % 30323);
		}
		wr32(0x101FF9D4u, r0);
		wr32(0x101FF9D8u, r1);
		wr32(0x101FF9DCu, r2);
		s_rndMadeUp += n;
		if (FILE* f = RndLog(); f && n)
			fprintf(f, "%u %d A %u\n", wwhd::rt::GameFrame(wwhd::os::SwapCount()), g_rtHalfTick ? 1 : 0, n);
	}
	// a process's whole step is over: its half step's draws that didn't come again, made up for
	void RndFlush(uint32 proc)
	{
		const auto it = s_rndHalf.find(proc);
		if (it == s_rndHalf.end())
			return;
		if (FILE* f = RndLog(); f && !it->second.empty())
			fprintf(f, "%u %d F %d %u\n", wwhd::rt::GameFrame(wwhd::os::SwapCount()), g_rtHalfTick ? 1 : 0,
				(int)rd16(proc + 0x08), (uint32)it->second.size());
		RndAdvance((uint32)it->second.size());
		s_rndHalf.erase(it);
	}
	void RndFlushAll()
	{
		for (const auto& [proc, callers] : s_rndHalf)
		{
			if (FILE* f = RndLog(); f && !callers.empty())
				fprintf(f, "%u %d F* %d %u\n", wwhd::rt::GameFrame(wwhd::os::SwapCount()), g_rtHalfTick ? 1 : 0,
					(int)rd16(proc + 0x08), (uint32)callers.size());
			RndAdvance((uint32)callers.size());
		}
		s_rndHalf.clear();
	}
}
void f_02019788(PPCInterpreter_t* __restrict ctx)
{
	// WWHD_DEBUG_RNDFIX=v (a probe, B62): every draw returns v, at 30 and 60 alike, so a fight's choices are
	// the same at both rates and the two can be compared tick by tick (the stream still moves as before)
	static const double fix = [] { const char* e = getenv("WWHD_DEBUG_RNDFIX"); return e ? atof(e) : -1.0; }();
	FILE* const f = RndLog();
	if (!f && !(RndSync() && g_rtSixty) && fix < 0.0)
		[[clang::musttail]] return orig_f_02019788(ctx);
	const uint32 actor = s_drawing ? s_drawing : g_rtActor, lr = (uint32)ctx->spr.LR;
	const bool live = actor >= 0x10000000u && actor < 0x50000000u;
	if (f)
		fprintf(f, "%u %d %d %08x %08x %08x %08x\n", wwhd::rt::GameFrame(wwhd::os::SwapCount()), g_rtHalfTick ? 1 : 0,
			live ? (int)rd16(actor + 8) : -1, lr, rd32(0x101FF9D4u), rd32(0x101FF9D8u), rd32(0x101FF9DCu));
	// a converted process's own execute's draw (a half tick runs only stepping ones)
	if (const uint32 exec = s_drawing == 0 ? s_rndExec : 0; exec && RndSync() && g_rtSixty)
	{
		if (g_rtHalfTick)
			s_rndHalf[exec].push_back(lr);
		else if (const auto it = s_rndHalf.find(exec); it != s_rndHalf.end())
		{
			// drawn in its whole step too: a draw a step, nothing to make up
			auto& callers = it->second;
			if (const auto c = std::find(callers.begin(), callers.end(), lr); c != callers.end())
				callers.erase(c);
		}
	}
	orig_f_02019788(ctx);
	if (fix >= 0.0)
		FPR(1).fp0 = FPR(1).fp1 = fix;
}

// WWHD_DEBUG_RNDSEED=tick (a probe, session bottom's B62): from that game tick on, the random stream is set as each
// whole tick's frame starts to a state made from the tick, at 30 and 60 alike, so two runs' fights draw the same
// numbers from there wherever their streams parted before (the save route's part at f909); a draw's place within
// its tick (the processes that drew before it) can still differ.
namespace
{
	void RndSeedProbe(bool half)
	{
		static const uint32 at = [] { const char* e = getenv("WWHD_DEBUG_RNDSEED"); return e ? (uint32)atoi(e) : 0u; }();
		if (at == 0 || half)
			return;
		const uint32 tick = wwhd::rt::GameFrame(wwhd::os::SwapCount());
		if (tick < at)
			return;
		const uint32 h = tick * 2654435761u;
		wr32(0x101FF9D4u, 1 + (h >> 4) % 30268);
		wr32(0x101FF9D8u, 1 + (h >> 9) % 30306);
		wr32(0x101FF9DCu, 1 + (h >> 14) % 30322);
	}
}

// fw_procFrame, sead's procFrame_: one whole frame (the tick, the draw, the present, the vsync wait).
// At 60 fps it decides whether the frame is a whole or a half tick (see the top); with the store
// census on, a half tick's frame is watched from end to end.
void f_0274C264(PPCInterpreter_t* __restrict ctx)
{
	const uint32 from = wwhd::rt::SixtyFrom();      // ~0 when 60 fps is off
	if (from == ~0u)
	{
		RndSeedProbe(false);
		[[clang::musttail]] return orig_f_0274C264(ctx);
	}
	const uint32 swap = wwhd::os::SwapCount();
	if (swap == from && from != 0)
	{
		wwhd_SetSwapInterval(1);                       // from here on the game presents every vsync
		cemuLog_log(LogType::Force, "wwhd sixty: 60 fps from swap {}", swap);
	}
	if (RndSync() && g_rtSixty && !g_rtHalfTick)
		RndFlushAll();                              // the whole tick is over: a process that didn't step (the random stream)
	g_rtHalfTick = swap >= from && (swap - from) % 2 != 0;
	g_rtSixty = swap >= from;
	RndSeedProbe(g_rtHalfTick);
	if (!g_rtHalfTick)
	{
		s_uiAnims.clear();                           // the HD UI's animations that play this tick (above)
		s_uiScreens.clear();
		s_cam = CamSave{};                           // the camera's whole step (above) keeps its bytes anew
	}
	else if (CamTick())
		CamTickRestore();                            // the half tick reads the tick-start camera (above)
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
	{
		if (s_resolveCcs)
			ResolvePending(ctx, "whole");              // a whole tick's resolution whose half tick didn't come
		[[clang::musttail]] return orig_f_0274C264(ctx);
	}
	// The game's random stream (cM_rnd, f_02019788: Wichmann-Hill state at 101FF9D4) belongs to its
	// ticks: actors' draws take numbers from it too (the lighting's flicker, f_025615B8, through
	// settingTevStruct), so a half tick's frame would move it on. Drawing it gets the numbers the next
	// whole tick's draws will get, and the stream is put back.
	uint32 rng[3] = { rd32(0x101FF9D4u), rd32(0x101FF9D8u), rd32(0x101FF9DCu) };
	const uint32 input = AnyConverted() ? rd32(kInputObject) : 0;
	const uint32 pressed = input ? rd32(input + kPressed) : 0;
	if (input)
		wr32(input + kPressed, 0);
	if (UiAnims())
		UiAnimsHalfStep(ctx);                        // the HD UI's animations half a step on, for this frame
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
		{
			JournalPages();
			if (!Census())
				g_rtStorePages = s_page.data();         // HalfTickStore does nothing with stores into other pages
		}
	}
	if (PushSplit())
		PushesHalfTickBegin();                       // a stepping process's push, half of it this half tick
	orig_f_0274C264(ctx);
	if (PushSplit())
		PushesHalfTickEnd();                         // and the other half for its whole step
	if (watch)
	{
		g_rtJournalOn = wwhd::rt::QuietWatching();   // a fast path's watch may still need it
		g_rtStorePages = nullptr;
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
	if (s_resolveCcs)
		ResolvePending(ctx, "half");                   // the tick's collisions, its half step done (above)
}

// Link's reset flags (daPy_py_c::mResetFlg0, WWHD's Link +0x3C0: the GameCube's 0x2A4 + 0x11C; procTactWait
// sets its 0x01000000, "a note was judged", at 0243A7F0): what happened in his last step, cleared in the
// middle of his execute and read by others until his next (the Wind Waker's note display, a tree he
// rolls into, what a hammer blow shakes: checkTactInput, checkFrontRollCrash, checkHammerQuake...). With
// Link stepping at 60, a process that runs on whole ticks saw only the step just before it: the flags of
// his whole step were gone for those that execute before him (the note display never lit: the owner's
// Wind Waker), the flags of his half step for those after him. Now a whole-tick process sees both steps
// since it last looked (the last whole step's and the last half step's flags together), and Link and the
// other processes stepping at 60 see his last step's alone, as the game has it (his own tests before the
// clear must not see a flag twice: "not attacking" would end a cut begun in between).
// WWHD_60FPS_RESETFLAGS=0 (a probe): as before.
namespace
{
	constexpr uint32 kLinkResetFlags = 0x3C0u;
	uint32 s_rfLink = 0;                            // the Link they belong to
	uint32 s_rfWhole = 0, s_rfHalf = 0;             // what his last whole step and his last half step left
	uint32 s_rfHalfTerm = 0;                        // the half step's part for this whole tick's processes
	uint32 s_rfLast = 0;                            // what his last step left
	bool s_rfHalfRan = false;                       // he took a half step since the last whole tick began
	uint32 s_rfTickSwap = ~0u, s_rfLinkSwap = 0;    // the whole tick prepared; his last execute

	bool ResetFlagRules()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_RESETFLAGS"); return !(e && atoi(e) == 0); }();
		return on;
	}
	// the Link the flags belong to still exists (a deleted process's type word is 0) and ran lately
	bool ResetFlagsLink()
	{
		return s_rfLink && rd32(s_rfLink) != 0 && rd16(s_rfLink + 0x08) == 168 && wwhd::os::SwapCount() - s_rfLinkSwap <= 2;
	}
	void ResetFlagsExpose()
	{
		if (ResetFlagsLink())
			wr32(s_rfLink + kLinkResetFlags, s_rfWhole | (s_rfHalfRan ? s_rfHalf : s_rfHalfTerm));
	}
	// before a process's execute: stepping says it runs every frame (Link, a converted process)
	void ResetFlagsBefore(uint32 proc, bool link, bool stepping)
	{
		const uint32 swap = wwhd::os::SwapCount();
		if (!g_rtHalfTick && swap != s_rfTickSwap)
		{
			s_rfTickSwap = swap;                          // a whole tick begins
			s_rfHalfTerm = s_rfHalfRan ? s_rfHalf : 0;
			s_rfHalfRan = false;
			ResetFlagsExpose();
		}
		if (link)
		{
			if (proc != s_rfLink)
			{
				s_rfLink = proc;                             // a new Link: nothing carried over
				s_rfWhole = s_rfHalf = s_rfHalfTerm = 0;
				s_rfHalfRan = false;
				s_rfLast = rd32(proc + kLinkResetFlags);
			}
			s_rfLinkSwap = swap;
		}
		if (stepping && ResetFlagsLink())
			wr32(s_rfLink + kLinkResetFlags, s_rfLast);
	}
	void ResetFlagsAfter(uint32 proc, bool link, bool stepping)
	{
		if (link && proc == s_rfLink)
		{
			s_rfLast = rd32(proc + kLinkResetFlags);
			if (g_rtHalfTick)
			{
				s_rfHalf = s_rfLast;
				s_rfHalfRan = true;
			}
			else
				s_rfWhole = s_rfLast;
		}
		if (stepping)
			ResetFlagsExpose();
	}
}

// Link's one-shot flags (daPy_py_c::mNoResetFlg1, WWHD's +0x3BC: FORCE_VOMIT_JUMP 0x10, FORCE_VOMIT_JUMP_SHORT
// 0x10000, 0x4, 0x10000000): set by other processes for his next execute, which reads them and clears them at its
// end (d_a_player_main.cpp's execute, offNoResetFlg1 of these four). One set in a whole step after his reached his
// half step, half a tick before 30's next tick: a baba bud's launch (JBO 213 sets FORCE_VOMIT_JUMP; procVomitWait,
// checkNextMode) came half a tick early, 30 units high at the pairing, and the Deku Leaf's glide after it landed
// 2 ticks late (session top, dungeons2: route fwbud). His half step leaves the ones set since his whole step to his
// next whole step: hidden while it runs, put back after it. WWHD_60FPS_ONESHOT=0 (a probe): as before.
namespace
{
	constexpr uint32 kLinkNoResetFlg1 = 0x3BCu;
	constexpr uint32 kLinkOneShot = 0x10u | 0x10000u | 0x4u | 0x10000000u;
	uint32 s_oneShotAfterWhole = 0;                 // what of them his last whole step left
	bool OneShotDefer()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_ONESHOT"); return !(e && atoi(e) == 0); }();
		return on;
	}
}

// WWHD_DEBUG_MODELATE=name:off[:off...] (a probe, B62): a converted process's state fields (s16 at those offsets)
// that its whole step changes are put back after it and made at the tick's end, as `late` stores: its half step runs
// the old state again (which normally makes the same change), and if that leaves them as they were the whole step's
// values are written then. A state machine that moves one link a call (set the next mode, act on it next call)
// takes a tick a link as at 30, not half a tick.
namespace
{
	struct ModeLatePending { std::vector<uint16> before, after; };
	std::unordered_map<uint32, ModeLatePending> s_modeLate;
	std::vector<uint16> s_modeLateIn;
	const std::pair<uint16, std::vector<uint32>>& ModeLateSpec()
	{
		static const std::pair<uint16, std::vector<uint32>> spec = [] {
			std::pair<uint16, std::vector<uint32>> s{ 0, {} };
			if (const char* e = getenv("WWHD_DEBUG_MODELATE"))
			{
				char* p;
				s.first = (uint16)strtoul(e, &p, 10);
				while (*p == ':')
					s.second.push_back((uint32)strtoul(p + 1, &p, 16));
			}
			return s;
		}();
		return spec;
	}
	std::vector<uint16> ModeLateRead(uint32 proc)
	{
		std::vector<uint16> v;
		for (uint32 off : ModeLateSpec().second)
			v.push_back(rd16(proc + off));
		return v;
	}
	void ModeLateWrite(uint32 proc, const std::vector<uint16>& v)
	{
		for (size_t i = 0; i < v.size(); i++)
			wr16(proc + ModeLateSpec().second[i], v[i]);
	}
	void ModeLateBefore(uint32 proc)
	{
		if (ModeLateSpec().second.empty() || rd16(proc + 0x08) != ModeLateSpec().first)
			return;
		if (!g_rtHalfTick)
			if (const auto it = s_modeLate.find(proc); it != s_modeLate.end())
			{
				ModeLateWrite(proc, it->second.after);  // a half step that didn't come: the tick's change made
				s_modeLate.erase(it);
			}
		s_modeLateIn = ModeLateRead(proc);
	}
	void ModeLateAfter(uint32 proc)
	{
		if (ModeLateSpec().second.empty() || rd16(proc + 0x08) != ModeLateSpec().first)
			return;
		const std::vector<uint16> now = ModeLateRead(proc);
		if (!g_rtHalfTick)
		{
			if (now != s_modeLateIn)
			{
				s_modeLate[proc] = { s_modeLateIn, now };
				ModeLateWrite(proc, s_modeLateIn);
			}
		}
		else if (const auto it = s_modeLate.find(proc); it != s_modeLate.end())
		{
			if (now == it->second.before)
				ModeLateWrite(proc, it->second.after);
			s_modeLate.erase(it);
		}
	}
}

// fpcM_Execute: every process's execute goes through it (fpcM_Management's execute pass, f_025DE788):
// actors, the camera, the environment, the HUD and menus, scenes. Noted for the half ticks' rollback.
uint32 g_rtActor = 0;                               // the process whose execute runs (rt_step_fall's)
bool g_rtLinkGroundLost = false;                    // Link's half step after his whole step's move lost the ground

void f_025DE58C(PPCInterpreter_t* __restrict ctx)
{
	const uint32 from = wwhd::rt::SixtyFrom();      // ~0 when 60 fps is off
	const uint32 proc = GPR(3);
	g_rtActor = proc;
	if (FILE* f = RndLog(); f && !g_rtHalfTick)     // (the probe: the stream as each whole step begins)
		fprintf(f, "%u 0 E %d %08x %08x %08x\n", wwhd::rt::GameFrame(wwhd::os::SwapCount()), (int)rd16(proc + 0x08),
			rd32(0x101FF9D4u), rd32(0x101FF9D8u), rd32(0x101FF9DCu));
	// WWHD_DEBUG_PLACE=tick:x,y,z (a test aid): Link's position set at that game frame, to reach an actor
	// without steering a route there: current and old (the ground check runs a line from the old one), and
	// the copy his execute starts from (daPy_lk_c::execute's current.pos = l_debug_keep_pos, kept at the end
	// of each execute; WWHD's at 0x1046CD48, f_0240CDD0), without which he stood back where he was by his
	// next execute (session bottom's testaids); his speed 0.
	static const std::array<float, 4> place = [] {
		std::array<float, 4> p{ -1.0f, 0, 0, 0 };
		if (const char* e = getenv("WWHD_DEBUG_PLACE"))
			sscanf(e, "%f:%f,%f,%f", &p[0], &p[1], &p[2], &p[3]);
		return p;
	}();
	// Link's process: noted for the test aids at every rate (WWHD_DEBUG_SPAWN uses his room)
	if (rd16(proc + 0x08) == 168)
	{
		s_link = proc;
		if (place[0] >= 0 && !g_rtHalfTick && wwhd::rt::GameFrame(wwhd::os::SwapCount()) == (uint32)place[0])
			for (uint32 i = 0; i < 3; i++)
			{
				const uint32 v = std::bit_cast<uint32>(place[1 + i]);
				wr32(proc + 0x314 + 4 * i, v);
				wr32(proc + 0x300 + 4 * i, v);
				wr32(0x1046CD48u + 4 * i, v);           // l_debug_keep_pos
				wr32(proc + 0x33C + 4 * i, 0);          // speed
				wr32(proc + 0x370, 0);                  // speedF
			}
	}
	// WWHD_DEBUG_POKE=tick:process,offset,size,value[;...] (a test aid; offset and value hex, size 1, 2
	// or 4): at that game frame, before each process of that name executes, the value is written at
	// its offset: a boss's state or health, to reach a later phase of its fight in a test. Process 0:
	// the offset is an address, written once that frame. The game info (0x1046F0B0) starts with the
	// save's dSv_player_status_a_c (+9, +A, +B: the inventory slots on X, Y and R), and its play part
	// has the items on them (+0x5BBB, +0x5BBC, +0x5BBD: item numbers, the GameCube's mSelectItem at play
	// +0x4933), which Link reads: an item goes on a button with both
	{
		struct Poke { int tick, proc; uint32 offset, size, value; };
		static const std::vector<Poke> pokes = [] {
			std::vector<Poke> v;
			if (const char* e = getenv("WWHD_DEBUG_POKE"))
				for (const char* p = e; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : nullptr)
				{
					Poke w{ -1, 0, 0, 0, 0 };
					if (sscanf(p, "%d:%d,%x,%u,%x", &w.tick, &w.proc, &w.offset, &w.size, &w.value) == 5)
						v.push_back(w);
				}
			return v;
		}();
		static std::vector<bool> pokedAt(pokes.size());
		if (!pokes.empty() && !g_rtHalfTick)
		{
			const int now = (int)wwhd::rt::GameFrame(wwhd::os::SwapCount());
			for (size_t i = 0; i < pokes.size(); i++)
			{
				const Poke& w = pokes[i];
				if (w.tick != now || (w.proc == 0 ? pokedAt[i] : w.proc != (int)rd16(proc + 0x08)))
					continue;
				const uint32 at = w.proc == 0 ? w.offset : proc + w.offset;
				pokedAt[i] = true;
				if (w.size == 1)
					wr8(at, (uint8)w.value);
				else if (w.size == 2)
					wr16(at, (uint16)w.value);
				else
					wr32(at, w.value);
				cemuLog_log(LogType::Force, "wwhd debug: poked process {} +{:x} ({} bytes) = {:x}", w.proc, w.offset, w.size, w.value);
			}
		}
	}
	// WWHD_DEBUG_TIME=tick:degrees (a test aid): at that game frame the day's clock is set, in degrees (15 an
	// hour: 0 midnight, 270 18:00), to test what only runs at night. The clock is the float at +0x44 of the
	// save info the pointer at 0x101F84DC holds (dKy_getdaytime_hour, f_02556C34, reads it there; the save
	// file has it at +0x24 of the player's status); 0x1046F0B0's copy isn't the one the day reads.
	{
		static const std::pair<int, float> clockAt = [] {
			int tick = -1;
			float degrees = 0.0f;
			if (const char* e = getenv("WWHD_DEBUG_TIME"))
				sscanf(e, "%d:%f", &tick, &degrees);
			return std::make_pair(tick, degrees);
		}();
		static bool clockSet = false;
		if (clockAt.first >= 0 && !clockSet && !g_rtHalfTick && (int)wwhd::rt::GameFrame(wwhd::os::SwapCount()) == clockAt.first)
			if (const uint32 info = rd32(0x101F84DC))
			{
				wr32(info + 0x44, std::bit_cast<uint32>(clockAt.second));
				clockSet = true;
				cemuLog_log(LogType::Force, "wwhd debug: the clock ({:x}) set to {} degrees", info + 0x44, clockAt.second);
			}
	}
	if (from == ~0u && !Probe())
		[[clang::musttail]] return orig_f_025DE58C(ctx);
	// WWHD_60FPS_TRIAL_TICKS=a-b: the trial at those game frames only (one moment of a route)
	static const std::pair<uint32, uint32> trialTicks = [] {
		std::pair<uint32, uint32> r{ 0, ~0u };
		if (const char* e = getenv("WWHD_60FPS_TRIAL_TICKS"))
			sscanf(e, "%u-%u", &r.first, &r.second);
		return r;
	}();
	const uint32 tickNow = wwhd::rt::GameFrame(wwhd::os::SwapCount());
	if (from == ~0u && s_trialPhase == 0 && tickNow >= trialTicks.first && tickNow <= trialTicks.second && Trial(rd16(proc + 0x08)))
	{
		s_processes.push_back(proc);
		TrialExecute(ctx);
		return;
	}
	bool converted = from != ~0u && wwhd::os::SwapCount() >= from &&
		(Converted(rd16(proc + 0x08)) || (ConvertAll() && s_knownActors.count(proc) && !ExcludedFromAll(rd16(proc + 0x08))));
	if (converted && g_rtHalfTick && s_cam.restored && proc == s_cam.proc)
	{
		// the camera's half step: a whole 30 fps tick from the tick's start (the camera's tick, above)
		if ((Probe() || Flight()) && Tracked(rd16(proc + 0x08)))
			s_tracked.push_back(proc);
		s_lateNoted.erase(proc);                        // its whole tick makes its late stores
		const float step = g_rtStep;
		g_rtStep = 1.0f;
		g_rtHalfTick = false;
		s_converting++;
		orig_f_025DE58C(ctx);
		s_converting--;
		g_rtHalfTick = true;
		g_rtStep = step;
		s_cam.restored = false;
		return;
	}
	if (converted)
	{
		if (!g_rtHalfTick)
		{
			s_eventAtWhole[proc] = EventRunning();
			s_stepInEventAtWhole[proc] = StepInEvents();
			s_stepping[proc] = s_stepInEventAtWhole[proc] || !s_eventAtWhole[proc];
		}
		else if (s_stepping[proc])
		{
			static bool once = [] { atexit(StepStats); at_quick_exit(StepStats); return true; }();
			(void)once;
			// a tick that began stepping in a running event finishes its half step, though Link's action left
			// the list in the whole tick (the Wind's Requiem's wind change, 0xC4, turning to wait, 0x17: his
			// turn took half a step there, lost the other, and the event ended a tick late, the camera then
			// taking the view saved at the song's start); an event that began, is ordered or ends still stops it
			const bool running = EventRunning() && !(s_eventAtWhole[proc] && s_stepInEventAtWhole[proc]);
			const bool ordered = !running && EventOrdered();
			const bool ending = !running && !ordered && EventEnding();
			const bool edge = running || ordered || ending;
			if (edge && !(EventEdgeFinish() && !InEvent(proc)))
			{
				s_eventStops += running;
				s_orderStops += ordered;
				s_endStops += ending;
				s_stepping[proc] = false;
				MakeLateNotes(proc);                    // the tick's late stores, which its half step would make
			}
			else
			{
				s_edgeFinishes += edge;
				s_halfSteps++;
				s_lateNoted.erase(proc);                // its half step makes them
			}
		}
		converted = s_stepping[proc];
	}
	if ((Probe() || Flight()) && Tracked(rd16(proc + 0x08)) && (!g_rtHalfTick || converted))
		s_tracked.push_back(proc);
	if (!g_rtHalfTick)
		s_processes.push_back(proc);
	else if (!converted)
	{
		GPR(3) = 1;                                   // a half tick: only converted processes run
		return;
	}
	// Link's reset flags, as a whole-tick process or one stepping at 60 sees them (above)
	const bool link = rd16(proc + 0x08) == 168;
	const bool resetFlags = from != ~0u && wwhd::os::SwapCount() >= from && ResetFlagRules();
	if (resetFlags)
		ResetFlagsBefore(proc, link, link || converted);
	if (!converted)
	{
		if (!(resetFlags && link))
			[[clang::musttail]] return orig_f_025DE58C(ctx);
		orig_f_025DE58C(ctx);                           // Link on whole ticks only (an event)
		ResetFlagsAfter(proc, true, true);
		return;
	}
	const float step = g_rtStep;
	g_rtStep = 0.5f;
	s_converting++;
	const bool camTick = CamTick() && !g_rtHalfTick && rd16(proc + 0x08) == 476;
	if (camTick)
		CamTickBefore(proc);                        // its whole tick's half step is for show (the camera's tick)
	if (link && !g_rtHalfTick)
	{
		s_linkActionChanged = false;                // until his action call changes it (rt_hold_leave)
		s_linkGroundAtWhole = rd32(proc + 0x834) & 0x20;
	}
	const uint32 groundNow = link ? rd32(proc + 0x834) & 0x20 : 0;
	const bool groundLost = g_rtHalfTick && link && GroundHold() && s_linkGroundAtWhole && !groundNow &&
		GroundAction(rd32(proc + 0x65F0));
	const bool landing = g_rtHalfTick && link && GroundHold() && !s_linkGroundAtWhole && groundNow;
	const uint32 landingBits = landing ? rd32(proc + 0x834) & 0xE0 : 0;   // the whole step's landing: hit, find, landing
	const bool groundHold = landing || groundLost;
	s_groundStops += groundHold;
	g_rtLinkGroundLost = groundLost;
	const bool hold = (g_rtHalfTick && link && s_linkActionChanged && ActionHold()) || groundHold;
	s_actionStops += hold;
	g_rtHold = hold || camTick;                     // his half step leaves his new action's call out; the camera's
	                                                // half step for show leaves its sound and rumble to its replay
	// its whole step: the late stores it passes over are noted for its half step (s_lateNoted)
	const bool notes = !g_rtHalfTick;
	const bool notesOuter = g_rtLateNotes;
	std::vector<LateNote> outer;
	if (notes)
	{
		outer.swap(s_lateNotes);
		g_rtLateNotes = true;
	}
	const uint32 rndOuter = s_rndExec;
	s_rndExec = proc;                               // its draws are its own (the random stream, f_02019788)
	uint32 oneShotHidden = 0;                       // Link's one-shot flags set since his whole step (above)
	if (link && g_rtHalfTick && OneShotDefer())
	{
		const uint32 f = rd32(proc + kLinkNoResetFlg1);
		oneShotHidden = f & kLinkOneShot & ~s_oneShotAfterWhole;
		if (oneShotHidden)
			wr32(proc + kLinkNoResetFlg1, f & ~oneShotHidden);
	}
	ModeLateBefore(proc);
	orig_f_025DE58C(ctx);
	ModeLateAfter(proc);
	if (landing && LandSnap())
		LandSnapAfter(proc, landingBits);
	if (oneShotHidden)
		wr32(proc + kLinkNoResetFlg1, rd32(proc + kLinkNoResetFlg1) | oneShotHidden);
	else if (link && !g_rtHalfTick)
		s_oneShotAfterWhole = rd32(proc + kLinkNoResetFlg1) & kLinkOneShot;
	s_rndExec = rndOuter;
	if (!g_rtHalfTick && RndSync())
		RndFlush(proc);                             // its half step's draws its whole step didn't make again
	if (camTick)
		CamTickAfter();
	if (notes)
	{
		g_rtLateNotes = notesOuter;
		if (s_lateNotes.empty())
			s_lateNoted.erase(proc);
		else
			s_lateNoted[proc].swap(s_lateNotes);
		s_lateNotes.swap(outer);
	}
	g_rtHold = false;
	g_rtLinkGroundLost = false;
	s_converting--;
	g_rtStep = step;
	if (resetFlags)
		ResetFlagsAfter(proc, link, true);
	static const bool attached = [] { const char* e = getenv("WWHD_60FPS_ATTACHED"); return !(e && atoi(e) == 0); }();
	if (g_rtHalfTick && attached && rd16(proc + 0x08) == 168 && Rollback())
		NoteHeld(proc);
	if (link && !g_rtHalfTick)
		ThrowPlace(proc);
}

// dEvent_manager_c::getIsAddvance (f_025447C8: the staff's mAdvance, set while its cut is in its first
// tick; DOOR10's demoProc, f_021268C8, asks it before a cut's init, as in the decomp). The event manager
// advances cuts on whole ticks, so on a half tick a converted actor saw the cut as new again and ran its
// init twice (a door's smoke: a second emitter and its shake count reset; its goal for Link set again
// from where he had walked to). A half step sees no new cut: the whole tick's step did the init.
void f_025447C8(PPCInterpreter_t* __restrict ctx)
{
	if (g_rtHalfTick && s_converting > 0 && EventEndRules())
	{
		GPR(3) = 0;
		return;
	}
	[[clang::musttail]] return orig_f_025447C8(ctx);
}

// The play scene's plants (its execute, f_025AF8A0, calls each): the grass, trees, bushes (dWood) and
// flowers are packets of the scene, not processes, whose calc sways them, cuts them and grows them
// back. They ran on whole ticks (tick rules). At 60 they now run every frame with a time step of half a
// tick (WWHD_60FPS_PLANTS=0: whole ticks only), their stores standing (config/US_v0/tick_rules.txt has their steps);
// at 30 with the probe, WWHD_60FPS_TRIAL_PLANTS=1 runs them as the step-doubling trial.
// The lava (executeMagma, f_02524CA0; the play scene's draw calls it at 025B00CC): dMagma_packet_c's calc
// scrolls the lava's texture, cycles its glow's color and bobs its bubbles; the same, with
// WWHD_60FPS_MAGMA and WWHD_60FPS_TRIAL_MAGMA (config/US_v0/tick_rules/objects.txt has its steps).
namespace
{
	// a scene packet's calc (converted: every frame at 60 with a time step; trial: the step-doubling trial at 30)
	void ScenePacket(PPCInterpreter_t* ctx, void (*fn)(PPCInterpreter_t*), bool converted, bool trial)
	{
		const uint32 from = wwhd::rt::SixtyFrom();
		if (from == ~0u)
		{
			if (trial && s_trialPhase == 0)
				TrialExecute(ctx, fn);
			else
				fn(ctx);
			return;
		}
		if (!converted || wwhd::os::SwapCount() < from)
		{
			if (!g_rtHalfTick)
				fn(ctx);                                // whole ticks only, as the game's logic
			return;
		}
		const float step = g_rtStep;
		g_rtStep = 0.5f;
		s_converting++;
		fn(ctx);
		s_converting--;
		g_rtStep = step;
	}

	void Plants(PPCInterpreter_t* ctx, void (*fn)(PPCInterpreter_t*))
	{
		static const bool converted = [] { const char* e = getenv("WWHD_60FPS_PLANTS"); return !e || atoi(e) == 1; }();   // on unless =0
		static const bool trial = [] { const char* e = getenv("WWHD_60FPS_TRIAL_PLANTS"); return Probe() && e && atoi(e) == 1; }();
		ScenePacket(ctx, fn, converted, trial);
	}
}
void f_02524CA0(PPCInterpreter_t* __restrict ctx)
{
	static const bool converted = [] { const char* e = getenv("WWHD_60FPS_MAGMA"); return !e || atoi(e) == 1; }();   // on unless =0
	static const bool trial = [] { const char* e = getenv("WWHD_60FPS_TRIAL_MAGMA"); return Probe() && e && atoi(e) == 1; }();
	ScenePacket(ctx, orig_f_02524CA0, converted, trial);
}
void f_02524DA0(PPCInterpreter_t* __restrict ctx) { Plants(ctx, orig_f_02524DA0); }
void f_02524EA0(PPCInterpreter_t* __restrict ctx) { Plants(ctx, orig_f_02524EA0); }
void f_02524FA0(PPCInterpreter_t* __restrict ctx) { Plants(ctx, orig_f_02524FA0); }
void f_025250A0(PPCInterpreter_t* __restrict ctx) { Plants(ctx, orig_f_025250A0); }

// dPa_control_c::calc3D (f_025A81A0; the play scene's draw calls it, f_025B019C): every 3D particle
// emitter's calc, JPAEmitterManager::calc (f_0282167C) on groups 0 to 6: emission, the particles'
// motion and ageing, the emitters' callbacks (the ship's wake and splashes). At 60 fps it runs on
// whole ticks only, as the game's logic does, unless WWHD_60FPS_PARTICLES=1: then every frame, with
// a time step of half a tick (config/US_v0/tick_rules.txt has JParticle's steps) and its stores
// standing, as a converted process's do. At 30 fps with the probe, WWHD_60FPS_TRIAL_PARTICLES=1 runs
// it as the step-doubling trial (TrialExecute).
void f_025A81A0(PPCInterpreter_t* __restrict ctx)
{
	static const bool converted = [] { const char* e = getenv("WWHD_60FPS_PARTICLES"); return !e || atoi(e) == 1; }();   // on unless =0
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
// WWHD_60FPS_DRAWDIFF=swap (a probe): .data and .bss at that swap's first draw, against the next
// swap's first draw: the words the draw pass changed that a half tick's draws then see
// (WWHD_60FPS_DRAWDIFF_OUT, default drawdiff.txt)
namespace
{
	void DrawDiff()
	{
		static const uint32 at = [] { const char* e = getenv("WWHD_60FPS_DRAWDIFF"); return e ? (uint32)atoi(e) : 0u; }();
		if (!at)
			return;
		static std::vector<uint8> snap;
		const uint32 swap = wwhd::os::SwapCount();
		if (swap == at)
			snap.assign(memory_base + kGlobalsLow, memory_base + kGlobalsHigh);
		else if (swap == at + 1 && !snap.empty())
		{
			const char* path = getenv("WWHD_60FPS_DRAWDIFF_OUT");
			FILE* f = fopen(path ? path : "drawdiff.txt", "w");
			if (!f)
				return;
			uint32 n = 0;
			for (uint32 o = 0; o + 4 <= snap.size(); o += 4)
				if (memcmp(&snap[o], memory_base + kGlobalsLow + o, 4) != 0 && n++ < 4000)
					fprintf(f, "drawdiff %08x %08x -> %08x\n", kGlobalsLow + o, __builtin_bswap32(*(uint32*)&snap[o]),
						__builtin_bswap32(*(uint32*)(memory_base + kGlobalsLow + o)));
			fprintf(f, "drawdiff: %u words differ (swap %u, %s)\n", n, swap, g_rtHalfTick ? "half" : "whole");
			fclose(f);
		}
	}
}

namespace
{
	void ChainArrays(uint32 proc, std::vector<std::pair<uint32, uint32>>& saved);   // the chains (below)
	void ChainRestore(const std::vector<std::pair<uint32, uint32>>& saved);
}

void f_025DE2CC(PPCInterpreter_t* __restrict ctx)
{
	if (s_firstDraw)
	{
		s_firstDraw = false;
		DrawDiff();
		// WWHD_60FPS_WATCH=addr[,addr] (a probe): those words at each frame's first draw (watch.txt)
		if (const char* e = getenv("WWHD_60FPS_WATCH"))
		{
			static FILE* f = fopen("watch.txt", "w");
			fprintf(f, "%u %c", wwhd::os::SwapCount(), g_rtHalfTick ? 'h' : 'w');
			for (const char* p = e; p && *p; p = strchr(p, ','))
			{
				if (*p == ',')
					p++;
				const uint32 ea = (uint32)strtoul(p, nullptr, 16);
				fprintf(f, " %08x", rd32(ea));
			}
			fprintf(f, "\n");
			fflush(f);
		}
		// WWHD_60FPS_FIND=swap:value[,value] (a probe): where in MEM2 those words are, at that swap's first draw
		if (const char* e = getenv("WWHD_60FPS_FIND"); e && (uint32)atoi(e) == wwhd::os::SwapCount())
			if (FILE* f = fopen("find.txt", "w"))
			{
				for (const char* p = strchr(e, ':'); p && *p; p = strchr(p + 1, ','))
				{
					const uint32 v = __builtin_bswap32((uint32)strtoul(p + 1, nullptr, 16));
					for (uint32 ea = 0x10000000u; ea < 0x50000000u; ea += 4)
						if (*(uint32*)(memory_base + ea) == v)
							fprintf(f, "%08x at %08x\n", __builtin_bswap32(v), ea);
				}
				fclose(f);
			}
	}
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
	std::vector<std::pair<uint32, uint32>> chains;   // its chains moved on for this draw (the chains, below)
	ChainArrays(s_drawing, chains);
	if (!(g_rtHalfTick && !s_held.empty() && HeldDraw(ctx, orig_f_025DE2CC)))
		orig_f_025DE2CC(ctx);
	ChainRestore(chains);
	s_drawing = outer;
}

// ---- chains at 60 fps (the chains item, D21) ---------------------------------------------------------
// One-pass chain solvers (a Stalfos' hair: ke_move) keep the 30 Hz step on whole ticks (tick_rules: a half
// step's projection doesn't add up to a tick's), so on a half tick's frame their points are still the whole
// tick's while the actor they hang on has moved on. Drawn there, they're moved half a tick on along their
// last tick's motion (the points as the whole tick left them plus half of what that tick moved them: exact
// for an even swing), only while drawing (the solver's state is put back after the draw) and only for a
// converted actor that stepped this half tick. WWHD_60FPS_CHAINS=0 draws them as they are.
namespace
{
	// WWHD_60FPS_CHAINS_LOG=path (a probe): each smoothed chain's tip (a line's last point, an array's
	// middle point) per drawn frame, as drawn
	FILE* ChainLog()
	{
		static FILE* log = [] { const char* e = getenv("WWHD_60FPS_CHAINS_LOG"); return e ? fopen(e, "w") : nullptr; }();
		return log;
	}

	bool ChainsOn()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_CHAINS"); return !(e && atoi(e) == 0); }();
		return on;
	}

	struct ChainPoints { std::vector<uint32> prev, curr; };    // the points' words at the last two whole ticks' draws
	std::unordered_map<uint32, ChainPoints> s_chainPoints;     // by the points' address

	// n points (cXyz) at ea, about to be drawn: noted on a whole tick's frame; on a half tick's, each point still
	// the whole tick's is moved on (one that moved this half tick, a root that follows its actor every step,
	// stays); the words they had are added to saved, to put back after the draw
	void ChainDraw(uint32 ea, uint32 n, std::vector<std::pair<uint32, uint32>>& saved)
	{
		std::vector<uint32> now(3 * n);
		for (uint32 i = 0; i < 3 * n; i++)
			now[i] = rd32(ea + 4 * i);
		ChainPoints& c = s_chainPoints[ea];
		if (!g_rtHalfTick)
		{
			c.prev = c.curr.size() == now.size() ? std::move(c.curr) : now;
			c.curr = std::move(now);
			return;
		}
		if (c.curr.size() != now.size() || c.prev.size() != now.size())
			return;                                        // not seen at a whole tick
		for (uint32 p = 0; p < n; p++)
		{
			const uint32 i = 3 * p;
			if (now[i] != c.curr[i] || now[i + 1] != c.curr[i + 1] || now[i + 2] != c.curr[i + 2])
				continue;
			for (uint32 k = i; k < i + 3; k++)
			{
				const float a = std::bit_cast<float>(c.prev[k]), b = std::bit_cast<float>(c.curr[k]);
				saved.emplace_back(ea + 4 * k, now[k]);
				wr32(ea + 4 * k, std::bit_cast<uint32>(b + 0.5f * (b - a)));
			}
		}
	}

	// the same for n angle triplets (csXyz, s16s) at ea: each moved on by half its last tick's wrapped change
	void ChainDrawAngles(uint32 ea, uint32 n, std::vector<std::pair<uint32, uint32>>& saved)
	{
		const uint32 words = (6 * n + 3) / 4;
		std::vector<uint32> now(words);
		for (uint32 i = 0; i < words; i++)
			now[i] = rd32(ea + 4 * i);
		ChainPoints& c = s_chainPoints[ea];
		if (!g_rtHalfTick)
		{
			c.prev = c.curr.size() == now.size() ? std::move(c.curr) : now;
			c.curr = std::move(now);
			return;
		}
		if (c.curr.size() != now.size() || c.prev.size() != now.size())
			return;
		auto half = [](const std::vector<uint32>& w, uint32 k) { return (sint16)(uint16)(w[k / 2] >> (k % 2 ? 0 : 16)); };
		std::vector<bool> taken(words);
		for (uint32 p = 0; p < n; p++)
		{
			bool same = true;
			for (uint32 k = 3 * p; k < 3 * p + 3; k++)
				same = same && half(now, k) == half(c.curr, k);
			if (!same)
				continue;
			for (uint32 k = 3 * p; k < 3 * p + 3; k++)
			{
				if (!taken[k / 2])
				{
					taken[k / 2] = true;
					saved.emplace_back(ea + 4 * (k / 2), now[k / 2]);
				}
				const sint16 a = half(c.prev, k), b = half(c.curr, k);
				wr16(ea + 2 * k, (uint16)(sint16)(b + (sint16)(b - a) / 2));
			}
		}
	}

	// Chains in an actor that its draw makes models from (a whole tick's solver; its draw reads them): by process
	// name, the arrays drawn half a tick on in a half tick's draw of the actor (if it stepped) and put back after.
	// Helmaroc King's tail feathers (BDK 238: four tails at +0x414, 0x17C each; tail_draw reads each one's
	// places at +0x24 and angles at +0x9C; tail_control on whole ticks but for the roots); a Kargaroc's tail
	// (BB 181: places at +0xC1C, angles at +0xC94, read by the draw's inlined tail_draw; tail_control on whole
	// ticks, its root set every step); the opening's Helmaroc King (Dk 167: BDK's four tails at +0x3E8, session
	// bottom's census-left)
	struct ChainArray { uint16 name; uint32 offset, count; bool angles; };
	constexpr ChainArray kChainArrays[] = {
		{ 238, 0x414 + 0x24, 10, false }, { 238, 0x414 + 0x9C, 10, true },
		{ 238, 0x590 + 0x24, 10, false }, { 238, 0x590 + 0x9C, 10, true },
		{ 238, 0x70C + 0x24, 10, false }, { 238, 0x70C + 0x9C, 10, true },
		{ 238, 0x888 + 0x24, 10, false }, { 238, 0x888 + 0x9C, 10, true },
		{ 181, 0xC1C, 10, false }, { 181, 0xC94, 10, true },
		{ 167, 0x3E8 + 0x24, 10, false }, { 167, 0x3E8 + 0x9C, 10, true },
		{ 167, 0x564 + 0x24, 10, false }, { 167, 0x564 + 0x9C, 10, true },
		{ 167, 0x6E0 + 0x24, 10, false }, { 167, 0x6E0 + 0x9C, 10, true },
		{ 167, 0x85C + 0x24, 10, false }, { 167, 0x85C + 0x9C, 10, true },
	};

	void ChainRestore(const std::vector<std::pair<uint32, uint32>>& saved)
	{
		for (const auto& [ea, w] : saved)
			wr32(ea, w);
	}

	// the actors whose 3D lines are such chains, by process name: a Stalfos (190: its hair, ke_move on whole
	// ticks), a rat (198: its tail, tail_control on whole ticks), Ganondorf (246: his hair, ke_control on whole
	// ticks; session bottom's N13)
	bool ChainLines(uint16 name)
	{
		return name == 190 || name == 198 || name == 246 || name == 253;
	}

	// Actors left on whole ticks (not converted: a solver built into their execute, which also builds their
	// models' matrices) whose draws are smoothed all the same: on a half tick's frame their 3D lines (ChainLines)
	// and their models' base matrices (WWHD's J3DModel +0xC8, 3x4 floats: Obj_Monument's set_mtx f_02375E1C stores them there, at the model pointers listed) are moved half a
	// tick on along their last tick, as the chains are, and put back after the draw (session bottom's B68).
	// SITEM (253, d_a_sitem.cpp: the rope items in Ganon's Tower's forest trial, GanonD room 0): its two rope lines
	// (+0x428, +0x688) and the item on the rope (its model at +0x3D0).
	struct DrawModel { uint16 name; uint32 offset; };
	constexpr DrawModel kDrawModels[] = { { 253, 0x3D0 } };
	bool DrawSmoothed(uint16 name)
	{
		return name == 253;
	}

	// a process about to draw: its chain arrays noted (a whole tick) or moved on (a half tick it stepped)
	void ChainArrays(uint32 proc, std::vector<std::pair<uint32, uint32>>& saved)
	{
		if (!ChainsOn() || !proc)
			return;
		const uint16 name = rd16(proc + 0x08);
		if (DrawSmoothed(name))
			for (const DrawModel& m : kDrawModels)
				if (m.name == name)
					if (const uint32 model = rd32(proc + m.offset); model >= 0x10000000u && model < 0x50000000u)
					{
						ChainDraw(model + 0xC8, 4, saved);   // its base matrix's 12 floats, as 4 points
						if (FILE* log = ChainLog())
						{
							fprintf(log, "%u %c %08x m %.3f %.3f %.3f\n", wwhd::os::SwapCount(), g_rtHalfTick ? 'h' : 'w', proc,
								std::bit_cast<float>(rd32(model + 0xC8 + 12)), std::bit_cast<float>(rd32(model + 0xC8 + 28)),
								std::bit_cast<float>(rd32(model + 0xC8 + 44)));
							fflush(log);
						}
					}
		if (std::none_of(std::begin(kChainArrays), std::end(kChainArrays), [&](const ChainArray& a) { return a.name == name; }))
			return;
		if (g_rtHalfTick)
		{
			const auto it = s_stepping.find(proc);
			if (it == s_stepping.end() || !it->second)
				return;
		}
		for (const ChainArray& a : kChainArrays)
			if (a.name == name)
				(a.angles ? ChainDrawAngles : ChainDraw)(proc + a.offset, a.count, saved);
		if (FILE* log = ChainLog())
		{
			const ChainArray& a = *std::find_if(std::begin(kChainArrays), std::end(kChainArrays), [&](const ChainArray& x) { return x.name == name; });
			const uint32 mid = proc + a.offset + 12 * (a.count / 2);
			fprintf(log, "%u %c %08x %.3f %.3f %.3f\n", wwhd::os::SwapCount(), g_rtHalfTick ? 'h' : 'w', proc,
				std::bit_cast<float>(rd32(mid)), std::bit_cast<float>(rd32(mid + 4)), std::bit_cast<float>(rd32(mid + 8)));
			fflush(log);
		}
	}

	// the process drawing is one whose chains are noted (a whole tick) or moved on (a half tick it stepped)
	bool ChainOwner(uint32 proc)
	{
		if (!ChainsOn() || !proc || !ChainLines(rd16(proc + 0x08)))
			return false;
		if (!g_rtHalfTick || DrawSmoothed(rd16(proc + 0x08)))
			return true;
		const auto it = s_stepping.find(proc);
		return it != s_stepping.end() && it->second;
	}
}

namespace
{
	// a 3D line class's update (in a draw): its lines' points moved on for a half tick's frame (above). count,
	// most and lines: where the class keeps its lines' count (u16), their most points (u16) and the lines (16
	// bytes each, the points' address first)
	void ChainLineUpdate(PPCInterpreter_t* __restrict ctx, void (*orig)(PPCInterpreter_t*), uint32 count, uint32 most, uint32 lines)
	{
		FILE* log = ChainLog();
		std::vector<std::pair<uint32, uint32>> saved;
		const uint32 mat = GPR(3), at = rd32(mat + lines);
		const uint32 n = rd16(mat + count), points = std::min<uint32>(GPR(4) & 0xFFFFu, rd16(mat + most));
		for (uint32 i = 0; i < n; i++)
			if (const uint32 ea = rd32(at + 16 * i))
				ChainDraw(ea, points, saved);
		if (log && n && points)
		{
			const uint32 tip = rd32(at) + 12 * (points - 1);
			fprintf(log, "%u %c %08x %.3f %.3f %.3f\n", wwhd::os::SwapCount(), g_rtHalfTick ? 'h' : 'w', s_drawing,
				std::bit_cast<float>(rd32(tip)), std::bit_cast<float>(rd32(tip + 4)), std::bit_cast<float>(rd32(tip + 8)));
			fflush(log);
		}
		orig(ctx);
		ChainRestore(saved);
	}
}

// mDoExt_3DlineMat1_c::update(mat r3, points r4, width f1, color r5, ?, tevStr r7): builds its lines' vertices
// from their points, in the draw (a Stalfos' hair, ke_disp): +0x13C the lines, +0x13E their most points, +0x144
// the lines
void orig_f_025EA548(PPCInterpreter_t* __restrict ctx);
void f_025EA548(PPCInterpreter_t* __restrict ctx)
{
	if (!ChainOwner(s_drawing))
		[[clang::musttail]] return orig_f_025EA548(ctx);
	ChainLineUpdate(ctx, orig_f_025EA548, 0x13C, 0x13E, 0x144);
}

// the other 3D line class's update, the same with the class's fields 0x40 on (a rat's tail, the draw f_0230AE30;
// rope bridges, ropes, the ships' lines too): +0x17C, +0x17E, +0x184
void orig_f_025EC62C(PPCInterpreter_t* __restrict ctx);
void f_025EC62C(PPCInterpreter_t* __restrict ctx)
{
	if (!ChainOwner(s_drawing))
		[[clang::musttail]] return orig_f_025EC62C(ctx);
	ChainLineUpdate(ctx, orig_f_025EC62C, 0x17C, 0x17E, 0x184);
}

// mDoExt_3DlineMat1_c::update's other form (segments, color, tevStr: no size or spacing; SITEM's ropes): the same
void orig_f_025EAF58(PPCInterpreter_t* __restrict ctx);
void f_025EAF58(PPCInterpreter_t* __restrict ctx)
{
	if (!ChainOwner(s_drawing))
		[[clang::musttail]] return orig_f_025EAF58(ctx);
	ChainLineUpdate(ctx, orig_f_025EAF58, 0x13C, 0x13E, 0x144);
}

// fopAc_Execute: every actor's execute goes through it (from fpcM_Management's execute pass)
void f_025D475C(PPCInterpreter_t* __restrict ctx)
{
	if ((Probe() || wwhd::rt::SixtyFrom() != ~0u) && !g_rtHalfTick && s_trialPhase != 1)
		s_actors.push_back(GPR(3));
	s_knownActors[GPR(3)] = true;                     // actors (ConvertAll's list; InEvent's test)
	[[clang::musttail]] return orig_f_025D475C(ctx);
}


// the `hold` rules' notes on Link's action call (tools/recomp/generate.py; link_actions.txt): whether the call
// changed his action in his whole step (ActionHold above)
void rt_hold_enter()
{
	if (s_link && !g_rtHalfTick)
		s_linkActionAtCall = rd32(s_link + 0x65F0);
}

void rt_hold_leave()
{
	if (s_link && !g_rtHalfTick)
	{
		const uint32 action = rd32(s_link + 0x65F0);
		if (action != s_linkActionAtCall && HoldsAfter(action))
			s_linkActionChanged = true;
	}
}
