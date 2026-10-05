// The debug menu (the owner's idea: the test aids from a controller): a panel over the game that
// warps to islands, dungeons and boss rooms (the bosses' refights on), runs a boss rush and spawns
// enemies ahead of Link. Clicking both sticks at once, or F1, opens and closes it; while it is open the D-pad (or the left
// stick) moves, A chooses, B goes back, and the game sees no input. debug_menu.cpp; the overlay
// (src/frontend/overlay.cpp) draws it.
#pragma once
#include "input.h"
#include <string>
#include <vector>

namespace wwhd::os::debug_menu
{
	// The pad the game reads, after the menu has taken what it needs (KPADReadEx, input.cpp).
	input::Pad Filter(const input::Pad& pad);
	// F1 on the keyboard (the owner's ask): opens or closes the menu as both sticks do; Escape closes
	// it (window_system.cpp). On the keyboard the arrows move, X (A) or Enter (+) chooses, Z (B) goes back.
	void Toggle();
	bool IsOpen();
	// The mouse while the menu is open (window_system.cpp; the overlay finds the item under it):
	// hovering an item selects it, a left click chooses it, a right click goes back, the wheel moves.
	void Hover(int item);
	void Click(int item);
	void Back();
	void Scroll(int steps);

	struct View
	{
		bool open = false;
		uint32 version = 0;                     // changes whenever what is shown changes
		std::string title;
		std::vector<std::string> items;
		int cursor = 0;
		std::string hint;
	};
	View Current();
}

namespace wwhd::debug
{
	// The test aids' runtime side (src/overrides/sixty.cpp): a stage change on the next game frame, as
	// WWHD_DEBUG_STAGE makes one, and the bosses' refights (WWHD_DEBUG_BOSS) on or off.
	void RequestStage(const char* name, int point, int room, int layer);
	// an actor of that process (parameters, its angle's x for some) 150 units ahead of Link, facing him
	void RequestSpawn(int process, uint32 param, uint32 anglex);
	void SetBossRefight(bool on);
	bool BossRefight();
	// the boss rush: the game set a boss's "beaten" bit (its dungeon item 3, f_025B9098)
	void RushBossBeaten();
	// in a rush whose boss is beaten, the next boss for the game's next stage change (and on to it)
	bool RushNextStage(const char*& name, int& point, int& room, int& layer);
}
