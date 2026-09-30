// gx2: scan buffers, TV and DRC setup, cache invalidation and other small functions
// (docs/recompiler-design.md D18). Derived from Cemu's src/Cafe/OS/libs/gx2/GX2.cpp and GX2_Misc.cpp
// (Mozilla Public License 2.0): the functions of those two files that need none of gx2's core (the
// command pool, GX2Init, swaps, GPU waits), which stays Cemu's for now. Commands go through gx2's
// command pipe and must stay byte for byte what Cemu's sent (tools/reference/stream_check.sh).
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "Cafe/OS/libs/gx2/GX2_Misc.h"
#include "Cafe/OS/libs/gx2/GX2_Surface.h"
#include "ported.h"
#include "../os.h"

void LatteBufferCache_notifyDCFlush(MPTR address, uint32 size);

namespace wwhd::gx2
{
	using namespace ::GX2;

	#define GX2_TV_RENDER_NONE			0
#define GX2_TV_RENDER_480			1
#define GX2_TV_RENDER_480_WIDE		2
#define GX2_TV_RENDER_720			3
#define GX2_TV_RENDER_720I			4
#define GX2_TV_RENDER_1080			5
#define GX2_TV_RENDER_COUNT			6
	
	struct
	{
		sint32 width;
		sint32 height;
	}tvScanBufferResolutions[GX2_TV_RENDER_COUNT] = {
	0,0,
	640,480,
	854,480,
	1280,720,
	1280,720,
	1920,1080
	};

	void GX2SetTVBuffer(void* imageBuffePtr, uint32 imageBufferSize, E_TVRES tvResolutionMode, uint32 _surfaceFormat, E_TVBUFFERMODE bufferMode)
	{
		Latte::E_GX2SURFFMT surfaceFormat = (Latte::E_GX2SURFFMT)_surfaceFormat;
		LatteGPUState.tvBufferUsesSRGB = HAS_FLAG(surfaceFormat, Latte::E_GX2SURFFMT::FMT_BIT_SRGB);
		// todo - actually allocate a scanbuffer
	}

	void GX2SetTVGamma(float gamma)
	{
		LatteGPUState.tvGamma = (1.0f - gamma);
	}

	void GX2SetDRCGamma(float gamma)
	{
		LatteGPUState.drcGamma = (1.0f - gamma);
	}

	uint64 GX2GPUTimeToCPUTime(uint64 gpuTime)
	{
		return 0; // hack, see note in GX2SampleBottomGPUCycle
	}

	uint32 GX2GetSystemDRCMode()
	{
		return 1;
	}

	uint32 GX2IsVideoOutReady()
	{
		return 1;
	}

