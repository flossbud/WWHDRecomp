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

// ---- animation (J3DAnimation.cpp) ------------------------------------------------------------------

// J3DFrameCtrl::checkPass(pass frame f1, ctrl r3): whether the next update (mFrame to mFrame + mRate)
// passes the frame. With a step h the next update covers mRate h, so the rate is h of itself for the
// call: the half steps' windows tile the 30 Hz tick's, and a frame is passed once.
void f_027F2BF8(PPCInterpreter_t* __restrict ctx)
{
	if (!Stepped())
		[[clang::musttail]] return orig_f_027F2BF8(ctx);
	const uint32 frameCtrl = GPR(3);
	const uint32 rate = rd32(frameCtrl);
	wrf(frameCtrl, std::bit_cast<float>(rate) * Step());
	orig_f_027F2BF8(ctx);
	wr32(frameCtrl, rate);
}

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

// dCcD_GStts::Move(stts r3): clears last tick's hit state and counts a byte (+0xA8) down to 0. On a
// half step the count stays.
void f_0251621C(PPCInterpreter_t* __restrict ctx)
{
	if (!HalfStep())
		[[clang::musttail]] return orig_f_0251621C(ctx);
	const uint32 stts = GPR(3);
	const uint8 count = rd8(stts + 0xA8);
	orig_f_0251621C(ctx);
	wr8(stts + 0xA8, count);
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
// blend starts: that one is part of the start, a whole 1.
void f_025E3EC8(PPCInterpreter_t* __restrict ctx)
{
	const uint32 lr = ctx->spr.LR;
	if (!Stepped() || (lr >= 0x025E3F3Cu && lr < 0x025E3FB4u))
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
	struct ToeHistory { Toes last, before; uint32 lastStep = 0; bool haveBefore = false; };
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
// after a pause measures from the last one, half a tick.
void f_023FCB9C(PPCInterpreter_t* __restrict ctx)
{
	const uint32 link = GPR(3);
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
	orig_f_023FCB9C(ctx);
	history.before = now;
	history.haveBefore = true;
	history.lastStep = step;
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
