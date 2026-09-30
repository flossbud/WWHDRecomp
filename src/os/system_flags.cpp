// System settings a game reads or toggles and nothing else consults (os.h, D18): the HOME button
// menu, the screen dimming, the debugger. The state lives here now.
#include "os.h"

namespace
{
	using namespace wwhd::os;

	bool homeButtonMenuEnabled = true;
	bool dimEnabled = true;

	constexpr uint32 kImErrorNone = 0;
}

// BOOL OSEnableHomeButtonMenu(BOOL enable)
WWHD_OS_FUNCTION(coreinit, OSEnableHomeButtonMenu)
{
	homeButtonMenuEnabled = Arg(ctx, 0) != 0;
	Return(ctx, 1);
}

// BOOL OSIsHomeButtonMenuEnabled(void)
WWHD_OS_FUNCTION(coreinit, OSIsHomeButtonMenuEnabled) { Return(ctx, homeButtonMenuEnabled ? 1 : 0); }

// IMError IMEnableDim(void)
WWHD_OS_FUNCTION(coreinit, IMEnableDim)
{
	dimEnabled = true;
	Return(ctx, kImErrorNone);
}

// IMError IMIsDimEnabled(uint32* enabled)
WWHD_OS_FUNCTION(coreinit, IMIsDimEnabled)
{
	Write32(Arg(ctx, 0), dimEnabled ? 1 : 0);
	Return(ctx, kImErrorNone);
}

// BOOL OSIsDebuggerInitialized(void)
WWHD_OS_FUNCTION(coreinit, OSIsDebuggerInitialized) { Return(ctx, 0); }
