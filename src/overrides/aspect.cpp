// The game's camera at the TV picture's aspect ratio (b-wide, os/aspect.h): hooks (generate.py's hook:NAME,
// config/US_v0/tick_rules/aspect.txt) at the places the camera's aspect is set and used: the six the rival recomp found
// (docs/research/rival-study.md 4.7) and camera_draw's call giving the HD engine its camera (rt_aspect_engine):
//   camera_execute and the camera's creation store the 16/9 constant (1004AAF0) as view.mAspect: the latched aspect
//   instead (rt_aspect_store, on f0);
//   camera_draw's and the file select scene's C_MTXPerspective, and view_setup's two mDoLib_clipper::setup (the culling
//   frustum, so nothing pops in at the new edges), take f1 the vertical field of view in degrees and f2 the aspect:
//   the latched aspect, and for a picture narrower than 16:9 the vertical field of view widened so that the horizontal
//   one stays (Vert+) (rt_aspect_projection).
// Only an aspect the game itself puts there is changed (16:9, or one of ours a frame old): a cutscene's own isn't. At
// 16:9 nothing is touched, so every check's run is the game's own.
#include "override.h"
#include "../os/aspect.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace
{
	namespace aspect = wwhd::os::aspect;

	void SetF(PPCInterpreter_t* ctx, int n, float v)
	{
		ctx->fpr[n].fp0 = v;
		ctx->fpr[n].fp1 = v;
	}
}

// stfs f0 -> view.mAspect: f0 holds the 16/9 constant
void rt_aspect_store(PPCInterpreter_t* ctx)
{
	const float a = aspect::Current();
	if (aspect::Same(a, aspect::kBase) || !aspect::Same((float)ctx->fpr[0].fp0, aspect::kBase))
		return;
	SetF(ctx, 0, a);
}

namespace
{
	// Vert+: the vertical field of view (degrees) widened so that the horizontal one stays
	double Widened(double fovyDeg, float ky)
	{
		return 2.0 * std::atan(std::tan(fovyDeg * (M_PI / 360.0)) * ky) * (180.0 / M_PI);
	}
}

// bl 0274E04C in camera_draw: the HD engine's camera, f3 = f9 (fovy in degrees) x a constant, f4 the aspect
void rt_aspect_engine(PPCInterpreter_t* ctx)
{
	const float a = aspect::Current();
	if (aspect::Same(a, aspect::kBase) || !aspect::Recent((float)ctx->fpr[4].fp0))
		return;
	SetF(ctx, 4, a);
	float kx, ky;
	aspect::Factors(a, kx, ky);
	const double deg = ctx->fpr[9].fp0, f3 = ctx->fpr[3].fp0;
	if (ky != 1.0f && deg > 0.0 && f3 > 0.0)
		SetF(ctx, 3, (float)(Widened(deg, ky) * (f3 / deg)));
}

// bl C_MTXPerspective / mDoLib_clipper::setup (f1 fovy in degrees, f2 aspect)
void rt_aspect_projection(PPCInterpreter_t* ctx)
{
	const float a = aspect::Current(), have = (float)ctx->fpr[2].fp0;
	if (aspect::Same(have, a) && aspect::Same(a, aspect::kBase))
		return;                                                  // 16:9 (every check)
	if (!aspect::Recent(have))
		return;                                                  // an aspect of the game's own (not its 16:9 camera)
	float kx, ky;
	aspect::Factors(a, kx, ky);
	SetF(ctx, 2, a);
	if (ky != 1.0f)                                              // Vert+: the vertical field of view widened
		SetF(ctx, 1, (float)Widened(ctx->fpr[1].fp0, ky));
}

