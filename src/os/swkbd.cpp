// swkbd, the software keyboard (os.h, D18): name entry.
//
// The game loads swkbd.rpl at runtime and finds its functions by name; 17 of the 35 it looks up are
// implemented by Cemu, and 16 of those are here (SwkbdInitLearnDic, which only formats a dictionary
// in the game's memory, stays Cemu's). The keyboard is the system's to draw: the frontend shows it
// and types into it (swkbd.h). With CEMU_SWKBD_AUTO=text the keyboard answers itself as the
// reference's does (cemu-patches/0009): the text is entered at the first SwkbdCalc after it appears
// and OK is pressed 30 calls later, so the name-entry route replays identically.
//
// What must match Cemu exactly, because the game can see it: the sleeps (1 s in SwkbdCreate, 5 ms
// per sub-thread calculation), the state's allocation in the system area (Cemu's layout: 24,784
// bytes, the returned string at +16,388), the text written into the game's receiver buffer, and the
// changeString callback with Cemu's parameter block.
#include "os.h"
#include "swkbd.h"

namespace wwhd::os
{
	uint32 SwkbdChangeStringParam();       // Cemu's swkbd parameter block for the callback (install.cpp)
}

namespace wwhd::os::swkbd
{
	namespace
	{
		constexpr uint32 kStateSize = 24784, kStateStringBE = 16388;     // Cemu's swkbdInternalState_t on Linux
		constexpr uint32 kMaxText = 4096 - 1;
		constexpr uint32 kHidden = 0, kDisplayed = 2;
		constexpr uint64 kTimerClock = 62156250;
		constexpr uint32 kKeyboardArgReceiver = 0xA8;    // SwkbdKeyboardArg's ReceiverArg
		constexpr uint32 kAppearArgInitialText = 0xC8, kAppearArgMaxLength = 0xD0;

		struct Receiver
		{
			uint32 eventReceiver = 0, stringBuf = 0;
			sint32 stringBufSize = 0;
		};

		struct State
		{
			uint32 guest = 0;                   // the allocation in the system area; 0 before SwkbdCreate
			std::u16string text;
			bool active = false, decided = false, keyboardOnly = false;
			sint32 maxTextLength = 0;
			Receiver receiver;
			int needFont = 0, needPredict = 0;
			sint32 autoCalls = -1;              // scripted: calls since the text was entered
			uint32 version = 0;
		} s;

		enum class Key { Type, Backspace, Confirm };
		struct Event { Key key; std::u16string chars; };
		std::mutex s_lock;                      // s.text, s.active, s.decided, s.version, s_events
		std::vector<Event> s_events;

		const char* AutoText()
		{
			static const char* text = getenv("CEMU_SWKBD_AUTO");
			return text;
		}

		Receiver ReadReceiver(uint32 arg)
		{
			return { Read32(arg), Read32(arg + 4), (sint32)Read32(arg + 8) };
		}

		// the text into the game's buffer, and its changeString callback
		void StringChanged()
		{
			uint32 size = (uint32)s.receiver.stringBufSize;
			if (size > 1)
			{
				size--;
				sint32 n = std::min((sint32)size, (sint32)s.text.size());
				for (sint32 i = 0; i < n; i++)
					Write16(s.receiver.stringBuf + i * 2, s.text[i]);
				Write16(s.receiver.stringBuf + n * 2, 0);
			}
			if (uint32 receiver = s.receiver.eventReceiver)
			{
				uint32 changeString = Read32(Read32(receiver) + 0xC);
				if (changeString)
				{
					uint32 param = SwkbdChangeStringParam();
					Write32(param, 0);
					Write32(param + 4, 0);
					QueueGuestCallback(changeString, receiver, param);
				}
			}
		}

		sint32 MaxLength()
		{
			if (!s.keyboardOnly)
				return s.maxTextLength;
			return s.receiver.stringBufSize > 1 ? s.receiver.stringBufSize - 1 : 0;
		}

