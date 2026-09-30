// gx2: surface sizes and layouts (docs/recompiler-design.md D18). Derived from Cemu's
// src/Cafe/OS/libs/gx2/GX2_Surface.cpp (Mozilla Public License 2.0) by tools/gx2_port.py: our namespace
// and registration. Commands go through gx2's command pipe (GX2_Command.h) and must stay byte for byte
// what Cemu's sent (WWHD_GPU_STREAM, tools/reference/stream_check.sh).
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/gx2/GX2_Surface.h"
#include "Cafe/OS/libs/gx2/GX2_Resource.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "ported.h"
#include "../os.h"

namespace wwhd::gx2
{
	using namespace ::GX2;


	uint32 GX2GetSurfaceMipPitch(GX2Surface* surface, uint32 level)
	{
		LatteAddrLib::AddrSurfaceInfo_OUT surfOut;
		wwhd::gx2::GX2CalculateSurfaceInfo(surface, level, &surfOut);
		return surfOut.pitch;
	}

	uint32 GX2GetSurfaceFormatBits(Latte::E_GX2SURFFMT surfaceFormat)
	{
		uint32 bpp = Latte::GetFormatBits(surfaceFormat);
		if (Latte::IsCompressedFormat(surfaceFormat))
		{
			cemu_assert_debug((bpp & 0xF) == 0);
			bpp /= (4 * 4);
		}
		return bpp;
	}

	uint32 _GX2CalculateSliceSize(GX2Surface* surface, const LatteAddrLib::AddrSurfaceInfo_OUT* surfaceInfo)
	{
		uint32 aaScaler = 1 << surface->aa;
		return aaScaler * (surfaceInfo->bpp >> 3) * surfaceInfo->height * surfaceInfo->pitch;
	}

	uint32 GX2GetSurfaceMipSliceSize(GX2Surface* surface, uint32 level)
	{
		LatteAddrLib::AddrSurfaceInfo_OUT surfOut;
		wwhd::gx2::GX2CalculateSurfaceInfo(surface, level, &surfOut);
		return wwhd::gx2::_GX2CalculateSliceSize(surface, &surfOut);		
	}

	uint32 GX2GetSurfaceSwizzleOffset(GX2Surface* surface, uint32 level)
	{
		uint32 swizzleOffset = 0;
		uint32 swizzle = surface->swizzle;
		if (!Latte::TM_IsMacroTiled(surface->tileMode) || level >= ((swizzle >> 16) & 0xFF))
			swizzleOffset = 0;
		else
			swizzleOffset = swizzle & 0xFFFF;
		return swizzleOffset;
	}

	uint32 GX2GetSurfaceSwizzle(GX2Surface* surface)
	{
		uint32 swizzle = surface->swizzle;
		swizzle = (swizzle >> 8) & 0xFF;
		return swizzle;
	}

	uint32 GX2SurfaceIsCompressed(Latte::E_GX2SURFFMT surfaceFormat)
	{
		return Latte::IsCompressedFormat(surfaceFormat) ? GX2_TRUE : GX2_FALSE;
	}

	void GX2CalcDepthBufferHiZInfo(GX2DepthBuffer* depthBuffer, uint32be* sizeOut, uint32be* alignOut)
	{
		*sizeOut = 0x1000;
		*alignOut = 0x100;

		// todo: implement
	}

	void GX2CalcColorBufferAuxInfo(GX2ColorBuffer* colorBuffer, uint32be* sizeOut, uint32be* alignOut)
	{
		*sizeOut = 0x1000;
		*alignOut = 0x100;

		// todo: implement
	}

	void GX2CalculateSurfaceInfo(GX2Surface* surfacePtr, uint32 level, LatteAddrLib::AddrSurfaceInfo_OUT* pSurfOut)
	{
		bool optimizeForDepthBuffer = (surfacePtr->resFlag & GX2_RESFLAG_USAGE_DEPTH_BUFFER) != 0;
		bool optimizeForScanBuffer = (surfacePtr->resFlag & GX2_RESFLAG_USAGE_SCAN_BUFFER) != 0;
		LatteAddrLib::GX2CalculateSurfaceInfo(surfacePtr->format, surfacePtr->width, surfacePtr->height, surfacePtr->depth, surfacePtr->dim, surfacePtr->tileMode, surfacePtr->aa, level, pSurfOut, optimizeForDepthBuffer, optimizeForScanBuffer);
	}

	uint32 _CalculateLevels(uint32 resolution)
	{
		uint32 x = 0x80000000;
		uint32 v = resolution;
		uint32 n = 0;
		while (!(v & x))
		{
			n++;
			if (n == 32)
				break;
			x >>= 1;
		}
		return 32 - n;
	}

	uint32 _GX2AdjustLevelCount(GX2Surface* surfacePtr)
	{
		if (surfacePtr->numLevels <= 1)
			return 1;
		uint32 levels = std::max(wwhd::gx2::_CalculateLevels(surfacePtr->width), wwhd::gx2::_CalculateLevels(surfacePtr->height));
		if (surfacePtr->dim == Latte::E_DIM::DIM_3D)
			levels = std::max(levels, wwhd::gx2::_CalculateLevels(surfacePtr->depth));
		return levels;
	}

