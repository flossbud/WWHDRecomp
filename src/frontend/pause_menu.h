// The pause menu (t-pausemenu; the owner's design, assets/pause_menu, its README the spec): opened with Esc or F2, or
// the pad's SELECT (− on the Pro Controller, View on an Xbox pad, the Deck's Select: no longer the game's), it holds the game at the top of a whole tick (os/savestate.h RequestHold: the game parked,
// silent, reading no input) over its own blurred last frame, and draws itself in the window's pixels as the renderer's
// menu panel (never in captures). Its own thread animates it while it is up. pause_menu.cpp.
#pragma once
#include "Common/precompiled.h"
#include "../os/input.h"

namespace wwhd::pause_menu
{
	bool Available();                       // a window and the renderer, and its assets found
	bool Up();                              // shown, opening or closing: the game gets no input
	void Toggle();
	// a key press from the window (SDL keycode, true when repeated by the system): true when the menu took it
	bool Key(uint32 key, bool repeat);
	// the pad as the frontend reads it, every time it publishes input: the menu's own while it is up
	void Pad(const os::input::Pad& pad);
	// the pad's buttons the game mustn't see: all while the menu is up, then those held since it closed until released
	uint32 Swallowed();
	// the mouse in the window's pixels: button 0 a move, 1 the left button pressed, 2 the right
	void Mouse(float x, float y, int button);
}