		// the player's typing (live, not scripted), on the game's thread
		void ApplyEvents()
		{
			std::vector<Event> events;
			{
				std::lock_guard lock(s_lock);
				events.swap(s_events);
			}
			for (const Event& e : events)
			{
				if (!s.active || s.decided)
					break;
				std::unique_lock lock(s_lock);
				switch (e.key)
				{
				case Key::Type:
					for (char16_t c : e.chars)
						if (c >= 0x20 && c < 0x7F && (sint32)s.text.size() < MaxLength())
							s.text.push_back(c);
					break;
				case Key::Backspace:
					if (!s.text.empty())
						s.text.pop_back();
					break;
				case Key::Confirm:
					if (s.text.empty())
						s.text = u"Link";
					s.decided = true;
					break;
				}
				s.version++;
				lock.unlock();
				StringChanged();
			}
		}

		void Push(Event e)
		{
			std::lock_guard lock(s_lock);
			s_events.push_back(std::move(e));
		}
	}

	View Current()
	{
		std::lock_guard lock(s_lock);
		View v;
		v.open = s.active && !s.decided && !AutoText();
		v.text = s.text;
		v.maxLength = (uint32)std::max(0, MaxLength());
		v.version = s.version;
		return v;
	}

	void Type(std::u16string_view chars) { Push({ Key::Type, std::u16string(chars) }); }
	void Backspace() { Push({ Key::Backspace, {} }); }
	void Confirm() { Push({ Key::Confirm, {} }); }
}

namespace
{
	using namespace wwhd::os;
	using namespace wwhd::os::swkbd;

	void SetActive(bool active, bool keyboardOnly)
	{
		std::lock_guard lock(s_lock);
		s.text.clear();
		s.active = active;
		s.decided = false;
		s.keyboardOnly = keyboardOnly;
		s.version++;
		if (!AutoText())
			cemuLog_log(LogType::Force, "wwhd swkbd: the keyboard is up, waiting for the player");
	}
}

// void SwkbdCreate(uint8* work, RegionType region, uint32 unknown, FSClient* client)
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient", SwkbdCreate)
{
	if (!s.guest)
	{
		s.guest = AllocSystemArea(kStateSize, 4);
		memset(Guest(s.guest), 0, kStateSize);
	}
	SleepTicks(kTimerClock);          // Cemu's: the keyboard takes a while to set up (MH3U needs it)
	s.needFont = s.needPredict = 3;
	Return(ctx);
}

// State SwkbdGetStateKeyboard(void), State SwkbdGetStateInputForm(void)
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdGetStateKeyboard__3RplFv", SwkbdGetStateKeyboard) { Return(ctx, s.active ? kDisplayed : kHidden); }
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdGetStateInputForm__3RplFv", SwkbdGetStateInputForm) { Return(ctx, s.active ? kDisplayed : kHidden); }

// BOOL SwkbdSetReceiver(const ReceiverArg& arg)
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdSetReceiver__3RplFRCQ3_2nn5swkbd11ReceiverArg", SwkbdSetReceiver)
{
	if (s.guest)
		s.receiver = ReadReceiver(Arg(ctx, 0));
	Return(ctx, 0);
}

// BOOL SwkbdAppearInputForm(const AppearArg& arg): the text form, with its initial text
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdAppearInputForm__3RplFRCQ3_2nn5swkbd9AppearArg", SwkbdAppearInputForm)
{
	uint32 arg = Arg(ctx, 0);
	SetActive(true, false);
	sint32 max = (sint32)Read32(arg + kAppearArgMaxLength);
	s.maxTextLength = max <= 0 ? (sint32)kMaxText : std::min(max, (sint32)kMaxText);
	std::lock_guard lock(s_lock);
	if (uint32 initial = Read32(arg + kAppearArgInitialText))
		for (sint32 i = 0; i < s.maxTextLength; i++)
		{
			char16_t c = Read16(initial + i * 2);
			if (c == 0)
				break;
			s.text.push_back(c);
		}
	Return(ctx, 1);
}

// BOOL SwkbdAppearKeyboard(const KeyboardArg& arg): the keyboard alone, typing into the receiver
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdAppearKeyboard__3RplFRCQ3_2nn5swkbd11KeyboardArg", SwkbdAppearKeyboard)
{
	SetActive(true, true);
	s.receiver = ReadReceiver(Arg(ctx, 0) + kKeyboardArgReceiver);
	Return(ctx, 1);
}

