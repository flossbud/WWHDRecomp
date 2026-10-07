// 60 fps (docs/recompiler-design.md D21): the game's per-tick helpers, with a time step.
//
// A converted process (src/overrides/sixty.cpp, WWHD_60FPS_CONVERT) runs its logic every frame with
// g_rtStep = h, the part of a 30 Hz tick a frame is (0.5 at 60 fps); everything else runs on the
// game's whole ticks with g_rtStep = 1, and then each helper here is the game's own function
// (musttail to orig_f_X). At a step h they do what h of a 30 Hz tick does, mostly by calling the
// game's function with its per-tick arguments converted, so its own float arithmetic stays:
//   exponential approach (x += k (t - x), clamped):  k -> 1 - (1 - k)^h, steps and caps x h
//   linear chase (x += step):                        step x h
//   integer versions (s16 angles, u8):               a step split between the whole tick and the
//                                                    half tick (s - s/2, then s/2); a divisor s of
//                                                    an approach 2s - 1, then 2s: exact over a tick
//   gravity (fopAcM_calcSpeed):                      v += g h
//   movement (fopAcM_posMove):                       p += h v + (1 - h)/2 dv, dv what gravity added
//                                                    this step: the 30 Hz semi-implicit arc exactly
//                                                    at whole ticks
//   animation (J3DFrameCtrl::update):                frame += rate h
// Velocities stay in the game's units (per 30 Hz tick). The helpers' addresses and arguments are in
// D21 ("Where the per-tick steps are"); the GameCube decomp has their source (c_lib.cpp,
// f_op_actor_mng.cpp, J3DAnimation.cpp).
#include "override.h"
#include <algorithm>
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

	// An integer argument for a half step (h = 1/2: the whole tick's, then the half tick's), returned
	// sign-extended as the game passes it. A per-tick amount s is split, s - s/2 then s/2, so the two
	// add up to s; an approach's divisor s (x += d / s) becomes 2s - 1 then 2s, since
	// (1 - 1/(2s - 1))(1 - 1/(2s)) = 1 - 1/s: two half steps approach as one tick does.
	uint32 ScaledInt(uint32 v, bool divisor)
	{
		const int s = (int)(sint16)v;
		if (s == 0)
			return v;
		int r;
		if (divisor)
		{
			const int a = std::abs(s);
			r = g_rtHalfTick ? 2 * a : 2 * a - 1;
			r = s < 0 ? -r : r;
		}
		else
			r = g_rtHalfTick ? s / 2 : s - s / 2;
		return (uint32)(sint32)r;
	}

	// the same for a 32-bit argument (cLib_addCalcAngleL's)
	uint32 ScaledInt32(uint32 v, bool divisor)
	{
		const sint32 s = (sint32)v;
		if (s == 0)
			return v;
		if (divisor)
		{
			const sint32 a = s < 0 ? -s : s;
			const sint32 r = g_rtHalfTick ? 2 * a : 2 * a - 1;
			return (uint32)(s < 0 ? -r : r);
		}
		return (uint32)(g_rtHalfTick ? s / 2 : s - s / 2);
	}

	// the same for an unsigned byte (cLib_chaseUC's step)
	uint32 ScaledByte(uint32 v)
	{
		const uint32 s = v & 0xFFu;
		return g_rtHalfTick ? s / 2 : s - s / 2;
	}

	float rdf(uint32 ea) { return std::bit_cast<float>(rd32(ea)); }
	void wrf(uint32 ea, float v) { wr32(ea, std::bit_cast<uint32>(v)); }

	// This step (a frame at 60 fps; in the trial, either half step of the tick), numbered so that
	// consecutive steps differ by 1
	uint32 StepId()
	{
		if (wwhd::rt::SixtyFrom() != ~0u)
			return wwhd::os::SwapCount();              // 60 fps: a step a frame
		return wwhd::os::SwapCount() * 2 + (g_rtHalfTick ? 1 : 0);
	}

	// fopAcM_calcSpeed: what gravity added to each actor's speed.y in which step, for posMove's arc
	// correction in the same step
	struct Fall { float dv; uint32 step; };
	std::unordered_map<uint32, Fall> s_fall;
}

// The process whose execute runs (src/overrides/sixty.cpp's fpcM_Execute override sets it)
extern uint32 g_rtActor;

// fall@ step rules (tools/recomp/generate.py): an actor's execute that inlines fopAcM_calcSpeed's
// `speed.y += gravity` (AM2's in each of its states) notes what this step's gravity added, as the
// calcSpeed override does, so posMove's arc correction reaches it too
void rt_step_fall(double dv)
{
	if (g_rtActor)
		s_fall[g_rtActor] = { (float)dv, StepId() };
}

// vnote step rules: speed.y noted for an arc@, the executing actor's first in the step kept: speed.y as the step
// began, before every acceleration the arc spreads. Link's swim lift (changeSwimUpProc, in his procedure) came
// before posMoveFromFootPos's note before gravity, so two half steps moved v + 3/4 of it against 30's v + all of
// it: he sank 1.5 units a tick deeper and came up a tick late (session top)
namespace
{
	std::unordered_map<uint32, Fall> s_vnote;               // dv: the noted speed.y
}
float rt_vnote(float v)
{
	Fall& f = s_vnote[g_rtActor];
	if (f.step != StepId())
		f = { v, StepId() };
	return f.dv;
}

