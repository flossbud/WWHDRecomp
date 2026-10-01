// Imports (docs/recompiler-design.md D4). Generated code calls rt_import(ctx, id) where the guest
// branches to an import, and reads a data import's address with rt_import_data(id).
//
// Both are bound at boot from what Cemu's loader wrote into guest memory, not from names, so a
// native call reaches exactly the code the interpreter would:
// * every relocated branch to a function import is followed to its target (a trampoline holding
//   Cemu's HLE opcode, (1 << 26) | index); all sites of one import must agree;
// * every relocated immediate of a data import gives half of its address plus an addend; all
//   must agree on the address.
// * a branch site that no longer holds a branch was patched by Cemu (GamePatch NOPs some calls,
//   D10): the hash in CheckCode can't see that (it masks import sites), so its function is marked
//   patched here and stays interpreted.
// Cemu's own name lookups (osLib_getFunctionIndex, osLib_getPointer) are compared as a
// cross-check and only reported.
#include "rt_internal.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/OS/common/OSCommon.h"
#include <map>

namespace wwhd::rt
{
	struct BoundImport
	{
		uint32 target = 0;    // where the guest's branch lands (the trampoline)
		uint32 opcode = 0;    // the HLE opcode there, or 0 if it is not one (then it is interpreted)
	};
	static std::vector<BoundImport> s_func;
	static std::vector<uint32> s_data;

	static uint32 BranchTarget(uint32 ea)       // target of the b/bl at ea, 0 if it is not one
	{
		uint32 w = memory_readU32(ea);
		if ((w >> 26) != 18)
			return 0;
		uint32 li = (uint32)(((sint32)(w << 6)) >> 6) & ~3u;
		return (w & 2) ? li : ea + li;
	}

	static std::string LibName(const char* lib)   // "nn_olv.rpl" -> "nn_olv", as Cemu's loader does
	{
		std::string s(lib);
		return s.substr(0, s.find('.'));
	}

