// erreula, the system's error dialogs (os.h, D18).
//
// The game loads erreula.rpl at runtime and finds its functions by name; Cemu implements 15 of the
// 20 it looks up, all here. A dialog fades in and out over 80 ms of OS time and waits for a button,
// which the frontend supplies (erreula.h). Results are as Cemu's, including its inverted
// ErrEulaIsDecideSelectLeft/RightButtonError, since the game sees them. Cemu's handlers also lock
// a mutex of its own in ErrEulaCreate that nothing else uses; ours don't.
#include "os.h"
#include "erreula.h"

namespace wwhd::os::erreula
{
	namespace
	{
		enum State : uint32 { Hidden = 0, Appearing = 1, Visible = 2, Disappearing = 3 };
		constexpr uint32 kNone = 0xFFFFFFFF, kLeft = 0, kRight = 1;   // button selections
		constexpr uint32 kFadeTicks = 80 * 62156250ull / 1000;        // 80 ms
		enum DialogType : uint32 { Code = 0, Text = 1, TextOneButton = 2, TextTwoButton = 3 };

		struct Instance
		{
			State state = Hidden;
			uint32 lastStateChange = 0;          // the OS time's low word, as Cemu keeps it
			sint32 resultCode = -1;
			uint32 selection = kNone;
			sint32 resultLeft = 0, resultRight = 0;
		};

		std::optional<Instance> s_instance;
		uint32 s_region = 0, s_lang = 0, s_fsClient = 0;
		bool s_homeNixSign = false;
		View s_view;                             // what the dialog shows, taken when it appears
		std::mutex s_lock;                       // s_view, s_choices
		std::vector<bool> s_choices;

		uint32 Now() { return (uint32)(Timebase() + TimebaseAt2000()); }

		void SetState(State state)
		{
			s_instance->state = state;
			s_instance->lastStateChange = Now();
			std::lock_guard lock(s_lock);
			s_view.dialog = state == Appearing || state == Visible;
			s_view.version++;
		}

		std::u16string ReadText(uint32 addr, const char16_t* fallback)
		{
			if (!addr)
				return fallback;
			std::u16string t;
			for (char16_t c; (c = Read16(addr)) != 0 && t.size() < 1024; addr += 2)
				t.push_back(c);
			return t;
		}

		uint32 ResultType(sint32 code)
		{
			if (code == -1) return 0;       // none
			if (code < 10) return 1;        // finish
			if (code >= 9999) return 2;     // next
			if (code == 40) return 4;       // password
			return 3;                       // jump
		}
	}

	View Current()
	{
		std::lock_guard lock(s_lock);
		View v = s_view;
		v.homeNixSign = s_homeNixSign;
		return v;
	}

	void Choose(bool right)
	{
		std::lock_guard lock(s_lock);
		s_choices.push_back(right);
	}
}

using namespace wwhd::os;
using namespace wwhd::os::erreula;

// void ErrEulaCreate(uint8* work, RegionType region, LangType lang, FSClient* client)
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient", ErrEulaCreate)
{
	s_region = Arg(ctx, 1);
	s_lang = Arg(ctx, 2);
	s_fsClient = Arg(ctx, 3);
	s_instance.emplace();
	SetState(Hidden);
	Return(ctx);
}

// void ErrEulaDestroy(void)
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaDestroy__3RplFv", ErrEulaDestroy)
{
	s_instance.reset();
	Return(ctx);
}

// void ErrEulaAppearError(const AppearArg& arg)
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg", ErrEulaAppearError)
{
	uint32 arg = Arg(ctx, 0);
	{
		std::lock_guard lock(s_lock);
		uint32 type = Read32(arg);
		s_view.errorCode = Read32(arg + 0x10);
		s_view.text = ReadText(Read32(arg + 0x18), type == Code ? u"" : u"Unknown Error");
		s_view.left = ReadText(Read32(arg + 0x1C), type == TextTwoButton ? u"Yes" : u"OK");
		s_view.right = type == TextTwoButton ? ReadText(Read32(arg + 0x20), u"No") : u"";
		s_choices.clear();
	}
	if (s_instance)
	{
		s_instance->selection = kNone;
		s_instance->resultCode = -1;
		s_instance->resultLeft = 0;
		s_instance->resultRight = 1;
		SetState(Appearing);
	}
	Return(ctx);
}

// void ErrEulaDisappearError(void): only a visible dialog fades out
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaDisappearError__3RplFv", ErrEulaDisappearError)
{
	if (s_instance && s_instance->state == Visible)
		SetState(Disappearing);
	Return(ctx);
}

// State ErrEulaGetStateErrorViewer(void)
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaGetStateErrorViewer__3RplFv", ErrEulaGetStateErrorViewer)
{
	Return(ctx, s_instance ? s_instance->state : Hidden);
}

// void ErrEulaCalc(const ControllerInfo& info): fades, and the player's answer
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo", ErrEulaCalc)
{
	if (s_instance)
	{
		Instance& in = *s_instance;
		if ((in.state == Appearing || in.state == Disappearing) && Now() - in.lastStateChange > kFadeTicks)
			SetState(in.state == Appearing ? Visible : Hidden);
		std::vector<bool> choices;
		{
			std::lock_guard lock(s_lock);
			choices.swap(s_choices);
		}
		for (bool right : choices)
			if (in.state != Hidden && in.selection == kNone)
			{
				in.selection = right ? kRight : kLeft;
				in.resultCode = right ? in.resultRight : in.resultLeft;
				std::lock_guard lock(s_lock);
				s_view.dialog = false;       // answered: the dialog no longer waits
				s_view.version++;
			}
	}
	Return(ctx);
}

// BOOL ErrEulaIsDecideSelectButtonError(void) and its Left/Right forms (inverted, as Cemu's are)
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaIsDecideSelectButtonError__3RplFv", ErrEulaIsDecideSelectButtonError)
{
	Return(ctx, s_instance && s_instance->selection != kNone ? 1 : 0);
}
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaIsDecideSelectLeftButtonError__3RplFv", ErrEulaIsDecideSelectLeftButtonError)
{
	Return(ctx, s_instance && s_instance->selection != kLeft ? 1 : 0);
}
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaIsDecideSelectRightButtonError__3RplFv", ErrEulaIsDecideSelectRightButtonError)
{
	Return(ctx, s_instance && s_instance->selection != kRight ? 1 : 0);
}

// sint32 ErrEulaGetResultCode(void), ResultType ErrEulaGetResultType(void)
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaGetResultCode__3RplFv", ErrEulaGetResultCode)
{
	Return(ctx, (uint32)(s_instance ? s_instance->resultCode : -1));
}
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaGetResultType__3RplFv", ErrEulaGetResultType)
{
	Return(ctx, s_instance ? ResultType(s_instance->resultCode) : 0);
}

// the "HOME Menu can't be used now" sign
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg", ErrEulaAppearHomeNixSign)
{
	s_homeNixSign = true;
	Return(ctx, 0);
}
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaDisappearHomeNixSign__3RplFv", ErrEulaDisappearHomeNixSign)
{
	s_homeNixSign = false;
	Return(ctx, 0);
}
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaIsAppearHomeNixSign__3RplFv", ErrEulaIsAppearHomeNixSign) { Return(ctx, s_homeNixSign ? 1 : 0); }

// void ErrEulaChangeLang(LangType lang)
WWHD_OS_FUNCTION_NAMED(erreula, "ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType", ErrEulaChangeLang)
{
	s_lang = Arg(ctx, 0);
	Return(ctx, 0);
}
