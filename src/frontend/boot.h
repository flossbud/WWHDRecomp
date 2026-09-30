// Startup steps shared by the frontend (see cemu_boot.cpp).
#pragma once

namespace wwhd
{
	[[noreturn]] void Fatal(std::string_view msg);
	void SetupPaths();              // portable layout next to the executable
	void LoadConfig();              // portable/settings.xml (created with defaults if missing)
	void CreateDefaultMLCFiles();   // the emulated NAND's base folders and files
	void PrepareTitle(const fs::path& path);
	bool OpenAudio();               // the TV's sound on SDL3 in place of Cemu's device (audio_sdl.cpp, wwhd-null)
	bool OpenAudioHash();           // WWHD_AUDIO_HASH=path: a device that hashes the sound instead (audio_sdl.cpp)
	void UpdateOverlay();           // the system's keyboard and dialogs over the game (overlay.cpp, wwhd-null)
}
