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
// The recompiled program (WWHD_RT_GENERATED, src/CMakeLists.txt) reads the base through a constant: under
// -fno-strict-aliasing a guest store may alias any global, so through memory_base every access after a store reloads
// it; a constant global no store can change. The runtime sets the variable behind it once, before any recompiled code
// runs (dispatch.cpp BuildDirectTable), and the base never moves after.
#if defined(WWHD_RT_GENERATED)
extern uint8* const rt_memBase __asm__("wwhd_rt_membase");
#define RT_MEM rt_memBase
#else
#define RT_MEM memory_base
#endif

// Store journal: while g_rtJournalOn is set, every store first hands the runtime the range it is
// about to overwrite and the guest instruction making it (0 from hand-written code), so that a
// diff-mode native run can be rewound (D8.2), so that a real-time fast path can tell whether a call
// wrote anything (the quiet watch, D19), or for the 60 fps tools (D21). Off otherwise.
// While the half tick's journal is on (60 fps, src/overrides/sixty.cpp), it keeps only some 4 KB pages, and a store
// into neither of the pages it touches is passed over here, without the call (most of a half tick's frame's stores
// are into the heap); a fast path's quiet watch needs every store regardless (g_rtStoreAll).
// The inline test is one TLS load: g_rtStoreTable (u8-journal), a byte per 4 KB page of the 32-bit space, non-zero
// for a page whose stores go on; null while the journal is off; every byte set for a journal that takes every store
// (a diff run's, a quiet watch's). The runtime derives it from the switches (diff.cpp rt_store_table_update) whenever
// one changes. Before, the test read three TLS switches and a 4 KB-page table of words, out of line, on each of the
// ~1M stores of a half frame: ~1.5 ms of en-tn's 7.4 on the desktop worker. The switches and the table are constinit
// (their first values are constants): generated code reads them straight from the thread's TLS block (U0).
extern thread_local constinit bool g_rtJournalOn;          // per host thread (docs/research/threads.md)
extern thread_local constinit const uint8* g_rtStoreTable;
void rt_journal_store(uint32 ea, uint32 size, uint32 pc);
#define RT_STORE(ea, n, pc) do { if (const uint8* rt_t = g_rtStoreTable) [[unlikely]] { \
	if (rt_t[(uint32)(ea) >> 12] | rt_t[((uint32)(ea) + (n) - 1) >> 12]) \
		rt_journal_store((ea), (n), (pc)); } } while (0)

