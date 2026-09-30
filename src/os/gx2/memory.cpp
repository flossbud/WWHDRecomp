// gx2: the default allocator (docs/recompiler-design.md D18). Derived from Cemu's
// src/Cafe/OS/libs/gx2/GX2_Memory.cpp (Mozilla Public License 2.0) by tools/gx2_port.py: our namespace
// and registration. Commands go through gx2's command pipe (GX2_Command.h) and must stay byte for byte
// what Cemu's sent (WWHD_GPU_STREAM, tools/reference/stream_check.sh).
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/gx2/GX2_Resource.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "ported.h"
#include "../os.h"

namespace wwhd::gx2
{
	using namespace ::GX2;

// default GX2 allocator (not the same as the GX2R allocator, but GX2R uses this allocator by default)
MPTR gx2Mem_defaultAlloc = MPTR_NULL;
MPTR gx2Mem_defaultFree = MPTR_NULL;

void gx2Memory_GX2SetDefaultAllocator(MPTR defaultAllocFunc, MPTR defaulFreeFunc)
{
	gx2Mem_defaultAlloc = defaultAllocFunc;
	gx2Mem_defaultFree = defaulFreeFunc;
}

void _GX2DefaultAlloc_Alloc(PPCInterpreter_t* hCPU)
{
	// parameters:
	// r3	uint32	userParam
	// r4	uint32  size
	// r5	sint32	alignment
	hCPU->gpr[3] = hCPU->gpr[4];
	hCPU->gpr[4] = hCPU->gpr[5];
	hCPU->instructionPointer = gCoreinitData->MEMAllocFromDefaultHeapEx.GetMPTR();
}

void _GX2DefaultAlloc_Free(PPCInterpreter_t* hCPU)
{
	hCPU->gpr[3] = hCPU->gpr[4];
	hCPU->instructionPointer = gCoreinitData->MEMFreeToDefaultHeap.GetMPTR();
}

void gx2Export_GX2SetDefaultAllocator(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetDefaultAllocator(0x{:08x}, 0x{:08x})", hCPU->gpr[3], hCPU->gpr[4]);
	gx2Mem_defaultAlloc = hCPU->gpr[3];
	gx2Mem_defaultFree = hCPU->gpr[4];
	osLib_returnFromFunction(hCPU, 0);
}

void _GX2DefaultAllocR_Alloc(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2DefaultAllocate(0x{:08x}, 0x{:08x}, 0x{:08x})", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
	// parameters:
	// r3	uint32	userParam
	// r4	uint32  size
	// r5	sint32	alignment
	hCPU->instructionPointer = gx2Mem_defaultAlloc;
}

void _GX2DefaultAllocR_Free(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2DefaultFree(0x{:08x}, 0x{:08x})", hCPU->gpr[3], hCPU->gpr[4]);
	// parameters:
	// r3	uint32	userParam
	// r4	void*	mem
	hCPU->instructionPointer = gx2Mem_defaultFree;
}

	void GX2MEMAllocatorsInit()
	{
		// set default allocators (can be overwritten by GX2SetDefaultAllocator)
		gx2Mem_defaultAlloc = PPCInterpreter_makeCallableExportDepr(_GX2DefaultAlloc_Alloc);
		gx2Mem_defaultFree = PPCInterpreter_makeCallableExportDepr(_GX2DefaultAlloc_Free);
		// set resource default allocator
		wwhd::gx2::GX2RSetAllocator(PPCInterpreter_makeCallableExportDepr(_GX2DefaultAllocR_Alloc), PPCInterpreter_makeCallableExportDepr(_GX2DefaultAllocR_Free));
	}
}

static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2SetDefaultAllocator("gx2", "GX2SetDefaultAllocator", wwhd::gx2::gx2Export_GX2SetDefaultAllocator);