	void _GX2CalcSurfaceSizeAndAlignmentWorkaround(GX2Surface* surface)
	{
		// this workaround catches an issue where the FFL (Mii) library embedded into games tries to use an uninitialized texture
		// and subsequently crashes. It only happens when FFL files are not present in mlc.
		// seen in Sonic Lost World and in Super Mario 3D World
		if ((uint32)surface->dim.value() >= 50 || surface->aa.value() >= 0x100 || (uint32)surface->width.value() >= 0x01000000 || 
			(uint32)surface->height.value() >= 0x01000000 || (uint32)surface->depth.value() >= 0x01000000 || (uint32)surface->format.value() >= 0x10000)
		{
			cemuLog_log(LogType::Force, "wwhd::gx2::GX2CalcSurfaceSizeAndAlignment(): Uninitialized surface encountered\n");
			// overwrite surface parameters with placeholder values to avoid crashing later down the line
			surface->dim = Latte::E_DIM::DIM_2D;
			surface->width = 8;
			surface->height = 8;
			surface->depth = 1;
			surface->tileMode = Latte::E_GX2TILEMODE::TM_2D_TILED_THIN1;
			surface->pitch = 8;
			surface->numLevels = 0;
			surface->imagePtr = MEMORY_TILINGAPERTURE_AREA_ADDR;
			surface->swizzle = 0;
			surface->aa = 0;
			surface->format = Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM;
			surface->alignment = 0x400;
		}
	}

	void GX2CalcSurfaceSizeAndAlignment(GX2Surface* surface)
	{
		wwhd::gx2::_GX2CalcSurfaceSizeAndAlignmentWorkaround(surface);
		LatteAddrLib::AddrSurfaceInfo_OUT surfOut = { 0 };
		uint32 firstMipOffset = 0;
		bool changeTilemode = false;
		Latte::E_GX2TILEMODE lastTilemode = surface->tileMode;
		bool hasTileMode32 = surface->tileMode == Latte::E_GX2TILEMODE::TM_32_SPECIAL;
		if (surface->tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_GENERAL || hasTileMode32)
		{
			if (surface->dim != Latte::E_DIM::DIM_1D || (surface->resFlag & GX2_RESFLAG_USAGE_DEPTH_BUFFER) != 0 || surface->aa)
			{
				if (surface->dim != Latte::E_DIM::DIM_3D || (surface->resFlag & GX2_RESFLAG_USAGE_COLOR_BUFFER) != 0)
					surface->tileMode = Latte::E_GX2TILEMODE::TM_2D_TILED_THIN1;
				else
					surface->tileMode = Latte::E_GX2TILEMODE::TM_2D_TILED_THICK;
				changeTilemode = true;
			}
			else
			{
				surface->tileMode = Latte::E_GX2TILEMODE::TM_LINEAR_ALIGNED;
			}
			lastTilemode = surface->tileMode;
		}

		if (surface->numLevels == 0)
			surface->numLevels = 1;
		surface->numLevels = std::min<uint32>(surface->numLevels, wwhd::gx2::_GX2AdjustLevelCount(surface));
		surface->mipOffset[0] = 0;
		if (Latte::TM_IsMacroTiled(surface->tileMode))
			surface->swizzle = (surface->swizzle & 0xFF00FFFF) | 0xD0000;
		else
			surface->swizzle = surface->swizzle & 0xFF00FFFF;
		// FIX 32
		uint32 fix32Mode;
		if (hasTileMode32)
		{
			if (Latte::IsCompressedFormat(surface->format))
				fix32Mode = 2;
			else
				fix32Mode = 0;
		}
		else
		{
			fix32Mode = 0;
		}
		// setup levels
		uint32 prevSize = 0;
		for (uint32 level = 0; level < surface->numLevels; ++level)
		{
			wwhd::gx2::GX2CalculateSurfaceInfo(surface, level, &surfOut);
			if (level)
			{
				uint32 pad = 0;
				if (Latte::TM_IsMacroTiled(lastTilemode) && !Latte::TM_IsMacroTiled(surfOut.hwTileMode))
				{
					surface->swizzle = (surface->swizzle & 0xFF00FFFF) | (level << 16);
					lastTilemode = (Latte::E_GX2TILEMODE)surfOut.hwTileMode;
					if (level > 1)
						pad = surface->swizzle & 0xFFFF;
				}
				pad += (surfOut.baseAlign - prevSize % surfOut.baseAlign) % surfOut.baseAlign;
				if (level == 1)
				{
					firstMipOffset = pad + prevSize;
				}
				else if (level > 1)
				{
					surface->mipOffset[level - 1] = pad + prevSize + surface->mipOffset[level - 2];
				}
			}
			else
			{
				if (changeTilemode)
				{
					if (surface->tileMode != (Latte::E_GX2TILEMODE)surfOut.hwTileMode)
					{
						surface->tileMode = (Latte::E_GX2TILEMODE)surfOut.hwTileMode;
						wwhd::gx2::GX2CalculateSurfaceInfo(surface, 0, &surfOut);
						if (!Latte::TM_IsMacroTiled(surface->tileMode))
							surface->swizzle = surface->swizzle & 0xFF00FFFF;
						lastTilemode = surface->tileMode;
					}
					if (surface->width < (surfOut.pitchAlign << fix32Mode)
						&& surface->height < (surfOut.heightAlign << fix32Mode))
					{
						if (surface->tileMode == Latte::E_GX2TILEMODE::TM_2D_TILED_THICK)
							surface->tileMode = Latte::E_GX2TILEMODE::TM_1D_TILED_THICK;
						else
							surface->tileMode = Latte::E_GX2TILEMODE::TM_1D_TILED_THIN1;
						wwhd::gx2::GX2CalculateSurfaceInfo(surface, 0, &surfOut);
						surface->swizzle = surface->swizzle & 0xFF00FFFF;
						lastTilemode = surface->tileMode;
					}
				}

				surface->imageSize = (uint32)(surfOut.surfSize);
				surface->alignment = surfOut.baseAlign;
				surface->pitch = surfOut.pitch;
			}
			prevSize = (uint32)(surfOut.surfSize);
		}
		if (surface->numLevels > 1)
			surface->mipSize = prevSize + surface->mipOffset[surface->numLevels - 2];
		else
			surface->mipSize = 0;
		surface->mipOffset[0] = firstMipOffset;
		if (surface->format == Latte::E_GX2SURFFMT::NV12_UNORM)
		{
			uint32 padding = (surface->alignment - surface->imageSize % surface->alignment) % surface->alignment;
			surface->mipOffset[0] = padding + surface->imageSize;
			surface->imageSize = surface->mipOffset[0] + ((uint32)surface->imageSize >> 1);
		}
	}