// surf step rules (tools/recomp/generate.py): a height put back on a surface (the water's) that the actor's
// posMove then moves off by speed.y (+0x340). At 30 the tick puts it back and moves it a tick: speed.y + gravity
// (+0x374), at least maxFallSpeed (+0x378). Sinking (speed.y <= 0: each step ends under the surface again and is
// put back again) the put-back keeps (1 - h) of the speed it starts the step with and (1 - h)/2 of what gravity
// adds in it, so that h of the speed after (the posMove override, with its arc correction) ends the step where
// 30's tick ends: on the half step exactly 30's height, and a speed held at maxFallSpeed rests at 30's height on
// both steps (put back on the whole step only, it rested 50 under the surface on the whole step and 100 on the
// half step: the fishman's, outset.txt). Rising (a jump out of the water) the step leaves the surface and isn't
// put back on the next: the plain put-back, two half moves make 30's tick.
double rt_step_surface(double y)
{
	if (!g_rtActor)
		return y;
	const double h = Step(), vy = rdf(g_rtActor + 0x340);
	if (vy > 0.0)
		return y;
	const double vy1 = std::max(vy + h * (double)rdf(g_rtActor + 0x374), (double)rdf(g_rtActor + 0x378));
	return (double)(float)(y + (1.0 - h) * vy + (1.0 - h) * 0.5 * (vy1 - vy));
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

// Approach call sites run once a tick (by their return address): in the whole step unscaled, a tick's approach, and
// left out of the half step, so what reads the value reads 30's for the tick (WWHD_60FPS_ONCEATICK=0 off):
// - 0247E8F0, cLib_addCalcAngleS: the boat's tiller (daShip procSteerMove, below at f_0200F378)
// - 02171158, cLib_addCalc2: the grappling hook's catch, m24B4 (+0x2AE4) toward -144 by 10 a tick (HIMO2 446,
//   new_himo2_move); the coil count starts once it passes -145. At 60 the whole step stopped at -145 and the half
//   step's -144 found the count's add (whole) left out: the coil started a tick late, and the catch, the tug and
//   the swing with it (route gohmatail: Link's swing 0x78 a tick late, 105 units at the end)
namespace
{
	constexpr uint32 kOnceATick[] = { 0x0247E8F0u, 0x02171158u };
	bool OnceATick(uint32 lr)
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_ONCEATICK"); return !(e && atoi(e) == 0); }();
		return on && std::find(std::begin(kOnceATick), std::end(kOnceATick), lr) != std::end(kOnceATick);
	}
}

// cLib_addCalc2(f32* v r3, target f1, scale f2, maxStep f3)
void f_0200ED84(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped() && OnceATick(ctx->spr.LR))
	{
		if (g_rtHalfTick)
			return;
		[[clang::musttail]] return orig_f_0200ED84(ctx);
	}
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
// Call sites whose approach runs once a tick (kOnceATick, by the return address): in the whole step unscaled, a tick's
// approach, and left out of the half step; what reads the value then reads 30's for the tick. The boat's tiller
// (daShip procSteerMove 0247E8EC: m0366 toward the stick's, scale 4): its scaled half steps turned the tiller, and the
// heading it drives (setMoveAngle: shape_angle.y -= m0366 >> 6, split), ~80 units behind 30's through a turn: route
// sail 2 -> 30 units in the turn at f1290-1340. WWHD_60FPS_ONCEATICK=0 off. (kOnceATick: above, with cLib_addCalc2.)
void f_0200F378(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped() && OnceATick(ctx->spr.LR))
	{
		if (g_rtHalfTick)
		{
			GPR(3) = (uint32)(sint32)(sint16)((sint16)GPR(4) - (sint16)rd16(GPR(3)));   // its return: target - value
			return;
		}
		[[clang::musttail]] return orig_f_0200F378(ctx);
	}
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

// cLib_addCalcAngleL(s32* v r3, target r4, scale r5 (a divisor), maxStep r6, minStep r7)
void f_0200F474(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
	{
		GPR(5) = ScaledInt32(GPR(5), true);
		GPR(6) = ScaledInt32(GPR(6), false);
		GPR(7) = ScaledInt32(GPR(7), false);
	}
	[[clang::musttail]] return orig_f_0200F474(ctx);
}

// ---- c_lib: linear chases ------------------------------------------------------------------------

// cLib_chaseUC(u8* v r3, target r4, step r5), cLib_chaseS(s16* v, target, step), cLib_chaseAngleS
void f_0200F4FC(PPCInterpreter_t* __restrict ctx)
{
	if (Stepped())
		GPR(5) = ScaledByte(GPR(5));
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

// daObj::posMoveF_grade (d_a_obj.cpp; actor r3, push r4, stream speed r5, resistance k1 f1 and k2 f2,
// slope normal r6, friction f3, no-grade cos f4, an extra acceleration r7; daObj::posMoveF_stream
// calls it too): what is thrown, rolls or floats (pots, stones, barrels, bombs) changes its speed by
// accelerations a tick (gravity +0x374, the stream's resistance k1 (v - s) + k2 |v - s| (v - s), the
// slope's pull and friction, the extra one), then calls fopAcM_posMove. With a step h each is h of
// itself for the call (gravity and the extra acceleration put back after), and what gravity adds is
// noted for posMove's arc correction, as the calcSpeed override does.
// In a tick-exact scope (s_tickExact: a pot sinking or floating, its water's resistance strong: k1 0.2 and k2 0.02 a
// tick, which two half steps of explicit Euler compound differently, B-hands-on) the whole step takes the tick's
// accelerations whole and the half step none: its speed is 30's tick's from the whole step on, and the two steps
// move half of it each (no arc correction), so the half step ends on 30's tick.
namespace
{
	bool s_tickExact = false;
	bool TickExactOn()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_TICKEXACT"); return !(e && atoi(e) == 0); }();
		return on;
	}
	template <void (*Orig)(PPCInterpreter_t*)>
	void TickExact(PPCInterpreter_t* __restrict ctx)
	{
		if (!Stepped() || !TickExactOn())
			return Orig(ctx);
		s_tickExact = true;
		Orig(ctx);
		s_tickExact = false;
	}
}

void f_023121C4(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped())
		[[clang::musttail]] return orig_f_023121C4(ctx);
	const uint32 actor = GPR(3), accel = GPR(7);
	const float h = s_tickExact ? (g_rtHalfTick ? 0.0f : 1.0f) : Step();
	const uint32 g = rd32(actor + 0x374);
	FPR(1).fp0 = Single(FPR(1).fp0 * h);
	FPR(2).fp0 = Single(FPR(2).fp0 * h);
	FPR(3).fp0 = Single(FPR(3).fp0 * h);
	uint32 a[3] = {};
	if (accel)
		for (int i = 0; i < 3; i++)
		{
			a[i] = rd32(accel + 4 * i);
			wrf(accel + 4 * i, std::bit_cast<float>(a[i]) * h);
		}
	wrf(actor + 0x374, std::bit_cast<float>(g) * h);
	s_fall[actor] = { s_tickExact ? 0.0f : std::bit_cast<float>(g) * h, StepId() };   // posMove, called inside, corrects the arc
	orig_f_023121C4(ctx);
	wr32(actor + 0x374, g);
	if (accel)
		for (int i = 0; i < 3; i++)
			wr32(accel + 4 * i, a[i]);
}

