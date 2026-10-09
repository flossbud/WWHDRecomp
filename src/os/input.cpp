// Input (os.h, D18): padscore with one Pro Controller on KPAD channel 0, and vpad with no GamePad
// (single screen, D17).
//
// The controller's state comes from the input script when there is one (CEMU_INPUT_SCRIPT, the
// reference's frame-keyed format, tools/reference/routes) and otherwise from the frontend (input.h:
// SDL3 gamepad and keyboard). Every read writes what Cemu's did for its emulated Pro Controller
// under the reference's scripted input (cemu-patches/0007 and 0008), so route traces stay identical.
//
// Of the game's 19 input imports, the 10 Cemu implements are here. The other 9 (KPADSetMplsWorkarea,
// WENCGetEncodeData, WPADCanSendStreamData, WPADControlSpeaker, WPADDisconnect, WPADEnableURCC,
// WPADEnableWiiRemote, WPADSendStreamData, VPADBASEGetHeadphoneStatus) have no HLE entry to take
// over: Cemu's loader binds them to its "unsupported import" stub. The routes never call them; they
// become ours with the loader. Cemu's 5 ms padscore alarm (started by its coreinit) only calls
// connect and sampling callbacks, which this game never registers.
#include "os.h"
#include "input.h"
#include "debug_menu.h"
#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace wwhd::rt { uint32 GameFrame(uint32 swap); }   // runtime/dispatch.cpp

namespace wwhd::os::input
{
	namespace
	{
		// ---- the input script: "FRAME BUTTON[+BUTTON...] [DURATION]" per line, # comments ----
		struct Step
		{
			uint32 frame, duration, buttons;
			float lx, ly, rx, ry;
		};

		struct Script
		{
			bool on = false;
			std::vector<Step> steps;

			Script()
			{
				const char* path = getenv("CEMU_INPUT_SCRIPT");
				if (!path || !*path)
					return;
				std::ifstream f(path);
				if (!f)
				{
					cemuLog_log(LogType::Force, "wwhd input: cannot open the input script {}", path);
					return;
				}
				static const std::unordered_map<std::string, uint32> kButtons = {
					{ "A", A }, { "B", B }, { "X", X }, { "Y", Y }, { "L", L }, { "R", R }, { "ZL", ZL }, { "ZR", ZR },
					{ "PLUS", PLUS }, { "MINUS", MINUS }, { "HOME", HOME }, { "UP", UP }, { "DOWN", DOWN },
					{ "LEFT", LEFT }, { "RIGHT", RIGHT }, { "LCLICK", LCLICK }, { "RCLICK", RCLICK } };
				for (std::string line; std::getline(f, line);)
				{
					if (size_t hash = line.find('#'); hash != std::string::npos)
						line.resize(hash);
					std::istringstream ls(line);
					Step st{};
					std::string names;
					if (!(ls >> st.frame >> names))
						continue;
					if (!(ls >> st.duration))
						st.duration = 1;
					std::istringstream ns(names);
					for (std::string n; std::getline(ns, n, '+');)
					{
						if (auto it = kButtons.find(n); it != kButtons.end()) st.buttons |= it->second;
						else if (n == "LUP") st.ly = 1.0f;
						else if (n == "LDOWN") st.ly = -1.0f;
						else if (n == "LLEFT") st.lx = -1.0f;
						else if (n == "LRIGHT") st.lx = 1.0f;
						else if (n == "RUP") st.ry = 1.0f;
						else if (n == "RDOWN") st.ry = -1.0f;
						else if (n == "RLEFT") st.rx = -1.0f;
						else if (n == "RRIGHT") st.rx = 1.0f;
						// a stick at a position of its own: LS=x,y or RS=x,y (-1 to 1; tools/sixty/tests/movement.py)
						else if ((n.rfind("LS=", 0) == 0 || n.rfind("RS=", 0) == 0) && n.find(',') != std::string::npos)
						{
							float x = std::stof(n.substr(3)), y = std::stof(n.substr(n.find(',') + 1));
							if (n[0] == 'L') st.lx = x, st.ly = y;
							else st.rx = x, st.ry = y;
						}
						else if (!n.empty()) cemuLog_log(LogType::Force, "wwhd input: unknown button '{}' in the input script", n);
					}
					steps.push_back(st);
				}
				on = true;
				cemuLog_log(LogType::Force, "wwhd input: script of {} steps from {}", steps.size(), path);
			}

