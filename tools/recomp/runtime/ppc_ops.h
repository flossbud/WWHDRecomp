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

// Store journal (diff mode, D8.2): while g_rtJournalOn is set, every store first hands the runtime
// the range it is about to overwrite, so that a native run can be rewound. Off otherwise.
extern bool g_rtJournalOn;
void rt_journal_store(uint32 ea, uint32 size);
#define RT_STORE(ea, n) do { if (g_rtJournalOn) [[unlikely]] rt_journal_store((ea), (n)); } while (0)

#define GPR(n) ctx->gpr[n]
#define FPR(n) ctx->fpr[n]
#define CRB(n) ctx->cr[n]

// ---- memory (big-endian guest, identity-mapped at memory_base) ----------------------------------
static inline uint8 rd8(uint32 ea) { return memory_base[ea]; }
static inline uint16 rd16(uint32 ea) { uint16 v; memcpy(&v, memory_base + ea, 2); return __builtin_bswap16(v); }
static inline uint32 rd32(uint32 ea) { uint32 v; memcpy(&v, memory_base + ea, 4); return __builtin_bswap32(v); }
static inline uint64 rd64(uint32 ea) { uint64 v; memcpy(&v, memory_base + ea, 8); return __builtin_bswap64(v); }
static inline void wr8(uint32 ea, uint8 v) { RT_STORE(ea, 1); memory_base[ea] = v; }
static inline void wr16(uint32 ea, uint16 v) { RT_STORE(ea, 2); v = __builtin_bswap16(v); memcpy(memory_base + ea, &v, 2); }
static inline void wr32(uint32 ea, uint32 v) { RT_STORE(ea, 4); v = __builtin_bswap32(v); memcpy(memory_base + ea, &v, 4); }
static inline void wr64(uint32 ea, uint64 v) { RT_STORE(ea, 8); v = __builtin_bswap64(v); memcpy(memory_base + ea, &v, 8); }
static inline void zero_line(uint32 ea) { ea &= ~31u; RT_STORE(ea, 32); memset(memory_base + ea, 0, 32); }   // dcbz

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

static inline void psq_write(uint32 ea, sint32 type, uint32 v)
{
	switch (psq_size(type))
	{
	case 1: wr8(ea, (uint8)v); break;
	case 2: wr16(ea, (uint16)v); break;
	default: wr32(ea, v); break;
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

static inline void psq_store(PPCInterpreter_t* ctx, int frS, uint32 ea, int gqr, bool single)
{
	sint32 type = ctx->spr.UGQR[gqr] & 7;
	uint8 scale = (ctx->spr.UGQR[gqr] >> 8) & 0x3F;
	psq_write(ea, type, quantize((float)ctx->fpr[frS].fp0, type, scale));
	if (!single)
		psq_write(ea + psq_size(type), type, quantize((float)ctx->fpr[frS].fp1, type, scale));
}

// ---- things the runtime provides (not the generated code) --------------------------------------
void rt_trap(PPCInterpreter_t* ctx, uint32 ea);                // tw/twi: guest assertion traps
void rt_dcache_flush(uint32 ea);                               // dcbf/dcbst: GX2 buffer coherency