// daTsubo::Act_c::mode_sink (f_024CFAB8: a pot sinking in water, daObj::posMoveF_stream) and mode_afl (f_024CFD3C:
// floating, posMoveF_grade with the water's stream): their moves tick-exact (above)
void f_024CFAB8(PPCInterpreter_t* __restrict ctx)
{
	TickExact<orig_f_024CFAB8>(ctx);
}

void f_024CFD3C(PPCInterpreter_t* __restrict ctx)
{
	TickExact<orig_f_024CFD3C>(ctx);
}

// ---- animation (J3DAnimation.cpp) ------------------------------------------------------------------

// Link's animations run in step with 30's (J3DFrameCtrl::update below): a control's frame at his half step
// is 30's at the tick's end. WWHD_60FPS_ANIMHOLD=0: off (his animations then led by half a tick).
namespace
{
	bool AnimHold()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_ANIMHOLD"); return !(e && atoi(e) == 0); }();
		return on;
	}
	// J3DFrameCtrl (HD): rate +0, frame +4, start +8, end +0xA (s16), loop mode +0xE, state +0xF
	struct CtrlWhole { float frame, rate; sint16 end; uint32 step, held = 0; };
	std::unordered_map<uint32, CtrlWhole> s_ctrlWhole;  // Link's controls as their whole step's update left them
	bool LinkCtrl(uint32 ctrl)
	{
		return g_rtActor && rd16(g_rtActor + 0x08) == 168 && ctrl >= g_rtActor && ctrl < g_rtActor + 0x8000;
	}
	bool AtStart(uint32 ctrl)
	{
		const float frame = rdf(ctrl + 4);
		if (frame == float(sint16(rd16(ctrl + 8))))
			return true;
		return rdf(ctrl) < 0.0f && std::fabs(frame - (float(sint16(rd16(ctrl + 0xA))) - 0.001f)) < 0.0005f;
	}
	uint64 s_animStarts = 0, s_animCarries = 0;
	void AnimHoldStats()
	{
		cemuLog_log(LogType::Force, "wwhd sixty: Link's animations set up in a whole step: {} started (not advanced in the half step), {} carried over (advanced at the whole step's pace)",
			s_animStarts, s_animCarries);
	}
}
// J3DFrameCtrl::checkPass(pass frame f1, ctrl r3): whether the next update (mFrame to mFrame + mRate)
// passes the frame. With a step h the next update covers mRate h, so the rate is h of itself for the
// call: the half steps' windows tile the 30 Hz tick's, and a frame is passed once. Link's controls (in
// step with 30's) look a step further: at 30 a tick's window starts at its end's frame, which his half step
// reaches, so the whole step's window starts a step on (frame + rate h), and the half step's after it;
// with the frame's own window a pass came in the next tick's whole step half the time, and the set-up it
// makes a tick late. A control his half step held at its start (below) has no window: at 30 that tick
// checked before its set-up (procHangMove passing frame 0 set itself up again every tick; session top).
// Under a `tick` rule (g_rtTickWindow, its call made in the whole step only) the window is the whole tick's: from
// where the whole step's starts, a tick's rate long (Link's slash while moving: its speed drop on 30's tick).
bool g_rtTickWindow = false;
void f_027F2BF8(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped())
		[[clang::musttail]] return orig_f_027F2BF8(ctx);
	const uint32 frameCtrl = GPR(3);
	const uint32 rate = rd32(frameCtrl);
	if (AnimHold() && LinkCtrl(frameCtrl))
	{
		const auto it = s_ctrlWhole.find(frameCtrl);
		if (g_rtHalfTick && it != s_ctrlWhole.end() && it->second.held == StepId())
		{
			GPR(3) = 0;
			return;
		}
		const uint32 frame = rd32(frameCtrl + 4);
		const float r = std::bit_cast<float>(rate), ahead = std::bit_cast<float>(frame) + r * Step();
		if (rd8(frameCtrl + 0xE) != 2 && (r >= 0.0f ? ahead >= float(sint16(rd16(frameCtrl + 0xA))) - 0.001f
			: ahead < float(sint16(rd16(frameCtrl + 8)))))
		{
			GPR(3) = 0;                                    // past its end: it stops there
			return;
		}
		wrf(frameCtrl + 4, ahead);
		wrf(frameCtrl, g_rtTickWindow ? r : r * Step());
		orig_f_027F2BF8(ctx);
		wr32(frameCtrl + 4, frame);
		wr32(frameCtrl, rate);
		return;
	}
	wrf(frameCtrl, g_rtTickWindow ? std::bit_cast<float>(rate) : std::bit_cast<float>(rate) * Step());
	orig_f_027F2BF8(ctx);
	wr32(frameCtrl, rate);
}

