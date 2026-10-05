// Helpers for recompiled Espresso code (see docs/recompiler-design.md D2/D3).
// Generated code works directly on Cemu's PPCInterpreter_t (`ctx`) and Cemu's flat guest memory,
// and reuses Cemu's FPU helpers (fres/frsqrte tables, quantisation), so that results match the
// interpreter bit for bit. Compile with Cemu's include paths and `-include Common/precompiled.h`.
#pragma once
#include <atomic>
#include <bit>
#include <cstring>
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterHelper.h"

extern uint8* memory_base;

// Store journal: while g_rtJournalOn is set, every store first hands the runtime the range it is
// about to overwrite and the guest instruction making it (0 from hand-written code), so that a
// diff-mode native run can be rewound (D8.2), so that a real-time fast path can tell whether a call
// wrote anything (the quiet watch, D19), or for the 60 fps tools (D21). Off otherwise.
extern bool g_rtJournalOn;
void rt_journal_store(uint32 ea, uint32 size, uint32 pc);
#define RT_STORE(ea, n, pc) do { if (g_rtJournalOn) [[unlikely]] rt_journal_store((ea), (n), (pc)); } while (0)

// 60 fps (D21): instructions listed in config/US_v0/tick_rules.txt run only on the game's whole
// ticks. g_rtHalfTick is set for a frame that falls between two of them (60 fps only; never at 30
// fps, so there every instruction runs). g_rtStep is the time step of a converted step, in 30 Hz
// ticks (1 whenever 60 fps is off). g_rtSixty is set while the game runs 60 frames a second (from
// WWHD_60FPS_FROM's swap on). The runtime owns them (src/runtime/dispatch.cpp, set by
// src/overrides/sixty.cpp).
extern bool g_rtHalfTick;
extern float g_rtStep;
extern bool g_rtSixty;
#define RT_WHOLE_TICK() (!g_rtHalfTick)
// `late` tick rules: once a tick, at its end: the half tick when the code steps at 60 (g_rtStep
// below 1 on both of its frames), the whole tick when it runs whole ticks (30 fps, an unconverted
// process, a converted one in an event)
#define RT_LATE_TICK() (g_rtHalfTick || g_rtStep == 1.0f)
// A `late` store that a stepping process's whole tick passes over is noted while g_rtLateNotes is set
// (its value then, by the generated code's else branch: late_wr8 and on, below); if an event's edge
// then holds that process's half step, src/overrides/sixty.cpp makes the noted stores there, so the
// tick's once-a-tick stores aren't lost (a count would fall a tick behind 30's for the whole event)
extern bool g_rtLateNotes;
void rt_late_note(uint32 ea, uint32 size, uint64 value);
#define RT_SIXTY() (g_rtSixty)
// `hold` tick rules: skipped while g_rtHold is up (src/overrides/sixty.cpp raises it around a step that must
// leave the instruction out: Link's half step after his whole step changed his action skips the action's call)
extern bool g_rtHold;
#define RT_HOLD() (g_rtHold)
// Step rules (config/US_v0/tick_rules.txt, tools/recomp/generate.py) for code run with a time step
#define RT_STEPPED() (g_rtStep != 1.0f)
static inline double rt_step_mul(double x) { return (double)(float)(x * g_rtStep); }
static inline double rt_step_div(double x) { return (double)(float)(x / g_rtStep); }
static inline double rt_step_approach(double k) { return k >= 0.0 && k <= 1.0 ? (double)(float)(1.0 - __builtin_pow(1.0 - k, (double)g_rtStep)) : k; }
static inline double rt_step_damp(double d) { return d > 0.0 ? (double)(float)__builtin_pow(d, (double)g_rtStep) : d; }
// k75: a factor k whose approach the code then follows with an approach of 0.75 (the camera's eye):
// together they move 0.75 k a tick, so a step takes approach(0.75 k) / approach(0.75) here
static inline double rt_step_approach75(double k)
{
	if (!(k >= 0.0 && k <= 1.0))
		return k;
	const double h = (double)g_rtStep;
	return (double)(float)((1.0 - __builtin_pow(1.0 - 0.75 * k, h)) / (1.0 - __builtin_pow(0.25, h)));
}
static inline uint32 rt_step_split(uint32 v) { const sint32 s = (sint32)v; return (uint32)(g_rtHalfTick ? s / 2 : s - s / 2); }
extern float g_rtNote;                 // a value noted by a step rule for a later one in the same step
// vec@ / arc@: for one call, the vector (three floats) at ea is h of itself; for arc@ (a velocity)
// y is h y + (1 - h)/2 (y - g_rtNote), g_rtNote its value before gravity was added this step. The
// vector is put back after the call (outside the store journal: nothing of it is left).
static inline float rt_vec_rd(uint32 ea) { uint32 v; memcpy(&v, memory_base + ea, 4); v = __builtin_bswap32(v); float f; memcpy(&f, &v, 4); return f; }
static inline void rt_vec_wr(uint32 ea, float f) { uint32 v; memcpy(&v, &f, 4); v = __builtin_bswap32(v); memcpy(memory_base + ea, &v, 4); }
static inline void rt_step_vec_begin(uint32 ea, float saved[3], bool arc)
{
	const float h = g_rtStep;
	for (int i = 0; i < 3; i++)
		saved[i] = rt_vec_rd(ea + 4 * i);
	rt_vec_wr(ea, saved[0] * h);
	rt_vec_wr(ea + 4, arc ? saved[1] * h + (1.0f - h) * 0.5f * (saved[1] - g_rtNote) : saved[1] * h);
	rt_vec_wr(ea + 8, saved[2] * h);
}
static inline void rt_step_vec_end(uint32 ea, const float saved[3])
{
	for (int i = 0; i < 3; i++)
		rt_vec_wr(ea + 4 * i, saved[i]);
}
// ssplit@: for one call, the s16 at ea (an angular speed a cSAngle's += is passed by address) is split
// between the whole tick and the half tick as rt_step_split splits a register, and put back after
// (outside the store journal, as vec@'s vector)
static inline uint16 rt_step_s16_begin(uint32 ea)
{
	uint16 v; memcpy(&v, memory_base + ea, 2); v = __builtin_bswap16(v);
	const uint16 s = (uint16)rt_step_split((uint32)(sint32)(sint16)v);
	const uint16 b = __builtin_bswap16(s); memcpy(memory_base + ea, &b, 2);
	return v;
}
static inline void rt_step_s16_end(uint32 ea, uint16 saved)
{
	const uint16 b = __builtin_bswap16(saved); memcpy(memory_base + ea, &b, 2);
}

