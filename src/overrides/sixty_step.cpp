// 60 fps (docs/recompiler-design.md D21): the game's per-tick helpers, with a time step.
//
// A converted process (src/overrides/sixty.cpp, WWHD_60FPS_CONVERT) runs its logic every frame with
// g_rtStep = h, the part of a 30 Hz tick a frame is (0.5 at 60 fps); everything else runs on the
// game's whole ticks with g_rtStep = 1, and then each helper here is the game's own function
// (musttail to orig_f_X). At a step h they do what h of a 30 Hz tick does, mostly by calling the
// game's function with its per-tick arguments converted, so its own float arithmetic stays:
//   exponential approach (x += k (t - x), clamped):  k -> 1 - (1 - k)^h, steps and caps x h
//   linear chase (x += step):                        step x h
//   integer versions (s16 angles, u8):               the same, rounded (a step of 1 stays 1)
//   gravity (fopAcM_calcSpeed):                      v += g h
//   movement (fopAcM_posMove):                       p += h v + (1 - h)/2 dv, dv what gravity added
//                                                    this step: the 30 Hz semi-implicit arc exactly
//                                                    at whole ticks
//   animation (J3DFrameCtrl::update):                frame += rate h
// Velocities stay in the game's units (per 30 Hz tick). The helpers' addresses and arguments are in
// D21 ("Where the per-tick steps are"); the GameCube decomp has their source (c_lib.cpp,
// f_op_actor_mng.cpp, J3DAnimation.cpp).
#include "override.h"
#include <bit>
#include <cmath>
#include <unordered_map>

namespace
{
	inline bool Stepped()
	{
		return g_rtStep != 1.0f;
	}

	inline float Step()
	{
		return g_rtStep;
	}

	// k of an exponential approach per 30 Hz tick, for a step of h ticks; outside [0, 1] as it is
	double Approach(double k)
	{
		return k >= 0.0 && k <= 1.0 ? 1.0 - std::pow(1.0 - k, (double)Step()) : k;
	}

	inline double Single(double x)
	{
		return (double)(float)x;                      // a single in an FPR, as the game keeps them
	}

	// an integer step (a divisor or a per-tick amount) for a step of h, rounded, at least 1 if it was
	uint32 ScaledInt(uint32 v, bool divisor)
	{
		const int s = (int)(sint16)v;
		if (s == 0)
			return v;
		double scaled;
		if (divisor)                                   // x += d / s: k = 1/s -> 1/k'
			scaled = 1.0 / Approach(1.0 / std::fabs((double)s));
		else
			scaled = std::fabs((double)s) * Step();
		int r = (int)std::lround(scaled);
		if (r < 1)
			r = 1;
		return (uint32)(s < 0 ? -r : r) & 0xFFFFu;
	}

	float rdf(uint32 ea) { return std::bit_cast<float>(rd32(ea)); }
	void wrf(uint32 ea, float v) { wr32(ea, std::bit_cast<uint32>(v)); }

	// This step (a frame at 60 fps; in the trial, either half step of the tick)
	uint32 StepId()
	{
		return wwhd::os::SwapCount() * 2 + (g_rtHalfTick ? 1 : 0);
	}

	// fopAcM_calcSpeed: what gravity added to each actor's speed.y in which step, for posMove's arc
	// correction in the same step
	struct Fall { float dv; uint32 step; };
	std::unordered_map<uint32, Fall> s_fall;
}

// ---- c_lib (c_lib.cpp): exponential approaches ------------------------------------------------------

// cLib_addCalc(f32* v r3, target f1, scale f2, maxStep f3, minStep f4) -> |target - v| f1
void f_0200ECD4(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
	{
		FPR(2).fp0 = Single(Approach(FPR(2).fp0));
		FPR(3).fp0 = Single(FPR(3).fp0 * Step());
		FPR(4).fp0 = Single(FPR(4).fp0 * Step());
	}
	[[clang::musttail]] return orig_f_0200ECD4(ctx);
}

// cLib_addCalc2(f32* v r3, target f1, scale f2, maxStep f3)
void f_0200ED84(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
	{
		FPR(2).fp0 = Single(Approach(FPR(2).fp0));
		FPR(3).fp0 = Single(FPR(3).fp0 * Step());
	}
	[[clang::musttail]] return orig_f_0200ED84(ctx);
}

// cLib_addCalc0(f32* v r3, scale f1, maxStep f2): toward 0
void f_0200EDC8(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
	{
		FPR(1).fp0 = Single(Approach(FPR(1).fp0));
		FPR(2).fp0 = Single(FPR(2).fp0 * Step());
	}
	[[clang::musttail]] return orig_f_0200EDC8(ctx);
}

// cLib_addCalcPos / cLib_addCalcPosXZ(cXyz* v r3, target r4, scale f1, maxStep f2, minStep f3)
void f_0200EE00(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
	{
		FPR(1).fp0 = Single(Approach(FPR(1).fp0));
		FPR(2).fp0 = Single(FPR(2).fp0 * Step());
		FPR(3).fp0 = Single(FPR(3).fp0 * Step());
	}
	[[clang::musttail]] return orig_f_0200EE00(ctx);
}

void f_0200EF78(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
	{
		FPR(1).fp0 = Single(Approach(FPR(1).fp0));
		FPR(2).fp0 = Single(FPR(2).fp0 * Step());
		FPR(3).fp0 = Single(FPR(3).fp0 * Step());
	}
	[[clang::musttail]] return orig_f_0200EF78(ctx);
}