// J3DFrameCtrl::update(ctrl r3): mFrame (+4) += mRate (+0), then the loop mode (+0xE). With a step h
// the rate is h of itself for the call; a mode that stops (rate 0) or turns (negates) the
// animation keeps that, in the rate's own units.
// Link's animations set up in his whole step after their update (his action call, after animeUpdate):
// at 30 the tick's update runs whole before the call sets one up, at the old animation's pace, so his half
// step finishes it as 30's would. A start (setSingleMoveAnime, setActAnimeUpper, setMoveAnime from frame 0:
// the frame changed since the whole step's update and stands at its start) isn't advanced: that tick
// doesn't advance the new animation (its state cleared, as update does first). A set-up that carries the
// old animation's place over (setMoveAnime: the frame's share of the length, onto the new one; every step
// while he moves, with a new rate) advances at the whole step's update's rate, in the new length's frames
// (rate x end / the whole step's end). A control the whole step didn't update (no animation of its own
// then: the upper body's) and now at its start was set up after it too. Before, the half step advanced a
// start at once and a carried place at the new rate: his animations led 30's by half a tick (a cut's end,
// a bow's draw, a turn's frame then came a tick early half the time: B31, the sword combo's window ran out
// before the next press), and the walk from waiting, carrying a waiting animation's place (3.2 frames a tick
// of the walk's length) on at the walk's 1.2, a frame behind (route tour; session top, animstart).
// WWHD_60FPS_ANIMHOLD=0: off.
void f_027F2FC4(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped())
		[[clang::musttail]] return orig_f_027F2FC4(ctx);
	const uint32 frameCtrl = GPR(3);
	const bool link = AnimHold() && LinkCtrl(frameCtrl);
	const float rate = rdf(frameCtrl);
	float pace = rate;                                 // the rate this step advances at
	if (link && g_rtHalfTick)
	{
		static bool once = [] { atexit(AnimHoldStats); at_quick_exit(AnimHoldStats); return true; }();
		auto it = s_ctrlWhole.find(frameCtrl);
		const sint16 end = sint16(rd16(frameCtrl + 0xA));
		const bool whole = it != s_ctrlWhole.end() && it->second.step == StepId() - 1;
		// set up since its whole step's update; or not updated there (no animation of its own then: Link's
		// upper body's controls), so set up after it
		if (!whole || rdf(frameCtrl + 4) != it->second.frame || rate != it->second.rate || end != it->second.end)
		{
			if (AtStart(frameCtrl))
			{
				wr8(frameCtrl + 0xF, 0);
				if (it == s_ctrlWhole.end())
					it = s_ctrlWhole.emplace(frameCtrl, CtrlWhole{ rdf(frameCtrl + 4), rate, end, 0 }).first;
				it->second.held = StepId();
				s_animStarts++;
				return;                                // started after its whole step's update: from the next tick
			}
			if (whole && it->second.end > 0 && end > 0)
			{
				pace = it->second.rate * float(end) / float(it->second.end);
				s_animCarries += pace != rate;
			}
		}
	}
	const float scaled = pace * Step();
	wrf(frameCtrl, scaled);
	orig_f_027F2FC4(ctx);
	const float after = rdf(frameCtrl);
	wrf(frameCtrl, after == scaled ? rate : after == -scaled ? -rate : after / Step());
	if (link && !g_rtHalfTick)
		s_ctrlWhole[frameCtrl] = { rdf(frameCtrl + 4), rdf(frameCtrl), sint16(rd16(frameCtrl + 0xA)), StepId() };
}

// ---- systems that run on whole ticks --------------------------------------------------------------
// A converted process's half step must not hand work to systems that only run on whole ticks: it
// would be there twice when they next run. And per-tick countdowns count whole ticks.

namespace
{
	inline bool HalfStep()
	{
		return Stepped() && g_rtHalfTick;
	}
}

// cLib_calcTimer<u8>(u8* t r3) -> t: if (*t) --*t; return *t. On a half step it doesn't count.
void f_0207A9A0(PPCInterpreter_t* __restrict ctx)
{
	if (!HalfStep())
		[[clang::musttail]] return orig_f_0207A9A0(ctx);
	GPR(3) = rd8(GPR(3));
}

// dCcD_GObjInf::ClrAtHit(collider r3): clears its last At hit and counts its hit mark's effect counter
// (+0x64, SubtractAtEffCounter: the spark and sound of a hit come back when it or the Tg's is 0) down to 0.
// On a half step the count stays, as ClrTgHit's below.
void f_02516094(PPCInterpreter_t* __restrict ctx)
{
	if (!HalfStep())
		[[clang::musttail]] return orig_f_02516094(ctx);
	const uint32 collider = GPR(3);
	const uint8 count = rd8(collider + 0x64);
	orig_f_02516094(ctx);
	wr8(collider + 0x64, count);
}

// dCcD_GObjInf::ClrTgHit(collider r3): clears its last Tg hit and counts its hit mark's effect counter
// (+0xA8, SubtractTgEffCounter) down to 0. On a half step the count stays.
void f_0251621C(PPCInterpreter_t* __restrict ctx)
{
	if (!HalfStep())
		[[clang::musttail]] return orig_f_0251621C(ctx);
	const uint32 stts = GPR(3);
	const uint8 count = rd8(stts + 0xA8);
	orig_f_0251621C(ctx);
	wr8(stts + 0xA8, count);
}

// dCcS::ChkAtTgHitAfterCross(this r3, setAt r4, setTg r5, at r6, tg r7, atStts r8, tgStts r9, atGStts r10,
// tgGStts on the stack): sets the hitters' ids and is true when a NoConHit collider's lasting contact is
// skipped (the hitter still the old one, dCcD_GStts::Move below). A test aid: WWHD_DEBUG_CONHIT=path logs
// each skip (game frame, half tick, the At's and the Tg's process names; their stts's actor at +0xC), the
// contacts a converted process's extra Move made into new hits every tick.
void f_025185F8(PPCInterpreter_t* __restrict ctx)
{
	static FILE* log = [] { const char* e = getenv("WWHD_DEBUG_CONHIT"); return e ? fopen(e, "w") : nullptr; }();
	static const bool all = getenv("WWHD_DEBUG_CONHIT_ALL") != nullptr;   // every check, skipped or not
	if (!log)
		[[clang::musttail]] return orig_f_025185F8(ctx);
	const uint32 atInf = GPR(6), tgInf = GPR(7), atStts = GPR(8), tgStts = GPR(9), setAt = GPR(4), setTg = GPR(5);
	orig_f_025185F8(ctx);
	if (GPR(3) || all)
	{
		const uint32 at = atStts ? rd32(atStts + 0xC) : 0, tg = tgStts ? rd32(tgStts + 0xC) : 0;
		const auto name = [](uint32 a) { return a >= 0x10000000u && a < 0x50000000u ? (int)rd16(a + 8) : -1; };
		// with all: the skip, setAt/setTg, the At's NoConHit and StopNoConHit bits, the Tg's NoConHit bit
		fprintf(log, "%u %d %d %d %u %u%u %x %x\n", wwhd::rt::GameFrame(wwhd::os::SwapCount()), g_rtHalfTick ? 1 : 0,
			name(at), name(tg), GPR(3), setAt & 1, setTg & 1, rd32(atInf + 0x50) & 5, rd32(tgInf + 0x94) & 2);
		fflush(log);
	}
}

