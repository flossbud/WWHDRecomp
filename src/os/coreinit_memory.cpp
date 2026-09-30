// coreinit: memory and data-cache operations (os.h, D18). Guest memory is flat (memory_base + address)
// and coherent, so cache maintenance does nothing beyond what the game can observe: DCZeroRange
// writes zeros. Cemu's handlers additionally told its GPU buffer cache about flushed ranges; the
// renderer reads guest memory at every draw and needs no such notice.
#include "os.h"

namespace
{
	using namespace wwhd::os;

	// the 32-byte cache-line range a cache operation covers (DCZeroRange writes all of it)
	struct Lines { uint32 start, bytes; };
	Lines CacheLines(uint32 addr, uint32 size)
	{
		uint32 start = addr & ~31u;
		return { start, ((addr & 31) + size + 31) / 32 * 32 };
	}
}

// void* memcpy(void* dst, const void* src, uint32 size)
WWHD_OS_FUNCTION(coreinit, memcpy)
{
	uint32 dst = Arg(ctx, 0), src = Arg(ctx, 1), size = Arg(ctx, 2);
	if (size)
		memcpy(Guest(dst), Guest(src), size);
	Return(ctx, dst);
}

// void* memmove(void* dst, const void* src, uint32 size)
WWHD_OS_FUNCTION(coreinit, memmove)
{
	uint32 dst = Arg(ctx, 0), src = Arg(ctx, 1), size = Arg(ctx, 2);
	if (size)
		memmove(Guest(dst), Guest(src), size);
	Return(ctx, dst);
}

// void* memset(void* dst, int value, uint32 size)
WWHD_OS_FUNCTION(coreinit, memset)
{
	uint32 dst = Arg(ctx, 0), value = Arg(ctx, 1), size = Arg(ctx, 2);
	memset(Guest(dst), (int)value, size);
	Return(ctx, dst);
}

// void* OSBlockMove(void* dst, const void* src, uint32 size, BOOL flush)
WWHD_OS_FUNCTION(coreinit, OSBlockMove)
{
	uint32 dst = Arg(ctx, 0), src = Arg(ctx, 1), size = Arg(ctx, 2);
	if (size)
		memmove(Guest(dst), Guest(src), size);
	Return(ctx, dst);
}

// void* OSBlockSet(void* dst, int value, uint32 size)
WWHD_OS_FUNCTION(coreinit, OSBlockSet)
{
	uint32 dst = Arg(ctx, 0), value = Arg(ctx, 1), size = Arg(ctx, 2);
	memset(Guest(dst), (int)(value & 0xFF), size);
	Return(ctx, dst);
}

// void DC*Range(void* addr, uint32 size): nothing to do on coherent memory
WWHD_OS_FUNCTION(coreinit, DCFlushRange) { Return(ctx); }
WWHD_OS_FUNCTION(coreinit, DCFlushRangeNoSync) { Return(ctx); }
WWHD_OS_FUNCTION(coreinit, DCInvalidateRange) { Return(ctx); }
WWHD_OS_FUNCTION(coreinit, DCStoreRange) { Return(ctx); }
WWHD_OS_FUNCTION(coreinit, DCStoreRangeNoSync) { Return(ctx); }

// void DCZeroRange(void* addr, uint32 size): zeroes every cache line the range touches
WWHD_OS_FUNCTION(coreinit, DCZeroRange)
{
	Lines l = CacheLines(Arg(ctx, 0), Arg(ctx, 1));
	if (l.bytes)
		memset(Guest(l.start), 0, l.bytes);
	Return(ctx);
}

// BOOL OSIsAddressRangeDCValid(void* addr, uint32 size): inside the data-cacheable window
WWHD_OS_FUNCTION(coreinit, OSIsAddressRangeDCValid)
{
	uint32 start = Arg(ctx, 0), end = start + Arg(ctx, 1) - 1;
	auto inside = [](uint32 a) { return a >= 0xE8000000 && a < 0xEC000000; };
	Return(ctx, inside(start) && inside(end) ? 1 : 0);
}

// void OSMemoryBarrier(void)
WWHD_OS_FUNCTION(coreinit, OSMemoryBarrier) { Return(ctx); }

// int OSGetCoreId(void): the core running the caller (its UPIR)
WWHD_OS_FUNCTION(coreinit, OSGetCoreId) { Return(ctx, ctx->spr.UPIR); }