			// the steps covering this frame, added up
			Pad At(uint32 frame) const
			{
				Pad p;
				for (const Step& st : steps)
				{
					if (frame < st.frame || frame >= st.frame + st.duration)
						continue;
					p.buttons |= st.buttons;
					p.lx += st.lx; p.ly += st.ly; p.rx += st.rx; p.ry += st.ry;
				}
				return p;
			}
		};

		const Script& TheScript()
		{
			static const Script script;
			return script;
		}

		std::mutex s_liveLock;
		Pad s_live;
		std::function<void(bool)> s_rumble;

		Pad Current()
		{
			// at 60 fps (D21) the game's own frame runs on every other swap: scripts count game frames
			if (TheScript().on)
				return TheScript().At(wwhd::rt::GameFrame(SwapCount()));
			std::lock_guard lock(s_liveLock);
			return s_live;
		}

		// ---- padscore: KPAD channel 0 is a Pro Controller (URCC) ----
		constexpr uint32 kChannels = 4;
		constexpr uint8 kDevUrcc = 31;                      // WPADDeviceType
		constexpr uint8 kDevNone = 253;
		constexpr uint8 kFormatUrcc = 22;                   // WPADDataFormat
		constexpr sint32 kWpadOk = 0, kWpadNoController = -1;
		constexpr sint32 kKpadNoSample = -1, kKpadNoController = -2;
		constexpr uint32 kBatteryFull = 4;
		constexpr uint32 kMplsWorkSize = 0x5FE0;
		constexpr uint64 kSamplePeriod = 62156;             // 1 ms in timer ticks, as Cemu rounds it

		// KPADStatus (0xF0 bytes)
		constexpr uint32 kKpadSize = 0xF0, kKpadDevType = 0x5C, kKpadWpadErr = 0x5D, kKpadDataFormat = 0x5F,
			kUcHold = 0x60, kUcTrig = 0x64, kUcRelease = 0x68, kUcLStick = 0x6C, kUcRStick = 0x74, kUcCharge = 0x7C, kUcCable = 0x80;
		static_assert(sizeof(KPADStatus_t) == kKpadSize);
		static_assert(offsetof(KPADStatus_t, devType) == kKpadDevType && offsetof(KPADStatus_t, wpadErr) == kKpadWpadErr);
		static_assert(offsetof(KPADStatus_t, data_format) == kKpadDataFormat);
		static_assert(offsetof(KPADStatus_t, ex_status) + offsetof(decltype(KPADEXStatus_t::uc), hold) == kUcHold);
		static_assert(offsetof(KPADStatus_t, ex_status) + offsetof(decltype(KPADEXStatus_t::uc), lstick) == kUcLStick);
		static_assert(offsetof(KPADStatus_t, ex_status) + offsetof(decltype(KPADEXStatus_t::uc), cable) == kUcCable);

		// the script's buttons as the Pro Controller's
		constexpr std::pair<uint32, uint32> kProButtons[] = { { A, 0x10 }, { B, 0x40 }, { X, 0x8 }, { Y, 0x20 }, { L, 0x2000 },
			{ R, 0x200 }, { ZL, 0x80 }, { ZR, 0x4 }, { PLUS, 0x400 }, { MINUS, 0x1000 }, { HOME, 0x800 }, { UP, 0x1 },
			{ DOWN, 0x4000 }, { LEFT, 0x2 }, { RIGHT, 0x8000 }, { LCLICK, 0x20000 }, { RCLICK, 0x10000 } };

		uint64 s_lastRead[kChannels] = {};
		uint32 s_prevHold = 0;                              // channel 0's buttons at its last sample
		Pad s_lastPad;                                      // and the whole of it
		uint32 s_ringBuffer = 0, s_ringLength = 0;          // KPADInitEx's (only KPADGetUnifiedWpadStatus would use them)
		bool s_kpadInitialized = false;