// dCcD_GStts::Move(gstts r3), from an actor's execute: the At and Tg hitters' ids the last resolution set
// become the old ones (+0xC -> +0x10, +0x14 -> +0x18) and the current ones 0. A NoConHit collider skips a
// hitter that is still the old one (ChkAtTgHitAfterCross f_025185F8), so a contact that lasts hits once.
// A converted process's half step moved them again between two resolutions, the old ids then 0: a lasting
// contact hit again every tick (a converted barrier birthing a ripple a tick under Ganondorf; session
// bottom's "contacts"). Not on a half step: the ids move once a tick, before the tick's resolution.
void f_02515E50(PPCInterpreter_t* __restrict ctx)
{
	if (!HalfStep())
		[[clang::musttail]] return orig_f_02515E50(ctx);
}

// dCcS::Set(manager r3, collider r4): enters a collider into the collision manager's lists for this
// tick's resolution (Ccsp()->Move(), whole ticks only). Not on a half step.
void f_0200E240(PPCInterpreter_t* __restrict ctx)
{
	if (!HalfStep())
		[[clang::musttail]] return orig_f_0200E240(ctx);
}

// a per-tick request list (up to 5 entries at +0x44, count at +0x40; full: replaced by priority and
// at random), read by a whole-tick system. Not on a half step.
void f_02516C14(PPCInterpreter_t* __restrict ctx)
{
	if (!HalfStep())
		[[clang::musttail]] return orig_f_02516C14(ctx);
}

// ---- animation blends (m_Do_ext.cpp) ----------------------------------------------------------------

// mDoExt_MtxCalcOldFrame::decOldFrameMorfCounter(r3): the blend from the old pose, counted down 1 a
// tick (+4), the rate of the old pose recomputed from it (+0xC, +0x10, +0x14). The model's last joint
// calls it every calc (each step at 60 fps), so with a step h it counts h: the old pose's weight
// then falls on the 30 Hz line at whole ticks. initOldFrameMorf (f_025E3F3C) calls it once when a
// blend starts: that one is part of the start, a whole 1 when the blend starts in a half step (the
// tick's one calc then comes). Started in a whole step it counts h: at 30 the start and the tick's
// one calc are (m-1)/m of the old pose; at 60 the whole and the half step's calcs together are that
// tick's calc (with a whole 1, (m-1.5)/m: the old pose faded half a tick early, and Link's feet, whose
// move is his speed as he starts walking, with it: route tour's first walking tick 2.05 against 30's
// 1.41, now 1.44; session top, animstart). WWHD_60FPS_ANIMHOLD=0: off.
void f_025E3EC8(PPCInterpreter_t* __restrict ctx)
{
	const uint32 lr = ctx->spr.LR;
	const bool start = lr >= 0x025E3F3Cu && lr < 0x025E3FB4u;
	if (!Stepped() || (start && (g_rtHalfTick || !AnimHold())))
		[[clang::musttail]] return orig_f_025E3EC8(ctx);
	const uint32 counter = GPR(3) + 4;
	const float c = rdf(counter);
	if (c > 0.0f)
		wrf(counter, c + (1.0f - Step()));          // the game's - 1 then makes it - h
	orig_f_025E3EC8(ctx);
}

// ---- Link (d_a_player_main.cpp) ----------------------------------------------------------------------

namespace
{
	// daPy_lk_c::posMoveFromFootPos's toe positions (mFootData[i].field_0x018, three floats at Link
	// +0x7408 and +0x7520): the last two steps' for each Link, so a step's move is measured over a whole
	// tick
	struct Toes { float pos[2][3]; };
	// the model's matrices posMoveFromFootPos reads (m37B4, Link +0x73BC; the waist's, the left and the right foot's,
	// joints 30, 34, 39 of the matrices at *(*(*(Link +0x448) +0x2C) +0x10), 0x30 bytes each), as the whole step saw them
	struct FootMatrices { uint32 step = 0; uint8 base[0x30]; uint8 joint[3][0x30]; };
	struct ToeHistory { Toes last, before; uint32 lastStep = 0; bool haveBefore = false; FootMatrices whole; float smooth = 0, stick = 0; };
	constexpr uint32 kFootJoint[3] = { 0x5A0u, 0x660u, 0x750u };
	uint32 FootJoints(uint32 link)
	{
		const uint32 model = rd32(link + 0x448);
		const uint32 holder = model ? rd32(model + 0x2C) : 0;
		return holder ? rd32(holder + 0x10) : 0;
	}
	void SwapFootMatrices(uint32 link, FootMatrices& m)
	{
		const uint32 joints = FootJoints(link);
		if (!joints)
			return;
		uint8 t[0x30];
		memcpy(t, memory_base + link + 0x73BC, 0x30);
		memcpy(memory_base + link + 0x73BC, m.base, 0x30);
		memcpy(m.base, t, 0x30);
		for (int i = 0; i < 3; i++)
		{
			memcpy(t, memory_base + joints + kFootJoint[i], 0x30);
			memcpy(memory_base + joints + kFootJoint[i], m.joint[i], 0x30);
			memcpy(m.joint[i], t, 0x30);
		}
	}
	std::unordered_map<uint32, ToeHistory> s_toes;
	constexpr uint32 kToe[2] = { 0x7408u, 0x7520u };

	Toes ReadToes(uint32 link)
	{
		Toes t;
		for (int i = 0; i < 2; i++)
			for (int c = 0; c < 3; c++)
				t.pos[i][c] = rdf(link + kToe[i] + 4 * c);
		return t;
	}

