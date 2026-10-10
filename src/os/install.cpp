// Installing our OS functions over Cemu's HLE handlers (os.h, D18).
#include "os.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include <map>

extern HLECALL s_ppcHleTable[];    // Cemu's HLE handler table (PPCInterpreterHLE.cpp), indexed by HLE id
uint32 GX2_RefSwapCount();         // Cemu's gx2 (cemu-patches/0007)
void GX2_SkipSwap();
void GX2_SuppressNextSwap();
struct changeStringParam_t { uint32be beginIndex, endIndex; };
extern SysAllocator<changeStringParam_t> _changeStringParam;   // Cemu's swkbd.cpp: its slot in guest memory

namespace wwhd::os
{
	// Cemu's clock (the virtual clock in deterministic runs), scheduler and gx2 state, see os.h
	uint64 Timebase() { return PPCInterpreter_getMainCoreCycleCounter() / 20; }
	uint64 TimebaseAt2000() { return ppcCyclesSince2000TimerClock; }
	uint32 CurrentThread() { return MEMPTR<OSThread_t>(coreinit::OSGetCurrentThread()).GetMPTR(); }
	uint32 SwapCount() { return GX2_RefSwapCount(); }
	void SkipSwap() { GX2_SkipSwap(); }
	void SuppressNextSwap() { GX2_SuppressNextSwap(); }
	void SleepTicks(uint64 ticks) { coreinit::OSSleepTicks(ticks); }
	uint32 AllocSystemArea(uint32 size, uint32 alignment) { return coreinit_allocFromSysArea(size, alignment); }
	void QueueGuestCallback(uint32 fn, uint32 r3, uint32 r4) { coreinitAsyncCallback_add(fn, 2, r3, r4); }
	uint32 SwkbdChangeStringParam() { return _changeStringParam.GetMPTR(); }
	uint32 CurrentThreadStackEnd() { return coreinit::OSGetCurrentThread()->stackEnd.GetMPTR(); }

	std::vector<Export>& Exports()
	{
		static std::vector<Export> exports;
		return exports;
	}

	namespace
	{
		HLECALL s_cemuAcquire = nullptr;

		// Takes over every export whose handler Cemu has registered by now and isn't ours yet;
		// `first`: also says which ones wait for their library.
		void InstallRegistered(bool first)
		{
			std::map<std::string, int> perLib;
			int installed = 0, waiting = 0, total = 0;
			for (const Export& e : Exports())
			{
				sint32 index = osLib_getFunctionIndex(e.lib, e.name);
				if (index < 0)
				{
					waiting++;
					continue;
				}
				total++;
				if (s_ppcHleTable[index] == e.fn)
					continue;
				s_ppcHleTable[index] = e.fn;
				perLib[e.lib]++;
				installed++;
			}
			if (!installed && !first)
				return;
			std::string summary;
			for (auto& [lib, n] : perLib)
				summary += fmt::format("{}{} {}", summary.empty() ? "" : ", ", lib, n);
			cemuLog_log(LogType::Force, "wwhd os: {} more functions are ours ({}); {} in all, {} wait for their library to load",
				installed, summary, total, waiting);
		}

		// OSDynLoad_Acquire is Cemu's; afterwards the library it loaded has its handlers registered
		void Acquire(PPCInterpreter_t* ctx)
		{
			s_cemuAcquire(ctx);
			InstallRegistered(false);
		}
	}

	// Libraries the game loads itself (swkbd, erreula: OSDynLoad_Acquire) register their handlers
	// only then (Cemu's RPLMapped), so OSDynLoad_Acquire is wrapped to take those over as they appear.
	// The registration itself is Cemu's and unchanged, so handler indices, and traces, stay the
	// reference's.
	void Install()
	{
		const char* mode = getenv("WWHD_OS");
		if (mode && strcmp(mode, "cemu") == 0)
		{
			cemuLog_log(LogType::Force, "wwhd os: WWHD_OS=cemu, every OS function stays Cemu's");
			return;
		}
		InstallRegistered(true);
		sint32 acquire = osLib_getFunctionIndex("coreinit", "OSDynLoad_Acquire");
		if (acquire >= 0)
		{
			s_cemuAcquire = s_ppcHleTable[acquire];
			s_ppcHleTable[acquire] = Acquire;
		}
	}
}