		// ---- vpad: no GamePad ----
		constexpr sint32 kVpadNoController = -2;
		constexpr uint32 kVpadSize = 0xAC, kVpadErr = 0x50, kVpadTp = 0x52, kVpadTp1 = 0x5A, kVpadTp2 = 0x62, kTpValidity = 6,
			kVpadSlideVolume = 0xA0, kVpadBattery = 0xA1, kVpadSlideVolume2 = 0xA3;
		constexpr uint16 kTpInvalidXY = 3;
		static_assert(sizeof(VPADStatus) == kVpadSize && offsetof(VPADStatus, vpadErr) == kVpadErr);
		static_assert(offsetof(VPADStatus, tpData) == kVpadTp && offsetof(VPADStatus, tpProcessed1) == kVpadTp1);
		static_assert(offsetof(VPADStatus, tpProcessed2) == kVpadTp2 && offsetof(VPADTPData_t, validity) == kTpValidity);
		static_assert(offsetof(VPADStatus, slideVolume) == kVpadSlideVolume && offsetof(VPADStatus, batteryLevel) == kVpadBattery);
		static_assert(offsetof(VPADStatus, slideVolume2) == kVpadSlideVolume2);
	}

	void SetLive(const Pad& pad)
	{
		std::lock_guard lock(s_liveLock);
		s_live = pad;
	}

	void SetRumble(std::function<void(bool)> rumble) { s_rumble = std::move(rumble); }

	Pad LastRead() { return s_lastPad; }
}

using namespace wwhd::os;
using namespace wwhd::os::input;

namespace
{
	// WWHD_INPUT_CALLERS=path (a probe): at exit, which guest threads called KPADReadEx and VPADRead, how often, and
	// how many samples they got
	void NoteCaller(const char* fn, bool sample)
	{
		static const char* path = getenv("WWHD_INPUT_CALLERS");
		if (!path)
			return;
		static std::mutex lock;
		static std::map<std::string, std::pair<uint64, uint64>> counts;
		static bool once = [] {
			static void (*write)() = [] {
				if (FILE* f = fopen(path, "w"))
				{
					for (auto& [k, v] : counts)
						fprintf(f, "%s %llu calls %llu samples\n", k.c_str(), (unsigned long long)v.first, (unsigned long long)v.second);
					fclose(f);
				}
			};
			atexit(write);
			at_quick_exit(write);
			return true;
		}();
		(void)once;
		OSThread_t* t = coreinit::OSGetCurrentThread();
		const char* name = t ? t->threadName.GetPtr() : nullptr;
		std::lock_guard guard(lock);
		auto& c = counts[std::string(fn) + " " + (name ? name : "?")];
		c.first++;
		c.second += sample;
	}
}

// sint32 KPADReadEx(sint32 channel, KPADStatus* samples, uint32 length, sint32* error): at most one
// sample, and none within 1 ms of the last
WWHD_OS_FUNCTION(padscore, KPADReadEx)
{
	uint32 channel = Arg(ctx, 0), status = Arg(ctx, 1), length = Arg(ctx, 2), error = Arg(ctx, 3);
	auto fail = [&](sint32 code) {
		if (error)
			Write32(error, (uint32)code);
		Return(ctx, 0);
	};
	if (channel >= kChannels)
		return Return(ctx, 0);
	if (channel != 0)
		return fail(kKpadNoController);
	uint64 now = Timebase() + TimebaseAt2000();
	if (length == 0 || now - s_lastRead[channel] < kSamplePeriod)
	{
		NoteCaller("KPADReadEx", false);
		return fail(kKpadNoSample);
	}
	NoteCaller("KPADReadEx", true);
	s_lastRead[channel] = now;

	Pad pad = debug_menu::Filter(Current());          // the debug menu takes the pad while it is open
	s_lastPad = pad;
	uint32 hold = 0;
	for (auto [from, to] : kProButtons)
		if (pad.buttons & from)
			hold |= to;
	memset(Guest(status), 0, kKpadSize);
	Write8(status + kKpadWpadErr, (uint8)kWpadOk);
	Write8(status + kKpadDataFormat, kFormatUrcc);
	Write8(status + kKpadDevType, kDevUrcc);
	Write32(status + kUcHold, hold);
	Write32(status + kUcTrig, hold & ~s_prevHold);
	Write32(status + kUcRelease, s_prevHold & ~hold);
	s_prevHold = hold;
	WriteFloat(status + kUcLStick, pad.lx);
	WriteFloat(status + kUcLStick + 4, pad.ly);
	WriteFloat(status + kUcRStick, pad.rx);
	WriteFloat(status + kUcRStick + 4, pad.ry);
	Write32(status + kUcCharge, 0);
	Write32(status + kUcCable, 1);
	if (error)
		Write32(error, 0);
	Return(ctx, 1);
}