// 60 fps (D21): instructions listed in config/US_v0/tick_rules.txt run only on the game's whole
// ticks. g_rtHalfTick is set for a frame that falls between two of them (60 fps only; never at 30
// fps, so there every instruction runs). g_rtStep is the time step of a converted step, in 30 Hz
// ticks (1 whenever 60 fps is off). g_rtSixty is set while the game runs 60 frames a second (from
// WWHD_60FPS_FROM's swap on). The runtime owns them (src/runtime/dispatch.cpp, set by
// src/overrides/sixty.cpp).
extern bool g_rtHalfTick;
// U1 (docs/research/uncapped-plan.md): a tick is g_rtSteps frames (2 at 60 fps; WWHD_STEPS=N for 30 N), the frame's
// place in it g_rtSubStep (0: the whole tick's frame; g_rtHalfTick is set on all the others). g_rtEarlyStep is set
// on a frame that is neither the whole tick's nor its last (N > 2 only: at 2 it is never set, and every rule reads as
// before)
extern uint32 g_rtSteps, g_rtSubStep;
extern bool g_rtEarlyStep;
extern thread_local constinit float g_rtStep;   // the per-step values are per host thread (docs/research/threads.md)
// `tick` rules: a call made in a stepping process's whole step checks the whole tick ahead (sixty_step.cpp)
extern thread_local constinit bool g_rtTickWindow;
extern bool g_rtSixty;
#define RT_WHOLE_TICK() (!g_rtHalfTick)
// `late` tick rules: once a tick, at its end: the tick's last frame when the code steps at 60 (g_rtStep
// below 1 on all of its frames), the whole tick when it runs whole ticks (30 fps, an unconverted
// process, a converted one in an event)
#define RT_LATE_TICK() ((g_rtHalfTick && !g_rtEarlyStep) || g_rtStep == 1.0f)
// A `late` store that a stepping process's whole tick passes over is noted while g_rtLateNotes is set
// (its value then, by the generated code's else branch: late_wr8 and on, below); if an event's edge
// then holds that process's half step, src/overrides/sixty.cpp makes the noted stores there, so the
// tick's once-a-tick stores aren't lost (a count would fall a tick behind 30's for the whole event)
extern thread_local constinit bool g_rtLateNotes;
void rt_late_note(uint32 ea, uint32 size, uint64 value);
#define RT_SIXTY() (g_rtSixty)
// `hold` tick rules: skipped while g_rtHold is up (src/overrides/sixty.cpp raises it around a step that must
// leave the instruction out: Link's half step after his whole step changed his action skips the action's call)
extern thread_local constinit bool g_rtHold;
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
// surf (src/overrides/sixty_step.cpp): a height put back on a surface, with (1 - h) of the executing actor's
// speed.y (and of gravity's part) so that its posMove's step ends where 30's tick does
double rt_step_surface(double y);
// split: a tick's whole s over its steps, step i taking q(N - i) - q(N - i - 1) with q(k) = s k / N (truncated, as
// the game's own division): the steps sum to s, and at N 2 that is s - s / 2 and s / 2
static inline uint32 rt_step_split(uint32 v)
{
	const sint32 s = (sint32)v;
	if (g_rtSteps == 2) [[likely]]
		return (uint32)(g_rtHalfTick ? s / 2 : s - s / 2);
	const sint64 n = g_rtSteps, i = g_rtHalfTick ? g_rtSubStep : 0;
	return (uint32)(sint32)((sint64)s * (n - i) / n - (sint64)s * (n - i - 1) / n);
}
// lag (U2): a count kept to whole ticks, seen on step i of N, is (N - 1 - i)/N of a count ahead of where the tick's
// steps have taken it: lag/lagi/lagw/drawlagi take that much back (1/2 on the whole tick's step at N 2, 0 on the last
// step), lagdown gives it back; drawlag (a count the whole tick's draw already moved on, in a later step's draw) takes
// (N - i)/N back (1/2 at N 2). The integer forms truncate, as the game's division (at N 2: IMM/2, as before)
static inline uint32 rt_step_index() { return g_rtHalfTick ? g_rtSubStep : 0; }
static inline double rt_step_lag()
{
	if (g_rtSteps == 2) [[likely]]
		return g_rtHalfTick ? 0.0 : 0.5;
	return (double)(g_rtSteps - 1 - rt_step_index()) / g_rtSteps;
}
static inline double rt_step_drawlag()
{
	if (g_rtSteps == 2) [[likely]]
		return 0.5;
	return (double)(g_rtSteps - rt_step_index()) / g_rtSteps;
}
static inline uint32 rt_step_lagi(sint32 v)
{
	if (g_rtSteps == 2) [[likely]]
		return g_rtHalfTick ? 0 : (uint32)(v / 2);
	return (uint32)(sint32)((sint64)v * (g_rtSteps - 1 - rt_step_index()) / g_rtSteps);
}
// halfadd's modes (U2; each is h at N 2): on, a draw's i/N of a tick on (h is half a tick's); back, an execute's
// (N - i)/N of a tick back; key, a value of each step's own (h + (i - 1)(h >> 3): distinct for i up to 8)
static inline uint32 rt_step_halfadd_on(sint32 h)
{
	return g_rtSteps == 2 ? (uint32)h : (uint32)(sint32)((sint64)h * 2 * rt_step_index() / g_rtSteps);
}
static inline uint32 rt_step_halfadd_back(sint32 h)
{
	return g_rtSteps == 2 ? (uint32)h : (uint32)(sint32)((sint64)h * 2 * (g_rtSteps - rt_step_index()) / g_rtSteps);
}
static inline uint32 rt_step_halfadd_key(uint32 h) { return h + (rt_step_index() - 1) * (h >> 3); }
extern thread_local constinit float g_rtNote;                 // a value noted by a step rule for a later one in the same step
// vec@ / arc@: for one call, the vector (three floats) at ea is h of itself; for arc@ (a velocity)
// y is h y + (1 - h)/2 (y - g_rtNote), g_rtNote its value before gravity was added this step. The
// vector is put back after the call (outside the store journal: nothing of it is left).
static inline float rt_vec_rd(uint32 ea) { uint32 v; memcpy(&v, RT_MEM + ea, 4); v = __builtin_bswap32(v); float f; memcpy(&f, &v, 4); return f; }
static inline void rt_vec_wr(uint32 ea, float f) { uint32 v; memcpy(&v, &f, 4); v = __builtin_bswap32(v); memcpy(RT_MEM + ea, &v, 4); }
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
	uint16 v; memcpy(&v, RT_MEM + ea, 2); v = __builtin_bswap16(v);
	const uint16 s = (uint16)rt_step_split((uint32)(sint32)(sint16)v);
	const uint16 b = __builtin_bswap16(s); memcpy(RT_MEM + ea, &b, 2);
	return v;
}
static inline void rt_step_s16_end(uint32 ea, uint16 saved)
{
	const uint16 b = __builtin_bswap16(saved); memcpy(RT_MEM + ea, &b, 2);
}

