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
	bool OpenAAudio();              // Android: the TV's sound straight on AAudio, OpenAudio's first choice there (audio_aaudio.cpp)
	void AudioBackground(bool on);  // Android: the app in the background: AAudio's stream paused (audio_aaudio.cpp)
	// Android: a GPU driver package (zip: meta.json and its library) from a path or content:// URI into the app's
	// drivers; name: its own, id: its folder (Settings' value), why: what's wrong when it can't (gpu_drivers.cpp)
	bool ImportGpuDriver(const std::string& source, std::string& name, std::string& id, std::string& why);
	namespace android
	{
		// Android: the activity again, from a second process that ends this one (WindSongActivity.restart; Change
		// Files' restart, t-startscreen S3); false when it couldn't be asked (android_glue.cpp)
		bool RestartActivity();
		// the name a content:// URI's document goes by (the system picker's choice), "" unknown (android_glue.cpp)
		std::string DisplayName(const std::string& uri);
	}
	void UpdateOverlay();           // the system's keyboard and dialogs over the game (overlay.cpp, wwhd-null)
	void UpdateFpsOverlay();        // the settings' FPS overlay (overlay.cpp)
	// the debug menu's item at a point of the 1920x1080 TV frame: its index, -1 on the panel but no
	// item, -2 outside the panel (overlay.cpp)
	int DebugMenuItemAt(float tvx, float tvy);
	void ShowPreparing(uint32 done, uint32 total);   // the shader cache's progress screen (overlay.cpp, wwhd-null)
}
