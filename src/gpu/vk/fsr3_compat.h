// fsr3_compat.h: what AMD's FidelityFX SDK v1.1.4 host code (src/third_party/fsr3, MIT) takes from the Windows CRT,
// for building it unchanged elsewhere; force-included into the vendored sources only (src/CMakeLists.txt). Keep it
// to these few names, so the vendored copy can be updated by copying files.
#pragma once
#include <cstddef>
#include <bit>
#include <cwchar>
#include <cstring>                                      // memset, memcpy (frame interpolation: Windows headers bring them)

#ifndef _WIN32
// the contexts' opaque storage: twice the SDK's, as wchar_t, which the private contexts hold names in, is 4 bytes here
// (the one change to the vendored files: ffx_types.h takes this if it's defined)
#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 256)
template<size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src)
{
	wcsncpy(dst, src, N - 1);
	dst[N - 1] = 0;
	return 0;
}
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#endif
