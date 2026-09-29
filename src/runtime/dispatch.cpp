// The execution seam and dispatch (docs/recompiler-design.md D1, D5, D10).
//
// Install() points Cemu's g_ppcExecuteHook (cemu-patches/0011) at Execute(), which Cemu calls in
// place of its interpreter loop for every timeslice and every callback. By default Execute() runs
// that same loop, so the program behaves exactly like Cemu's interpreter (the M3 seam check: the
// OS-call trace equals the reference's). WWHD_NATIVE=diff runs diff mode instead (diff.cpp), and
// WWHD_NATIVE=on runs the recompiled program (M4): at every function entry the hook calls the
// generated function, which runs until its blr, and the loop continues where that returned.
//
// Guest time (D6, revised): generated code ticks remainingCycles once per instruction and, when
// the timeslice runs out, calls rt_yield, which does in place what the loop the hook was called
// from does at the end of a slice. The two loops differ: __OSFiberThreadEntry clears the
// reservation and switches (the next slice gets the quantum plus the scheduler's jitter);
// PPCCore_executeCallbackInternal switches and then resets the budget to the bare quantum. The
// hook is told which loop called it, and the runtime keeps a callback depth per guest thread.
//
// At the first timeslice (the executable is loaded, relocated and patched by then) the runtime
// builds the function table (D5) over the generated functions, hashes each function's code in
// guest memory against the RPX's (a mismatch means Cemu's GamePatch rewrote it, D10; such a
// function never runs natively) and binds the imports (imports.cpp).
//
// Environment:
//   WWHD_NATIVE=diff   diff mode for pure functions (see diff.cpp for its options)
//   WWHD_NATIVE=on     run the recompiled program (M4)
//   WWHD_RT_LOG=path   the runtime's log (appended); default stderr
#include "runtime.h"
#include "rt_internal.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <unordered_map>

void LatteBufferCache_notifyDCFlush(MPTR address, uint32 size);

namespace wwhd::rt
{
	uint32 g_codeBase, g_codeWords;
	std::vector<sint32> g_entryIndex;
	std::vector<uint8> g_patched;

	static FILE* s_log = stderr;
	static std::mutex s_logMutex;
	enum class Mode { Interpret, Diff, Native };
	static Mode s_mode = Mode::Interpret;

	// native mode's counters (M4 wants the fallbacks at 0)
	static uint64 s_interpretedCalls = 0;     // calls from native code that fell back to the interpreter
	static uint64 s_nativeEntries = 0;        // generated functions the hook called
	static uint64 s_fallbackInsns = 0;        // game instructions the hook interpreted in native mode
	static uint64 s_yields = 0;               // timeslices that ended inside native code
	static std::chrono::steady_clock::time_point s_lastReport;

	// per guest thread: how many callback loops (PPCCore_executeCallbackInternal) are running it
	static std::unordered_map<PPCInterpreter_t*, sint32> s_callbackDepth;
	static std::mutex s_callbackMutex;

	static void VLog(const char* prefix, const char* fmt, va_list ap)
	{
		std::unique_lock _l(s_logMutex);
		fputs(prefix, s_log);
		vfprintf(s_log, fmt, ap);
		fputc('\n', s_log);
		fflush(s_log);
	}

	void Log(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		VLog("rt: ", fmt, ap);
		va_end(ap);
	}

	void Fatal(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		VLog("rt: FATAL: ", fmt, ap);
		va_end(ap);
		std::abort();
	}

	static void BuildTable()
	{
		uint32 lo = 0xFFFFFFFF, hi = 0;
		for (size_t i = 0; i < g_funcCount; i++)
		{
			lo = std::min(lo, g_funcTable[i].address);
			hi = std::max(hi, g_funcTable[i].end);
		}
		g_codeBase = lo;
		g_codeWords = (hi - lo) / 4;
		g_entryIndex.assign(g_codeWords, 0);
		for (size_t i = 0; i < g_funcCount; i++)
			g_entryIndex[(g_funcTable[i].address - lo) / 4] = (sint32)i + 1;
	}

	// D10: which functions differ in guest memory from the code they were generated from
	static void CheckCode()
	{
		std::vector<uint8> masked(g_codeWords, 0);    // words the loader rewrites (imports, weak calls)
		for (size_t i = 0; i < g_importSiteCount; i++)
			masked[(g_importSites[i].ea - g_codeBase) / 4] = 1;
		g_patched.assign(g_funcCount, 0);
		size_t patched = 0, pure = 0;
		for (size_t i = 0; i < g_funcCount; i++)
		{
			const RecompFunc& f = g_funcTable[i];
			uint64 h = 0xCBF29CE484222325ull;
			for (uint32 ea = f.address; ea < f.end; ea += 4)
				h = (h ^ (masked[(ea - g_codeBase) / 4] ? 0 : memory_readU32(ea))) * 0x100000001B3ull;
			pure += (f.flags & kRecompPure) != 0;
			if (h != f.hash)
			{
				g_patched[i] = 1;
				patched++;
				Log("f_%08X differs in memory from the RPX (patched by Cemu): it stays interpreted", f.address);
			}
		}
		Log("%zu functions (%zu pure) over %08X-%08X, %zu patched in memory", (size_t)g_funcCount, pure,
			g_codeBase, g_codeBase + g_codeWords * 4, patched);
	}

