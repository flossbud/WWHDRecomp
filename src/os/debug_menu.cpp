// The debug menu's game side (debug_menu.h): where its warps go, the boss rush, the test arena and the spawn list's
// placement, on the test aids (src/overrides/sixty.cpp); the menu is the pause menu's (src/frontend/pause_menu.cpp).
#include "debug_menu.h"
#include "settings.h"
#include "savestate.h"
#include <cmath>
#include <mutex>

namespace
{
	using wwhd::os::input::Pad;
	namespace B = wwhd::os::input;

	// Where the menu warps: a stage, a spawn point, a room and a layer, as WWHD_DEBUG_STAGE takes them
	// (docs/handoff.md lists how each was found); boss rooms turn the refights on first.
	struct Dest { const char* label; const char* stage; int point, room, layer; bool boss; };
	constexpr Dest kIslands[] = {
		{ "Outset Island", "sea", 0, 44, -1, false },
		{ "Windfall Island", "sea", 0, 11, -1, false },
		{ "Dragon Roost Island", "Adanmae", 0, 0, -1, false },
		{ "Rito Aerie", "Atorizk", 0, 0, -1, false },
		{ "Forest Haven", "Omori", 0, 0, -1, false },
	};
	constexpr Dest kDungeons[] = {
		{ "Dragon Roost Cavern", "M_NewD2", 0, 0, -1, false },
		{ "Forbidden Woods", "kindan", 0, 0, -1, false },
		{ "Tower of the Gods", "Siren", 0, 0, -1, false },
		{ "Forsaken Fortress", "MajyuE", 0, 0, -1, false },
		{ "Earth Temple", "M_Dai", 0, 0, -1, false },
		{ "Wind Temple", "kaze", 0, 0, -1, false },
		{ "Hyrule Castle", "Hyrule", 0, 0, -1, false },
		{ "Ganon's Tower", "GanonA", 0, 0, -1, false },
	};
	constexpr Dest kBosses[] = {
		{ "Gohma (Dragon Roost Cavern)", "M_DragB", 0, 0, -1, true },
		{ "Kalle Demos (Forbidden Woods)", "kinBOSS", 0, 0, -1, true },
		{ "Gohdan (Tower of the Gods)", "SirenB", 0, 0, -1, true },
		{ "Helmaroc King (Forsaken Fortress)", "M2tower", 22, 0, 3, true },
		{ "Jalhalla (Earth Temple)", "M_DaiB", 0, 0, -1, true },
		{ "Molgera (Wind Temple)", "kazeB", 0, 0, -1, true },
		{ "Puppet Ganon (Ganon's Tower)", "GanonK", 0, 0, -1, true },
		{ "Ganondorf (Ganon's Tower)", "GTower", 0, 0, -1, true },
		// Ganon's Tower's rooms where four bosses are fought again (each boss placed there and present
		// after the warp: stage_actors.py, a tracked run)
		{ "Gohma again (Ganon's Tower)", "Xboss0", 0, 0, -1, true },
		{ "Kalle Demos again (Ganon's Tower)", "Xboss1", 0, 0, -1, true },
		{ "Jalhalla again (Ganon's Tower)", "Xboss2", 0, 0, -1, true },
		{ "Molgera again (Ganon's Tower)", "Xboss3", 0, 0, -1, true },
	};

	// enemies to spawn ahead of Link (each spawned so in a test: docs/handoff.md); anglex is more
	// parameters for some (a Darknut's 0x80: a shield and a cape)
	// radius: its spacing from Link and the others when spawned (generous for its body and weapon: a Moblin's spear, a
	// Kargaroc's wings)
	struct Foe { const char* label; int process; uint32 param, anglex; float radius; };
	constexpr Foe kFoes[] = {
		{ "Bokoblin", 189, 0, 0, 45 }, { "Moblin", 188, 0, 0, 75 }, { "Darknut", 191, 0, 0x80, 70 }, { "Chuchu", 206, 0, 0, 45 },
		{ "Keese", 215, 0, 0, 35 }, { "ReDead", 224, 0, 0, 50 }, { "Kargaroc", 181, 0, 0, 80 },
	};

	// the boss rush (the owner's idea: the bosses again from a finished save, one after another): kBosses'
	// first eight in order (not Ganon's Tower's refight rooms, where a beaten boss sends Link on by itself:
	// Gohma's death there changes the stage instead of setting the bit, d_a_btd.cpp), refights on; when the boss being fought is beaten (the game sets its dungeon's "boss beaten"
	// bit, wwhd::debug::RushBossBeaten) the game's next stage change (its warp out) goes to the next one
	constexpr int kRushBosses = 8;
	static_assert(kRushBosses <= (int)std::size(kBosses));
	std::mutex s_rushLock;
	int s_rush = -1;                                   // the boss being fought, or -1
	bool s_rushBeaten = false;
	void StopRush()
	{
		std::lock_guard lock(s_rushLock);
		s_rush = -1;
	}

	std::mutex s_lock;
	uint32 s_prev = 0;
	bool s_swallow = false;
	void (*s_opener)() = nullptr;
}