// ---- the HUD (stage 2): the HD layouts (nw::lyt) at another aspect ratio ------------------------------------------------
// Ported from the rival recomp's runtime/src/aspect.cpp and aspect_panes.h (ZeldaWWHDRecomp, MPL-2.0; rival-study.md
// 4.7), reimplemented on our overrides and command-stream marks:
//   DrawInfo::LoadProjectionMtx 02874038 and the nw::font text draw 028F8250 upload a layout's projection (16 vertex
//   uniform registers): their upload is marked (os::aspect::TagProjection, gx2's 0xFD), and the renderer narrows it for
//   draws into a widened target (draw.cpp): the layout keeps its proportions, centred.
//   Pane::CalculateMtx 028766CC: while a TV layout's root computes its matrices (the renderer reports each root's target:
//   Pane::Draw 02877100 marks it), the HUD's edge panes (hearts, rupees, buttons, keys...) move out by the extra half
//   width (height), the panes placed at an actor's projected position scale with the picture, and leaf panes covering the
//   whole screen (backgrounds, fades) stretch. Translations and scales change only for the call (the pane's dirty bit
//   set, so its matrix is computed again): the game's own values stay. Pane::Draw also keeps hidden the anchored panes
//   the game parks just off the 16:9 screen (they'd show in the extra width).
// Pane (nw::lyt::Pane): +0x0C parent, +0x14 the child list's sentinel (+0 of a child: the next), +0x1C translation x, y,
// +0x34 scale x, y, +0x3C size w, h, +0x44 flags (0x10 the matrix dirty), +0x48 the global 3x4 matrix, +0x80 the name.
namespace
{
	constexpr uint32 kParent = 0x0C, kChildren = 0x14, kTrans = 0x1C, kScale = 0x34, kSize = 0x3C, kFlags = 0x44, kDirty = 0x10,
		kGlobal = 0x48, kName = 0x80;

	float Rdf(uint32 ea) { return std::bit_cast<float>(rd32(ea)); }
	void Wrf(uint32 ea, float v) { wr32(ea, std::bit_cast<uint32>(v)); }
	std::string_view Name(uint32 pane)
	{
		const char* n = (const char*)(memory_base + pane + kName);
		return std::string_view(n, strnlen(n, 24));
	}

	// HUD parts that belong at the picture's edges (only these move out: the rival's list, as it settled)
	bool EdgePane(uint32 pane)
	{
		static const std::string_view kEdge[] = { "N_TV_00", "N_DRC_00", "N_Default_00", "N_HeartPos_01", "L_Rupy_00",
			"L_CompassClock_00", "L_DungeonKey_00", "N_SwimTimePos_00", "L_BossHP_00", "L_Arrow_00", "L_BatteryVol1_00",
			"L_RupySwordCounter_00", "N_Position_00", "N_Time_00" };
		const std::string_view n = Name(pane);
		for (auto e : kEdge)
			if (n == e)
				return true;
		return false;
	}
	bool ProjectedChild(uint32 pane) { return Name(pane) == "L_EnemyHP_00" || Name(pane) == "L_CommandA_00"; }
	bool ProjectedRootName(std::string_view n) { return n == "N_EnemyHP_00" || n == "T_CommandA_00" || n == "P_SetSeatASpecial_00"; }
	bool WorldAnchor(uint32 pane, int depth = 0)              // a root placed at an actor's projected position
	{
		if (depth > 32)
			return false;
		if (ProjectedRootName(Name(pane)))
			return true;
		const uint32 sentinel = pane + kChildren;
		for (uint32 c = rd32(sentinel); c != sentinel && c; c = rd32(c))
			if (WorldAnchor(c, depth + 1))
				return true;
		return false;
	}
	// authored backgrounds and wipes that cover the screen (pause pictures hold the captured game, not a frame)
	bool FillPane(uint32 pane, float x, float y, float w, float h)
	{
		static const std::string_view kFill[] = { "PF_PauseTV_00", "PF_PauseDRC_00", "BootBase_00", "BootDeco_00", "P_Base_00",
			"P_Base_01", "PF_BG_00", "P_BG_00", "W_BG_00", "P_NoDataBG_00", "P_Dark_00", "P_Dark_01", "P_Light_00", "P_Light_01",
			"P_Wave00", "P_Wave_01", "P_OpeningPic_00", "W_Wipe_00", "P_BgPattern_00", "W_Shutter_00", "W_FadeWipe_00",
			"W_FadeWipe_01", "P_FadeWipe_00" };
		const std::string_view n = Name(pane);
		bool named = false;
		for (auto f : kFill)
			named |= n == f;
		return named && std::fabs(x) < 8.0f && std::fabs(y) < 8.0f && w >= 1270.0f && h >= 710.0f;
	}

