// gx2: blend, depth, stencil, raster, viewport and scissor state (docs/recompiler-design.md D18). Derived from Cemu's
// src/Cafe/OS/libs/gx2/GX2_State.cpp (Mozilla Public License 2.0) by tools/gx2_port.py: our namespace
// and registration. Commands go through gx2's command pipe (GX2_Command.h) and must stay byte for byte
// what Cemu's sent (WWHD_GPU_STREAM, tools/reference/stream_check.sh).
#include "Common/precompiled.h"
#include "Cafe/OS/libs/gx2/GX2_State.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "ported.h"
#include "../os.h"

namespace wwhd::gx2
{
	using namespace ::GX2;

	using namespace Latte;

	void GX2InitAlphaTestReg(GX2AlphaTestReg* reg, uint32 alphaTestEnable, GX2_ALPHAFUNC alphaFunc, float alphaRef)
	{
		Latte::LATTE_SX_ALPHA_TEST_CONTROL tmpRegCtrl;
		tmpRegCtrl.set_ALPHA_FUNC(alphaFunc);
		tmpRegCtrl.set_ALPHA_TEST_ENABLE(alphaTestEnable != 0);
		reg->regAlphaTestControl = tmpRegCtrl;

		Latte::LATTE_SX_ALPHA_REF tmpRegRef;
		tmpRegRef.set_ALPHA_TEST_REF(alphaRef);
		reg->regAlphaTestRef = tmpRegRef;
	}

	void GX2SetAlphaTestReg(GX2AlphaTestReg* reg)
	{
		GX2ReserveCmdSpace(3 + 3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::SX_ALPHA_TEST_CONTROL - 0xA000,
			reg->regAlphaTestControl,
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::SX_ALPHA_REF - 0xA000,
			reg->regAlphaTestRef);
	}

	void GX2SetAlphaTest(uint32 alphaTestEnable, GX2_ALPHAFUNC alphaFunc, float alphaRef)
	{
		GX2AlphaTestReg tmpReg;
		wwhd::gx2::GX2InitAlphaTestReg(&tmpReg, alphaTestEnable, alphaFunc, alphaRef);
		wwhd::gx2::GX2SetAlphaTestReg(&tmpReg);
	}

	void GX2InitColorControlReg(GX2ColorControlReg* reg, GX2_LOGICOP logicOp, uint32 blendMask, uint32 multiwriteEnable, uint32 colorBufferEnable)
	{
		Latte::LATTE_CB_COLOR_CONTROL colorControlReg2;
		colorControlReg2.set_MULTIWRITE_ENABLE(multiwriteEnable != 0);
		if (colorBufferEnable == 0)
			colorControlReg2.set_SPECIAL_OP(Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE);
		else
			colorControlReg2.set_SPECIAL_OP(Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL);
		colorControlReg2.set_BLEND_MASK(blendMask);
		colorControlReg2.set_ROP(logicOp);
		reg->reg = colorControlReg2;
	}

