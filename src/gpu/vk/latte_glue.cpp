// The pieces of Latte that Cemu's shader decompiler (Cafe/HW/Latte/LegacyShaderDecompiler) and fetch
// shader parser (Core/FetchShader.cpp) call, which wwhd-null links without the rest of Latte:
// the pixel shader input table, the colour/depth buffer helpers, the texture format reconstruction,
// and a Renderer for g_renderer (the decompiler asks it which API it targets).
//
// Derived from Cemu (Core/LatteShader.cpp, Core/LatteRenderTarget.cpp, Core/LatteTextureLegacy.cpp,
// Renderer/Renderer.cpp); Mozilla Public License 2.0.
#include "renderer_internal.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInternal.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"

extern std::unique_ptr<Renderer> g_renderer;

// ---- pixel shader inputs (LatteShader.cpp) ------------------------------------------------------
static LatteShaderPSInputTable s_activePSImportTable;

void LatteShader_CreatePSInputTable(LatteShaderPSInputTable* psInputTable, uint32* contextRegisters)
{
	uint32 psControl0 = contextRegisters[mmSPI_PS_IN_CONTROL_0];
	uint32 spi0_positionEnable = (psControl0 >> 8) & 1;
	uint32 spi0_positionAddr = spi0_positionEnable ? ((psControl0 >> 10) & 0x1F) : 0xFFFFFFFF;
	uint32 spi0_paramGen = (psControl0 >> 15) & 0xF;
	uint32 spi0_paramGenAddr = (psControl0 >> 19) & 0x7F;
	uint32 numPSInputs = contextRegisters[mmSPI_PS_IN_CONTROL_0] & 0x3F;
	uint64 key = 0;
	if (spi0_positionEnable)
		key += (uint64)spi0_positionAddr + 1;
	if (spi0_paramGen != 0)
	{
		key += std::rotr<uint64>(spi0_paramGen, 7);
		key += std::rotr<uint64>(spi0_paramGenAddr, 3);
		psInputTable->paramGen = spi0_paramGen;
		psInputTable->paramGenGPR = spi0_paramGenAddr;
	}
	else
		psInputTable->paramGen = 0;
	numPSInputs = std::min<uint32>(numPSInputs, GPU7_PS_MAX_INPUTS);
	for (uint32 f = 0; f < numPSInputs; f++)
	{
		uint32 psInputControl = contextRegisters[mmSPI_PS_INPUT_CNTL_0 + f];
		uint32 psSemanticId = (psInputControl & 0xFF);
		key += (uint64)psInputControl;
		key = std::rotl<uint64>(key, 7);
		if (f == spi0_positionAddr)
		{
			psInputTable->import[f].semanticId = LATTE_ANALYZER_IMPORT_INDEX_SPIPOSITION;
			psInputTable->import[f].isFlat = false;
			psInputTable->import[f].isNoPerspective = false;
			key += (uint64)0x33;
		}
		else
		{
			psInputTable->import[f].semanticId = psSemanticId;
			psInputTable->import[f].isFlat = (psInputControl & (1 << 10)) != 0;
			psInputTable->import[f].isNoPerspective = (psInputControl & (1 << 12)) != 0;
		}
	}
	psInputTable->key = key;
	psInputTable->count = numPSInputs;
}

void LatteShader_UpdatePSInputs(uint32* contextRegisters)
{
	LatteShader_CreatePSInputTable(&s_activePSImportTable, contextRegisters);
}

LatteShaderPSInputTable* LatteSHRC_GetPSInputTable()
{
	return &s_activePSImportTable;
}

void LatteSHRC_RemoveShaderStateCacheEntryByKey(uint64 key)
{
	// no shader state cache here (FetchShader.cpp's destructor calls this)
}

// ---- texture format (LatteTextureLegacy.cpp) ------------------------------------------------------
Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N& texUnitWord1,
	const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N& texUnitWord4)
{
	Latte::E_GX2SURFFMT gx2Format = (Latte::E_GX2SURFFMT)texUnitWord1.get_DATA_FORMAT();
	auto nfa = texUnitWord4.get_NUM_FORM_ALL();
	if (nfa == Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_SCALED)
		gx2Format |= Latte::E_GX2SURFFMT::FMT_BIT_FLOAT;
	else if (nfa == Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_INT)
		gx2Format |= Latte::E_GX2SURFFMT::FMT_BIT_INT;
	if (texUnitWord4.get_FORCE_DEGAMMA())
		gx2Format |= Latte::E_GX2SURFFMT::FMT_BIT_SRGB;
	if (texUnitWord4.get_FORMAT_COMP_X() == Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_FORMAT_COMP::COMP_SIGNED)
		gx2Format |= Latte::E_GX2SURFFMT::FMT_BIT_SIGNED;
	return gx2Format;
}

