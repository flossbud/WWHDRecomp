// gx2 functions called across our gx2 files (tools/gx2_port.py writes this).
#pragma once
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/gx2/GX2_Blit.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "Cafe/OS/libs/gx2/GX2_Draw.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include "Cafe/OS/libs/gx2/GX2_Memory.h"
#include "Cafe/OS/libs/gx2/GX2_Misc.h"
#include "Cafe/OS/libs/gx2/GX2_Query.h"
#include "Cafe/OS/libs/gx2/GX2_Resource.h"
#include "Cafe/OS/libs/gx2/GX2_Shader.h"
#include "Cafe/OS/libs/gx2/GX2_State.h"
#include "Cafe/OS/libs/gx2/GX2_Streamout.h"
#include "Cafe/OS/libs/gx2/GX2_Surface.h"
#include "Cafe/OS/libs/gx2/GX2_Surface_Copy.h"
#include "Cafe/OS/libs/gx2/GX2_Texture.h"

namespace wwhd::gx2
{
	using namespace ::GX2;
	void GX2CalcSurfaceSizeAndAlignment(GX2Surface* surface);
	void GX2CalculateSurfaceInfo(GX2Surface* surfacePtr, uint32 level, LatteAddrLib::AddrSurfaceInfo_OUT* pSurfOut);
	void GX2DrawIndexedEx(GX2PrimitiveMode2 primitiveMode, uint32 count, GX2IndexType indexType, void* indexData, uint32 baseVertex, uint32 numInstances);
	void GX2RSetAllocator(MPTR funcAllocMPTR, MPTR funcFreeMPR);
	void GX2SetAlphaTest(uint32 alphaTestEnable, GX2_ALPHAFUNC alphaFunc, float alphaRef);
	void GX2SetAttribBuffer(uint32 bufferIndex, uint32 sizeInBytes, uint32 stride, void* data);
	void GX2SetBlendConstantColor(float red, float green, float blue, float alpha);
	void GX2SetColorControl(GX2_LOGICOP logicOp, uint32 blendMask, uint32 multiwriteEnable, uint32 colorBufferEnable);
	void GX2SetPointLimits(float minSize, float maxSize);
	void GX2SetPointSize(float width, float height);
	void GX2SetPolygonControl(Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace, uint32 cullFront, uint32 cullBack, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE usePolygonMode, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeFront, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeBack, uint32 polygonOffsetFrontEnable, uint32 polygonOffsetBackEnable, uint32 paraOffsetEnable);
	void GX2SetPolygonOffset(float frontOffset, float frontScale, float backOffset, float backScale, float clampOffset);
	void GX2SetPrimitiveRestartIndex(uint32 restartIndex);
	void GX2SetRasterizerClipControlEx(bool enableRasterizer, bool enableZClip, bool enableHalfZ);
	void GX2SetShaderModeEx(GX2_SHADER_MODE mode, uint32 shaderGprsVS, uint32 shaderStackVS, uint32 shaderGprsGS, uint32 shaderStackGS, uint32 shaderGprsPS, uint32 shaderStackPS);
	void GX2SetStreamOutBuffer(uint32 bufferIndex, GX2StreamOutBuffer* streamOutBuffer);
	void GX2SetTargetChannelMasks(GX2_CHANNELMASK t0, GX2_CHANNELMASK t1, GX2_CHANNELMASK t2, GX2_CHANNELMASK t3, GX2_CHANNELMASK t4, GX2_CHANNELMASK t5, GX2_CHANNELMASK t6, GX2_CHANNELMASK t7);
	void GX2SetVertexShader(GX2VertexShader* vertexShader);
	uint32 GetSurfaceColorBufferExportFormat(Latte::E_GX2SURFFMT fmt);
	Latte::E_ENDIAN_SWAP GetSurfaceFormatSwapMode(Latte::E_GX2SURFFMT fmt);
}
