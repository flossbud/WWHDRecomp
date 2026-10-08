// Boot helpers adapted from Cemu's wxWidgets frontend (src/gui/wxgui/CemuApp.cpp, MainWindow.cpp),
// minus the dialogs. This file is derived from Cemu and is under the Mozilla Public License 2.0.
#include "boot.h"
#include "../os/settings.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cemu/ncrypto/ncrypto.h"
#include <boost/algorithm/string.hpp>
#include <fstream>
#include <unistd.h>

namespace wwhd
{
	[[noreturn]] void Fatal(std::string_view msg)
	{
		cemuLog_log(LogType::Force, "wwhd: {}", msg);
		fprintf(stderr, "wwhd: %.*s\n", (int)msg.size(), msg.data());
		_exit(1);
	}

	// Always portable: user data, config and cache next to the executable (portable/), Cemu's
	// read-only data (resources/, gameProfiles/) from WWHD_CEMU_DATA or the executable's directory.
	void SetupPaths()
	{
		char buf[4096];
		ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (n <= 0)
			Fatal("cannot resolve /proc/self/exe");
		fs::path exe(std::string(buf, n));
		fs::path portable = exe.parent_path() / "portable";
		std::error_code ec;
		fs::create_directories(portable, ec);
		wwhd::os::settings::Load(portable.string());   // the player's settings, before anything reads a switch
		const char* data = getenv("WWHD_CEMU_DATA");
		fs::path dataPath = data && *data ? fs::path(data) : exe.parent_path();
		std::set<fs::path> failed;
		ActiveSettings::SetPaths(true, exe, portable, portable, portable, dataPath, failed);
		for (auto& p : failed)
			Fatal(fmt::format("cannot write to {}", _pathToUtf8(p)));
		fs::create_directories(ActiveSettings::GetConfigPath("controllerProfiles"), ec);
		fs::create_directories(ActiveSettings::GetUserDataPath("memorySearcher"), ec);
	}

	void LoadConfig()
	{
		GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
		std::error_code ec;
		if (fs::exists(ActiveSettings::GetConfigPath("settings.xml"), ec))
			GetConfigHandle().Load();
		else
			GetConfigHandle().Save();
		NetworkConfig::LoadOnce();
	}

	// CemuApp::CreateDefaultMLCFiles
	void CreateDefaultMLCFiles()
	{
		fs::path mlc = ActiveSettings::GetMlcPath();
		const fs::path directories[] = {
			mlc, mlc / "sys", mlc / "usr",
			mlc / "usr/title/00050000", mlc / "usr/title/0005000c", mlc / "usr/title/0005000e",
			mlc / "usr/save/00050010/1004a000/user/common/db",
			mlc / "usr/save/00050010/1004a100/user/common/db",
			mlc / "usr/save/00050010/1004a200/user/common/db",
			mlc / "sys/title/0005001b/1005c000/content",
		};
		std::error_code ec;
		for (auto& d : directories)
			if (!fs::exists(d, ec) && !fs::create_directories(d, ec))
				Fatal(fmt::format("cannot create {}", _pathToUtf8(d)));
		const fs::path langDir = mlc / "sys/title/0005001b/1005c000/content";
		if (!fs::exists(langDir / "language.txt", ec))
		{
			std::ofstream f(langDir / "language.txt");
			for (const char* lang : {"ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh"})
				f << fmt::format(R"("{}",)", lang) << std::endl;
		}
		if (!fs::exists(langDir / "country.txt", ec))
		{
			std::ofstream f(langDir / "country.txt");
			for (sint32 i = 0; i < NCrypto::GetCountryCount(); i++)
			{
				const char* code = NCrypto::GetCountryAsString(i);
				if (boost::iequals(code, "NN"))
					f << "NULL," << std::endl;
				else
					f << fmt::format(R"("{}",)", code) << std::endl;
			}
		}
	}

	// MainWindow::FileLoad, without the GUI bits
	void PrepareTitle(const fs::path& path)
	{
		TitleInfo title{path};
		if (title.IsValid())
		{
			CafeTitleList::AddTitleFromPath(path);
			TitleId base;
			if (!CafeTitleList::FindBaseTitleId(title.GetAppTitleId(), base))
				Fatal("cannot find the base title");
			auto r = CafeSystem::PrepareForegroundTitle(base);
			if (r != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
				Fatal(fmt::format("cannot prepare {} (status {})", _pathToUtf8(path), (int)r));
			return;
		}
		auto type = DetermineCafeSystemFileType(path);
		if (type != CafeTitleFileType::RPX && type != CafeTitleFileType::ELF)
			Fatal(fmt::format("{} is not a valid title", _pathToUtf8(path)));
		if (CafeSystem::PrepareForegroundTitleFromStandaloneRPX(path) != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			Fatal(fmt::format("cannot prepare {}", _pathToUtf8(path)));
	}
}