// Guest time (design D6, revised for M4): every instruction costs one cycle of the thread's
// timeslice, exactly as in Cemu's `while ((--remainingCycles) >= 0)` loop. When the slice is used
// up, the thread yields right here, before the instruction at pc, and runs it once scheduled again
// (rt_yield switches fibers in place; the native frames wait on the fiber's stack).
[[gnu::cold]] void rt_yield(PPCInterpreter_t* ctx, uint32 pc);
#define RT_TICK(pc) do { if (--ctx->remainingCycles < 0) [[unlikely]] rt_yield(ctx, (pc)); } while (0)
// Per basic block (tools/recomp/generate.py): a block of n instructions that fits in the slice (no
// RT_TICK in it would yield) is charged at once and runs unchecked; otherwise its checked copy runs.
#define RT_FITS(n) (ctx->remainingCycles >= (sint32)(n))
#define RT_CHARGE(n) (ctx->remainingCycles -= (sint32)(n))

// Indirect calls (bctrl, bctr): straight to the recompiled function when CTR holds the entry of one
// that runs natively, else the runtime (rt_call_ctr/rt_jump_ctr: HLE trampolines, patched code,
// the interpreter), exactly as the runtime's Dispatch would. rt_direct has one slot per guest code
// word; the runtime fills it at install (src/runtime/dispatch.cpp).
using rt_fn = void (*)(PPCInterpreter_t*);
extern rt_fn* rt_direct;
extern uint32 rt_directBase, rt_directWords;
static inline rt_fn rt_direct_at(uint32 target)
{
	uint32 w = (target - rt_directBase) >> 2;
	return w < rt_directWords ? rt_direct[w] : nullptr;
}
#define RT_CALL_CTR() do { rt_fn f_ = rt_direct_at(ctx->spr.CTR & ~3u); if (f_) [[likely]] f_(ctx); else rt_call_ctr(ctx); } while (0)
#define RT_JUMP_CTR() do { rt_fn f_ = rt_direct_at(ctx->spr.CTR & ~3u); \
	if (f_) [[likely]] { [[clang::musttail]] return f_(ctx); } [[clang::musttail]] return rt_jump_ctr(ctx); } while (0)

