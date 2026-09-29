// Tables that tools/recomp/generate.py emits next to the generated functions (func_table.cpp,
// imports.cpp) and the runtime (src/runtime) reads. Plain data, so both sides share this layout.
// The runtime links without generated code too (it then only interprets): it includes this
// header with RECOMP_TABLES_WEAK defined, which turns the tables into weak references.
#pragma once
#include <cstddef>
#include <cstdint>

struct PPCInterpreter_t;

// Bumped whenever these layouts change; generated code built against another version is refused.
constexpr uint32_t kRecompTablesVersion = 3;

enum RecompFuncFlags : uint32_t
{
	kRecompPure = 1,        // D8.2: no import, no indirect call or jump, no weak call, all callees pure
	kRecompSynthetic = 2,   // D7: a GHS save/restore entry the generator made up (overlaps its host)
};

struct RecompFunc
{
	uint32_t address, end;              // [address, end) in guest memory
	void (*fn)(PPCInterpreter_t*);
	uint32_t flags;                     // RecompFuncFlags
	uint64_t hash;                      // FNV-1a 64 over the RPX's words, loader-rewritten words as 0
	uint32_t calleeFirst, calleeCount;  // its direct callees (incl. tail calls): g_callees[first ...]
};

struct RecompImport
{
	uint32_t stub;                      // the symbol's address in the RPX (.fimport_/.dimport_)
	const char* lib;
	const char* name;
	uint32_t isData;                    // 1 for .dimport_ (a data symbol), 0 for a function
};

// A word in .text that Cemu's loader rewrites: a branch to an import (kind 10, REL24), a branch
// to a weak symbol at address 0 (kind 0, import 0xFFFF), or an immediate relocated against a
// data import plus an addend (kind 4/5/6: ADDR16_LO/HI/HA of the import's address + addend).
struct RecompImportSite
{
	uint32_t ea;
	uint16_t import;
	uint16_t kind;
	int32_t addend;
};

struct RecompStoreCount
{
	const char* mnemonic;
	uint32_t count;                     // occurrences in the listed (non-synthetic) functions
};

#ifdef RECOMP_TABLES_WEAK
#define RECOMP_TABLE extern __attribute__((weak))
#else
#define RECOMP_TABLE extern
#endif
RECOMP_TABLE const uint32_t g_recompTablesVersion;
RECOMP_TABLE const RecompFunc g_funcTable[];
RECOMP_TABLE const size_t g_funcCount;
RECOMP_TABLE const uint32_t g_callees[];         // indices into g_funcTable
RECOMP_TABLE const RecompImport g_imports[];
RECOMP_TABLE const size_t g_importCount;
RECOMP_TABLE const RecompImportSite g_importSites[];
RECOMP_TABLE const size_t g_importSiteCount;
RECOMP_TABLE const RecompStoreCount g_storeCensus[];
RECOMP_TABLE const size_t g_storeCensusCount;
#undef RECOMP_TABLE