	void WriteToes(uint32 link, const Toes& t)
	{
		for (int i = 0; i < 2; i++)
			for (int c = 0; c < 3; c++)
				wrf(link + kToe[i] + 4 * c, t.pos[i][c]);
	}
}

// daPy_lk_c::posMoveFromFootPos(Link r3): Link's speed from his feet, the planted toe's move since the
// last step (in model space, |dx dz|), then gravity and the move. With half steps a step's move is
// measured from the toe of two steps before, a whole tick: at whole ticks that is the 30 Hz
// measurement exactly, and the feet's small step-to-step jitter (their ground fitting) isn't
// doubled as a half step's move divided by h would be (the speed ran 20-30% high). The first step
// after a pause measures from the last one, half a tick. The toes come from the model's matrices as
// the last frame left them, and a half step's last frame is the whole tick's draw, half a tick on: its
// measure ran half a tick ahead of 30's (a walk's start ramped early, Link ~1 unit ahead from there,
// route tour; session top, walkstart). So a half step measures with the whole step's matrices (put in
// for the call, its own put back after): both of a tick's steps take 30's measure. Its smoothing (move * 0.3 +
// m359C (+0x69F4) * 0.7 when the feet's share is under 1 and the stick hasn't moved since the last call (+0x6A0C),
// the result kept in m359C) was an approach per step (k@/d@ rules), so with the same measure in both steps the whole
// step's speed was half-way there, high while he slows: a room's walk-in moved ~0.4 units a tick too far and stopped
// early on its distance check (B59), and an L-targeting release slid further (B58). Now 30's smoothing in both
// steps, the half step's from the m359C and the stick before the whole step: both take 30's speed for the tick.
extern bool g_rtLinkGroundLost;
void f_023FCB9C(PPCInterpreter_t* __restrict ctx)
{
	const uint32 link = GPR(3);
	if (g_rtLinkGroundLost)
		return;                                        // the ground lost: he waits for his next whole step (sixty.cpp's GroundHold)
	if (!Stepped())
	{
		s_toes.erase(link);
		[[clang::musttail]] return orig_f_023FCB9C(ctx);
	}
	const uint32 step = StepId();
	ToeHistory& history = s_toes[link];
	const Toes now = ReadToes(link);                 // as the last step left them
	const bool continuing = history.lastStep != 0 && step - history.lastStep == 1;
	if (!continuing)
		history.haveBefore = false;
	if (history.haveBefore)
		WriteToes(link, history.before);               // the move is measured from a tick ago
	const uint32 joints = FootJoints(link);
	const bool wholeMatrices = g_rtHalfTick && continuing && history.whole.step == step - 1 && joints;
	const bool smoothFromWhole = g_rtHalfTick && continuing;
	const float stickNow = rdf(link + 0x6A0C);
	if (!g_rtHalfTick)
	{
		history.smooth = rdf(link + 0x69F4);           // the smoothing's last move and stick before the tick, for the half step
		history.stick = stickNow;
	}
	else if (smoothFromWhole)
	{
		wrf(link + 0x69F4, history.smooth);            // the half step smooths from them too: the whole step's speed again
		wrf(link + 0x6A0C, history.stick);
	}
	if (!g_rtHalfTick && joints)
	{
		history.whole.step = step;                     // the whole step's matrices, for the half step
		memcpy(history.whole.base, memory_base + link + 0x73BC, 0x30);
		for (int i = 0; i < 3; i++)
			memcpy(history.whole.joint[i], memory_base + joints + kFootJoint[i], 0x30);
	}
	if (wholeMatrices)
		SwapFootMatrices(link, history.whole);         // in for the call ...
	orig_f_023FCB9C(ctx);
	if (wholeMatrices)
		SwapFootMatrices(link, history.whole);         // ... and the half step's own back
	if (smoothFromWhole)
		wrf(link + 0x6A0C, stickNow);                  // the stick's own back
	history.before = now;
	history.haveBefore = true;
	history.lastStep = step;
}

// Link's walking speed (B61, session top; the main session's priority, found by session bottom on route tour): at
// 30 a tick's move takes the speed mNormalSpeed (+0x6A14) ends the tick with; at 60 the whole step moved at the
// half-way one (half the tick's acceleration), and his feet's share and foot-measured speed with it (setBlendMoveAnime
// from mNormalSpeed), so a walk's start ramp left him ~1.2 units a tick behind, ~4 units in all (tour f966-970),
// and an exact-tick camera following him turned off 30's. The plain walk's speed (setNormalSpeedF from
// setSpeedAndAngleNormal) takes the whole tick's change in the whole step (a step of 1) and none in the half step:
// both of a tick's steps move at 30's speed for the tick. (The aim walk f_02416B70 passes steps it divided by h
// itself: not this.) WWHD_60FPS_SPEEDTICK=0 turns it off.
namespace
{
	bool s_inNormalSpeed = false;                  // in setSpeedAndAngleNormal's or procAtnMove's call
	bool SpeedTick()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_SPEEDTICK"); return !(e && atoi(e) == 0); }();
		return on;
	}
}

// daPy_lk_c::setSpeedAndAngleNormal(Link r3, ...): the plain walk's heading (current.angle.y +0x322, toward the
// stick's angle m34E8 by cLib_addCalcAngleS), the speed it reads from it (cos(m34E8 - heading)) and the facing drawn
// (shape_angle.y +0x32A, its own approach). B57: the half steps' turns rounded to whole angle units apart, and
// the whole step's speed read a half-turned heading, so his heading ended a few units off 30's after a turn, the
// camera following him took it up (it follows his facing) and his path drifted: route tour, with the exact camera,
// 30 units of heading off and 9 units aside by f1470, where 30's Link stops on a wall and 60's slides past. So the
// whole call once a tick, in the whole step with a step of 1: heading, speed and facing 30's for the tick. The
// facing drawn is put half way for the whole step's frame and at the tick's in the half step (a turn still
// animated at 60), unless something else turned him between.
namespace
{
	struct TurnHalf { uint32 step = 0; uint16 mid = 0, end = 0; };
	std::unordered_map<uint32, TurnHalf> s_turnHalf;
}
void f_0241650C(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped() || !SpeedTick())
		[[clang::musttail]] return orig_f_0241650C(ctx);
	const uint32 link = GPR(3);
	if (g_rtHalfTick)
	{
		const auto it = s_turnHalf.find(link);
		if (it != s_turnHalf.end() && it->second.step == StepId() - 1 && rd16(link + 0x32A) == it->second.mid)
			wr16(link + 0x32A, it->second.end);
		return;                                        // the whole step took the tick's turn and speed
	}
	const uint16 before = rd16(link + 0x32A);
	const float step = g_rtStep;
	const bool outer = s_inNormalSpeed;
	g_rtStep = 1.0f;
	s_inNormalSpeed = true;
	orig_f_0241650C(ctx);
	s_inNormalSpeed = outer;
	g_rtStep = step;
	const uint16 end = rd16(link + 0x32A);
	const uint16 mid = (uint16)(before + (sint16)(end - before) / 2);
	wr16(link + 0x32A, mid);
	s_turnHalf[link] = TurnHalf{ StepId(), mid, end };
}