	Latte::E_ENDIAN_SWAP GetSurfaceFormatSwapMode(Latte::E_GX2SURFFMT fmt)
	{
		// swap mode is 0 for all formats
		return Latte::E_ENDIAN_SWAP::SWAP_NONE;
	}

	uint32 GetSurfaceColorBufferExportFormat(Latte::E_GX2SURFFMT fmt)
	{
		const uint8 table[0x40] = {
			0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01,
			0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
			0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
			0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
		uint32 fmtHW = (uint32)fmt & 0x3F;
		return table[fmtHW];
	}

	uint32 GX2CheckSurfaceUseVsFormat(uint32 resFlags, uint32 surfaceFormat)
	{
		cemuLog_logDebug(LogType::Force, "GX2CheckSurfaceUseVsFormat - stub");
		return 1;
	}

	void GX2SetSurfaceSwizzle(GX2Surface* surface, uint32 newSwizzle)
	{
		uint32 currentSwizzle = surface->swizzle;
		currentSwizzle &= ~0xFF00;
		currentSwizzle |= (newSwizzle << 8); // newSwizzle isn't actually masked and some games set it to values above 0xFF
		surface->swizzle = currentSwizzle;
	}
}

WWHD_OS_EXPORT(gx2, GX2GetSurfaceMipPitch, wwhd::gx2::GX2GetSurfaceMipPitch);
WWHD_OS_EXPORT(gx2, GX2GetSurfaceFormatBits, wwhd::gx2::GX2GetSurfaceFormatBits);
WWHD_OS_EXPORT(gx2, GX2GetSurfaceMipSliceSize, wwhd::gx2::GX2GetSurfaceMipSliceSize);
WWHD_OS_EXPORT(gx2, GX2GetSurfaceSwizzleOffset, wwhd::gx2::GX2GetSurfaceSwizzleOffset);
WWHD_OS_EXPORT(gx2, GX2GetSurfaceSwizzle, wwhd::gx2::GX2GetSurfaceSwizzle);
WWHD_OS_EXPORT(gx2, GX2SurfaceIsCompressed, wwhd::gx2::GX2SurfaceIsCompressed);
WWHD_OS_EXPORT(gx2, GX2CalcDepthBufferHiZInfo, wwhd::gx2::GX2CalcDepthBufferHiZInfo);
WWHD_OS_EXPORT(gx2, GX2CalcColorBufferAuxInfo, wwhd::gx2::GX2CalcColorBufferAuxInfo);
WWHD_OS_EXPORT(gx2, GX2CalcSurfaceSizeAndAlignment, wwhd::gx2::GX2CalcSurfaceSizeAndAlignment);
WWHD_OS_EXPORT(gx2, GX2CheckSurfaceUseVsFormat, wwhd::gx2::GX2CheckSurfaceUseVsFormat);
WWHD_OS_EXPORT(gx2, GX2SetSurfaceSwizzle, wwhd::gx2::GX2SetSurfaceSwizzle);