// BOOL SwkbdDisappearInputForm(void), BOOL SwkbdDisappearKeyboard(void)
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdDisappearInputForm__3RplFv", SwkbdDisappearInputForm)
{
	std::lock_guard lock(s_lock);
	s.active = false;
	s.version++;
	Return(ctx, 1);
}
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdDisappearKeyboard__3RplFv", SwkbdDisappearKeyboard)
{
	std::lock_guard lock(s_lock);
	s.active = false;
	s.version++;
	Return(ctx, 1);
}

// const wchar16* SwkbdGetInputFormString(void): the text, in the state's buffer
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdGetInputFormString__3RplFv", SwkbdGetInputFormString)
{
	uint32 out = s.guest + kStateStringBE;
	std::lock_guard lock(s_lock);
	for (size_t i = 0; i < s.text.size(); i++)
		Write16(out + (uint32)i * 2, s.text[i]);
	Write16(out + (uint32)s.text.size() * 2, 0);
	Return(ctx, out);
}

// BOOL SwkbdIsDecideOkButton(bool* unused)
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdIsDecideOkButton__3RplFPb", SwkbdIsDecideOkButton) { Return(ctx, s.decided ? 1 : 0); }

// BOOL SwkbdGetDrawStringInfo(DrawStringInfo* info): nothing to draw
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdGetDrawStringInfo__3RplFPQ3_2nn5swkbd14DrawStringInfo", SwkbdGetDrawStringInfo)
{
	uint32 info = Arg(ctx, 0);
	for (uint32 i = 0; i < 6; i++)
		Write32(info + i * 4, 0xFFFFFFFF);
	Write8(info + 0x18, 0);
	Return(ctx, 0);
}

// the font and word-prediction work the system does on a sub-thread: three rounds of 5 ms each
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdIsNeedCalcSubThreadFont__3RplFv", SwkbdIsNeedCalcSubThreadFont) { Return(ctx, s.needFont > 0 ? 1 : 0); }
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdIsNeedCalcSubThreadPredict__3RplFv", SwkbdIsNeedCalcSubThreadPredict) { Return(ctx, s.needPredict > 0 ? 1 : 0); }
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdCalcSubThreadFont__3RplFv", SwkbdCalcSubThreadFont)
{
	if (s.needFont > 0)
	{
		s.needFont--;
		SleepTicks(kTimerClock / 200);
	}
	Return(ctx);
}
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdCalcSubThreadPredict__3RplFv", SwkbdCalcSubThreadPredict)
{
	if (s.needPredict > 0)
	{
		s.needPredict--;
		SleepTicks(kTimerClock / 200);
	}
	Return(ctx);
}

// void SwkbdCalc(const ControllerInfo& info): once a frame while the keyboard is up
WWHD_OS_FUNCTION_NAMED(swkbd, "SwkbdCalc__3RplFRCQ3_2nn5swkbd14ControllerInfo", SwkbdCalc)
{
	if (const char* text = AutoText())
	{
		if (s.guest && s.active && !s.decided)
		{
			if (s.autoCalls < 0)
			{
				{
					std::lock_guard lock(s_lock);
					s.text.clear();
					for (const char* c = text; *c && s.text.size() < kMaxText; c++)
						s.text.push_back((char16_t)(uint8)*c);
					s.version++;
				}
				StringChanged();
				s.autoCalls = 0;
				cemuLog_log(LogType::Force, "wwhd swkbd: entered \"{}\" (CEMU_SWKBD_AUTO)", text);
			}
			else if (++s.autoCalls >= 30)
			{
				std::lock_guard lock(s_lock);
				s.decided = true;
				s.version++;
				s.autoCalls = -1;
				cemuLog_log(LogType::Force, "wwhd swkbd: OK (CEMU_SWKBD_AUTO)");
			}
		}
	}
	else if (s.guest)
		ApplyEvents();
	Return(ctx);
}
