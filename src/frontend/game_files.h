// The player's copy of the game (t-startscreen): its check (the title, the version, cking.rpx's hash) and its
// registration (portable/game.txt: the copy is read in place, its path and the check's outcome kept there).
#pragma once
#include <atomic>
#include <string>

namespace wwhd::game_files
{
	struct Copy
	{
		std::string path;           // as the player chose it (a .wua, an extracted folder, a .wud/.wux)
		bool ok = false;            // the game this port runs: The Wind Waker HD, USA, v0, its cking.rpx
		std::string region;         // "USA", "EUR", "JPN" (from meta.xml), "" unknown
		int version = -1;           // the title's version (app.xml)
		std::string why;            // when not ok: what's wrong, for the player
	};

	// checks a copy: the title ID and version (Cemu's TitleInfo), then cking.rpx's sha256 (orig/README.md's); progress
	// (0..1) through the three steps (reading the files to .35, verifying to .75, preparing to 1) as it goes. Cancel
	// (set by another thread) stops it with why "cancelled"
	// (from and span: where its 0..1 falls in the progress shown, when a copy comes first)
	Copy Check(const std::string& path, std::atomic<float>* progress = nullptr, std::atomic<bool>* cancel = nullptr,
		float from = 0, float span = 1);

	// Android (t-startscreen S3): the system's picker gives a content:// URI, which the game can't read in place, so
	// the choice is copied in first: to <dir>/game/wwhd.wua, the app's files (WWHD_HOME, the activity's -g). Elsewhere
	// only with WWHD_COPY_IN=DIR, a test's
	bool CopiesIn();
	// the copy taken in: a .wua's footer looked at first (anything else is turned away before 1.5 GB are copied), the
	// room for it, then copied to game/wwhd-new.wua (progress 0 to .35: the first step, "Copying files"), checked there
	// (.35 to 1). Verified: renamed over game/wwhd.wua (a copy there before stays until then), its path the result's;
	// else removed. Cancel stops it as Check's does
	Copy CopyIn(const std::string& chosen, std::atomic<float>* progress = nullptr, std::atomic<bool>* cancel = nullptr);

	// the registered copy: its path, and whether its check passed for the file as it is now (its size and time)
	bool Registered(Copy& out);
	void Register(const Copy& copy);   // after a check that passed
	std::string RegisteredPath();      // "" none

	// the footer's line ("The Wind Waker HD · USA · Verified") and the version line (the build's short hash and date)
	std::string Version();
}
