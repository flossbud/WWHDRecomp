// The debug menu's game side (the owner's idea: the test aids from a controller): warps to islands, dungeons and boss
// rooms (the bosses' refights on), a boss rush, a test arena (Gohma's room with no warp in the middle) and enemies spawned
// ahead of Link. The menu itself is the pause menu's Debug Menu (Pause Menu v10, src/frontend/pause_menu.cpp), shown
// while Settings > Advanced > Enable Debug Menu is on; clicking both sticks at once, or F1, opens the pause menu straight
// onto it. debug_menu.cpp.
#pragma once
#include "input.h"
#include <string>
#include <vector>

namespace wwhd::os::debug_menu
{
	// The pad the game reads (KPADReadEx, input.cpp): both sticks clicked at once open the Debug Menu (SetOpener)
	input::Pad Filter(const input::Pad& pad);
	// what opens it (the pause menu, on the Debug Menu), set by the frontend
	void SetOpener(void (*open)());
	// Settings > Advanced > Enable Debug Menu (WWHD_DEBUG_MENU; off by default, applied at once)
	bool Enabled();

	// the Debug Menu's choices (its tabs' rows in the design's order: islands 5, dungeons 8, bosses 12, foes 7)
	enum class Group { Islands, Dungeons, Bosses };
	int Count(Group g);
	// a warp at the next game frame (a boss room turns the refights on); it ends a rush and the test arena
	void Warp(Group g, int index);
	void StartRush();                           // the first eight bosses in order, refights on, no heart containers
	void StartArena();                          // Gohma's room, the boss beaten, no warp in the middle
	int FoeCount();
	// the spawn list: counts per foe (the design's order), spawned ahead of Link over the next game frames (placed by
	// sixty.cpp, wwhd::debug::TakeSpawnReport): the number asked for (at most kSpawnCap, the rest left out)
	constexpr int kSpawnCap = 32;
	int SpawnFoes(const std::vector<int>& counts);
	void CancelSpawns();                        // those not spawned yet (a warp, a rush, a fight, a state load)
}

namespace wwhd::debug
{
	// The test aids' runtime side (src/overrides/sixty.cpp): a stage change on the next game frame, as
	// WWHD_DEBUG_STAGE makes one, and the bosses' refights (WWHD_DEBUG_BOSS) on or off.
	void RequestStage(const char* name, int point, int room, int layer);
	// enemies to spawn ahead of Link, facing him, once he's settled in play (four a game frame), placed clear of him and
	// of each other by their spacing radius, on the ground (anglex: more parameters for some: a Darknut's 0x80, a
	// shield and a cape); the report once the last is made: how many were placed and how many found no place
	struct SpawnAsk { int process; uint32 param, anglex; float radius; };
	void RequestSpawns(const std::vector<SpawnAsk>& asks);
	void CancelSpawns();
	bool TakeSpawnReport(int& placed, int& dropped);
	void SetBossRefight(bool on);
	bool BossRefight();
	// the test arena (N28): Gohma's room with no warp in the middle (WWHD_DEBUG_ARENA=1, or the menu)
	void SetArena(bool on);
	bool Arena();
	// a boss beaten: the game set its "beaten" bit (its dungeon item 3, f_025B9098; Puppet Ganon's event bit
	// 3F10, f_025B8B68); the refights answer the truth from then until the game's next stage change is in
	void BossBeaten();
	bool BossBeatenHere();
	// the boss rush: a boss beaten (BossBeaten)
	void RushBossBeaten();
	// a rush is on (no heart containers then: the owner's wish, B47)
	bool RushRunning();
	// the rush's boss being fought: 0 Gohma, 1 Kalle Demos ... (debug_menu.cpp's kBosses), or -1 when none
	int RushBoss();
	// in a rush whose boss is beaten, the next boss for the game's next stage change (and on to it)
	bool RushNextStage(const char*& name, int& point, int& room, int& layer);
}