	void BindImports()
	{
		s_func.assign(g_importCount, {});
		s_data.assign(g_importCount, 0);
		// data imports: the relocated halves of (address + addend), per import and addend
		struct Halves { sint32 hi = -1, lo = -1, hiKind = 0; };
		std::map<std::pair<uint32, sint32>, Halves> halves;
		size_t branches = 0, immediates = 0, weak = 0, patchedSites = 0;
		for (size_t k = 0; k < g_importSiteCount; k++)
		{
			const RecompImportSite& s = g_importSites[k];
			if (s.kind == 0)                           // weak symbol at 0: generated code traps there
			{
				weak++;
				continue;
			}
			const RecompImport& imp = g_imports[s.import];
			if (s.kind == 10)                          // REL24: b/bl to a function import
			{
				uint32 t = BranchTarget(s.ea);
				if (t == 0)                            // Cemu patched the call away (D10)
				{
					for (size_t i = 0; i < g_funcCount; i++)
						if (g_funcTable[i].address <= s.ea && s.ea < g_funcTable[i].end && !g_patched[i])
						{
							g_patched[i] = 1;
							Log("f_%08X: its call to %s.%s at %08X is now %08X (patched by Cemu): it stays interpreted",
								g_funcTable[i].address, imp.lib, imp.name, s.ea, memory_readU32(s.ea));
						}
					patchedSites++;
					continue;
				}
				BoundImport& b = s_func[s.import];
				if (b.target == 0)
				{
					b.target = t;
					uint32 w = memory_readU32(t);
					b.opcode = (w >> 26) == 1 ? w : 0;
				}
				else if (b.target != t)
					Fatal("import %s.%s: site %08X branches to %08X, another to %08X", imp.lib, imp.name, s.ea, t, b.target);
				branches++;
				continue;
			}
			// ADDR16_LO (4), _HI (5), _HA (6): one half of a data import's address + addend
			Halves& h = halves[{ s.import, s.addend }];
			sint32 imm = (sint32)(memory_readU32(s.ea) & 0xFFFF);
			sint32& half = (s.kind == 4) ? h.lo : h.hi;
			if (half >= 0 && half != imm)
				Fatal("data import %s.%s%+d: immediates %04X and %04X disagree (site %08X)", imp.lib, imp.name, s.addend,
					half, imm, s.ea);
			half = imm;
			if (s.kind != 4)
			{
				if (h.hiKind && h.hiKind != s.kind)
					Fatal("data import %s.%s: both HI and HA immediates", imp.lib, imp.name);
				h.hiKind = s.kind;
			}
			immediates++;
		}
		size_t funcs = 0, hle = 0, data = 0, crossFail = 0;
		std::vector<uint8> dataBound(g_importCount, 0);
		for (auto& [key, h] : halves)
		{
			const RecompImport& imp = g_imports[key.first];
			if (h.hi < 0 || h.lo < 0)
				Fatal("data import %s.%s%+d: only one half of its address is referenced", imp.lib, imp.name, key.second);
			uint32 sum = h.hiKind == 6 ? ((uint32)h.hi << 16) + (uint32)(sint32)(sint16)h.lo : ((uint32)h.hi << 16) | (uint32)h.lo;
			uint32 v = sum - (uint32)key.second;
			if (dataBound[key.first] && s_data[key.first] != v)
				Fatal("data import %s.%s: its immediates give %08X and %08X", imp.lib, imp.name, s_data[key.first], v);
			s_data[key.first] = v;
			dataBound[key.first] = 1;
		}
		for (size_t i = 0; i < g_importCount; i++)
		{
			const RecompImport& imp = g_imports[i];
			if (imp.isData)
			{
				if (!dataBound[i])
					continue;                          // not referenced by code
				uint32 v = s_data[i];
				data++;
				uint32 named = osLib_getPointer(LibName(imp.lib).c_str(), imp.name);
				if (named != 0xFFFFFFFF && named != v && crossFail++ < 10)
					Log("cross-check: data import %s.%s is %08X in memory, Cemu's table says %08X", imp.lib, imp.name, v, named);
				continue;
			}
			const BoundImport& b = s_func[i];
			if (b.target == 0)
				continue;                              // never branched to
			funcs++;
			if (b.opcode)
			{
				hle++;
				sint32 idx = osLib_getFunctionIndex(LibName(imp.lib).c_str(), imp.name);
				if (idx >= 0 && (uint32)idx != (b.opcode & 0xFFFF) && crossFail++ < 10)
					Log("cross-check: import %s.%s runs HLE %u, Cemu's table says %d", imp.lib, imp.name, b.opcode & 0xFFFF, idx);
			}
			else
				Log("import %s.%s at %08X is not an HLE trampoline (word %08X): interpreted", imp.lib, imp.name,
					b.target, memory_readU32(b.target));
		}
		Log("imports: %zu functions bound from %zu branch sites (%zu HLE), %zu data imports from %zu immediates, "
			"%zu weak call sites, %zu call sites patched away; %zu cross-check differences", funcs, branches, hle, data,
			immediates, weak, patchedSites, crossFail);
	}
}

using namespace wwhd::rt;

void rt_import(PPCInterpreter_t* ctx, uint32 id)
{
	const BoundImport& b = s_func[id];
	if (!b.opcode)
	{
		Interpret(ctx, b.target);
		return;
	}
	// what the interpreter does at the trampoline: one cycle for the instruction (D6), then trace
	// the call, charge its 300 cycles and run the handler
	ctx->instructionPointer = b.target;
	if (--ctx->remainingCycles < 0)
		Yield(ctx, b.target);
	QuietOsCall();
	PPCInterpreter_virtualHLE(ctx, b.opcode);
	if (ctx->instructionPointer != ctx->spr.LR) [[unlikely]]
		Dispatch(ctx, ctx->instructionPointer);   // D4: the handler tail-called guest code (MEM forwarding)
}

uint32 rt_import_data(uint32 id)
{
	return s_data[id];
}