namespace wwhd::debug
{
	void RushBossBeaten()
	{
		std::lock_guard lock(s_rushLock);
		if (s_rush >= 0)
			s_rushBeaten = true;
	}

	bool RushRunning()
	{
		std::lock_guard lock(s_rushLock);
		return s_rush >= 0;
	}

	int RushBoss()
	{
		std::lock_guard lock(s_rushLock);
		return s_rush;
	}

	bool RushNextStage(const char*& name, int& point, int& room, int& layer)
	{
		std::lock_guard lock(s_rushLock);
		if (s_rush < 0 || !s_rushBeaten)
			return false;
		s_rushBeaten = false;
		if (++s_rush >= kRushBosses)
		{
			s_rush = -1;                                // the last one beaten: the game's own warp
			return false;
		}
		const Dest& d = kBosses[s_rush];
		name = d.stage;
		point = d.point;
		room = d.room;
		layer = d.layer;
		return true;
	}
}

namespace wwhd::os::debug_menu
{
	void SetOpener(void (*open)())
	{
		std::lock_guard lock(s_lock);
		s_opener = open;
	}

	bool Enabled()
	{
		const int i = settings::Find("WWHD_DEBUG_MENU");
		if (i < 0)
			return false;
		const settings::OptionView v = settings::View(i);
		return v.current < (int)v.values.size() && v.values[v.current] == "1";
	}

	int Count(Group g)
	{
		return g == Group::Islands ? (int)std::size(kIslands) : g == Group::Dungeons ? (int)std::size(kDungeons) : (int)std::size(kBosses);
	}

	void Warp(Group g, int index)
	{
		if (index < 0 || index >= Count(g))
			return;
		const Dest& d = g == Group::Islands ? kIslands[index] : g == Group::Dungeons ? kDungeons[index] : kBosses[index];
		if (d.boss)
			wwhd::debug::SetBossRefight(true);
		StopRush();                                     // a warp of its own ends a rush
		wwhd::debug::SetArena(false);                   // and the test arena
		wwhd::debug::CancelSpawns();
		wwhd::debug::RequestStage(d.stage, d.point, d.room, d.layer);
	}

	void StartRush()
	{
		{
			std::lock_guard lock(s_rushLock);
			s_rush = 0;
			s_rushBeaten = false;
		}
		wwhd::debug::SetArena(false);
		wwhd::debug::SetBossRefight(true);
		wwhd::debug::CancelSpawns();
		const Dest& d = kBosses[0];
		wwhd::debug::RequestStage(d.stage, d.point, d.room, d.layer);
	}

	// the test arena (the owner's idea N28): Gohma's room, the boss beaten (refights off) and no warp in the
	// middle (wwhd::debug::SetArena, sixty.cpp), a big open floor for movement, physics and enemies (spawn them
	// there); any other warp from the menu turns it off
	void StartArena()
	{
		StopRush();
		wwhd::debug::SetBossRefight(false);
		wwhd::debug::SetArena(true);
		wwhd::debug::CancelSpawns();
		wwhd::debug::RequestStage("M_DragB", 0, 0, -1);
	}

	int FoeCount()
	{
		return (int)std::size(kFoes);
	}

	// The spawn list (the design left placement and the limit to us): in the list's order (a type's together), at most
	// kSpawnCap at once (the game's actor heap and frame time: a few dozen enemies stay playable), the rest left out (the
	// toast says so); placed by sixty.cpp (PlanSpawns: arcs ahead of Link on the ground, clear of him and each other)
	int SpawnFoes(const std::vector<int>& counts)
	{
		std::vector<wwhd::debug::SpawnAsk> asks;
		for (int f = 0; f < (int)std::size(kFoes) && f < (int)counts.size(); f++)
			for (int k = 0; k < counts[f] && (int)asks.size() < kSpawnCap; k++)
				asks.push_back({ kFoes[f].process, kFoes[f].param, kFoes[f].anglex, kFoes[f].radius });
		wwhd::debug::RequestSpawns(asks);
		return (int)asks.size();
	}

	void CancelSpawns()
	{
		wwhd::debug::CancelSpawns();
	}

	// both sticks clicked at once: the pause menu on the Debug Menu (while it's enabled); the game sees nothing of the
	// press until the sticks are let go
	input::Pad Filter(const input::Pad& pad)
	{
		void (*open)() = nullptr;
		{
			std::lock_guard lock(s_lock);
			const uint32 now = pad.buttons;
			const uint32 pressed = now & ~s_prev;
			s_prev = now;
			const uint32 both = B::LCLICK | B::RCLICK;
			if ((now & both) == both && (pressed & both) && Enabled())
				open = s_opener, s_swallow = true;
			if (s_swallow && (now & both) == 0)
				s_swallow = false;
		}
		if (open)
			open();
		if (s_swallow)
		{
			input::Pad p = pad;
			p.buttons &= ~(B::LCLICK | B::RCLICK);
			return p;
		}
		return pad;
	}
}