	void GX2Invalidate(GX2InvalidationFlag invalidationFlags, MPTR invalidationAddr, uint32 invalidationSize)
	{
		Latte::E_COHER_CNTL coherCntl{};

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_ATTRIB) || HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_TEXTURE))
	        coherCntl |= Latte::E_COHER_CNTL::TC_ACTION_ENA;

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_UNIFORM_BLOCK))
	        coherCntl |= Latte::E_COHER_CNTL::TC_ACTION_ENA | Latte::E_COHER_CNTL::SH_ACTION_ENA;

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_SHADER))
	        coherCntl |= Latte::E_COHER_CNTL::SH_ACTION_ENA;

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_COLOR_BUFFER))
	    {
	        coherCntl |= Latte::E_COHER_CNTL::CB_ACTION_ENA | Latte::E_COHER_CNTL::CB_ALL_DEST_BASE_ENA;
	    }

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_DEPTH_BUFFER))
	        coherCntl |= Latte::E_COHER_CNTL::DB_ACTION_ENA | Latte::E_COHER_CNTL::DB_DEST_BASE_ENA;

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_STREAM_OUT))
	    {
	        coherCntl |= Latte::E_COHER_CNTL::SX_ACTION_ENA
	                   | Latte::E_COHER_CNTL::SO0_DEST_BASE_ENA | Latte::E_COHER_CNTL::SO1_DEST_BASE_ENA
	                   | Latte::E_COHER_CNTL::SO2_DEST_BASE_ENA | Latte::E_COHER_CNTL::SO3_DEST_BASE_ENA;
	    }

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::GPU_EXPORT_BUFFER))
	    {
	        coherCntl |= Latte::E_COHER_CNTL::SX_ACTION_ENA
	                   | Latte::E_COHER_CNTL::DB_ACTION_ENA
	                   | Latte::E_COHER_CNTL::CB_ACTION_ENA
	                   | Latte::E_COHER_CNTL::TC_ACTION_ENA
	                   | Latte::E_COHER_CNTL::DEST_BASE_0_ENA;
	    }

	    if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::CPU))
	        LatteBufferCache_notifyDCFlush(invalidationAddr, invalidationSize);

	    if (coherCntl == static_cast<Latte::E_COHER_CNTL>(0))
	        return; // no GPU-side invalidation

	    coherCntl |= Latte::E_COHER_CNTL::ENGINE_ME;

	    uint32 physicalAddr = memory_virtualToPhysical(invalidationAddr);
	    uint32 sizeUnits = (invalidationSize == 0xFFFFFFFFu) ? 0x00FFFFFFu : ((invalidationSize + 0xFF) >> 8);

	    GX2ReserveCmdSpace(5);
	    gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_SURFACE_SYNC, 4));
	    gx2WriteGather_submitU32AsBE(static_cast<uint32>(coherCntl));
	    gx2WriteGather_submitU32AsBE(sizeUnits);
	    gx2WriteGather_submitU32AsBE(physicalAddr >> 8);
	    gx2WriteGather_submitU32AsBE(4u);
	}

	void gx2Export_GX2CopyColorBufferToScanBuffer(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::GX2, "GX2CopyColorBufferToScanBuffer(0x{:08x},{})", hCPU->gpr[3], hCPU->gpr[4]);
		GX2::GX2ReserveCmdSpace(10);

		// todo: proper implementation

		GX2ColorBuffer* colorBuffer = (GX2ColorBuffer*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]);

		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER, 9));
		gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(colorBuffer->surface.imagePtr));
		gx2WriteGather_submitU32AsBE((uint32)colorBuffer->surface.width);
		gx2WriteGather_submitU32AsBE((uint32)colorBuffer->surface.height);
		gx2WriteGather_submitU32AsBE((uint32)colorBuffer->surface.pitch);
		gx2WriteGather_submitU32AsBE((uint32)colorBuffer->surface.tileMode.value());
		gx2WriteGather_submitU32AsBE((uint32)colorBuffer->surface.swizzle);
		gx2WriteGather_submitU32AsBE((uint32)colorBuffer->viewFirstSlice);
		gx2WriteGather_submitU32AsBE((uint32)colorBuffer->surface.format.value());
		gx2WriteGather_submitU32AsBE(hCPU->gpr[4]);

		osLib_returnFromFunction(hCPU, 0);
	}

	void gx2Export_GX2WaitForFreeScanBuffer(PPCInterpreter_t* hCPU)
	{
		// todo: proper implementation
		debug_printf("GX2WaitForFreeScanBuffer(): Unimplemented\n");

		osLib_returnFromFunction(hCPU, 0);
	}

	void gx2Export_GX2GetCurrentScanBuffer(PPCInterpreter_t* hCPU)
	{
		// todo: proper implementation
		uint32 scanTarget = hCPU->gpr[3];
		GX2ColorBuffer* colorBufferBE = (GX2ColorBuffer*)memory_getPointerFromVirtualOffset(hCPU->gpr[4]);
		memset(colorBufferBE, 0x00, sizeof(GX2ColorBuffer));
		colorBufferBE->surface.width = 100;
		colorBufferBE->surface.height = 100;
		// note: For now we abuse the tiling aperture memory area as framebuffer pointers
		if( scanTarget == GX2_SCAN_TARGET_TV )
		{
			colorBufferBE->surface.imagePtr = MEMORY_TILINGAPERTURE_AREA_ADDR+0x200000;
		}
		else if( scanTarget == GX2_SCAN_TARGET_DRC_FIRST )
		{
			colorBufferBE->surface.imagePtr = MEMORY_TILINGAPERTURE_AREA_ADDR+0x40000;
		}
		osLib_returnFromFunction(hCPU, 0);
	}

	void coreinitExport_GX2GetSystemTVScanMode(PPCInterpreter_t* hCPU)
	{
		// 1080p = 7
		osLib_returnFromFunction(hCPU, 7);
	}

	void coreinitExport_GX2GetSystemTVAspectRatio(PPCInterpreter_t* hCPU)
	{
		osLib_returnFromFunction(hCPU, 1); // 16:9
	}

	void gx2Export_GX2TempGetGPUVersion(PPCInterpreter_t* hCPU)
	{
		osLib_returnFromFunction(hCPU, 2);
	}

	void _GX2InitScanBuffer(GX2ColorBuffer* colorBuffer, sint32 width, sint32 height, Latte::E_GX2SURFFMT format)
	{
		colorBuffer->surface.resFlag = GX2_RESFLAG_USAGE_TEXTURE | GX2_RESFLAG_USAGE_COLOR_BUFFER;
		colorBuffer->surface.width = width;
		colorBuffer->surface.height = height;
		colorBuffer->viewFirstSlice = 0;
		colorBuffer->viewNumSlices = 1;
		colorBuffer->viewMip = 0;
		colorBuffer->surface.numLevels = 1;
		colorBuffer->surface.dim = Latte::E_DIM::DIM_2D;
		colorBuffer->surface.swizzle = 0;
		colorBuffer->surface.depth = 1;
		colorBuffer->surface.tileMode = Latte::E_GX2TILEMODE::TM_LINEAR_GENERAL;
		colorBuffer->surface.format = format;
		colorBuffer->surface.mipPtr = MPTR_NULL;
		colorBuffer->surface.aa = 0;
		wwhd::gx2::GX2CalcSurfaceSizeAndAlignment(&colorBuffer->surface);
		colorBuffer->surface.resFlag = GX2_RESFLAG_USAGE_TEXTURE | GX2_RESFLAG_USAGE_COLOR_BUFFER | GX2_RESFLAG_USAGE_SCAN_BUFFER;
	}

	void gx2Export_GX2CalcTVSize(PPCInterpreter_t* hCPU)
	{
		uint32 tvRenderMode = hCPU->gpr[3];
		Latte::E_GX2SURFFMT format = (Latte::E_GX2SURFFMT)hCPU->gpr[4];
		uint32 bufferingMode = hCPU->gpr[5];
		uint32 outputSizeMPTR = hCPU->gpr[6];
		uint32 outputScaleNeededMPTR = hCPU->gpr[7];

		cemu_assert(tvRenderMode < GX2_TV_RENDER_COUNT);

		uint32 width = tvScanBufferResolutions[tvRenderMode].width;
		uint32 height = tvScanBufferResolutions[tvRenderMode].height;
		
		GX2ColorBuffer colorBuffer;
		memset(&colorBuffer, 0, sizeof(GX2ColorBuffer));
		_GX2InitScanBuffer(&colorBuffer, width, height, format);

		uint32 imageSize = colorBuffer.surface.imageSize;
		uint32 alignment = colorBuffer.surface.alignment;

		uint32 alignmentPaddingSize = (alignment - (imageSize%alignment)) % alignment;

		uint32 uknMult = 1; // probably for interlaced?
		if (tvRenderMode == GX2_TV_RENDER_720I)
			uknMult = 2;

		uint32 adjustedBufferingMode = bufferingMode;
		if (tvRenderMode < GX2_TV_RENDER_720)
			adjustedBufferingMode = 4;

		uint32 bufferedImageSize = (imageSize + alignmentPaddingSize) * adjustedBufferingMode;
		bufferedImageSize = bufferedImageSize * uknMult - alignmentPaddingSize;
		
		memory_writeU32(outputSizeMPTR, bufferedImageSize);
		memory_writeU32(outputScaleNeededMPTR, 0); // todo
		osLib_returnFromFunction(hCPU, 0);
	}

	void gx2Export_GX2CalcDRCSize(PPCInterpreter_t* hCPU)
	{

		ppcDefineParamS32(drcMode, 0);
		ppcDefineParamU32(format, 1);
		ppcDefineParamU32(bufferingMode, 2);
		ppcDefineParamMPTR(sizeMPTR, 3);
		ppcDefineParamMPTR(scaleNeededMPTR, 4);

		uint32 width = 0;
		uint32 height = 0;
		if (drcMode > 0)
		{
			width = 854;
			height = 480;
		}

		GX2ColorBuffer colorBuffer = {};
		memset(&colorBuffer, 0, sizeof(colorBuffer));
		_GX2InitScanBuffer(&colorBuffer, width, height, (Latte::E_GX2SURFFMT)format);

		uint32 imageSize = colorBuffer.surface.imageSize;
		uint32 alignment = colorBuffer.surface.alignment;

		uint32 alignmentPaddingSize = (alignment - (imageSize%alignment)) % alignment;


		uint32 adjustedBufferingMode = bufferingMode;

		uint32 bufferedImageSize = (imageSize + alignmentPaddingSize) * adjustedBufferingMode;
		bufferedImageSize = bufferedImageSize - alignmentPaddingSize;

		memory_writeU32(sizeMPTR, bufferedImageSize);
		memory_writeU32(scaleNeededMPTR, 0);

		osLib_returnFromFunction(hCPU, 0);
	}

	void gx2Export_GX2SetDRCScale(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::GX2, "GX2SetDRCScale({},{})", hCPU->gpr[3], hCPU->gpr[4]);
		osLib_returnFromFunction(hCPU, 0);
	}

	void gx2Export_GX2SetSemaphore(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::GX2, "GX2SetSemaphore(0x{:08x},{})", hCPU->gpr[3], hCPU->gpr[4]);
		ppcDefineParamMPTR(semaphoreMPTR, 0);
		ppcDefineParamS32(mode, 1);

		uint32 SEM_SEL;

		if (mode == 0)
		{
			// wait
			SEM_SEL = 7;
		}
		else if (mode == 1)
		{
			// signal
			SEM_SEL = 6;
		}
		else
		{
			cemu_assert_debug(false);
			osLib_returnFromFunction(hCPU, 0);
			return;
		}
		uint32 semaphoreControl = (SEM_SEL << 29);
		semaphoreControl |= 0x1000; // WAIT_ON_SIGNAL

		GX2::GX2ReserveCmdSpace(3);
		gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_MEM_SEMAPHORE, 2));
		gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(semaphoreMPTR)); // semaphore physical address
		gx2WriteGather_submitU32AsBE(semaphoreControl); // control

		osLib_returnFromFunction(hCPU, 0);
	}
}