// Guest time (design D6, revised for M4): every instruction costs one cycle of the thread's
// timeslice, exactly as in Cemu's `while ((--remainingCycles) >= 0)` loop. When the slice is used
// up, the thread yields right here, before the instruction at pc, and runs it once scheduled again
// (rt_yield switches fibers in place; the native frames wait on the fiber's stack).
[[gnu::cold]] void rt_yield(PPCInterpreter_t* ctx, uint32 pc);
#define RT_TICK(pc) do { if (--ctx->remainingCycles < 0) [[unlikely]] rt_yield(ctx, (pc)); } while (0)
// Per basic block (tools/recomp/generate.py): a block of n instructions that fits in the slice (no
// RT_TICK in it would yield) is charged at once and runs unchecked; otherwise its checked copy runs.
#if defined(WWHD_RT_NOCOUNT)
// t-codegen's measurement only (RECOMP_OPT=-DWWHD_RT_NOCOUNT): no cycle counting in blocks, so what it costs; the
// guest then yields only in OS calls (no slices, no virtual clock): not a build to play or check
#define RT_FITS(n) true
#define RT_CHARGE(n) ((void)0)
#define RT_LOOP(pc) ((void)0)
#elif defined(WWHD_RT_LOOSE)
// The real-time build (t-codegen): blocks are still charged but never checked (no second, checked copy: the code is
// ~43% smaller); a thread whose slice ran out yields at the next function entry or loop header (RT_LOOP), so a few
// blocks late. Real time only: the virtual clock needs the yield on the exact instruction (the counting build)
#define RT_FITS(n) true
#define RT_CHARGE(n) (ctx->remainingCycles -= (sint32)(n))
#undef RT_TICK
#define RT_TICK(pc) (--ctx->remainingCycles)
#define RT_LOOP(pc) do { if (ctx->remainingCycles < 0) [[unlikely]] rt_yield(ctx, (pc)); } while (0)
#else
#define RT_FITS(n) (ctx->remainingCycles >= (sint32)(n))
#define RT_CHARGE(n) (ctx->remainingCycles -= (sint32)(n))
#define RT_LOOP(pc) ((void)0)
#endif

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
static inline uint8 rd8(uint32 ea) { return RT_MEM[ea]; }
static inline uint16 rd16(uint32 ea) { uint16 v; memcpy(&v, RT_MEM + ea, 2); return __builtin_bswap16(v); }
static inline uint32 rd32(uint32 ea) { uint32 v; memcpy(&v, RT_MEM + ea, 4); return __builtin_bswap32(v); }
static inline uint64 rd64(uint32 ea) { uint64 v; memcpy(&v, RT_MEM + ea, 8); return __builtin_bswap64(v); }
static inline void wr8(uint32 ea, uint8 v, uint32 pc = 0) { RT_STORE(ea, 1, pc); RT_MEM[ea] = v; }
static inline void wr16(uint32 ea, uint16 v, uint32 pc = 0) { RT_STORE(ea, 2, pc); v = __builtin_bswap16(v); memcpy(RT_MEM + ea, &v, 2); }
static inline void wr32(uint32 ea, uint32 v, uint32 pc = 0) { RT_STORE(ea, 4, pc); v = __builtin_bswap32(v); memcpy(RT_MEM + ea, &v, 4); }
static inline void wr64(uint32 ea, uint64 v, uint32 pc = 0) { RT_STORE(ea, 8, pc); v = __builtin_bswap64(v); memcpy(RT_MEM + ea, &v, 8); }
static inline void zero_line(uint32 ea, uint32 pc = 0) { ea &= ~31u; RT_STORE(ea, 32, pc); memset(RT_MEM + ea, 0, 32); }   // dcbz
// Android (m-android A4): the app's library is loaded by dlopen, so a thread_local is dynamic TLS there, every access a
// call into bionic's resolver (tlsdesc_resolver_dynamic), and the store test's g_rtStoreTable was read that way on every
// guest store (4.8% of the game's core on the device). The generated code reads it through its context instead: ctx->
// rspTemp (Cemu's recompiler's scratch, which saves and restores it) holds the address of the host thread's
// g_rtStoreTable, set as a thread is loaded onto a core (coreinit_Thread.cpp __OSLoadThread, on the host thread that
// runs it); a context without it reads the thread_local as before. The stores' macros are at the end of this file
// (WWHD_RT_CTX_TABLE: on by default on Android; -DWWHD_RT_CTX_TABLE elsewhere to test it, e.g. on the arm64 worker)
#if defined(__ANDROID__) && !defined(WWHD_RT_CTX_TABLE)
#define WWHD_RT_CTX_TABLE 1
#endif
#if defined(WWHD_RT_CTX_TABLE) && defined(WWHD_RT_GENERATED)
const uint8* rt_store_table_tls();   // diff.cpp: the thread_local, out of line (inline, its address was taken on every
                                     // store anyway, the resolver's call made before the test that needed it)
