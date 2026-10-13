// The pause menu (t-pausemenu; the owner's design, assets/pause_menu, its README the spec): opened with Esc or F2, or
// the pad's SELECT (− on the Pro Controller, View on an Xbox pad, the Deck's Select: no longer the game's), it holds the game at the top of a whole tick (os/savestate.h RequestHold: the game parked,
// silent, reading no input) over its own blurred last frame, and draws itself in the window's pixels as the renderer's
// menu panel (never in captures). Its own thread animates it while it is up. The start screen (t-startscreen) comes up
// the same way over the title's first frame at boot (Launch: Start Game, Settings, Game Files, Quit; off under a route's
// input, WWHD_START_SCREEN=0|1). pause_menu.cpp.
#pragma once
#include "Common/precompiled.h"
#include "../os/input.h"

namespace wwhd::pause_menu
{
	bool Available();                       // a window and the renderer, and its assets found
	void Begin();                           // its thread started when Available (the window's loop calls it)
	bool Up();                              // shown, opening or closing: the game gets no input
	// the app to the background (Android: the screen off, another app) and back: in play the pause menu opens, as Esc
	// opens it; before play the game is held until it's back
	void Background(bool on);
	void Toggle();
	// a key press from the window (SDL keycode, true when repeated by the system): true when the menu took it
	bool Key(uint32 key, bool repeat);
	// the pad as the frontend reads it, every time it publishes input: the menu's own while it is up
	void Pad(const os::input::Pad& pad);
	// the pad's buttons the game mustn't see: all while the menu is up or the start screen is still to come, then those
	// held since it closed until released
	// (held: the buttons the game would see, keyboard and pad together; after Start Game all stay the menu's until none is)
	uint32 Swallowed(uint32 held);
	// the mouse in the window's pixels: button 0 a move, 1 the left button pressed, 2 the right
	void Mouse(float x, float y, int button);

	// the start screen (t-startscreen): the game files this run reads (the frontend's, before the title is prepared)
	void SetGame(const std::string& path);
	// S2 Setup before the game, when no copy is given or registered: its thread up over a sky (the window's loop presents
	// it, gpu PresentMenuOnly); SetupDone once a copy verified and Start Game was chosen, with its path
	void BeginSetup();
	bool SetupDone(std::string& path);
	// files dragged over the window (true when one enters, false when it leaves or drops) and dropped on it
	void Drag(bool over);
	void Drop(const std::string& path);
	// Browse asked for: the window's loop shows the system's picker (it must, on the main thread) and hands back what
	// was chosen ("" cancelled; failed: no picker, so the menu's own browser)
	bool TakeBrowse();
	bool BrowsingDriver();                  // the picker asked for is for a GPU driver package (zips), not game files
	void Chosen(const std::string& path, bool failed);
}