WWHD_OS_EXPORT(gx2, GX2SetTVBuffer, wwhd::gx2::GX2SetTVBuffer);
WWHD_OS_EXPORT(gx2, GX2SetTVGamma, wwhd::gx2::GX2SetTVGamma);
WWHD_OS_EXPORT(gx2, GX2SetDRCGamma, wwhd::gx2::GX2SetDRCGamma);
WWHD_OS_EXPORT(gx2, GX2GPUTimeToCPUTime, wwhd::gx2::GX2GPUTimeToCPUTime);
WWHD_OS_EXPORT(gx2, GX2GetSystemDRCMode, wwhd::gx2::GX2GetSystemDRCMode);
WWHD_OS_EXPORT(gx2, GX2IsVideoOutReady, wwhd::gx2::GX2IsVideoOutReady);
WWHD_OS_EXPORT(gx2, GX2Invalidate, wwhd::gx2::GX2Invalidate);
static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2CopyColorBufferToScanBuffer("gx2", "GX2CopyColorBufferToScanBuffer", wwhd::gx2::gx2Export_GX2CopyColorBufferToScanBuffer);
static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2WaitForFreeScanBuffer("gx2", "GX2WaitForFreeScanBuffer", wwhd::gx2::gx2Export_GX2WaitForFreeScanBuffer);
static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2GetCurrentScanBuffer("gx2", "GX2GetCurrentScanBuffer", wwhd::gx2::gx2Export_GX2GetCurrentScanBuffer);
static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2GetSystemTVScanMode("gx2", "GX2GetSystemTVScanMode", wwhd::gx2::coreinitExport_GX2GetSystemTVScanMode);
static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2GetSystemTVAspectRatio("gx2", "GX2GetSystemTVAspectRatio", wwhd::gx2::coreinitExport_GX2GetSystemTVAspectRatio);
static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2TempGetGPUVersion("gx2", "GX2TempGetGPUVersion", wwhd::gx2::gx2Export_GX2TempGetGPUVersion);
static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2CalcTVSize("gx2", "GX2CalcTVSize", wwhd::gx2::gx2Export_GX2CalcTVSize);
static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2CalcDRCSize("gx2", "GX2CalcDRCSize", wwhd::gx2::gx2Export_GX2CalcDRCSize);
static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2SetDRCScale("gx2", "GX2SetDRCScale", wwhd::gx2::gx2Export_GX2SetDRCScale);
static ::wwhd::os::Registration wwhd_os_reg_gx2_GX2SetSemaphore("gx2", "GX2SetSemaphore", wwhd::gx2::gx2Export_GX2SetSemaphore);
