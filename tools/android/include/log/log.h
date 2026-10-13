// tools/android/include/log/log.h: a stand-in for the Android platform's internal liblog header (not in the NDK),
// which Cemu's ih264d (from Android's media code) includes on __ANDROID__ for one error log. The NDK's log only.
#pragma once
#include <android/log.h>
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ih264d", __VA_ARGS__)
static inline int android_errorWriteLog(int tag, const char* subTag) { (void)tag; (void)subTag; return 0; }