	static void Init()
	{
		if (const char* path = getenv("WWHD_RT_LOG"); path && *path)
		{
			if (FILE* f = fopen(path, "a"))
				s_log = f;
		}
		if (!Linked())
		{
			Log("no recompiled code linked: interpreting");
			return;
		}
		if (&g_recompTablesVersion == nullptr || g_recompTablesVersion != kRecompTablesVersion)
			Fatal("the linked generated code has table version %u, this runtime reads %u: regenerate it",
				&g_recompTablesVersion ? g_recompTablesVersion : 0, kRecompTablesVersion);
		BuildTable();
		CheckCode();
		BindImports();
		const char* mode = getenv("WWHD_NATIVE");
		if (DiffInit())
			s_mode = Mode::Diff;
		else if (mode && strcmp(mode, "on") == 0)
		{
			s_mode = Mode::Native;
			s_lastReport = std::chrono::steady_clock::now();
			at_quick_exit([] { NativeReport(true); });   // the trace's exit-at-frame (patch 0011)
			atexit([] { NativeReport(true); });
			Log("native: the recompiled program runs; functions patched in memory stay interpreted");
		}
		else
			Log("interpreting (WWHD_NATIVE=diff for diff mode, =on to run the recompiled program)");
	}

	void NativeReport(bool final)
	{
		Log("native%s: %llu function entries from the loop, %llu timeslices ended in native code, %llu game "
			"instructions interpreted, %llu calls from native code interpreted", final ? " (final)" : "",
			(unsigned long long)s_nativeEntries, (unsigned long long)s_yields, (unsigned long long)s_fallbackInsns,
			(unsigned long long)s_interpretedCalls);
		s_lastReport = std::chrono::steady_clock::now();
	}

	// the loop in native mode: generated functions at their entries, anything else interpreted
	static void ExecuteNative(PPCInterpreter_t* hCPU)
	{
		if (std::chrono::steady_clock::now() - s_lastReport > std::chrono::seconds(30))
			NativeReport(false);
		for (;;)
		{
			uint32 ip = hCPU->instructionPointer;
			if (sint32 i = FuncIndexAt(ip); i >= 0 && !g_patched[i])
			{
				s_nativeEntries++;
				g_funcTable[i].fn(hCPU);                 // ticks and yields itself
				hCPU->instructionPointer = hCPU->spr.LR & ~3u;   // where its blr went
				continue;
			}
			if ((--hCPU->remainingCycles) < 0)
				break;
			if (((ip - g_codeBase) >> 2) < g_codeWords) [[unlikely]]
			{
				if (s_fallbackInsns++ < 20)
					Log("native: interpreting game code at %08X (LR %08X)", ip, hCPU->spr.LR);
			}
			PPCInterpreterSlim_executeInstruction(hCPU);
		}
	}

	static void Execute(PPCInterpreter_t* hCPU, bool callback)
	{
		static std::once_flag once;
		std::call_once(once, Init);
		if (callback)
		{
			std::unique_lock _l(s_callbackMutex);
			s_callbackDepth[hCPU]++;
		}
		switch (s_mode)
		{
		case Mode::Diff:
			DiffExecute(hCPU);
			break;
		case Mode::Native:
			ExecuteNative(hCPU);
			break;
		case Mode::Interpret:
			// exactly Cemu's loop (__OSFiberThreadEntry, PPCCore_executeCallbackInternal)
			while ((--hCPU->remainingCycles) >= 0)
				PPCInterpreterSlim_executeInstruction(hCPU);
			break;
		}
		if (callback)
		{
			std::unique_lock _l(s_callbackMutex);
			if (--s_callbackDepth[hCPU] == 0)
				s_callbackDepth.erase(hCPU);
		}
	}

	// End the timeslice from inside the hook, as the loop that called the hook would have
	// (__OSFiberThreadEntry or PPCCore_executeCallbackInternal, see the top of this file).
	static void EndTimeslice(PPCInterpreter_t* ctx)
	{
		bool callback;
		{
			std::unique_lock _l(s_callbackMutex);
			callback = s_callbackDepth.count(ctx) != 0;
		}
		if (callback)
		{
			PPCCore_switchToScheduler();            // OSYieldThread
			ctx->remainingCycles = ppcThreadQuantum;
			ctx->skippedCycles = 0;
		}
		else
		{
			ctx->reservedMemAddr = 0;
			ctx->reservedMemValue = 0;
			PPCCore_switchToScheduler();
		}
	}