// ---- colour and depth buffers (LatteRenderTarget.cpp) ---------------------------------------------
static const uint32 s_colorBufferFormatBits[] = { 0, 0x200, 0, 0, 0x100, 0x300, 0x400, 0x800 };

Latte::E_GX2SURFFMT LatteMRT::GetColorBufferFormat(const uint32 index, const LatteContextRegister& lcr)
{
	uint32 regColorInfo = lcr.GetRawView()[mmCB_COLOR0_INFO + index];
	uint32 colorBufferFormat = (regColorInfo >> 2) & 0x3F;          // base hardware format
	uint32 numberType = (regColorInfo >> 12) & 7;
	colorBufferFormat |= s_colorBufferFormatBits[numberType];
	return (Latte::E_GX2SURFFMT)colorBufferFormat;
}

Latte::E_GX2SURFFMT LatteMRT::GetDepthBufferFormat(const LatteContextRegister& lcr)
{
	switch (lcr.GetRawView()[mmDB_DEPTH_INFO] & 7)
	{
	case 1: return Latte::E_GX2SURFFMT::D16_UNORM;
	case 3: return Latte::E_GX2SURFFMT::D24_S8_UNORM;
	case 5: return Latte::E_GX2SURFFMT::D24_S8_FLOAT;
	case 6: return Latte::E_GX2SURFFMT::D32_FLOAT;
	case 7: return Latte::E_GX2SURFFMT::D32_S8_FLOAT;
	}
	return Latte::E_GX2SURFFMT::D16_UNORM;
}

uint8 LatteMRT::GetActiveColorBufferMask(const LatteDecompilerShader* pixelShader, const LatteContextRegister& lcr)
{
	if (!pixelShader)
		return 0;
	const uint32* regView = lcr.GetRawView();
	if (lcr.CB_COLOR_CONTROL.get_SPECIAL_OP() == Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE)
		return 0;
	uint8 colorBufferMask = pixelShader->pixelColorOutputMask;
	uint32 channelTargetMask = lcr.CB_TARGET_MASK.get_MASK();
	for (uint32 i = 0; i < 8; i++)
		if (((channelTargetMask >> (i * 4)) & 0xF) == 0)
			colorBufferMask &= ~(1 << i);
	uint32 scissorAccessWidth = lcr.PA_SC_GENERIC_SCISSOR_BR.get_BR_X();
	uint32 scissorAccessHeight = lcr.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y();
	for (uint32 i = 0; i < 8; i++)
	{
		if ((colorBufferMask & (1 << i)) == 0)
			continue;
		if (regView[mmCB_COLOR0_BASE + i] == MPTR_NULL)
			colorBufferMask &= ~(1 << i);
		uint32 regColorSize = regView[mmCB_COLOR0_SIZE + i];
		uint32 colorBufferPitch = ((regColorSize & 0x3FF) + 1) << 3;
		uint32 pitchHeight = (((regColorSize >> 10) & 0xFFFFF) + 1) << 6;
		uint32 colorBufferHeight = pitchHeight / colorBufferPitch;
		uint32 colorBufferWidth = colorBufferPitch;
		if ((colorBufferWidth < scissorAccessWidth) || (colorBufferHeight < scissorAccessHeight))
			colorBufferMask &= ~(1 << i);                              // targets smaller than the scissor, as Cemu
	}
	return colorBufferMask;
}

bool LatteMRT::GetActiveDepthBufferMask(const LatteContextRegister& lcr)
{
	return lcr.DB_DEPTH_CONTROL.get_Z_ENABLE() || lcr.DB_DEPTH_CONTROL.get_STENCIL_ENABLE() ||
		lcr.DB_DEPTH_CONTROL.get_BACK_STENCIL_ENABLE();
}

// the decompiler counts its work here (LattePerformanceMonitor.cpp isn't linked)
performanceMonitor_t performanceMonitor{};

// ---- g_renderer ------------------------------------------------------------------------------------
// Renderer's own non-pure virtuals (Renderer.cpp isn't linked); defining its key function here
// emits its vtable here.
void Renderer::Initialize() {}
void Renderer::Shutdown() {}
bool Renderer::GetVRAMInfo(int& usageInMB, int& totalInMB) const { return false; }
bool Renderer::ImguiBegin(bool mainWindow) { return false; }