// daPy_lk_c::procMoveTurn(Link r3): the turn while moving (0x18; the known issue "actions ending on an animation a tick
// early", session top): his facing (shape_angle.y) approaches his heading by cLib_addCalcAngleS, and checkNextMode
// ends the turn once they're equal. Stepped, the approach's half steps reached it half a tick sooner and the turn
// ended in a half step a tick before 30's (route moveturn's 180-degree flick: 0x18 -> 6 at 60's key 2285 against
// 30's tick 1143). So the call's whole step with a step of 1 (the approach a tick's, its end test 30's) and the
// approach left out of the half step (tick_rules link_actions.txt), whose end test then reads the same facing; the
// facing drawn half way for the whole step's frame, as setSpeedAndAngleNormal's.
void f_0241B050(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped() || !SpeedTick())
		[[clang::musttail]] return orig_f_0241B050(ctx);
	const uint32 link = GPR(3);
	if (g_rtHalfTick)
	{
		const auto it = s_turnHalf.find(link);
		if (it != s_turnHalf.end() && it->second.step == StepId() - 1 && rd16(link + 0x32A) == it->second.mid)
			wr16(link + 0x32A, it->second.end);       // the tick's facing back for its end test
		[[clang::musttail]] return orig_f_0241B050(ctx);
	}
	const uint16 before = rd16(link + 0x32A);
	const float step = g_rtStep;
	g_rtStep = 1.0f;
	orig_f_0241B050(ctx);
	g_rtStep = step;
	if (rd32(link + 0x65F0) != 0x18)
		return;                                        // the turn ended: the next action's own facing
	const uint16 end = rd16(link + 0x32A);
	const uint16 mid = (uint16)(before + (sint16)(end - before) / 2);
	wr16(link + 0x32A, mid);
	s_turnHalf[link] = TurnHalf{ StepId(), mid, end };
}

// daPy_lk_c::procAtnMove(Link r3): the L-targeting walk (B58, the owner's note N4): WWHD's setSpeedAndAngleAtn
// (f_02417538) calls setNormalSpeedF itself. Its speed change too once a tick, in the whole step: at 30 releasing
// L while moving takes the atn walk's whole drop (12 to 6) on the tick it switches to procMove; at 60 the whole
// step took half of it (12 to 8.5) and procMove's slower rule the rest, so Link slid on a tick longer. Not in
// f_02417538 itself: the aims call it too, then multiply mNormalSpeed by 1.2 each step (link_items.txt).
void f_02419BF0(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped() || !SpeedTick())
		[[clang::musttail]] return orig_f_02419BF0(ctx);
	const bool outer = s_inNormalSpeed;
	s_inNormalSpeed = true;
	orig_f_02419BF0(ctx);
	s_inNormalSpeed = outer;
}

// daPy_lk_c::procWHideMove(Link r3, 0x15): sidling along a wall, its speed by setNormalSpeedF too (the stick's
// share of the HIO's sidle speed, steps 0.5 to 2.0): at 60 its rise came in each step, the half step's end ahead of 30's
// tick by up to 2.3 units (the known issue's hands-on check: route sidleboth, owner_drc). Once a tick as above.
void f_0242AFB8(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped() || !SpeedTick())
		[[clang::musttail]] return orig_f_0242AFB8(ctx);
	const bool outer = s_inNormalSpeed;
	s_inNormalSpeed = true;
	orig_f_0242AFB8(ctx);
	s_inNormalSpeed = outer;
}

// daShip_c::execute (ship r3): the boat takes a new mode in checkNextMode when its mNextMode (+0x636, u8) differs from
// its mode, a request Link's ship actions make in his execute (procShipSteer_init: ship->setSteerMove()), and its
// execute ends with mNextMode = mCurMode (+0x635). At 30 the boat runs before him and takes a tick's request in the
// next tick; at 60 its half step took the request his whole step had just made, a tick early: route sail's mast
// animation (procSteerMove_init) started in the half step of f1110 where 30 starts it in f1111, and the launch (its
// frame 7) came a tick early, 52 units at the end (predeploy's last WARN). A request made since the boat's whole step
// is kept from its half step (mNextMode as that step left it) and put back after, for its next whole step.
// WWHD_60FPS_SHIPHOLD=0 off.
namespace
{
	struct ShipNext { uint32 tick; uint8 mode; };
	std::unordered_map<uint32, ShipNext> s_shipNext;    // mNextMode as the boat's whole step left it
	bool ShipHold()
	{
		static const bool on = [] { const char* e = getenv("WWHD_60FPS_SHIPHOLD"); return !(e && atoi(e) == 0); }();
		return on;
	}
}
void f_02477A24(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped() || !ShipHold())
		[[clang::musttail]] return orig_f_02477A24(ctx);
	const uint32 ship = GPR(3), tick = wwhd::rt::GameFrame(wwhd::os::SwapCount());
	if (!g_rtHalfTick)
	{
		orig_f_02477A24(ctx);
		s_shipNext[ship] = { tick, rd8(ship + 0x636) };
		return;
	}
	const uint8 request = rd8(ship + 0x636);
	const auto it = s_shipNext.find(ship);
	if (it == s_shipNext.end() || it->second.tick != tick || it->second.mode == request)
		[[clang::musttail]] return orig_f_02477A24(ctx);
	wr8(ship + 0x636, it->second.mode);
	orig_f_02477A24(ctx);
	wr8(ship + 0x636, request);                        // for its next whole step, as 30's next tick
}

