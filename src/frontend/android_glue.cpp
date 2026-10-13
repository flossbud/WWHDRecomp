// The Android app's glue (m-android A2), built only there.
#define SDL_MAIN_HANDLED        // as Cemu's main.cpp: SDL_main.h renames nothing here
#include <SDL3/SDL_main.h>
#include <cstdlib>
#include <unistd.h>

// Cemu's main() calls Xlib's XInitThreads on every Linux, Android too (main.cpp, BOOST_OS_LINUX): there's no X there
// and nothing else of Xlib's is called (tools/android/cmake/FindX11.cmake), so it's a no-op that reports success.
extern "C" int XInitThreads(void)
{
	return 1;
}

// SDL's Java side calls SDL_main (it looks the name up in libmain.so). Cemu's main.cpp defines SDL_MAIN_HANDLED, so
// its main() keeps its name: SDL_main is ours, and calls it from the app's files directory (play.sh's cd; an app
// starts in /).
int main(int argc, char* argv[]);   // Cemu's main.cpp

extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char* argv[])
{
	SDL_SetMainReady();
	if (const char* home = getenv("WWHD_HOME"); home && *home)
		chdir(home);
	return main(argc, argv);
}