	void GX2SetColorControlReg(GX2ColorControlReg* reg)
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::CB_COLOR_CONTROL - 0xA000,
			reg->reg);
	}

	void GX2SetColorControl(GX2_LOGICOP logicOp, uint32 blendMask, uint32 multiwriteEnable, uint32 colorBufferEnable)
	{
		GX2ColorControlReg colorControlReg;
		wwhd::gx2::GX2InitColorControlReg(&colorControlReg, logicOp, blendMask, multiwriteEnable, colorBufferEnable);
		wwhd::gx2::GX2SetColorControlReg(&colorControlReg);
	}


	void GX2InitPolygonControlReg(GX2PolygonControlReg* reg,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace,
		uint32 cullFront,
		uint32 cullBack,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE usePolygonMode,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeFront,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeBack,
		uint32 polygonOffsetFrontEnable,
		uint32 polygonOffsetBackEnable,
		uint32 paraOffsetEnable)
	{
		Latte::LATTE_PA_SU_SC_MODE_CNTL v;
		v.set_FRONT_FACE(frontFace);
		v.set_CULL_FRONT((cullFront & 1) != 0);
		v.set_CULL_BACK((cullBack & 1) != 0);
		v.set_POLYGON_MODE(usePolygonMode);
		v.set_FRONT_POLY_MODE(polyModeFront);
		v.set_BACK_POLY_MODE(polyModeBack);
		v.set_OFFSET_PARA_ENABLED((paraOffsetEnable & 1) != 0);
		v.set_OFFSET_FRONT_ENABLED((polygonOffsetFrontEnable & 1) != 0);
		v.set_OFFSET_BACK_ENABLED((polygonOffsetBackEnable & 1) != 0);
		reg->reg = v;
	}

	void GX2SetPolygonControlReg(GX2PolygonControlReg* reg)
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::PA_SU_SC_MODE_CNTL - 0xA000,
			reg->reg);
	}

	void GX2SetPolygonControl(Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace,
		uint32 cullFront,
		uint32 cullBack,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE usePolygonMode,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeFront,
		Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE polyModeBack,
		uint32 polygonOffsetFrontEnable,
		uint32 polygonOffsetBackEnable,
		uint32 paraOffsetEnable)
	{
		GX2PolygonControlReg reg{};
		wwhd::gx2::GX2InitPolygonControlReg(&reg, frontFace, cullFront, cullBack, usePolygonMode, polyModeFront, polyModeBack, polygonOffsetFrontEnable, polygonOffsetBackEnable, paraOffsetEnable);
		wwhd::gx2::GX2SetPolygonControlReg(&reg);
	}

	void GX2SetCullOnlyControl(Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE frontFace, uint32 cullFront, uint32 cullBack)
	{
		GX2PolygonControlReg reg{};
		wwhd::gx2::GX2InitPolygonControlReg(&reg, frontFace, cullFront, cullBack, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE::UKN0, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::POINTS, Latte::LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::POINTS, 0, 0, 0);
		wwhd::gx2::GX2SetPolygonControlReg(&reg);
	}

	void GX2InitPolygonOffsetReg(GX2PolygonOffsetReg* reg, float frontOffset, float frontScale, float backOffset, float backScale, float clampOffset)
	{
		frontScale *= 16.0;
		backScale *= 16.0;
		reg->regFrontScale = Latte::LATTE_PA_SU_POLY_OFFSET_FRONT_SCALE().set_SCALE(frontScale);
		reg->regFrontOffset = Latte::LATTE_PA_SU_POLY_OFFSET_FRONT_OFFSET().set_OFFSET(frontOffset);
		reg->regBackScale = Latte::LATTE_PA_SU_POLY_OFFSET_BACK_SCALE().set_SCALE(backScale);
		reg->regBackOffset = Latte::LATTE_PA_SU_POLY_OFFSET_BACK_OFFSET().set_OFFSET(backOffset);
		reg->regClamp = Latte::LATTE_PA_SU_POLY_OFFSET_CLAMP().set_CLAMP(clampOffset);
	}

	void GX2SetPolygonOffsetReg(GX2PolygonOffsetReg* reg)
	{
		GX2ReserveCmdSpace(6 + 3);

		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 4),
			Latte::REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE - 0xA000,
			reg->regFrontScale,
			reg->regFrontOffset,
			reg->regBackScale,
			reg->regBackOffset,

			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::PA_SU_POLY_OFFSET_CLAMP - 0xA000,
			reg->regClamp);
	}

	void GX2SetPolygonOffset(float frontOffset, float frontScale, float backOffset, float backScale, float clampOffset)
	{
		GX2PolygonOffsetReg tmpReg;
		wwhd::gx2::GX2InitPolygonOffsetReg(&tmpReg, frontOffset, frontScale, backOffset, backScale, clampOffset);
		wwhd::gx2::GX2SetPolygonOffsetReg(&tmpReg);
	}

	void GX2SetRasterizerClipControlEx(bool enableRasterizer, bool enableZClip, bool enableHalfZ)
	{
		GX2ReserveCmdSpace(3);

		//if (enableHalfZ)
		//{
		//	// Smash has a bug where it enables half space clipping during streamout drawcalls and shadowing and then doesn't turn it off until the next GX2SetRasterizerClipControl call
		//	// this leads to some stuff being rendered at the wrong z-plane (e.g. shields behind characters) if the game's default depth range -1 to 1 isn't supported (on OpenGL only Nvidia's glDepthRangedNV allows unclamped values)
		//	uint64 titleId = gameMeta_getTitleId();
		//	if (titleId == 0x0005000010144F00ULL ||
		//		titleId == 0x0005000010145000ULL ||
		//		titleId == 0x0005000010110E00ULL)
		//	{
		//		// force disable half space clipping
		//		if (g_renderer && g_renderer->GetType() == RendererAPI::OpenGL && LatteGPUState.glVendor != GLVENDOR_NVIDIA)
		//			enableHalfZ = false;
		//	}
		//}

		Latte::LATTE_PA_CL_CLIP_CNTL reg{};
		reg.set_ZCLIP_NEAR_DISABLE(!enableZClip).set_ZCLIP_FAR_DISABLE(!enableZClip);
		reg.set_DX_RASTERIZATION_KILL(!enableRasterizer);
		reg.set_DX_CLIP_SPACE_DEF(enableHalfZ);
		reg.set_DX_LINEAR_ATTR_CLIP_ENA(true);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::PA_CL_CLIP_CNTL - 0xA000,
			reg);
	}

	void GX2SetRasterizerClipControl(bool enableRasterizer, bool enableZClip)
	{
		wwhd::gx2::GX2SetRasterizerClipControlEx(enableRasterizer, enableZClip, false);
	}

	void GX2SetRasterizerClipControlHalfZ(bool enableRasterizer, bool enableZClip, bool enableHalfZ)
	{
		wwhd::gx2::GX2SetRasterizerClipControlEx(enableRasterizer, enableZClip, enableHalfZ);
	}

	void GX2InitViewportReg(GX2ViewportReg* viewportReg, float x, float y, float width, float height, float nearZ, float farZ)
	{
		// todo: set clipping registers and zMin/zMax registers
		viewportReg->xScale = Latte::LATTE_PA_CL_VPORT_XSCALE().set_SCALE(width * 0.5f);
		viewportReg->xOffset = Latte::LATTE_PA_CL_VPORT_XOFFSET().set_OFFSET(x + (width * 0.5f));
		viewportReg->yScale = Latte::LATTE_PA_CL_VPORT_YSCALE().set_SCALE(height * -0.5f);
		viewportReg->yOffset = Latte::LATTE_PA_CL_VPORT_YOFFSET().set_OFFSET(y + (height * 0.5f));
		viewportReg->zScale = Latte::LATTE_PA_CL_VPORT_ZSCALE().set_SCALE((farZ - nearZ) * 0.5f);
		viewportReg->zOffset = Latte::LATTE_PA_CL_VPORT_ZOFFSET().set_OFFSET((nearZ + farZ) * 0.5f);
	}

	void GX2SetViewportReg(GX2ViewportReg* viewportReg)
	{
		GX2ReserveCmdSpace(2 + 6);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 6),
			Latte::REGADDR::PA_CL_VPORT_XSCALE - 0xA000,
			viewportReg->xScale, viewportReg->xOffset,
			viewportReg->yScale, viewportReg->yOffset,
			viewportReg->zScale, viewportReg->zOffset);
	}

	void GX2SetViewport(float x, float y, float width, float height, float nearZ, float farZ)
	{
		GX2ViewportReg viewportReg;
		wwhd::gx2::GX2InitViewportReg(&viewportReg, x, y, width, height, nearZ, farZ);
		wwhd::gx2::GX2SetViewportReg(&viewportReg);
	}

	void GX2InitScissorReg(GX2ScissorReg* scissorReg, uint32 x, uint32 y, uint32 width, uint32 height)
	{
		uint32 tlx = x;
		uint32 tly = y;
		uint32 brx = x + width;
		uint32 bry = y + height;

		tlx = std::min(tlx, 8192u);
		tly = std::min(tly, 8192u);
		brx = std::min(brx, 8192u);
		bry = std::min(bry, 8192u);

		scissorReg->scissorTL = Latte::LATTE_PA_SC_GENERIC_SCISSOR_TL().set_TL_X(tlx).set_TL_Y(tly).set_WINDOW_OFFSET_DISABLE(true);
		scissorReg->scissorBR = Latte::LATTE_PA_SC_GENERIC_SCISSOR_BR().set_BR_X(brx).set_BR_Y(bry);
	}

	void GX2SetScissorReg(GX2ScissorReg* scissorReg)
	{
		GX2ReserveCmdSpace(4);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 2),
			Latte::REGADDR::PA_SC_GENERIC_SCISSOR_TL - 0xA000,
			scissorReg->scissorTL, scissorReg->scissorBR);
	}

	void GX2GetScissorReg(GX2ScissorReg* scissorReg, uint32be* x, uint32be* y, uint32be* width, uint32be* height)
	{
		*x = scissorReg->scissorTL.value().get_TL_X();
		*y = scissorReg->scissorTL.value().get_TL_Y();
		*width = scissorReg->scissorBR.value().get_BR_X() - scissorReg->scissorTL.value().get_TL_X();
		*height = scissorReg->scissorBR.value().get_BR_Y() - scissorReg->scissorTL.value().get_TL_Y();
	}

	void GX2SetScissor(uint32 x, uint32 y, uint32 width, uint32 height)
	{
		GX2ScissorReg scissorReg;
		wwhd::gx2::GX2InitScissorReg(&scissorReg, x, y, width, height);
		wwhd::gx2::GX2SetScissorReg(&scissorReg);
	}

	void GX2SetDepthOnlyControl(bool depthTestEnable, bool depthWriteEnable, LATTE_DB_DEPTH_CONTROL::E_ZFUNC depthFunction)
	{
		// disables any currently set stencil test
		GX2ReserveCmdSpace(3);

		Latte::LATTE_DB_DEPTH_CONTROL reg{};
		reg.set_Z_ENABLE(depthTestEnable);
		reg.set_Z_WRITE_ENABLE(depthWriteEnable);
		reg.set_Z_FUNC(depthFunction);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::DB_DEPTH_CONTROL - 0xA000,
			reg);
	}

	void GX2SetDepthStencilControl(
		bool depthTestEnable, bool depthWriteEnable, LATTE_DB_DEPTH_CONTROL::E_ZFUNC depthFunction,
		bool stencilTestEnable, bool backStencilTestEnable, 
		LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC frontStencilFunction, 
		LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilZPass, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilZFail, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilFail,
		LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC backStencilFunction, 
		LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilZPass, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilZFail, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilFail
	)
	{
		GX2ReserveCmdSpace(3);

		Latte::LATTE_DB_DEPTH_CONTROL reg{};
		reg.set_Z_ENABLE(depthTestEnable).set_Z_WRITE_ENABLE(depthWriteEnable).set_Z_FUNC(depthFunction);
		reg.set_STENCIL_ENABLE(stencilTestEnable).set_BACK_STENCIL_ENABLE(backStencilTestEnable);
		reg.set_STENCIL_FUNC_F(frontStencilFunction).set_STENCIL_FUNC_B(backStencilFunction);
		reg.set_STENCIL_ZPASS_F(frontStencilZPass).set_STENCIL_ZFAIL_F(frontStencilZFail).set_STENCIL_FAIL_F(frontStencilFail);
		reg.set_STENCIL_ZPASS_B(backStencilZPass).set_STENCIL_ZFAIL_B(backStencilZFail).set_STENCIL_FAIL_B(backStencilFail);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::DB_DEPTH_CONTROL - 0xA000,
			reg);
	}

	void GX2InitDepthStencilControlReg(
		GX2DepthStencilControlReg* depthStencilControlReg,
		bool depthTestEnable, bool depthWriteEnable, LATTE_DB_DEPTH_CONTROL::E_ZFUNC depthFunction,
		bool stencilTestEnable, bool backStencilTestEnable,
		LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC frontStencilFunction,
		LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilZPass, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilZFail, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION frontStencilFail,
		LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC backStencilFunction,
		LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilZPass, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilZFail, LATTE_DB_DEPTH_CONTROL::E_STENCILACTION backStencilFail)
	{
		Latte::LATTE_DB_DEPTH_CONTROL reg{};
		reg.set_Z_ENABLE(depthTestEnable).set_Z_WRITE_ENABLE(depthWriteEnable).set_Z_FUNC(depthFunction);
		reg.set_STENCIL_ENABLE(stencilTestEnable).set_BACK_STENCIL_ENABLE(backStencilTestEnable);
		reg.set_STENCIL_FUNC_F(frontStencilFunction).set_STENCIL_FUNC_B(backStencilFunction);
		reg.set_STENCIL_ZPASS_F(frontStencilZPass).set_STENCIL_ZFAIL_F(frontStencilZFail).set_STENCIL_FAIL_F(frontStencilFail);
		reg.set_STENCIL_ZPASS_B(backStencilZPass).set_STENCIL_ZFAIL_B(backStencilZFail).set_STENCIL_FAIL_B(backStencilFail);
		depthStencilControlReg->reg = reg;
	}

	void GX2SetDepthStencilControlReg(GX2DepthStencilControlReg* depthStencilControlReg)
	{
		GX2ReserveCmdSpace(3);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::DB_DEPTH_CONTROL - 0xA000,
			depthStencilControlReg->reg);
	}

	void GX2GetDepthStencilControlReg(
		GX2DepthStencilControlReg* depthStencilControlReg,
		uint32be* depthTestEnable, uint32be* depthWriteEnable, uint32be* depthFunction,
		uint32be* stencilTestEnable, uint32be* backStencilTestEnable,
		uint32be* frontStencilFunction,
		uint32be* frontStencilZPass, uint32be* frontStencilZFail, uint32be* frontStencilFail,
		uint32be* backStencilFunction,
		uint32be* backStencilZPass, uint32be* backStencilZFail, uint32be* backStencilFail)
	{
		// used by Hyrule Warriors

		*depthTestEnable = depthStencilControlReg->reg.value().get_Z_ENABLE();

		*depthWriteEnable = depthStencilControlReg->reg.value().get_Z_WRITE_ENABLE();
		*depthFunction = (uint32)depthStencilControlReg->reg.value().get_Z_FUNC();

		*stencilTestEnable = depthStencilControlReg->reg.value().get_STENCIL_ENABLE();
		*backStencilTestEnable = depthStencilControlReg->reg.value().get_BACK_STENCIL_ENABLE();
	
		*frontStencilFunction = (uint32)depthStencilControlReg->reg.value().get_STENCIL_FUNC_F();
		*backStencilFunction = (uint32)depthStencilControlReg->reg.value().get_STENCIL_FUNC_B();

		*frontStencilZPass = (uint32)depthStencilControlReg->reg.value().get_STENCIL_ZPASS_F();
		*frontStencilZFail = (uint32)depthStencilControlReg->reg.value().get_STENCIL_ZFAIL_F();
		*frontStencilFail = (uint32)depthStencilControlReg->reg.value().get_STENCIL_FAIL_F();

		*backStencilZPass = (uint32)depthStencilControlReg->reg.value().get_STENCIL_ZPASS_B();
		*backStencilZFail = (uint32)depthStencilControlReg->reg.value().get_STENCIL_ZFAIL_B();
		*backStencilFail = (uint32)depthStencilControlReg->reg.value().get_STENCIL_FAIL_B();
	}

	void GX2InitStencilMaskReg(GX2StencilMaskReg* stencilMaskReg, uint8 compareMaskFront, uint8 writeMaskFront, uint8 refFront, uint8 compareMaskBack, uint8 writeMaskBack, uint8 refBack)
	{
		stencilMaskReg->stencilRefMaskFrontReg = LATTE_DB_STENCILREFMASK().set_STENCILREF_F(refFront).set_STENCILMASK_F(compareMaskFront).set_STENCILWRITEMASK_F(writeMaskFront);
		stencilMaskReg->stencilRefMaskBackReg = LATTE_DB_STENCILREFMASK_BF().set_STENCILREF_B(refBack).set_STENCILMASK_B(compareMaskBack).set_STENCILWRITEMASK_B(writeMaskBack);
	}

	void GX2SetStencilMask(uint8 compareMaskFront, uint8 writeMaskFront, uint8 refFront, uint8 compareMaskBack, uint8 writeMaskBack, uint8 refBack)
	{
		GX2ReserveCmdSpace(3 + 3);

		LATTE_DB_STENCILREFMASK frontReg;
		frontReg.set_STENCILREF_F(refFront).set_STENCILMASK_F(compareMaskFront).set_STENCILWRITEMASK_F(writeMaskFront);
		LATTE_DB_STENCILREFMASK_BF backReg;
		backReg.set_STENCILREF_B(refBack).set_STENCILMASK_B(compareMaskBack).set_STENCILWRITEMASK_B(writeMaskBack);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
							REGADDR::DB_STENCILREFMASK - 0xA000,
							frontReg,
							pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
							REGADDR::DB_STENCILREFMASK_BF - 0xA000,
							backReg);
	}

	void GX2SetStencilMaskReg(GX2StencilMaskReg* stencilMaskReg)
	{
		GX2ReserveCmdSpace(3 + 3);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			REGADDR::DB_STENCILREFMASK - 0xA000,
			stencilMaskReg->stencilRefMaskFrontReg,
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			REGADDR::DB_STENCILREFMASK_BF - 0xA000,
			stencilMaskReg->stencilRefMaskBackReg);
	}

	void GX2SetPrimitiveRestartIndex(uint32 restartIndex)
	{
		GX2ReserveCmdSpace(3);
		Latte::LATTE_VGT_MULTI_PRIM_IB_RESET_INDX reg{};
		reg.set_RESTART_INDEX(restartIndex);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX - 0xA000,
			reg);
	}

	void GX2InitTargetChannelMasksReg(GX2TargetChannelMaskReg* reg, GX2_CHANNELMASK t0, GX2_CHANNELMASK t1, GX2_CHANNELMASK t2, GX2_CHANNELMASK t3, GX2_CHANNELMASK t4, GX2_CHANNELMASK t5, GX2_CHANNELMASK t6, GX2_CHANNELMASK t7)
	{
		uint32 targetMask = 0;
		targetMask |= ((t0 & 0xF) << 0);
		targetMask |= ((t1 & 0xF) << 4);
		targetMask |= ((t2 & 0xF) << 8);
		targetMask |= ((t3 & 0xF) << 12);
		targetMask |= ((t4 & 0xF) << 16);
		targetMask |= ((t5 & 0xF) << 20);
		targetMask |= ((t6 & 0xF) << 24);
		targetMask |= ((t7 & 0xF) << 28);
		Latte::LATTE_CB_TARGET_MASK r;
		r.set_MASK(targetMask);
		reg->reg = r;
	}

	void GX2SetTargetChannelMasksReg(GX2TargetChannelMaskReg* reg)
	{
		GX2ReserveCmdSpace(3);

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::CB_TARGET_MASK - 0xA000,
			reg->reg);
	}

	void GX2SetTargetChannelMasks(GX2_CHANNELMASK t0, GX2_CHANNELMASK t1, GX2_CHANNELMASK t2, GX2_CHANNELMASK t3, GX2_CHANNELMASK t4, GX2_CHANNELMASK t5, GX2_CHANNELMASK t6, GX2_CHANNELMASK t7)
	{
		GX2TargetChannelMaskReg tmpReg;
		wwhd::gx2::GX2InitTargetChannelMasksReg(&tmpReg, t0, t1, t2, t3, t4, t5, t6, t7);
		wwhd::gx2::GX2SetTargetChannelMasksReg(&tmpReg);
	}

	static_assert(sizeof(GX2_CHANNELMASK) == 4);

	void GX2GetTargetChannelMasksReg(GX2TargetChannelMaskReg* reg, betype<GX2_CHANNELMASK>* t0, betype<GX2_CHANNELMASK>* t1, betype<GX2_CHANNELMASK>* t2, betype<GX2_CHANNELMASK>* t3,
									betype<GX2_CHANNELMASK>* t4, betype<GX2_CHANNELMASK>* t5, betype<GX2_CHANNELMASK>* t6, betype<GX2_CHANNELMASK>* t7)
	{
		uint32 maskValue = reg->reg.value().get_MASK();
		*t0 = (maskValue >> 0) & 0xF;
		*t1 = (maskValue >> 4) & 0xF;
		*t2 = (maskValue >> 8) & 0xF;
		*t3 = (maskValue >> 12) & 0xF;
		*t4 = (maskValue >> 16) & 0xF;
		*t5 = (maskValue >> 20) & 0xF;
		*t6 = (maskValue >> 24) & 0xF;
		*t7 = (maskValue >> 28) & 0xF;
	}

	void GX2InitBlendControlReg(GX2BlendControlReg* reg, uint32 renderTargetIndex, GX2_BLENDFACTOR colorSrcFactor, GX2_BLENDFACTOR colorDstFactor, GX2_BLENDFUNC colorCombineFunc, uint32 separateAlphaBlend, GX2_BLENDFACTOR alphaSrcFactor, GX2_BLENDFACTOR alphaDstFactor, GX2_BLENDFUNC alphaCombineFunc)
	{
		Latte::LATTE_CB_BLENDN_CONTROL tmpReg;
		tmpReg.set_COLOR_SRCBLEND(colorSrcFactor);
		tmpReg.set_COLOR_DSTBLEND(colorDstFactor);
		tmpReg.set_COLOR_COMB_FCN(colorCombineFunc);
		tmpReg.set_ALPHA_SRCBLEND(alphaSrcFactor);
		tmpReg.set_ALPHA_DSTBLEND(alphaDstFactor);
		tmpReg.set_ALPHA_COMB_FCN(alphaCombineFunc);
		tmpReg.set_SEPARATE_ALPHA_BLEND(separateAlphaBlend != 0);

		reg->index = renderTargetIndex;
		reg->reg = tmpReg;
	}

	void GX2SetBlendControlReg(GX2BlendControlReg* reg)
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			(Latte::REGADDR::CB_BLEND0_CONTROL + (uint32)reg->index) - 0xA000,
			reg->reg
		);
	}

	void GX2SetBlendControl(uint32 renderTargetIndex, GX2_BLENDFACTOR colorSrcFactor, GX2_BLENDFACTOR colorDstFactor, GX2_BLENDFUNC colorCombineFunc, uint32 separateAlphaBlend, GX2_BLENDFACTOR alphaSrcFactor, GX2_BLENDFACTOR alphaDstFactor, GX2_BLENDFUNC alphaCombineFunc)
	{
		GX2BlendControlReg tmpReg;
		wwhd::gx2::GX2InitBlendControlReg(&tmpReg, renderTargetIndex, colorSrcFactor, colorDstFactor, colorCombineFunc, separateAlphaBlend, alphaSrcFactor, alphaDstFactor, alphaCombineFunc);
		wwhd::gx2::GX2SetBlendControlReg(&tmpReg);
	}

	void GX2InitBlendConstantColorReg(GX2BlendConstantColorReg* reg, float red, float green, float blue, float alpha)
	{
		reg->regRed = Latte::LATTE_CB_BLEND_RED().set_RED(red);
		reg->regGreen = Latte::LATTE_CB_BLEND_GREEN().set_GREEN(green);
		reg->regBlue = Latte::LATTE_CB_BLEND_BLUE().set_BLUE(blue);
		reg->regAlpha = Latte::LATTE_CB_BLEND_ALPHA().set_ALPHA(alpha);
	}

	void GX2SetBlendConstantColorReg(GX2BlendConstantColorReg* reg)
	{
		GX2ReserveCmdSpace(6);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 4),
			Latte::REGADDR::CB_BLEND_RED - 0xA000,
			reg->regRed,
			reg->regGreen,
			reg->regBlue,
			reg->regAlpha
		);
	}

	void GX2SetBlendConstantColor(float red, float green, float blue, float alpha)
	{
		GX2BlendConstantColorReg tmpReg;
		wwhd::gx2::GX2InitBlendConstantColorReg(&tmpReg, red, green, blue, alpha);
		wwhd::gx2::GX2SetBlendConstantColorReg(&tmpReg);
	}

	void GX2InitHiStencilInfoRegs(GX2HiStencilInfoReg* hiStencilInfo)
	{
		// seen in Color Splash
		// but the game never calls GX2SetHiStencilInfo thus this has no effect
	}

	void GX2InitPointSizeReg(GX2PointSizeReg* reg, float width, float height)
	{
		if (width < 0.0f || height < 0.0f)
		{
			cemu_assert_suspicious();
		}

		uint32 widthI = (uint32)(width * 8.0f);
		uint32 heightI = (uint32)(height * 8.0f);

		widthI = std::min<uint32>(widthI, 0xFFFF);
		heightI = std::min<uint32>(heightI, 0xFFFF);

		reg->reg = Latte::LATTE_PA_SU_POINT_SIZE().set_WIDTH(widthI).set_HEIGHT(heightI);
	}

	void GX2SetPointSizeReg(GX2PointSizeReg* reg)
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::PA_SU_POINT_SIZE - 0xA000,
			reg->reg
		);
	}

	void GX2SetPointSize(float width, float height)
	{
		GX2PointSizeReg tmpReg;
		wwhd::gx2::GX2InitPointSizeReg(&tmpReg, width, height);
		wwhd::gx2::GX2SetPointSizeReg(&tmpReg);
	}

	void GX2InitPointLimitsReg(GX2PointLimitsReg* reg, float minSize, float maxSize)
	{
		if (minSize < 0.0f || maxSize < 0.0f)
		{
			cemu_assert_suspicious();
		}

		uint32 minSizeI = (uint32)(minSize * 8.0f);
		uint32 maxSizeI = (uint32)(maxSize * 8.0f);

		minSizeI = std::min<uint32>(minSizeI, 0xFFFF);
		maxSizeI = std::min<uint32>(maxSizeI, 0xFFFF);

		reg->reg = Latte::LATTE_PA_SU_POINT_MINMAX().set_MIN_SIZE(minSizeI).set_MAX_SIZE(maxSizeI);
	}

	void GX2SetPointLimitsReg(GX2PointLimitsReg* reg)
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
			Latte::REGADDR::PA_SU_POINT_MINMAX - 0xA000,
			reg->reg
		);
	}

	void GX2SetPointLimits(float minSize, float maxSize)
	{
		GX2PointLimitsReg tmpReg;
		wwhd::gx2::GX2InitPointLimitsReg(&tmpReg, minSize, maxSize);
		wwhd::gx2::GX2SetPointLimitsReg(&tmpReg);
	}

	enum class GX2_SPECIAL_STATE : uint32
	{
		FAST_CLEAR = 0,
		FAST_CLEAR_HIZ = 1,
	};

	void _setSpecialState0(bool isEnabled)
	{
		GX2ReserveCmdSpace(6);
		if (isEnabled)
		{
			// set PA_CL_VTE_CNTL to 0x300
			Latte::LATTE_PA_CL_VTE_CNTL regVTE{};
			regVTE.set_VTX_XY_FMT(true);
			regVTE.set_VTX_Z_FMT(true);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
				Latte::REGADDR::PA_CL_VTE_CNTL - 0xA000,
				regVTE);
			// set PA_CL_CLIP_CNTL to 0x490000
			Latte::LATTE_PA_CL_CLIP_CNTL regClip{};
			regClip.set_CLIP_DISABLE(true); // 0x10000
			regClip.set_DX_CLIP_SPACE_DEF(true); // 0x80000
			regClip.set_DX_RASTERIZATION_KILL(true); // 0x400000
			
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
				Latte::REGADDR::PA_CL_CLIP_CNTL - 0xA000,
				regClip);
		}
		else
		{
			// set PA_CL_VTE_CNTL to 0x43F
			Latte::LATTE_PA_CL_VTE_CNTL reg{};
			reg.set_VPORT_X_OFFSET_ENA(true).set_VPORT_X_SCALE_ENA(true);
			reg.set_VPORT_Y_OFFSET_ENA(true).set_VPORT_Y_SCALE_ENA(true);
			reg.set_VPORT_Z_OFFSET_ENA(true).set_VPORT_Z_SCALE_ENA(true);
			reg.set_VTX_W0_FMT(true);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + 1),
				Latte::REGADDR::PA_CL_VTE_CNTL - 0xA000,
				reg);
			// reset PA_CL_CLIP_CNTL
			wwhd::gx2::GX2SetRasterizerClipControl(true, true);
		}
	}

	void GX2SetSpecialState(GX2_SPECIAL_STATE stateId, uint32 isEnabled)
	{
		if (stateId == GX2_SPECIAL_STATE::FAST_CLEAR)
		{
			wwhd::gx2::_setSpecialState0(isEnabled != 0);
		}
		else if (stateId == GX2_SPECIAL_STATE::FAST_CLEAR_HIZ)
		{
			// todo
			// enables additional flags for special state 0
		}
		else
		{
			// legacy style
			GX2ReserveCmdSpace(3);
			gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_SPECIAL_STATE, 2));
			gx2WriteGather_submitU32AsBE((uint32)stateId); // state id
			gx2WriteGather_submitU32AsBE(isEnabled); // enable/disable bool
		}
	}
}

