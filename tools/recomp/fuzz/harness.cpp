// M1 instruction fuzzer (docs/recompiler-design.md D8.1): for every generated case, run the
// instruction through Cemu's interpreter and through our emitted C++ from the same random CPU
// state and memory window, then compare every register and every byte of the window.
// Linked into Cemu_release's own link line with -Wl,--wrap=main (see build.sh), so Cemu's
// interpreter, FPU helpers and memory are the real ones. No game data involved.
#include "ppc_ops.h"
#include "fuzz.h"
#include "Cafe/HW/MMU/MMU.h"
#include <random>
#include <map>
#include <string>

static constexpr uint32 WIN = 0x10100000, WIN_SIZE = 0x10000;

void rt_trap(PPCInterpreter_t*, uint32 ea) { fprintf(stderr, "trap at %08x\n", ea); abort(); }
void rt_dcache_flush(uint32) {}
bool g_rtJournalOn = false;
void rt_journal_store(uint32, uint32) {}

static std::mt19937_64 rng(1);

static uint32 randGpr()
{
	static const uint32 edge[] = {0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x8000, 0xFFFF, 0x10000, 31, 32, 63};
	switch (rng() % 4)
	{
	case 0: return edge[rng() % std::size(edge)];
	case 1: return (uint32)(rng() % 64);
	default: return (uint32)rng();
	}
}

static double bitsToDouble(uint64 v) { double d; memcpy(&d, &v, 8); return d; }
static float bitsToFloat(uint32 v) { float f; memcpy(&f, &v, 4); return f; }

static double randDouble()
{
	switch (rng() % 9)
	{
	case 0: return bitsToDouble(rng());                                   // anything, incl. NaNs
	case 1: case 2: return (double)bitsToFloat((uint32)rng());            // single-representable
	case 3: return (double)(sint32)(rng() % 2001) - 1000.0;              // small integers
	case 4: return (rng() & 1) ? -0.0 : 0.0;
	case 5:
	{
		static const uint64 special[] = {0x7FF0000000000000ULL, 0xFFF0000000000000ULL, 0x7FF8000000000000ULL,
			0x7FF4000000000000ULL, 0xFFF8000000000001ULL, 0x0000000000000001ULL, 0x000FFFFFFFFFFFFFULL,
			0x3810000000000000ULL, 0x380FFFFFE0000000ULL, 0x47EFFFFFE0000000ULL, 0x47EFFFFFF0000000ULL};
		return bitsToDouble(special[rng() % std::size(special)]);
	}
	case 6: return (double)bitsToFloat((uint32)(rng() & 0x807FFFFF));     // float denormals
	case 7: return ((double)(rng() % 2000000) - 1000000.0) / 1024.0;
	default: return ldexp((double)(rng() % 0x1000000) / 0x1000000, (int)(rng() % 300) - 150);
	}
}

static void randomState(PPCInterpreter_t& c)
{
	for (auto& g : c.gpr) g = randGpr();
	for (auto& f : c.fpr) { f.fp0 = randDouble(); f.fp1 = randDouble(); }
	for (auto& b : c.cr) b = rng() & 1;
	c.xer_ca = rng() & 1; c.xer_so = rng() & 1; c.xer_ov = rng() & 1;
	c.fpscr = (uint32)rng() & 0x000FF000;
	c.spr.LR = (uint32)rng(); c.spr.CTR = randGpr();
	for (auto& q : c.spr.UGQR)
		q = (uint32)((rng() % 64) << 24 | (rng() % 8) << 16 | (rng() % 64) << 8 | (rng() % 8));
	c.reservedMemAddr = 0; c.reservedMemValue = 0;
}

static uint32 windowAddress(uint32 align)
{
	uint32 off = 0x200 + (uint32)(rng() % (WIN_SIZE - 0x400));
	return WIN + (off & ~(align - 1));
}

// Point the case's address registers into the window.
static uint32 aim(PPCInterpreter_t& c, const FuzzCase& fc)
{
	bool atomic = !strcmp(fc.op, "lwarx") || !strcmp(fc.op, "stwcx.");
	uint32 align = (atomic || (rng() % 4)) ? 4 : 1;
	uint32 ea = windowAddress(align);
	switch (fc.kind)
	{
	case 1: c.gpr[fc.rA] = ea - (uint32)fc.d; break;
	case 3: c.gpr[fc.rA] = ea; break;
	case 2:
		if (fc.rA)
		{
			uint32 b = (uint32)(rng() % 0x100) - 0x80;
			c.gpr[fc.rB] = b; c.gpr[fc.rA] = ea - b;
		}
		else
			c.gpr[fc.rB] = ea;
		break;
	}
	return ea;
}

static long g_nanOnly = 0;

struct Diff { std::string what; uint64 interp, native; };

