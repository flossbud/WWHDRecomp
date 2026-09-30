// gx2: stream-out (docs/recompiler-design.md D18). Derived from Cemu's
// src/Cafe/OS/libs/gx2/GX2_Streamout.cpp (Mozilla Public License 2.0) by tools/gx2_port.py: our namespace
// and registration. Commands go through gx2's command pipe (GX2_Command.h) and must stay byte for byte
// what Cemu's sent (WWHD_GPU_STREAM, tools/reference/stream_check.sh).
#include "Cafe/OS/libs/gx2/GX2_Streamout.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "ported.h"
#include "../os.h"

namespace wwhd::gx2
{
	using namespace ::GX2;

	void GX2SetStreamOutBuffer(uint32 bufferIndex, GX2StreamOutBuffer* streamOutBuffer)
	{
		if (bufferIndex >= GX2_MAX_STREAMOUT_BUFFERS)
		{
			cemu_assert_suspicious();
			debug_printf("wwhd::gx2::GX2SetStreamOutBuffer(): Set out-of-bounds buffer\n");
			return;
		}

		MPTR bufferAddr;
		uint32 bufferSize;
		if (streamOutBuffer->dataPtr.IsNull())
		{
			bufferAddr = streamOutBuffer->rBuffer.GetVirtualAddr();
			bufferSize = streamOutBuffer->rBuffer.GetSize();
		}
		else
		{
			bufferAddr = streamOutBuffer->dataPtr.GetMPTR();
			bufferSize = streamOutBuffer->size;
		}

		GX2ReserveCmdSpace(3 + 3);
		// set buffer size
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1));
		gx2WriteGather_submitU32AsBE((mmVGT_STRMOUT_BUFFER_SIZE_0 + bufferIndex * 4) - 0xA000);
		gx2WriteGather_submitU32AsBE((bufferSize >> 2));
		// set buffer base
		uint32 physMem = memory_virtualToPhysical(bufferAddr);
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1));
		gx2WriteGather_submitU32AsBE((mmVGT_STRMOUT_BUFFER_BASE_0 + bufferIndex * 4) - 0xA000);
		gx2WriteGather_submitU32AsBE((physMem >> 8));
		// todo: Research and send IT_STRMOUT_BASE_UPDATE (0x72)
		// note: Other stream out registers maybe set in wwhd::gx2::GX2SetVertexShader() or GX2SetGeometryShader()
	}

	void GX2SetStreamOutEnable(uint32 enable)
	{
		cemu_assert_debug(enable == 0 || enable == 1);
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1));
		gx2WriteGather_submitU32AsBE(mmVGT_STRMOUT_EN - 0xA000);
		gx2WriteGather_submitU32AsBE(enable & 1);
	}

	void GX2SetStreamOutContext(uint32 bufferIndex, GX2StreamOutBuffer* streamOutBuffer, uint32 mode)
	{
		if (bufferIndex >= GX2_MAX_STREAMOUT_BUFFERS)
		{
			cemu_assert_suspicious();
			debug_printf("wwhd::gx2::GX2SetStreamOutContext(): Set out-of-bounds buffer\n");
			return;
		}

		GX2ReserveCmdSpace(6);
		if (mode == 0)
		{
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_STRMOUT_BUFFER_UPDATE, 5));
			gx2WriteGather_submitU32AsBE((2 << 1) | (bufferIndex << 8));
			gx2WriteGather_submitU32AsBE(MPTR_NULL);
			gx2WriteGather_submitU32AsBE(0);
			gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(streamOutBuffer->ctxPtr.GetMPTR()));
			gx2WriteGather_submitU32AsBE(0);
		}
		else if (mode == 1)
		{
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_STRMOUT_BUFFER_UPDATE, 5));
			gx2WriteGather_submitU32AsBE((0 << 1) | (bufferIndex << 8));
			gx2WriteGather_submitU32AsBE(MPTR_NULL);
			gx2WriteGather_submitU32AsBE(0);
			gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(streamOutBuffer->ctxPtr.GetMPTR()));
			gx2WriteGather_submitU32AsBE(0);
		}
		else
		{
			cemu_assert_unimplemented();
		}
	}

	void GX2SaveStreamOutContext(uint32 bufferIndex, GX2StreamOutBuffer* streamOutBuffer)
	{
		if (bufferIndex >= GX2_MAX_STREAMOUT_BUFFERS)
		{
			cemu_assert_suspicious();
			debug_printf("wwhd::gx2::GX2SaveStreamOutContext(): Set out-of-bounds buffer\n");
			return;
		}
		GX2ReserveCmdSpace(6);
		
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_STRMOUT_BUFFER_UPDATE, 5));
		gx2WriteGather_submitU32AsBE(1 | (3 << 1) | (bufferIndex << 8));
		gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(streamOutBuffer->ctxPtr.GetMPTR()));
		gx2WriteGather_submitU32AsBE(0);
		gx2WriteGather_submitU32AsBE(MPTR_NULL);
		gx2WriteGather_submitU32AsBE(0);
	}
}

WWHD_OS_EXPORT(gx2, GX2SetStreamOutBuffer, wwhd::gx2::GX2SetStreamOutBuffer);
WWHD_OS_EXPORT(gx2, GX2SetStreamOutEnable, wwhd::gx2::GX2SetStreamOutEnable);
WWHD_OS_EXPORT(gx2, GX2SetStreamOutContext, wwhd::gx2::GX2SetStreamOutContext);
WWHD_OS_EXPORT(gx2, GX2SaveStreamOutContext, wwhd::gx2::GX2SaveStreamOutContext);
