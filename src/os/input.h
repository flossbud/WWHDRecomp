// Input (os.h, D18): the frontend's side of our padscore and vpad (input.cpp).
#pragma once
#include "Common/precompiled.h"

namespace wwhd::os::input
{
	// A controller's state in the terms of the input script (tools/reference/routes): the Pro
	// Controller's buttons, and sticks from -1 to 1 with up and right positive.
	enum Button : uint32
	{
		A = 1u << 0, B = 1u << 1, X = 1u << 2, Y = 1u << 3, L = 1u << 4, R = 1u << 5, ZL = 1u << 6, ZR = 1u << 7,
		PLUS = 1u << 8, MINUS = 1u << 9, HOME = 1u << 10, UP = 1u << 11, DOWN = 1u << 12, LEFT = 1u << 13, RIGHT = 1u << 14,
		LCLICK = 1u << 15, RCLICK = 1u << 16,
	};
	struct Pad
	{
		uint32 buttons = 0;
		float lx = 0, ly = 0, rx = 0, ry = 0;
	};

	// What the player is pressing (the frontend's gamepad and keyboard). Ignored while an input
	// script plays.
	void SetLive(const Pad& pad);
	// Called when the game starts or stops the controller's rumble.
	void SetRumble(std::function<void(bool on)> rumble);
	// The controller as the game last read it (the flight recorder, overrides/sixty.cpp).
	Pad LastRead();
}
