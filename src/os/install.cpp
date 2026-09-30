// Installing our OS functions over Cemu's HLE handlers (os.h, D18).
#include "os.h"
#include "Cafe/OS/common/OSCommon.h"
#include <map>

extern HLECALL s_ppcHleTable[];    // Cemu's HLE handler table (PPCInterpreterHLE.cpp), indexed by HLE id

namespace wwhd::os
{
	std::vector<Export>& Exports()
	{
		static std::vector<Export> exports;
		return exports;
	}

	void Install()
	{
		const char* mode = getenv("WWHD_OS");
		if (mode && strcmp(mode, "cemu") == 0)
		{
			cemuLog_log(LogType::Force, "wwhd os: WWHD_OS=cemu, every OS function stays Cemu's");
			return;
		}
		std::map<std::string, int> perLib;
		int installed = 0;
		for (const Export& e : Exports())
		{
			sint32 index = osLib_getFunctionIndex(e.lib, e.name);
			if (index < 0)
			{
				// Cemu has no handler under that name: the game can't import it through Cemu either
				cemuLog_log(LogType::Force, "wwhd os: {}.{} has no HLE entry to take over", e.lib, e.name);
				continue;
			}
			s_ppcHleTable[index] = e.fn;
			perLib[e.lib]++;
			installed++;
		}
		std::string summary;
		for (auto& [lib, n] : perLib)
			summary += fmt::format("{}{} {}", summary.empty() ? "" : ", ", lib, n);
		cemuLog_log(LogType::Force, "wwhd os: {} functions are ours ({})", installed, summary);
	}
}
