// Internals shared by the runtime's sources (see runtime.h and src/README.md).
#pragma once
#include "Cafe/HW/Espresso/PPCState.h"
#define RECOMP_TABLES_WEAK
#include "recomp_tables.h"

// generated code's inline indirect calls (tools/recomp/runtime/ppc_ops.h), filled by dispatch.cpp
extern void (**rt_direct)(PPCInterpreter_t*);
extern uint32 rt_directBase, rt_directWords;

// what generated code calls (declared for it in funcs.h and ppc_ops.h)
void rt_import(PPCInterpreter_t* ctx, uint32 importId);
uint32 rt_import_data(uint32 importId);
void rt_call_ctr(PPCInterpreter_t* ctx);
void rt_jump_ctr(PPCInterpreter_t* ctx);
void rt_bad_branch(PPCInterpreter_t* ctx, uint32 ea, uint32 target);
void rt_trap(PPCInterpreter_t* ctx, uint32 ea);
void rt_dcache_flush(uint32 ea);
extern bool g_rtJournalOn;
void rt_journal_store(uint32 ea, uint32 size, uint32 pc);
void rt_yield(PPCInterpreter_t* ctx, uint32 pc);

namespace wwhd::rt
{
	// Is recompiled code linked in (build/recomp)? Without it the runtime only interprets.
	inline bool Linked() { return &g_funcCount != nullptr; }

	// The guest code the function table covers, and for each word of it the index of the
	// function starting there plus one (0: not a function entry).
	extern uint32 g_codeBase, g_codeWords;
	extern std::vector<sint32> g_entryIndex;
	// per function: the code in guest memory differs from the RPX (Cemu patched it, D10)
	extern std::vector<uint8> g_patched;

	inline sint32 FuncIndexAt(uint32 ea)
	{
		uint32 w = (ea - g_codeBase) >> 2;     // wraps for ea below the base
		return (w < g_codeWords && !(ea & 3)) ? g_entryIndex[w] - 1 : -1;
	}

	void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
	[[noreturn]] void Fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

	// imports.cpp
	void BindImports();

	// dispatch.cpp: the end of a timeslice inside native code (D6), and calling guest code from
	// native code (D4, D5)
	void Yield(PPCInterpreter_t* ctx, uint32 pc);
	void NativeReport(bool final);
	void Dispatch(PPCInterpreter_t* ctx, uint32 target);   // native, HLE trampoline, or interpreted
	void Interpret(PPCInterpreter_t* ctx, uint32 target);  // interpret from target until it returns to LR

	// dispatch.cpp: the quiet watch (D19's fast paths; src/overrides/override.h has its interface)
	struct Quiet
	{
		uint64 token = 0;                     // the running watch, 0 for none
		PPCInterpreter_t* ctx = nullptr;
		uint32 low = 0, high = 0;
		uint64 wakeUps = 0;                   // the thread's OSThread wakeUpCount when it began
		uint32 osCalls = 0;
		bool visible = false;
	};
	extern Quiet g_quiet;
	void QuietStore(uint32 ea, uint32 size);               // rt_journal_store during a watch
	inline void QuietOsCall()                              // an OS call from native code
	{
		if (g_quiet.token) [[unlikely]]
			g_quiet.osCalls++;
	}

	// diff.cpp (D8.2)
	bool DiffInit();                                       // true if diff mode is on
	void DiffExecute(PPCInterpreter_t* hCPU);              // the hook's loop in diff mode
	void DiffReport(bool final);
	[[noreturn]] void DiffNativeFault(uint32 ea, uint32 target);   // rt_bad_branch in a diff native run
	bool DiffInNative();                                   // a diff-mode native run is going on
}