#define GPR(n) ctx->gpr[n]
#define FPR(n) ctx->fpr[n]
#define CRB(n) ctx->cr[n]

// Cemu's fcmpu_espresso (PPCInterpreterFPU.cpp), inline: CR field crfD from comparing a and b (LT,
// GT, EQ, or SO when unordered), FPSCR's FPCC, and VXSNAN for a signalling NaN. The fuzzer checks it
// against Cemu's interpreter (fcmpu, ps_cmpu0/1, ps_cmpo0).
static inline void rt_fcmpu(PPCInterpreter_t* ctx, int crfD, double a, double b)
{
	uint64 ia, ib;
	memcpy(&ia, &a, 8);
	memcpy(&ib, &b, 8);
	uint32 c = (IS_NAN(ia) || IS_NAN(ib)) ? 1 : a < b ? 8 : a > b ? 4 : 2;
	ctx->cr[crfD + CR_BIT_LT] = (c >> 3) & 1;
	ctx->cr[crfD + CR_BIT_GT] = (c >> 2) & 1;
	ctx->cr[crfD + CR_BIT_EQ] = (c >> 1) & 1;
	ctx->cr[crfD + CR_BIT_SO] = c & 1;
	if (IS_SNAN(ia) || IS_SNAN(ib))
		ctx->fpscr |= FPSCR_VXSNAN;
	ctx->fpscr = (ctx->fpscr & 0xffff0fff) | (c << 12);
}

// ---- memory (big-endian guest, identity-mapped at memory_base) ----------------------------------
static inline uint8 rd8(uint32 ea) { return memory_base[ea]; }
static inline uint16 rd16(uint32 ea) { uint16 v; memcpy(&v, memory_base + ea, 2); return __builtin_bswap16(v); }
static inline uint32 rd32(uint32 ea) { uint32 v; memcpy(&v, memory_base + ea, 4); return __builtin_bswap32(v); }
static inline uint64 rd64(uint32 ea) { uint64 v; memcpy(&v, memory_base + ea, 8); return __builtin_bswap64(v); }
static inline void wr8(uint32 ea, uint8 v, uint32 pc = 0) { RT_STORE(ea, 1, pc); memory_base[ea] = v; }
static inline void wr16(uint32 ea, uint16 v, uint32 pc = 0) { RT_STORE(ea, 2, pc); v = __builtin_bswap16(v); memcpy(memory_base + ea, &v, 2); }
static inline void wr32(uint32 ea, uint32 v, uint32 pc = 0) { RT_STORE(ea, 4, pc); v = __builtin_bswap32(v); memcpy(memory_base + ea, &v, 4); }
static inline void wr64(uint32 ea, uint64 v, uint32 pc = 0) { RT_STORE(ea, 8, pc); v = __builtin_bswap64(v); memcpy(memory_base + ea, &v, 8); }
static inline void zero_line(uint32 ea, uint32 pc = 0) { ea &= ~31u; RT_STORE(ea, 32, pc); memset(memory_base + ea, 0, 32); }   // dcbz
// a `late` store passed over on a stepping whole tick: noted, not made (RT_LATE_TICK, above)
static inline void late_wr8(uint32 ea, uint8 v, uint32 = 0) { if (g_rtLateNotes) rt_late_note(ea, 1, v); }
static inline void late_wr16(uint32 ea, uint16 v, uint32 = 0) { if (g_rtLateNotes) rt_late_note(ea, 2, v); }
static inline void late_wr32(uint32 ea, uint32 v, uint32 = 0) { if (g_rtLateNotes) rt_late_note(ea, 4, v); }
static inline void late_wr64(uint32 ea, uint64 v, uint32 = 0) { if (g_rtLateNotes) rt_late_note(ea, 8, v); }

