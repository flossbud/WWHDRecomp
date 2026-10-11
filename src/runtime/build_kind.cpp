// Which form of the recompiled program this binary holds (src/CMakeLists.txt, wwhd_recomp_lib): wwhd-null's counts every
// block's cycles exactly (the virtual clock, every check); wwhd-play's (WWHD_RT_LOOSE, tools/recomp/runtime/ppc_ops.h)
// yields only at function entries and loop headers, for real time only. src/runtime/espresso/PPCTimer.cpp refuses the
// virtual clock in wwhd-play.
#ifdef WWHD_RT_LOOSE
extern const bool g_rtLooseBuild = true;
#else
extern const bool g_rtLooseBuild = false;
#endif

// A profiling build (tools/recomp/pgo.sh: -fprofile-generate) writes its profile at exit, but the runs end in
// quick_exit (WWHD_EXIT_FRAME), which skips that: write it there too. Weak: nothing in any other build
#include <cstdlib>
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));
static const int s_profileAtQuickExit = [] {
	if (__llvm_profile_write_file)
		std::at_quick_exit([] { __llvm_profile_write_file(); });
	return 0;
}();