// void KPADInitEx(KPADUnifiedWpadStatus* ring, uint32 length)
WWHD_OS_FUNCTION(padscore, KPADInitEx)
{
	if (!s_kpadInitialized)
	{
		s_ringBuffer = Arg(ctx, 0);
		s_ringLength = Arg(ctx, 1);
		s_kpadInitialized = true;
	}
	Return(ctx, 0);
}

// uint32 KPADGetMplsWorkSize(void)
WWHD_OS_FUNCTION(padscore, KPADGetMplsWorkSize) { Return(ctx, kMplsWorkSize); }

// sint32 WPADProbe(sint32 channel, uint32* type)
WWHD_OS_FUNCTION(padscore, WPADProbe)
{
	uint32 channel = Arg(ctx, 0), type = Arg(ctx, 1);
	bool connected = channel == 0;
	if (type)
		Write32(type, connected ? kDevUrcc : kDevNone);
	Return(ctx, (uint32)(connected ? kWpadOk : kWpadNoController));
}

// WPADBatteryLevel WPADGetBatteryLevel(sint32 channel)
WWHD_OS_FUNCTION(padscore, WPADGetBatteryLevel) { Return(ctx, kBatteryFull); }

// void WPADControlMotor(sint32 channel, uint32 command): 1 starts the rumble, anything else stops it
WWHD_OS_FUNCTION(padscore, WPADControlMotor)
{
	if (Arg(ctx, 0) == 0 && s_rumble)
		s_rumble(Arg(ctx, 1) == 1);
	Return(ctx, 0);
}

// sint32 VPADRead(sint32 channel, VPADStatus* samples, uint32 length, sint32* error): no GamePad.
// The sample says so too, with the GamePad's volume slider at 0 (the reference's setting).
WWHD_OS_FUNCTION(vpad, VPADRead)
{
	uint32 status = Arg(ctx, 1), length = Arg(ctx, 2), error = Arg(ctx, 3);
	memset(Guest(status), 0, kVpadSize);
	Write8(status + kVpadBattery, 0xC0);
	Write16(status + kVpadTp + kTpValidity, kTpInvalidXY);
	Write16(status + kVpadTp1 + kTpValidity, kTpInvalidXY);
	Write16(status + kVpadTp2 + kTpValidity, kTpInvalidXY);
	if (error)
		Write32(error, (uint32)kVpadNoController);
	if (length > 0)
		Write8(status + kVpadErr, 0xFF);
	Return(ctx, 0);
}

// sint32 VPADControlMotor(sint32 channel, uint8* pattern, uint8 length), void VPADStopMotor(sint32
// channel): no GamePad to rumble
WWHD_OS_FUNCTION(vpad, VPADControlMotor) { Return(ctx, 0); }
WWHD_OS_FUNCTION(vpad, VPADStopMotor) { Return(ctx, 0); }

// void VPADGetTPCalibratedPoint(sint32 channel, VPADTPData* display, const VPADTPData* raw): a raw
// touch point in 1280x720 screen coordinates
WWHD_OS_FUNCTION(vpad, VPADGetTPCalibratedPoint)
{
	uint32 display = Arg(ctx, 1), raw = Arg(ctx, 2);
	uint8 point[8];
	memcpy(point, Guest(raw), 8);
	memmove(Guest(display), point, 8);
	sint32 x = (sint16)Read16(display), y = (sint16)Read16(display + 2);
	x -= 92;
	y = (sint32)(4095.0 - y - 254);
	x = std::max(x, 0);
	y = std::max(y, 0);
	Write16(display, (uint16)(sint32)(((double)x / 3883.0) * 1280.0));
	Write16(display + 2, (uint16)(sint32)(((double)y / 3694.0) * 720.0));
	Return(ctx, 0);
}
