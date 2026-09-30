// The software keyboard (os.h, D18): the frontend's side of our swkbd (swkbd.cpp). The system draws
// the keyboard, not the game, so the frontend shows it (an overlay) and types into it.
#pragma once
#include "Common/precompiled.h"

namespace wwhd::os::swkbd
{
	struct View
	{
		bool open = false;         // waiting for the player (not scripted, OK not yet pressed)
		std::u16string text;
		uint32 maxLength = 0;
		uint32 version = 0;        // changes whenever anything above does
	};
	View Current();

	// The player's typing, applied at the game's next SwkbdCalc. Characters outside printable ASCII
	// are dropped; OK with no text enters "Link".
	void Type(std::u16string_view chars);
	void Backspace();
	void Confirm();
}