WWHD_OS_EXPORT(gx2, GX2InitAlphaTestReg, wwhd::gx2::GX2InitAlphaTestReg);
WWHD_OS_EXPORT(gx2, GX2SetAlphaTestReg, wwhd::gx2::GX2SetAlphaTestReg);
WWHD_OS_EXPORT(gx2, GX2SetAlphaTest, wwhd::gx2::GX2SetAlphaTest);
WWHD_OS_EXPORT(gx2, GX2InitColorControlReg, wwhd::gx2::GX2InitColorControlReg);
WWHD_OS_EXPORT(gx2, GX2SetColorControl, wwhd::gx2::GX2SetColorControl);
WWHD_OS_EXPORT(gx2, GX2SetColorControlReg, wwhd::gx2::GX2SetColorControlReg);
WWHD_OS_EXPORT(gx2, GX2InitPolygonControlReg, wwhd::gx2::GX2InitPolygonControlReg);
WWHD_OS_EXPORT(gx2, GX2SetPolygonControlReg, wwhd::gx2::GX2SetPolygonControlReg);
WWHD_OS_EXPORT(gx2, GX2SetPolygonControl, wwhd::gx2::GX2SetPolygonControl);
WWHD_OS_EXPORT(gx2, GX2SetCullOnlyControl, wwhd::gx2::GX2SetCullOnlyControl);
WWHD_OS_EXPORT(gx2, GX2InitPolygonOffsetReg, wwhd::gx2::GX2InitPolygonOffsetReg);
WWHD_OS_EXPORT(gx2, GX2SetPolygonOffsetReg, wwhd::gx2::GX2SetPolygonOffsetReg);
WWHD_OS_EXPORT(gx2, GX2SetPolygonOffset, wwhd::gx2::GX2SetPolygonOffset);
WWHD_OS_EXPORT(gx2, GX2SetRasterizerClipControl, wwhd::gx2::GX2SetRasterizerClipControl);
WWHD_OS_EXPORT(gx2, GX2SetRasterizerClipControlHalfZ, wwhd::gx2::GX2SetRasterizerClipControlHalfZ);
WWHD_OS_EXPORT(gx2, GX2SetRasterizerClipControlEx, wwhd::gx2::GX2SetRasterizerClipControlEx);
WWHD_OS_EXPORT(gx2, GX2InitViewportReg, wwhd::gx2::GX2InitViewportReg);
WWHD_OS_EXPORT(gx2, GX2SetViewportReg, wwhd::gx2::GX2SetViewportReg);
WWHD_OS_EXPORT(gx2, GX2SetViewport, wwhd::gx2::GX2SetViewport);
WWHD_OS_EXPORT(gx2, GX2InitScissorReg, wwhd::gx2::GX2InitScissorReg);
WWHD_OS_EXPORT(gx2, GX2SetScissorReg, wwhd::gx2::GX2SetScissorReg);
WWHD_OS_EXPORT(gx2, GX2GetScissorReg, wwhd::gx2::GX2GetScissorReg);
WWHD_OS_EXPORT(gx2, GX2SetScissor, wwhd::gx2::GX2SetScissor);
WWHD_OS_EXPORT(gx2, GX2InitDepthStencilControlReg, wwhd::gx2::GX2InitDepthStencilControlReg);
WWHD_OS_EXPORT(gx2, GX2SetDepthStencilControlReg, wwhd::gx2::GX2SetDepthStencilControlReg);
WWHD_OS_EXPORT(gx2, GX2GetDepthStencilControlReg, wwhd::gx2::GX2GetDepthStencilControlReg);
WWHD_OS_EXPORT(gx2, GX2SetDepthOnlyControl, wwhd::gx2::GX2SetDepthOnlyControl);
WWHD_OS_EXPORT(gx2, GX2SetDepthStencilControl, wwhd::gx2::GX2SetDepthStencilControl);
WWHD_OS_EXPORT(gx2, GX2InitStencilMaskReg, wwhd::gx2::GX2InitStencilMaskReg);
WWHD_OS_EXPORT(gx2, GX2SetStencilMask, wwhd::gx2::GX2SetStencilMask);
WWHD_OS_EXPORT(gx2, GX2SetStencilMaskReg, wwhd::gx2::GX2SetStencilMaskReg);
WWHD_OS_EXPORT(gx2, GX2SetPrimitiveRestartIndex, wwhd::gx2::GX2SetPrimitiveRestartIndex);
WWHD_OS_EXPORT(gx2, GX2InitTargetChannelMasksReg, wwhd::gx2::GX2InitTargetChannelMasksReg);
WWHD_OS_EXPORT(gx2, GX2SetTargetChannelMasksReg, wwhd::gx2::GX2SetTargetChannelMasksReg);
WWHD_OS_EXPORT(gx2, GX2SetTargetChannelMasks, wwhd::gx2::GX2SetTargetChannelMasks);
WWHD_OS_EXPORT(gx2, GX2GetTargetChannelMasksReg, wwhd::gx2::GX2GetTargetChannelMasksReg);
WWHD_OS_EXPORT(gx2, GX2InitBlendControlReg, wwhd::gx2::GX2InitBlendControlReg);
WWHD_OS_EXPORT(gx2, GX2SetBlendControlReg, wwhd::gx2::GX2SetBlendControlReg);
WWHD_OS_EXPORT(gx2, GX2SetBlendControl, wwhd::gx2::GX2SetBlendControl);
WWHD_OS_EXPORT(gx2, GX2InitBlendConstantColorReg, wwhd::gx2::GX2InitBlendConstantColorReg);
WWHD_OS_EXPORT(gx2, GX2SetBlendConstantColorReg, wwhd::gx2::GX2SetBlendConstantColorReg);
WWHD_OS_EXPORT(gx2, GX2SetBlendConstantColor, wwhd::gx2::GX2SetBlendConstantColor);
WWHD_OS_EXPORT(gx2, GX2InitHiStencilInfoRegs, wwhd::gx2::GX2InitHiStencilInfoRegs);
WWHD_OS_EXPORT(gx2, GX2InitPointSizeReg, wwhd::gx2::GX2InitPointSizeReg);
WWHD_OS_EXPORT(gx2, GX2SetPointSizeReg, wwhd::gx2::GX2SetPointSizeReg);
WWHD_OS_EXPORT(gx2, GX2SetPointSize, wwhd::gx2::GX2SetPointSize);
WWHD_OS_EXPORT(gx2, GX2InitPointLimitsReg, wwhd::gx2::GX2InitPointLimitsReg);
WWHD_OS_EXPORT(gx2, GX2SetPointLimitsReg, wwhd::gx2::GX2SetPointLimitsReg);
WWHD_OS_EXPORT(gx2, GX2SetPointLimits, wwhd::gx2::GX2SetPointLimits);
WWHD_OS_EXPORT(gx2, GX2SetSpecialState, wwhd::gx2::GX2SetSpecialState);