	void Install()
	{
		g_ppcExecuteHook = Execute;
	}

	// generated code: the timeslice ran out before the instruction at pc (RT_TICK, ppc_ops.h)
	void Yield(PPCInterpreter_t* ctx, uint32 pc)
	{
		ctx->instructionPointer = pc;                // the thread's saved context shows where it stopped
		s_yields++;
		do
			EndTimeslice(ctx);
		while (--ctx->remainingCycles < 0);          // the new slice pays for the instruction at pc
	}

	// Run guest code at target until it returns to the caller (LR, with the stack pointer back
	// where it was), preempted like Cemu's own loop when the timeslice runs out. Like the hook's
	// loop in native mode, it calls the generated function whenever it reaches a function entry,
	// so only code outside the function table (e.g. the far-jump trampolines Cemu's loader writes
	// into the trampoline area: addi/addis r11, mtctr, bctr) is interpreted.
	void Interpret(PPCInterpreter_t* ctx, uint32 target)
	{
		const uint32 ret = ctx->spr.LR & ~3u, sp = ctx->gpr[1];
		if (s_interpretedCalls++ < 20)
			Log("native code calls %08X (LR %08X, words %08X %08X): interpreted up to the next function entry", target,
				ret, memory_readU32(target), memory_readU32(target + 4));
		ctx->instructionPointer = target;
		while (ctx->instructionPointer != ret || ctx->gpr[1] != sp)
		{
			uint32 ip = ctx->instructionPointer;
			if (sint32 i = FuncIndexAt(ip); i >= 0 && !g_patched[i])
			{
				g_funcTable[i].fn(ctx);
				ctx->instructionPointer = ctx->spr.LR & ~3u;
				continue;
			}
			if ((--ctx->remainingCycles) < 0)
			{
				EndTimeslice(ctx);
				continue;
			}
			if (((ip - g_codeBase) >> 2) < g_codeWords) [[unlikely]]
			{
				if (s_fallbackInsns++ < 20)
					Log("native: interpreting game code at %08X (LR %08X)", ip, ctx->spr.LR);
			}
			PPCInterpreterSlim_executeInstruction(ctx);
		}
	}

	// A call from native code to guest address target (D5): a generated function if there is an
	// unpatched one, else an HLE trampoline (D4), else the interpreter.
	void Dispatch(PPCInterpreter_t* ctx, uint32 target)
	{
		if (sint32 i = FuncIndexAt(target); i >= 0 && !g_patched[i])
		{
			g_funcTable[i].fn(ctx);
			return;
		}
		uint32 word = memory_readU32(target);
		if ((word >> 26) == 1)
		{
			ctx->instructionPointer = target;
			if (--ctx->remainingCycles < 0)          // the trampoline is an instruction too
				Yield(ctx, target);
			PPCInterpreter_virtualHLE(ctx, word);
			if (ctx->instructionPointer != ctx->spr.LR)
				Dispatch(ctx, ctx->instructionPointer);    // the handler tail-called guest code (D4)
			return;
		}
		Interpret(ctx, target);
	}
}

using namespace wwhd::rt;

// ---- called by generated code (funcs.h, ppc_ops.h) -------------------------------------------------
void rt_yield(PPCInterpreter_t* ctx, uint32 pc)
{
	Yield(ctx, pc);
}

void rt_call_ctr(PPCInterpreter_t* ctx)
{
	Dispatch(ctx, ctx->spr.CTR & ~3u);
}

void rt_jump_ctr(PPCInterpreter_t* ctx)
{
	Dispatch(ctx, ctx->spr.CTR & ~3u);
}

void rt_bad_branch(PPCInterpreter_t* ctx, uint32 ea, uint32 target)
{
	if (g_rtJournalOn)
		DiffNativeFault(ea, target);    // inside a diff-mode native run: unwind it (diff.cpp)
	Fatal("bad branch at %08X to %08X (LR %08X, r1 %08X, r3 %08X)", ea, target, ctx->spr.LR, ctx->gpr[1], ctx->gpr[3]);
}

// tw/twi: Cemu's interpreter steps over both (TO != 0; TO = 0 is its debugger's breakpoint marker)
void rt_trap(PPCInterpreter_t*, uint32)
{
}

// dcbf/dcbst: tell the GPU side the CPU wrote this line, as the interpreter does. Not during a
// diff-mode native run: that run is rewound, and the interpreter repeats the call for real.
void rt_dcache_flush(uint32 ea)
{
	if (!g_rtJournalOn)
		LatteBufferCache_notifyDCFlush(ea, 32);
}