// ---- condition register ------------------------------------------------------------------------
static inline void cr_record(PPCInterpreter_t* ctx, uint32 r)       // Rc=1: CR0 from a result
{
	ctx->cr[CR_BIT_SO] = ctx->xer_so;
	ctx->cr[CR_BIT_LT] = (sint32)r < 0;
	ctx->cr[CR_BIT_EQ] = r == 0;
	ctx->cr[CR_BIT_GT] = (sint32)r > 0;
}

template<typename T>
static inline void cr_compare(PPCInterpreter_t* ctx, int crf, T a, T b)
{
	uint8* c = ctx->cr + crf * 4;
	c[CR_BIT_LT] = a < b;
	c[CR_BIT_GT] = a > b;
	c[CR_BIT_EQ] = a == b;
	c[CR_BIT_SO] = ctx->xer_so;
}

static inline uint32 cr_pack(PPCInterpreter_t* ctx)
{
	uint32 v = 0;
	for (int i = 0; i < 32; i++)
		v = (v << 1) | (ctx->cr[i] != 0);
	return v;
}

// ---- floating point (mirrors PPCInterpreterFPU.cpp / PPCInterpreterPS.cpp) ----------------------
static inline uint64 fctiw_result(double b, bool toZero)
{
	uint64 v;
	if (b > (double)0x7FFFFFFF)
		v = 0x7FFFFFFF;
	else if (b < -(double)0x80000000)
		v = 0x80000000;
	else if (toZero)
		v = (uint64)(uint32)(sint32)b;
	else
	{
		double t = b + 0.5;
		sint32 i = (sint32)t;
		if (t - i < 0 || (t - i == 0 && b > 0))
			i--;
		v = (uint64)(uint32)i;
	}
	uint64 r = 0xFFF8000000000000ULL | v;
	uint64 bits;
	memcpy(&bits, &b, 8);
	if (v == 0 && (bits >> 63))
		r |= 0x100000000ULL;
	return r;
}

// psq_l / psq_st element sizes by GQR type: 4,6 = byte, 5,7 = half, else word
static inline uint32 psq_size(sint32 type) { return (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4; }

static inline uint32 psq_read(uint32 ea, sint32 type)
{
	uint32 d;
	switch (psq_size(type))
	{
	case 1: d = rd8(ea); if (type == 6 && (d & 0x80)) d |= 0xFFFFFF00; break;
	case 2: d = rd16(ea); if (type == 7 && (d & 0x8000)) d |= 0xFFFF0000; break;
	default: d = rd32(ea); break;
	}
	return d;
}

static inline void psq_write(uint32 ea, sint32 type, uint32 v, uint32 pc)
{
	switch (psq_size(type))
	{
	case 1: wr8(ea, (uint8)v, pc); break;
	case 2: wr16(ea, (uint16)v, pc); break;
	default: wr32(ea, v, pc); break;
	}
}

static inline void psq_load(PPCInterpreter_t* ctx, int frD, uint32 ea, int gqr, bool single)
{
	sint32 type = (ctx->spr.UGQR[gqr] >> 16) & 7;
	uint8 scale = (ctx->spr.UGQR[gqr] >> 24) & 0x3F;
	uint32 d0 = psq_read(ea, type);
	if (single)
	{
		ctx->fpr[frD].fp0 = (double)dequantize(d0, type, scale);
		ctx->fpr[frD].fp1 = 1.0f;
		return;
	}
	uint32 d1 = psq_read(ea + psq_size(type), type);
	ctx->fpr[frD].fp0 = (double)dequantize(d0, type, scale);
	ctx->fpr[frD].fp1 = (double)dequantize(d1, type, scale);
}

static inline void psq_store(PPCInterpreter_t* ctx, int frS, uint32 ea, int gqr, bool single, uint32 pc = 0)
{
	sint32 type = ctx->spr.UGQR[gqr] & 7;
	uint8 scale = (ctx->spr.UGQR[gqr] >> 8) & 0x3F;
	psq_write(ea, type, quantize((float)ctx->fpr[frS].fp0, type, scale), pc);
	if (!single)
		psq_write(ea + psq_size(type), type, quantize((float)ctx->fpr[frS].fp1, type, scale), pc);
}

// ---- things the runtime provides (not the generated code) --------------------------------------
void rt_trap(PPCInterpreter_t* ctx, uint32 ea);                // tw/twi: guest assertion traps
void rt_dcache_flush(uint32 ea);                               // dcbf/dcbst: GX2 buffer coherency
