// Startup steps shared by the frontend (see cemu_boot.cpp).
#pragma once

namespace wwhd
{
	[[noreturn]] void Fatal(std::string_view msg);
	void SetupPaths();              // portable layout next to the executable
	void LoadConfig();              // portable/settings.xml (created with defaults if missing)
	void CreateDefaultMLCFiles();   // the emulated NAND's base folders and files
	void PrepareTitle(const fs::path& path);
}