static bool compare(const PPCInterpreter_t& a, const PPCInterpreter_t& b, const uint8* ma, const uint8* mb, Diff& d)
{
	auto chk = [&](const char* name, int idx, uint64 x, uint64 y) {
		if (x == y) return false;
		d.what = idx < 0 ? std::string(name) : std::string(name) + std::to_string(idx);
		d.interp = x; d.native = y;
		return true;
	};
	if (chk("ip", -1, a.instructionPointer, b.instructionPointer)) return false;
	for (int i = 0; i < 32; i++) if (chk("r", i, a.gpr[i], b.gpr[i])) return false;
	// Any NaN equals any NaN: which input NaN x86 propagates depends on the operand order the
	// compiler picked (and neither side follows Espresso's NaN rules). Counted in g_nanOnly.
	auto fchk = [&](const char* name, int idx, uint64 x, uint64 y) {
		if (x != y && IS_NAN(x) && IS_NAN(y)) { g_nanOnly++; return false; }
		return chk(name, idx, x, y);
	};
	for (int i = 0; i < 32; i++) if (fchk("f", i, a.fpr[i].fp0int, b.fpr[i].fp0int)) return false;
	for (int i = 0; i < 32; i++) if (fchk("f.ps1_", i, a.fpr[i].fp1int, b.fpr[i].fp1int)) return false;
	for (int i = 0; i < 32; i++) if (chk("crbit", i, a.cr[i], b.cr[i])) return false;
	if (chk("fpscr", -1, a.fpscr, b.fpscr)) return false;
	if (chk("xer_ca", -1, a.xer_ca, b.xer_ca) || chk("xer_so", -1, a.xer_so, b.xer_so) || chk("xer_ov", -1, a.xer_ov, b.xer_ov)) return false;
	if (chk("lr", -1, a.spr.LR, b.spr.LR) || chk("ctr", -1, a.spr.CTR, b.spr.CTR)) return false;
	for (int i = 0; i < 8; i++) if (chk("ugqr", i, a.spr.UGQR[i], b.spr.UGQR[i])) return false;
	if (chk("resv_addr", -1, a.reservedMemAddr, b.reservedMemAddr) || chk("resv_val", -1, a.reservedMemValue, b.reservedMemValue)) return false;
	if (memcmp(ma, mb, WIN_SIZE) != 0)
		for (uint32 i = 0; i < WIN_SIZE; i++)
			if (chk("mem+", (int)i, ma[i], mb[i])) return false;
	return true;
}

extern "C" int __wrap_main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	int iters = argc > 1 ? atoi(argv[1]) : 200;
	const char* only = argc > 2 ? argv[2] : nullptr;
	memory_init();
	mmuRange_TEXT_AREA.mapMem();
	mmuRange_MEM2.mapMem();
	PPCInterpreterGlobal_t global{};
	uint8* win = memory_base + WIN;
	std::vector<uint8> init(WIN_SIZE), after(WIN_SIZE);
	struct Stat { int runs = 0, fails = 0; };
	std::map<std::string, Stat> stats;
	int shown = 0;
	for (size_t k = 0; k < g_caseCount; k++)
	{
		const FuzzCase& fc = g_cases[k];
		if (only && strcmp(only, fc.op) != 0)
			continue;
		wr32(g_caseEA, fc.word);
		if (stats.find(fc.op) == stats.end())
			printf("... %s\n", fc.op);             // progress (tools/worker/job watches the log)
		Stat& st = stats[fc.op];
		for (int it = 0; it < iters; it++)
		{
			static PPCInterpreter_t base, a, b;
			memset(&base, 0, sizeof(base));
			randomState(base);
			base.global = &global;
			base.instructionPointer = g_caseEA;
			uint32 ea = aim(base, fc);
			for (auto& byte : init) byte = (uint8)rng();
			if (!strcmp(fc.op, "stwcx.") && (rng() & 1))
			{
				base.reservedMemAddr = ea;
				base.reservedMemValue = (rng() & 1) ? __builtin_bswap32(*(uint32*)&init[ea - WIN]) : (uint32)rng();
			}
			memcpy(win, init.data(), WIN_SIZE);
			a = base;
			PPCInterpreterSlim_executeInstruction(&a);
			memcpy(after.data(), win, WIN_SIZE);
			memcpy(win, init.data(), WIN_SIZE);
			b = base;
			fc.native(&b);
			Diff d;
			st.runs++;
			if (!compare(a, b, after.data(), win, d))
			{
				st.fails++;
				if (st.fails <= 3 && shown < 200)
				{
					shown++;
					printf("FAIL %-10s %08x iter %d: %s interp=%llx native=%llx\n", fc.op, fc.word, it, d.what.c_str(),
						(unsigned long long)d.interp, (unsigned long long)d.native);
				}
			}
		}
	}
	int bad = 0;
	for (auto& [op, st] : stats)
	{
		if (st.fails) bad++;
		printf("%-12s %7d runs %7d fails\n", op.c_str(), st.runs, st.fails);
	}
	printf("%zu mnemonics, %d failing (%ld NaN-payload-only differences tolerated)\n", stats.size(), bad, g_nanOnly);
	fflush(stdout);
	_exit(bad ? 1 : 0);   // skip Cemu's global destructors
}