// cLib_addCalcPos2 / cLib_addCalcPosXZ2(cXyz* v r3, target r4, scale f1, maxStep f2)
void f_0200F164(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
	{
		FPR(1).fp0 = Single(Approach(FPR(1).fp0));
		FPR(2).fp0 = Single(FPR(2).fp0 * Step());
	}
	[[clang::musttail]] return orig_f_0200F164(ctx);
}

void f_0200F268(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
	{
		FPR(1).fp0 = Single(Approach(FPR(1).fp0));
		FPR(2).fp0 = Single(FPR(2).fp0 * Step());
	}
	[[clang::musttail]] return orig_f_0200F268(ctx);
}

// cLib_addCalcAngleS(s16* v r3, target r4, scale r5 (a divisor), maxStep r6, minStep r7)
void f_0200F378(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
	{
		GPR(5) = ScaledInt(GPR(5), true);
		GPR(6) = ScaledInt(GPR(6), false);
		GPR(7) = ScaledInt(GPR(7), false);
	}
	[[clang::musttail]] return orig_f_0200F378(ctx);
}

// cLib_addCalcAngleS2(s16* v r3, target r4, scale r5 (a divisor), maxStep r6)
void f_0200F428(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
	{
		GPR(5) = ScaledInt(GPR(5), true);
		GPR(6) = ScaledInt(GPR(6), false);
	}
	[[clang::musttail]] return orig_f_0200F428(ctx);
}

// ---- c_lib: linear chases ------------------------------------------------------------------------

// cLib_chaseUC(u8* v r3, target r4, step r5), cLib_chaseS(s16* v, target, step), cLib_chaseAngleS
void f_0200F4FC(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
		GPR(5) = ScaledInt(GPR(5), false);
	[[clang::musttail]] return orig_f_0200F4FC(ctx);
}

void f_0200F564(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
		GPR(5) = ScaledInt(GPR(5), false);
	[[clang::musttail]] return orig_f_0200F564(ctx);
}

void f_0200F8D0(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
		GPR(5) = ScaledInt(GPR(5), false);
	[[clang::musttail]] return orig_f_0200F8D0(ctx);
}

// cLib_chaseF(f32* v r3, target f1, step f2)
void f_0200F5C8(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
		FPR(2).fp0 = Single(FPR(2).fp0 * Step());
	[[clang::musttail]] return orig_f_0200F5C8(ctx);
}

// cLib_chasePos / cLib_chasePosXZ(cXyz* v r3, target r4, step f1)
void f_0200F62C(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
		FPR(1).fp0 = Single(FPR(1).fp0 * Step());
	[[clang::musttail]] return orig_f_0200F62C(ctx);
}

void f_0200F764(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
		FPR(1).fp0 = Single(FPR(1).fp0 * Step());
	[[clang::musttail]] return orig_f_0200F764(ctx);
}

// ---- actors (f_op_actor_mng.cpp) -------------------------------------------------------------------

// fopAcM_calcSpeed(actor r3): speed.y += gravity (+0x374), at least maxFallSpeed (+0x378); speed.xz
// from speedF (+0x370) and angle.y. Gravity is per tick squared: scaled by h for the call.
void f_025D67A8(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped())
		[[clang::musttail]] return orig_f_025D67A8(ctx);
	const uint32 actor = GPR(3);
	const uint32 g = rd32(actor + 0x374);
	const float before = rdf(actor + 0x340);
	wrf(actor + 0x374, std::bit_cast<float>(g) * Step());
	orig_f_025D67A8(ctx);
	wr32(actor + 0x374, g);
	s_fall[actor] = { rdf(actor + 0x340) - before, StepId() };
}

// fopAcM_posMove(actor r3, const cXyz* move r4): current.pos (+0x314) += speed (+0x33C), then
// += *move (the collision's push-out, a displacement per tick). With a step h the position takes h
// of the speed and of the push, plus (1 - h)/2 of what gravity added to speed.y in this step
// (fopAcM_calcSpeed): two half steps then land exactly where the game's semi-implicit 30 Hz step
// (v += g; p += v) does, through a jump's impulse too (only gravity's part is spread).
void f_025D6800(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped())
		[[clang::musttail]] return orig_f_025D6800(ctx);
	const uint32 actor = GPR(3);
	const float h = Step();
	const float vx = rdf(actor + 0x33C), vy = rdf(actor + 0x340), vz = rdf(actor + 0x344);
	const auto fall = s_fall.find(actor);
	const float dv = fall != s_fall.end() && fall->second.step == StepId() ? fall->second.dv : 0.0f;
	float x = rdf(actor + 0x314) + vx * h, y = rdf(actor + 0x318) + vy * h + (1.0f - h) * 0.5f * dv, z = rdf(actor + 0x31C) + vz * h;
	if (const uint32 move = GPR(4))
	{
		x += rdf(move) * h;
		y += rdf(move + 4) * h;
		z += rdf(move + 8) * h;
	}
	wrf(actor + 0x314, x);
	wrf(actor + 0x318, y);
	wrf(actor + 0x31C, z);
}

// ---- animation (J3DAnimation.cpp) ------------------------------------------------------------------

// J3DFrameCtrl::update(ctrl r3): mFrame (+4) += mRate (+0), then the loop mode (+0xE). With a step h
// the rate is h of itself for the call; a mode that stops (rate 0) or turns (negates) the
// animation keeps that, in the rate's own units.
void f_027F2FC4(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped())
		[[clang::musttail]] return orig_f_027F2FC4(ctx);
	const uint32 frameCtrl = GPR(3);
	const float rate = rdf(frameCtrl), scaled = rate * Step();
	wrf(frameCtrl, scaled);
	orig_f_027F2FC4(ctx);
	const float after = rdf(frameCtrl);
	wrf(frameCtrl, after == scaled ? rate : after / Step());
}