	std::mutex s_lock;
	std::unordered_map<uint32, std::pair<float, float>> s_offsets;   // what anchoring moved a pane by (Pane::Draw's test)
	std::unordered_map<uint32, std::pair<bool, float>> s_rootCalc;   // the anchoring and aspect a root last computed with
	void SetOffset(uint32 pane, float dx, float dy)
	{
		std::lock_guard guard(s_lock);
		if (dx == 0.0f && dy == 0.0f)
			s_offsets.erase(pane);
		else
			s_offsets[pane] = { dx, dy };
	}
	std::pair<float, float> OffsetOf(uint32 pane)
	{
		std::lock_guard guard(s_lock);
		auto it = s_offsets.find(pane);
		return it == s_offsets.end() ? std::pair<float, float>{ 0.0f, 0.0f } : it->second;
	}
	bool RootChanged(uint32 root, bool anchor, float a)          // its whole tree computed again after a change
	{
		std::lock_guard guard(s_lock);
		auto [it, made] = s_rootCalc.try_emplace(root, anchor, a);
		const bool changed = made || it->second.first != anchor || !aspect::Same(it->second.second, a);
		it->second = { anchor, a };
		return changed;
	}

	thread_local uint32 t_root = 0;                              // the layout root whose matrices are being computed
	thread_local bool t_anchor = false;                          // ... a TV layout at another aspect
}

// DrawInfo::LoadProjectionMtx, the text draw: their projection upload marked
void f_02874038(PPCInterpreter_t* ctx)
{
	if (aspect::Same(aspect::Current(), aspect::kBase))
		[[clang::musttail]] return orig_f_02874038(ctx);
	aspect::TagProjection(1);
	orig_f_02874038(ctx);
	aspect::TagProjection(-1);
}

void f_028F8250(PPCInterpreter_t* ctx)
{
	if (aspect::Same(aspect::Current(), aspect::kBase))
		[[clang::musttail]] return orig_f_028F8250(ctx);
	aspect::TagProjection(1);
	orig_f_028F8250(ctx);
	aspect::TagProjection(-1);
}

// Pane::CalculateMtx(this r3, DrawInfo& r4, bool parentDirty r5)
void f_028766CC(PPCInterpreter_t* ctx)
{
	const float a = aspect::Current();
	static bool s_everOther = false;                             // another aspect was in use (else 16:9 touches nothing)
	if (aspect::Same(a, aspect::kBase) && !t_anchor && !t_root)
	{
		const uint32 pane = (uint32)ctx->gpr[3];
		if (!s_everOther || rd32(pane + kParent) || !RootChanged(pane, false, a))   // back at 16:9: a root's tree once more
			[[clang::musttail]] return orig_f_028766CC(ctx);
		ctx->gpr[5] = 1;
		[[clang::musttail]] return orig_f_028766CC(ctx);
	}
	s_everOther = true;
	float kx, ky;
	aspect::Factors(a, kx, ky);
	const uint32 pane = (uint32)ctx->gpr[3], parent = rd32(pane + kParent);
	if (!parent)                                                 // a layout's root: decided for its whole tree
	{
		const uint32 saveRoot = t_root;
		const bool saveAnchor = t_anchor;
		t_root = pane;
		t_anchor = (kx != 1.0f || ky != 1.0f) && aspect::RootOnTv(pane);
		const float tx = Rdf(pane + kTrans), ty = Rdf(pane + kTrans + 4);
		const bool placed = t_anchor && (tx != 0.0f || ty != 0.0f) && WorldAnchor(pane);
		if (RootChanged(pane, t_anchor, a))
			ctx->gpr[5] = 1;
		if (placed)
			Wrf(pane + kTrans, tx * kx), Wrf(pane + kTrans + 4, ty * ky);
		SetOffset(pane, placed ? tx * (kx - 1.0f) : 0.0f, placed ? ty * (ky - 1.0f) : 0.0f);
		orig_f_028766CC(ctx);
		if (placed)
			Wrf(pane + kTrans, tx), Wrf(pane + kTrans + 4, ty);
		t_root = saveRoot, t_anchor = saveAnchor;
		return;
	}
	if (!t_anchor)
	{
		if (parent == t_root)
			SetOffset(pane, 0.0f, 0.0f);
		[[clang::musttail]] return orig_f_028766CC(ctx);
	}
	const float tx = Rdf(pane + kTrans), ty = Rdf(pane + kTrans + 4), sx = Rdf(pane + kScale), sy = Rdf(pane + kScale + 4);
	float nx = tx, ny = ty, nsx = sx, nsy = sy;

	if (parent == t_root && ProjectedChild(pane))
		nx = tx * kx, ny = ty * ky;
	else if (parent == t_root && EdgePane(pane))
	{
		if (tx <= -300.0f) nx = tx - 640.0f * (kx - 1.0f);
		if (tx >= 300.0f) nx = tx + 640.0f * (kx - 1.0f);
		if (ty <= -200.0f) ny = ty - 360.0f * (ky - 1.0f);
		if (ty >= 200.0f) ny = ty + 360.0f * (ky - 1.0f);
	}
	const uint32 sentinel = pane + kChildren;

	if (rd32(sentinel) == sentinel && FillPane(pane, tx, ty, Rdf(pane + kSize) * std::fabs(sx), Rdf(pane + kSize + 4) * std::fabs(sy)))
		nsx = sx * kx, nsy = sy * ky;                            // a leaf covering the screen: stretched to the new shape
	const bool moved = nx != tx || ny != ty, scaled = nsx != sx || nsy != sy;
	if (parent == t_root)
		SetOffset(pane, nx - tx, ny - ty);
	if (moved || scaled)
		wr8(pane + kFlags, rd8(pane + kFlags) | kDirty);
	if (moved)
		Wrf(pane + kTrans, nx), Wrf(pane + kTrans + 4, ny);
	if (scaled)
		Wrf(pane + kScale, nsx), Wrf(pane + kScale + 4, nsy);
	orig_f_028766CC(ctx);
	if (moved)
		Wrf(pane + kTrans, tx), Wrf(pane + kTrans + 4, ty);
	if (scaled)
		Wrf(pane + kScale, sx), Wrf(pane + kScale + 4, sy);
}

