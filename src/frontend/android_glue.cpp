// The Android app's glue (m-android A2), built only there.
#define SDL_MAIN_HANDLED        // as Cemu's main.cpp: SDL_main.h renames nothing here
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_hints.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// Cemu's main() calls Xlib's XInitThreads on every Linux, Android too (main.cpp, BOOST_OS_LINUX): there's no X there
// and nothing else of Xlib's is called (tools/android/cmake/FindX11.cmake), so it's a no-op that reports success.
extern "C" int XInitThreads(void)
{
	return 1;
}

// stdout and stderr go nowhere in an app: a pipe in their place, read line by line into logcat (tag wwhd). At exit
// they're closed and the reader joined, so a Fatal's last line gets out before the process ends.
static int s_logPipe[2] = {-1, -1};
static pthread_t s_logThread;

static void* LogReader(void*)
{
	char buf[1024];
	size_t n = 0;
	for (;;)
	{
		ssize_t r = read(s_logPipe[0], buf + n, sizeof(buf) - 1 - n);
		if (r <= 0)
			break;
		n += r;
		size_t start = 0;
		for (size_t i = 0; i < n; i++)
			if (buf[i] == '\n')
			{
				buf[i] = 0;
				__android_log_write(ANDROID_LOG_INFO, "wwhd", buf + start);
				start = i + 1;
			}
		if (start == 0 && n == sizeof(buf) - 1)   // a line longer than the buffer: in pieces
			start = n, buf[n] = 0, __android_log_write(ANDROID_LOG_INFO, "wwhd", buf);
		memmove(buf, buf + start, n - start);
		n -= start;
	}
	if (n)
		buf[n] = 0, __android_log_write(ANDROID_LOG_INFO, "wwhd", buf);
	return nullptr;
}

static void LogToLogcat()
{
	if (pipe(s_logPipe) != 0)
		return;
	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	dup2(s_logPipe[1], 1);
	dup2(s_logPipe[1], 2);
	close(s_logPipe[1]);
	if (pthread_create(&s_logThread, nullptr, LogReader, nullptr) != 0)
		return;
	atexit([] {
		fflush(stdout);
		close(1);
		close(2);
		pthread_join(s_logThread, nullptr);
	});
}

// SDL's Java side calls SDL_main (it looks the name up in libmain.so). Cemu's main.cpp defines SDL_MAIN_HANDLED, so
// its main() keeps its name: SDL_main is ours, and calls it from the app's files directory (play.sh's cd; an app
// starts in /).
int main(int argc, char* argv[]);   // Cemu's main.cpp

extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char* argv[])
{
	SDL_SetMainReady();
	LogToLogcat();
	// landscape only, as the manifest says: SDL sets the activity's orientation again when the window opens, from this
	// hint, and without it from the window (resizable: any orientation, which put the game in portrait)
	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
	Dl_info self{};   // where the library is loaded: crash backtraces print addresses only (symbols are local)
	if (dladdr(reinterpret_cast<void*>(&SDL_main), &self))
		__android_log_print(ANDROID_LOG_INFO, "wwhd", "libmain.so at %p", self.dli_fbase);
	if (const char* home = getenv("WWHD_HOME"); home && *home)
		chdir(home);
	return main(argc, argv);
}