namespace
{
	// A Renderer that does nothing: Cemu code that sees a non-null g_renderer may call it, and the
	// decompiler only reads its API (Vulkan). The real work is in wwhd::gpu.
	class NoopRenderer final : public Renderer
	{
	public:
		NoopRenderer() : Renderer(RendererAPI::Vulkan) {}
		bool IsPadWindowActive() override { return false; }
		void ClearColorbuffer(bool) override {}
		void DrawEmptyFrame(bool) override {}
		void SwapBuffers(bool, bool) override {}
		void DrawBackbufferQuad(LatteTextureView*, RendererOutputShader*, bool, sint32, sint32, sint32, sint32, bool, bool) override {}
		bool BeginFrame(bool) override { return false; }
		void Flush(bool) override {}
		void NotifyLatteCommandProcessorIdle() override {}
		void ImguiEnd() override {}
		ImTextureID GenerateTexture(const std::vector<uint8>&, const Vector2i&) override { return nullptr; }
		void DeleteTexture(ImTextureID) override {}
		void DeleteFontTextures() override {}
		void AppendOverlayDebugInfo() override {}
		void renderTarget_setViewport(float, float, float, float, float, float, bool) override {}
		void renderTarget_setScissor(sint32, sint32, sint32, sint32) override {}
		LatteCachedFBO* rendertarget_createCachedFBO(uint64) override { return nullptr; }
		void rendertarget_deleteCachedFBO(LatteCachedFBO*) override {}
		void rendertarget_bindFramebufferObject(LatteCachedFBO*) override {}
		void* texture_acquireTextureUploadBuffer(uint32) override { return nullptr; }
		void texture_releaseTextureUploadBuffer(uint8*) override {}
		TextureDecoder* texture_chooseDecodedFormat(Latte::E_GX2SURFFMT, bool, Latte::E_DIM, uint32, uint32) override { return nullptr; }
		void texture_clearSlice(LatteTexture*, sint32, sint32) override {}
		void texture_loadSlice(LatteTexture*, sint32, sint32, sint32, void*, sint32, sint32, uint32) override {}
		void texture_clearColorSlice(LatteTexture*, sint32, sint32, float, float, float, float) override {}
		void texture_clearDepthSlice(LatteTexture*, uint32, sint32, bool, bool, float, uint32) override {}
		LatteTexture* texture_createTextureEx(Latte::E_DIM, MPTR, MPTR, Latte::E_GX2SURFFMT, uint32, uint32, uint32, uint32, uint32,
			uint32, Latte::E_HWTILEMODE, bool) override { return nullptr; }
		void texture_setLatteTexture(LatteTextureView*, uint32) override {}
		void texture_copyImageSubData(LatteTexture*, sint32, sint32, sint32, sint32, LatteTexture*, sint32, sint32, sint32, sint32,
			sint32, sint32, sint32) override {}
		LatteTextureReadbackInfo* texture_createReadback(LatteTextureView*) override { return nullptr; }
		void surfaceCopy_copySurfaceWithFormatConversion(LatteTexture*, sint32, sint32, LatteTexture*, sint32, sint32, sint32, sint32) override {}
		void bufferCache_init(const sint32) override {}
		void bufferCache_upload(uint8*, sint32, uint32) override {}
		void bufferCache_copy(uint32, uint32, uint32) override {}
		void bufferCache_copyStreamoutToMainBuffer(uint32, uint32, uint32) override {}
		void buffer_bindVertexBuffers(std::span<BindBufferParam>) override {}
		void buffer_bindUniformBuffer(LatteConst::ShaderType, uint32, uint32, uint32) override {}
		RendererShader* shader_create(RendererShader::ShaderType, uint64, uint64, const std::string&, bool, bool) override { return nullptr; }
		void streamout_setupXfbBuffer(uint32, sint32, uint32, uint32) override {}
		void streamout_begin() override {}
		void streamout_rendererFinishDrawcall() override {}
		void draw_beginSequence() override {}
		void draw_execute(uint32, uint32, uint32, uint32, MPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE, const LatteDrawcallContext&) override {}
		void draw_endSequence() override {}
		IndexAllocation indexData_reserveIndexMemory(uint32) override { return {}; }
		void indexData_releaseIndexMemory(IndexAllocation&) override {}
		void indexData_uploadIndexMemory(IndexAllocation&) override {}
		LatteQueryObject* occlusionQuery_create() override { return nullptr; }
		void occlusionQuery_destroy(LatteQueryObject*) override {}
		void occlusionQuery_flush() override {}
		void occlusionQuery_updateState() override {}
	};
}

namespace wwhd::gpu
{
	void InstallNoopRenderer()
	{
		if (!g_renderer)
			g_renderer = std::make_unique<NoopRenderer>();
	}
}
