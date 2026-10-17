// Which form of the recompiled program this binary holds (src/CMakeLists.txt, wwhd_recomp_lib): wwhd-null's counts every
// block's cycles exactly (the virtual clock, every check); wwhd-play's (WWHD_RT_LOOSE, tools/recomp/runtime/ppc_ops.h)
// yields only at function entries and loop headers, for real time only. src/runtime/espresso/PPCTimer.cpp refuses the
// virtual clock in wwhd-play.
#ifdef WWHD_RT_LOOSE
extern const bool g_rtLooseBuild = true;
#else
extern const bool g_rtLooseBuild = false;
#endif

// A profiling build (tools/recomp/pgo.sh, tools/android/pgo.sh generate: -fprofile-generate) writes its profile at exit,
// but the runs end in quick_exit (WWHD_EXIT_FRAME), which skips that: write it there too. WWHD_PROFILE_FILE names the
// file (LLVM_PROFILE_FILE's patterns, %p the process): the Android app's environment is set after the library loads, and
// its working directory isn't writable, so it's read at the write. Weak: nothing in any other build
#include <cstdlib>
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));
extern "C" void __llvm_profile_set_filename(const char*) __attribute__((weak));
static void WriteProfile()
{
	if (const char* e = getenv("WWHD_PROFILE_FILE"); e && *e && __llvm_profile_set_filename)
		__llvm_profile_set_filename(e);
	__llvm_profile_write_file();
}
static const int s_profileAtQuickExit = [] {
	if (__llvm_profile_write_file)
	{
		std::at_quick_exit(WriteProfile);
		std::atexit(WriteProfile);
	}
	return 0;
}();