// daPy_lk_c::setNormalSpeedF(Link r3, target f1, ...): mNormalSpeed toward the stick's speed
void f_02416230(PPCInterpreter_t* __restrict ctx)
{
	if (!s_inNormalSpeed || !Stepped())
		[[clang::musttail]] return orig_f_02416230(ctx);
	if (g_rtHalfTick)
		return;                                        // the whole step took the tick's change
	const float step = g_rtStep;
	g_rtStep = 1.0f;
	orig_f_02416230(ctx);
	g_rtStep = step;
}

// ---- the camera (d_camera.cpp) ----------------------------------------------------------------------

namespace
{
	// dCamera_c::updateMonitor's last player position (mMonitor.mPos, dCamera +0x22C): the last two
	// steps' for each camera
	struct Position { float p[3]; };
	struct PositionHistory { Position before; uint32 lastStep = 0; bool haveBefore = false; };
	std::unordered_map<uint32, PositionHistory> s_monitor;

	Position ReadPosition(uint32 ea)
	{
		return { { rdf(ea), rdf(ea + 4), rdf(ea + 8) } };
	}

	void WritePosition(uint32 ea, const Position& p)
	{
		for (int c = 0; c < 3; c++)
			wrf(ea + 4 * c, p.p[c]);
	}
}

// dCamera_c::updateMonitor(camera r3): mMonitor.x (+0x238) = the player's horizontal move since the
// last update, measured from mMonitor.mPos (+0x22C), then mPos = the player's position. With half
// steps the move is measured from the position of two steps before, a whole tick: right at whole
// ticks, and right when something unconverted moves the player on whole ticks only (the boat moved
// Link 33 units at a whole tick and none at the half tick: per step and divided by h, the camera
// read 66 and 0 and held still through a turn). The first step after a pause measures from the last.
void f_024F9A48(PPCInterpreter_t* __restrict ctx)
{
	const uint32 camera = GPR(3);
	if (!Stepped())
	{
		s_monitor.erase(camera);
		[[clang::musttail]] return orig_f_024F9A48(ctx);
	}
	const uint32 step = StepId();
	PositionHistory& history = s_monitor[camera];
	const uint32 mPos = camera + 0x22C;
	const Position last = ReadPosition(mPos);
	if (history.lastStep == 0 || step - history.lastStep != 1)
		history.haveBefore = false;
	if (history.haveBefore)
		WritePosition(mPos, history.before);
	orig_f_024F9A48(ctx);
	history.before = last;
	history.haveBefore = true;
	history.lastStep = step;
}

// ---- particles (JParticle, d_particle.cpp) ----------------------------------------------------------

// JPABaseEmitter::calcCreatePtcls (emitter r3): emission. With the particle calc converted
// (WWHD_60FPS_PARTICLES) an emitter emits on whole ticks' cadence (its rate step's timer counts
// ticks: tick_rules.txt), except the two strips drawn through their particles: the boat's wake
// (dPa_trackEcallBack, callback vtable 0x10052268: execute f_025A9E6C chases the alpha by 5 and 10
// and lays the particles on the water, draw f_025AA12C a triangle strip through each 3) and its bow
// waves (dPa_waveEcallBack, vtable 0x100521A8: executeAfter f_025A92F4, draw f_025A9508 a strip
// through each one). A strip's newest particles are its end at the boat; at a tick's cadence that
// end lagged the boat by half a tick on half ticks and the wake flickered. They emit every frame at
// a frame's count (twice the particles: the same strip through twice the points; their draws spread
// the texture over however many there are, and the wake's room, 150 segments, holds the 80 that 40
// ticks of life make).
void f_0281F878(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped())
		[[clang::musttail]] return orig_f_0281F878(ctx);
	const uint32 cb = rd32(GPR(3) + 0x1E4);
	const uint32 vtable = cb ? rd32(cb) : 0;
	if (vtable != 0x10052268u && vtable != 0x100521A8u)
		[[clang::musttail]] return orig_f_0281F878(ctx);
	const float step = g_rtStep;
	g_rtStep = 1.0f;
	orig_f_0281F878(ctx);
	g_rtStep = step;
}


// ---- the Tower of the Gods' water (session top, the owner's note N9) -------------------------------------

// daObjTide::Act_c::mode_norm (tide r3; Obj_Tide 39): its height is home.y + (1 - M_now) x its rise, M_now the
// water level Tag_Waterlevel (471) keeps in a static (0x101D5F28; its state bits at 0x101D5F2C). The tide
// executes before the tag, so at 30 it shows the level the tag left a tick before. Both converted at 60, a step
// of the tide read the level the tag's step before it left, half a tick old: smooth, but ~0.6 tick ahead of 30's
// water, and each change ended 1.5 ticks early (88.5 ticks against 90). A stepped tide reads the level and state
// as its own last step saw them, a step older: at half ticks 30's water, at whole ticks halfway to it.
void f_023A14A8(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped())
		[[clang::musttail]] return orig_f_023A14A8(ctx);
	constexpr uint32 kNow = 0x101D5F28u, kState = 0x101D5F2Cu;
	struct Seen { uint32 now, state, step; };
	static std::unordered_map<uint32, Seen> s_seen;
	const uint32 tide = GPR(3), step = StepId(), now = rd32(kNow), state = rd32(kState);
	const auto it = s_seen.find(tide);
	const bool older = it != s_seen.end() && step - it->second.step == 1;
	if (older)
	{
		wr32(kNow, it->second.now);
		wr32(kState, it->second.state);
	}
	orig_f_023A14A8(ctx);
	if (older)
	{
		wr32(kNow, now);
		wr32(kState, state);
	}
	s_seen[tide] = { now, state, step };
}