// Pane::Draw's DrawSelf (r3 the pane): content (clipped to the 16:9 picture: menus, and panes parked off its edges) or not
// (fills, the anchored HUD and what is placed at actors)
void rt_aspect_drawself(PPCInterpreter_t* ctx)
{
	if (aspect::Same(aspect::Current(), aspect::kBase))
		return;
	const uint32 pane = (uint32)ctx->gpr[3];
	bool content = true;
	const uint32 sentinel = pane + kChildren;
	if (rd32(sentinel) == sentinel && FillPane(pane, Rdf(pane + kTrans), Rdf(pane + kTrans + 4),
		Rdf(pane + kSize) * std::fabs(Rdf(pane + kScale)), Rdf(pane + kSize + 4) * std::fabs(Rdf(pane + kScale + 4))))
		content = false;
	for (uint32 p = pane, i = 0; p && i < 32; p = rd32(p + kParent), i++)
	{
		const uint32 parent = rd32(p + kParent);
		if (parent && !rd32(parent + kParent) && (EdgePane(p) || ProjectedChild(p)))
			content = false;
		if (!parent && (Rdf(p + kTrans) != 0.0f || Rdf(p + kTrans + 4) != 0.0f) && WorldAnchor(p))
			content = false;
	}
	wwhd::os::LayoutMarker(3, content ? 1 : 0);
}

// Pane::Draw(this r3, DrawInfo& r4): a root's draw marked (the renderer reports its target); an anchored pane the game
// parked off the 16:9 screen stays hidden
void f_02877100(PPCInterpreter_t* ctx)
{
	if (aspect::Same(aspect::Current(), aspect::kBase))
		[[clang::musttail]] return orig_f_02877100(ctx);
	const uint32 pane = (uint32)ctx->gpr[3], parent = rd32(pane + kParent);
	if (!parent)
	{
		wwhd::os::LayoutMarker(2, pane);
		orig_f_02877100(ctx);
		wwhd::os::LayoutMarker(3, 0);                            // the layout's draws are over: nothing clipped after them
		return;
	}
	else
	{
		uint32 child = pane, root = parent;
		for (int i = 0; i < 16 && rd32(root + kParent); i++)
			child = root, root = rd32(root + kParent);
		const bool world = !rd32(root + kParent) && (Rdf(root + kTrans) != 0.0f || Rdf(root + kTrans + 4) != 0.0f) && WorldAnchor(root);
		if (!rd32(root + kParent) && aspect::RootOnTv(root) && (world || EdgePane(child) || ProjectedChild(child)))
		{
			const auto c = OffsetOf(child), r = OffsetOf(root);
			const float gx = Rdf(pane + kGlobal + 12) - c.first - r.first, gy = Rdf(pane + kGlobal + 28) - c.second - r.second;
			if (std::fabs(gx) > 672.0f || std::fabs(gy) > 392.0f)
				return;                                          // parked off the screen by the game
		}
	}
	[[clang::musttail]] return orig_f_02877100(ctx);
}