static inline const uint8* rt_store_table(const PPCInterpreter_t* ctx)
{
	if (const void* p = ctx->rspTemp) [[likely]]
		return *static_cast<const uint8* const*>(p);
	return rt_store_table_tls();
}
#define RT_STORE_T(t, ea, n, pc) do { if (const uint8* rt_t = (t)) [[unlikely]] { \
	if (rt_t[(uint32)(ea) >> 12] | rt_t[((uint32)(ea) + (n) - 1) >> 12]) \
		rt_journal_store((ea), (n), (pc)); } } while (0)
static inline void rt_wr8(const uint8* t, uint32 ea, uint8 v, uint32 pc = 0) { RT_STORE_T(t, ea, 1, pc); RT_MEM[ea] = v; }
static inline void rt_wr16(const uint8* t, uint32 ea, uint16 v, uint32 pc = 0) { RT_STORE_T(t, ea, 2, pc); v = __builtin_bswap16(v); memcpy(RT_MEM + ea, &v, 2); }
static inline void rt_wr32(const uint8* t, uint32 ea, uint32 v, uint32 pc = 0) { RT_STORE_T(t, ea, 4, pc); v = __builtin_bswap32(v); memcpy(RT_MEM + ea, &v, 4); }
static inline void rt_wr64(const uint8* t, uint32 ea, uint64 v, uint32 pc = 0) { RT_STORE_T(t, ea, 8, pc); v = __builtin_bswap64(v); memcpy(RT_MEM + ea, &v, 8); }
static inline void rt_zero_line(const uint8* t, uint32 ea, uint32 pc = 0) { ea &= ~31u; RT_STORE_T(t, ea, 32, pc); memset(RT_MEM + ea, 0, 32); }
#define RT_WR8(ea, v, pc) rt_wr8(rt_store_table(ctx), (ea), (v), (pc))     // in a helper that has ctx
#define RT_WR16(ea, v, pc) rt_wr16(rt_store_table(ctx), (ea), (v), (pc))
#define RT_WR32(ea, v, pc) rt_wr32(rt_store_table(ctx), (ea), (v), (pc))
#else
#define RT_WR8(ea, v, pc) wr8((ea), (v), (pc))
#define RT_WR16(ea, v, pc) wr16((ea), (v), (pc))
#define RT_WR32(ea, v, pc) wr32((ea), (v), (pc))
#endif
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

