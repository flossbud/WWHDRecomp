// Which form of the recompiled program this binary holds (src/CMakeLists.txt, wwhd_recomp_lib): wwhd-null's counts every
// block's cycles exactly (the virtual clock, every check); wwhd-play's (WWHD_RT_LOOSE, tools/recomp/runtime/ppc_ops.h)
// yields only at function entries and loop headers, for real time only. src/runtime/espresso/PPCTimer.cpp refuses the
// virtual clock in wwhd-play.
#ifdef WWHD_RT_LOOSE
extern const bool g_rtLooseBuild = true;
#else
extern const bool g_rtLooseBuild = false;
#endif
