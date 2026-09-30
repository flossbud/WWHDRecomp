// Error dialogs (os.h, D18): the frontend's side of our erreula (erreula.cpp). The system draws them,
// not the game, so the frontend shows them (an overlay) and answers their buttons.
#pragma once
#include "Common/precompiled.h"

namespace wwhd::os::erreula
{
	struct View
	{
		bool dialog = false;              // an error is showing and waits for a button
		uint32 errorCode = 0;             // shown as XXX-YYYY when not 0
		std::u16string text, left, right; // right is empty for one button
		bool homeNixSign = false;         // "the HOME Menu can't be used now"
		uint32 version = 0;               // changes whenever anything above does
	};
	View Current();

	// The player's answer, applied at the game's next ErrEulaCalc.
	void Choose(bool right);
}