// ---- barriers (sync, isync, eieio) ---------------------------------------------------------------
// x86 keeps stores in order and loads in order, as the guest's locks need: nothing, as before. arm64
// doesn't: sync is a full fence, isync an acquire, eieio orders stores (docs/research/android-plan.md).
#if defined(__aarch64__)
#define RT_SYNC()  __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define RT_ISYNC() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define RT_EIEIO() __atomic_thread_fence(__ATOMIC_RELEASE)
#else
#define RT_SYNC()  ((void)0)
#define RT_ISYNC() ((void)0)
#define RT_EIEIO() ((void)0)
#endif

// ---- floating point (mirrors PPCInterpreterFPU.cpp / PPCInterpreterPS.cpp) ----------------------
static inline uint64 fctiw_result(double b, bool toZero)
{
	uint64 v;
	if (b != b)
		v = 0x80000000;                             // a NaN: what x86's conversion gives and the PPC's does (arm64's gives 0)
	else if (b > (double)0x7FFFFFFF)
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

static inline void psq_write(PPCInterpreter_t* ctx, uint32 ea, sint32 type, uint32 v, uint32 pc)
{
	switch (psq_size(type))
	{
	case 1: RT_WR8(ea, (uint8)v, pc); break;
	case 2: RT_WR16(ea, (uint16)v, pc); break;
	default: RT_WR32(ea, v, pc); break;
	}
}

// the quantized types (4-7: u8, u16, s8, s16) out of line; psq_load / psq_store test the type inline and do the
// float types (0-3, where dequantize and quantize copy the bits and ignore the scale) as plain word accesses
[[gnu::noinline]] static void psq_load_q(PPCInterpreter_t* ctx, int frD, uint32 ea, int gqr, bool single)
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

[[gnu::noinline]] static void psq_store_q(PPCInterpreter_t* ctx, int frS, uint32 ea, int gqr, bool single, uint32 pc)
{
	sint32 type = ctx->spr.UGQR[gqr] & 7;
	uint8 scale = (ctx->spr.UGQR[gqr] >> 8) & 0x3F;
	psq_write(ctx, ea, type, quantize((float)ctx->fpr[frS].fp0, type, scale), pc);
	if (!single)
		psq_write(ctx, ea + psq_size(type), type, quantize((float)ctx->fpr[frS].fp1, type, scale), pc);
}

static inline void psq_load(PPCInterpreter_t* ctx, int frD, uint32 ea, int gqr, bool single)
{
	if (ctx->spr.UGQR[gqr] & 0x40000) [[unlikely]]
		return psq_load_q(ctx, frD, ea, gqr, single);
	ctx->fpr[frD].fp0 = (double)std::bit_cast<float>(rd32(ea));
	ctx->fpr[frD].fp1 = single ? 1.0 : (double)std::bit_cast<float>(rd32(ea + 4));
}

static inline void psq_store(PPCInterpreter_t* ctx, int frS, uint32 ea, int gqr, bool single, uint32 pc = 0)
{
	if (ctx->spr.UGQR[gqr] & 4) [[unlikely]]
		return psq_store_q(ctx, frS, ea, gqr, single, pc);
	RT_WR32(ea, std::bit_cast<uint32>((float)ctx->fpr[frS].fp0), pc);
	if (!single)
		RT_WR32(ea + 4, std::bit_cast<uint32>((float)ctx->fpr[frS].fp1), pc);
}

// ---- things the runtime provides (not the generated code) --------------------------------------
void rt_trap(PPCInterpreter_t* ctx, uint32 ea);                // tw/twi: guest assertion traps
void rt_dcache_flush(uint32 ea);                               // dcbf/dcbst: GX2 buffer coherency

// Android: the generated code's stores through its context (above, at zero_line)
#if defined(WWHD_RT_CTX_TABLE) && defined(WWHD_RT_GENERATED)
#define wr8(...) rt_wr8(rt_store_table(ctx), __VA_ARGS__)
#define wr16(...) rt_wr16(rt_store_table(ctx), __VA_ARGS__)
#define wr32(...) rt_wr32(rt_store_table(ctx), __VA_ARGS__)
#define wr64(...) rt_wr64(rt_store_table(ctx), __VA_ARGS__)
#define zero_line(...) rt_zero_line(rt_store_table(ctx), __VA_ARGS__)
#endif
