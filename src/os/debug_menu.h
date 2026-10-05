// The debug menu (the owner's idea: the test aids from a controller): a panel over the game that
// warps to islands, dungeons and boss rooms, with the bosses' refights on. Clicking both sticks at once
// opens and closes it; while it is open the D-pad (or the left stick) moves, A chooses, B goes back,
// and the game sees no input. debug_menu.cpp; the overlay (src/frontend/overlay.cpp) draws it.
#pragma once
#include "input.h"
#include <string>
#include <vector>

namespace wwhd::os::debug_menu
{
	// The pad the game reads, after the menu has taken what it needs (KPADReadEx, input.cpp).
	input::Pad Filter(const input::Pad& pad);

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
	void SetBossRefight(bool on);
	bool BossRefight();
}
