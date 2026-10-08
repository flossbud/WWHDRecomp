// Diff mode (docs/recompiler-design.md D8.2, milestone M3): check pure functions' native code
// against Cemu's interpreter along a real run, without changing what the guest sees.
//
// When a sampled call of a pure function begins (the interpreter is about to run its entry):
//  1. its native code runs first, from the same registers, with every store journaled;
//  2. the native run is rewound: memory from the journal, registers from a copy;
//  3. the interpreter runs the call as it always would, charging its cycles and being preempted
//     as usual, and this runtime decodes each store it executes to record what it wrote;
//  4. when the interpreter reaches the point where the native run left the guest (its final LR,
//     where blr went, with its final stack pointer), its registers and the bytes it stored are
//     compared with the native run's.
// For an ordinary function that point is the caller's return address with the stack pointer back
// where it was; the GHS restore-and-exit helpers (D7) pop their caller's frame too and return to
// the caller's caller. A native run that is wrong about it never meets the interpreter there and
// shows up as escaped or runaway, so it is still caught.
// Guest-visible behaviour stays the interpreter's, so the OS-call trace must still equal the
// reference's, and every sampled call is checked on real game state.
//
// Guest time (D6): the native run starts with an unlimited timeslice, so it never yields, and the
// cycles it charged must equal the instructions the interpreter executed for the same call.
// A native run that reaches rt_bad_branch (a branch the generator couldn't place) is unwound with
// longjmp (generated code holds no resources), rewound, and counted as a native fault.
// A call whose timeslice ended before it returned is "preempted": other threads ran meanwhile and
// may have changed memory it reads, so its mismatches are counted apart. A call that reaches an
// HLE trampoline or leaves the function table ("escaped") contradicts the purity analysis and is
// reported. Runs are single-core (the reference profile), so there is one journal.
//
// Environment:
//   WWHD_NATIVE=diff          turn diff mode on
//   WWHD_DIFF_ONLY=A,B,...    check only these functions (hex addresses), every call
//   WWHD_DIFF_EXCLUDE=A,...   never check these
//   WWHD_DIFF_FIRST=N         check each function's first N calls (default 16) ...
//   WWHD_DIFF_EVERY=K         ... and every K-th call after that (default 256; 0 = never)
//   WWHD_RT_LOG=path          the log; per-function results go to path.funcs.csv at exit
#include "rt_internal.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

bool g_rtJournalOn = false;
void (*g_rtStoreCensus)(uint32 ea, uint32 size, uint32 pc) = nullptr;
const uint32* g_rtStorePages = nullptr;
const PPCInterpreter_t* g_rtJournalThread = nullptr;
bool g_rtStoreAll = false;
uint64 g_rtStoresPassed = 0;
const uint64* g_rtStoreBits = nullptr;
const int* g_rtStoreHold = nullptr;

namespace wwhd::rt
{
	using Clock = std::chrono::steady_clock;

	// ---- native store journal ------------------------------------------------------------------
	struct JournalEntry { uint32 ea, size, offset; };
	static std::vector<JournalEntry> s_journal;
	static std::vector<uint8> s_journalBytes;          // the bytes each store overwrote
}

void rt_journal_store(uint32 ea, uint32 size, uint32 pc)
{
	using namespace wwhd::rt;
	// the half tick's journal is the frame thread's (g_rtJournalThread, set by overrides/sixty.cpp): another guest
	// thread running meanwhile (the sound's AX thread, its frame callbacks every 3 ms) is neither put back nor watched
	// there, and its stores going through all of it made the game's audio code about six times dearer at 60 fps
	// (docs/research/perf-baseline.md). And the half tick's journal takes only stores into the pages it marked (most
	// of a frame's stores are into the heap: matrices, display lists, packets; RT_STORE checks this too, without the
	// call; this is for the stores made by calling here directly). A store it doesn't take still goes to a fast
	// path's quiet watch (another thread's store, or one outside the watched call's dead stack, ends it), and only
	// there: before, under a watch every store went through the half tick's hook as well, which drops them
	const uint32* pages = g_rtStorePages;
	// the half tick's journal's bytes that need no call (g_rtStoreBits, override.h): a store into those only is passed
	// over here, before the rest (most of a half tick's stores, ~32,000 a frame on continue, go again into bytes saved
	// already: the matrix stack, a draw's fields)
	if (const uint64* bits = g_rtStoreBits; bits && pages && !*g_rtStoreHold && !QuietLive())
	{
		const uint32 e = pages[ea >> 12], off = ea & 0xFFF;
		if (e && off + size <= 0x1000 && size <= 32)
		{
			const uint64* b = bits + ((e & 0x3FFFFFFFu) - 1) * 64;
			const uint32 i0 = off >> 6, i1 = (off + size - 1) >> 6;
			uint64 m0, m1 = 0;
			if (i0 == i1)
				m0 = ((1ull << size) - 1) << (off & 63);
			else
			{
				m0 = ~0ull << (off & 63);
				const uint32 end = (off + size) & 63;
				m1 = end ? (1ull << end) - 1 : ~0ull;
			}
			if ((b[i0] & m0) == m0 && (b[i1] & m1) == m1)
			{
				g_rtStoresPassed++;
				return;
			}
		}
	}
	const PPCInterpreter_t* t = g_rtJournalThread;
	if ((t && PPCInterpreter_getCurrentInstance() != t) || (pages && !pages[ea >> 12] && !pages[(ea + size - 1) >> 12]))
	{
		if (QuietLive())
			QuietStore(ea, size);
		else
			g_rtStoresPassed++;
		return;
	}
	if (g_rtStoreCensus) [[unlikely]]                // the 60 fps tools (overrides/sixty.cpp)
	{
		g_rtStoreCensus(ea, size, pc);
		if (QuietLive())                             // and a fast path's watch (real time at 60 fps)
			QuietStore(ea, size);
		return;
	}
	if (g_quiet.token) [[unlikely]]                  // the fast paths' quiet watch (dispatch.cpp), not a diff run
	{
		QuietStore(ea, size);
		return;
	}
	s_journal.push_back({ ea, size, (uint32)s_journalBytes.size() });
	s_journalBytes.insert(s_journalBytes.end(), memory_base + ea, memory_base + ea + size);
}

namespace wwhd::rt
{
	// ---- guest state generated code reads and writes (D2) ------------------------------------------
	struct Regs
	{
		uint32 gpr[32];
		uint64 fpr[32][2];
		uint32 fpscr;
		uint8 cr[32];
		uint8 xer_ca, xer_so, xer_ov;
		uint32 LR, CTR, XER;
		uint32 UGQR[8];
		uint32 resAddr, resValue;
	};

	static void Capture(Regs& r, const PPCInterpreter_t* c)
	{
		memcpy(r.gpr, c->gpr, sizeof(r.gpr));
		for (int i = 0; i < 32; i++)
		{
			r.fpr[i][0] = c->fpr[i].fp0int;
			r.fpr[i][1] = c->fpr[i].fp1int;
		}
		r.fpscr = c->fpscr;
		memcpy(r.cr, c->cr, sizeof(r.cr));
		r.xer_ca = c->xer_ca; r.xer_so = c->xer_so; r.xer_ov = c->xer_ov;
		// XER as mfspr reads it (PPCInterpreter_getXER): CA/SO/OV live in xer_ca/so/ov, and spr.XER's
		// copies of those bits are stale after a context switch reloads the thread's XER
		r.LR = c->spr.LR; r.CTR = c->spr.CTR;
		r.XER = c->spr.XER & ~((1u << XER_BIT_CA) | (1u << XER_BIT_SO) | (1u << XER_BIT_OV));
		memcpy(r.UGQR, c->spr.UGQR, sizeof(r.UGQR));
		r.resAddr = c->reservedMemAddr; r.resValue = c->reservedMemValue;
	}

	using Bytes = std::vector<std::pair<uint32, uint8>>;   // (address, value), sorted by address

	// keep the last value written to each address (entries in store order)
	static void Canonical(Bytes& b)
	{
		std::stable_sort(b.begin(), b.end(), [](auto& x, auto& y) { return x.first < y.first; });
		size_t n = 0;
		for (size_t i = 0; i < b.size(); i++)
		{
			if (n && b[n - 1].first == b[i].first)
				b[n - 1] = b[i];
			else
				b[n++] = b[i];
		}
		b.resize(n);
	}

	// ---- the interpreter's stores, decoded before it executes each instruction ------------------------
	enum class StoreForm : uint8 { None, D, Psq, PsqX, X, Stmw, Stswi, Stswx, Dcbz };
	struct StoreInsn { const char* name = nullptr; StoreForm form = StoreForm::None; uint8 size = 0; bool conditional = false; };

	// Mirrors tools/recomp/ppc.py's decoding of the store instructions (generate.py STORES).
	static StoreInsn DecodeStore(uint32 w)
	{
		using F = StoreForm;
		switch (w >> 26)
		{
		case 36: return { "stw", F::D, 4 };     case 37: return { "stwu", F::D, 4 };
		case 38: return { "stb", F::D, 1 };     case 39: return { "stbu", F::D, 1 };
		case 44: return { "sth", F::D, 2 };     case 45: return { "sthu", F::D, 2 };
		case 47: return { "stmw", F::Stmw };
		case 52: return { "stfs", F::D, 4 };    case 53: return { "stfsu", F::D, 4 };
		case 54: return { "stfd", F::D, 8 };    case 55: return { "stfdu", F::D, 8 };
		case 60: return { "psq_st", F::Psq };   case 61: return { "psq_stu", F::Psq };
		case 4:
			switch ((w >> 1) & 0x3F)
			{
			case 7: return { "psq_stx", F::PsqX };
			case 39: return { "psq_stux", F::PsqX };
			}
			return {};                          // dcbz_l is a no-op in Cemu's interpreter
		case 31:
			switch ((w >> 1) & 0x3FF)
			{
			case 151: return { "stwx", F::X, 4 };    case 183: return { "stwux", F::X, 4 };
			case 215: return { "stbx", F::X, 1 };    case 247: return { "stbux", F::X, 1 };
			case 407: return { "sthx", F::X, 2 };    case 439: return { "sthux", F::X, 2 };
			case 662: return { "stwbrx", F::X, 4 };  case 918: return { "sthbrx", F::X, 2 };
			case 663: return { "stfsx", F::X, 4 };   case 695: return { "stfsux", F::X, 4 };
			case 727: return { "stfdx", F::X, 8 };   case 759: return { "stfdux", F::X, 8 };
			case 983: return { "stfiwx", F::X, 4 };
			case 150: return { "stwcx.", F::X, 4, true };
			case 725: return { "stswi", F::Stswi };
			case 661: return { "stswx", F::Stswx };
			case 1014: return { "dcbz", F::Dcbz };
			}
		}
		return {};
	}

	static uint32 PsqBytes(const PPCInterpreter_t* c, uint32 gqr, bool single)
	{
		uint32 type = c->spr.UGQR[gqr] & 7;
		uint32 elem = (type == 4 || type == 6) ? 1 : (type == 5 || type == 7) ? 2 : 4;
		return single ? elem : elem * 2;
	}

	// the range the store will write, from the registers before it executes
	static uint32 StoreRange(const PPCInterpreter_t* c, uint32 w, const StoreInsn& s, uint32& ea)
	{
		using F = StoreForm;
		uint32 rS = (w >> 21) & 31, rA = (w >> 16) & 31, rB = (w >> 11) & 31;
		uint32 base = rA ? c->gpr[rA] : 0;
		switch (s.form)
		{
		case F::D: ea = base + (uint32)(sint32)(sint16)(w & 0xFFFF); return s.size;
		case F::Stmw: ea = base + (uint32)(sint32)(sint16)(w & 0xFFFF); return (32 - rS) * 4;
		case F::Psq: ea = base + (uint32)(((sint32)(w << 20)) >> 20); return PsqBytes(c, (w >> 12) & 7, (w >> 15) & 1);
		case F::PsqX: ea = base + c->gpr[rB]; return PsqBytes(c, (w >> 7) & 7, (w >> 10) & 1);
		case F::X: ea = base + c->gpr[rB]; return s.size;
		case F::Stswi: ea = base; return rB ? rB : 32;
		case F::Stswx: ea = base + c->gpr[rB]; return c->spr.XER & 0x7F;
		case F::Dcbz: ea = (base + c->gpr[rB]) & ~31u; return 32;
		case F::None: break;
		}
		return 0;
	}

	// ---- samples ---------------------------------------------------------------------------------
	struct Sample
	{
		sint32 func;
		uint64 call;
		uint32 ret, sp;
		Regs entry, native;
		Bytes nativeWrites, interpWrites;
		uint64 nativeCycles = 0;
		uint64 steps = 0;
		uint32 preempted = 0;
	};

	struct FuncStats
	{
		uint64 calls = 0, samples = 0, ok = 0, mismatch = 0, preempted = 0, preemptedMismatch = 0, escaped = 0,
			runaway = 0, nativeFault = 0, nanTolerated = 0;
		uint32 reported = 0;
	};

	static constexpr uint64 kRunawaySteps = 500'000'000;
	static constexpr int kWatchdogSeconds = 60;

	static std::vector<uint8> s_eligible;               // per code word: a function entry to check
	static std::vector<FuncStats> s_stats;
	static std::unordered_map<PPCInterpreter_t*, Sample*> s_pending;   // per guest thread
	static std::mutex s_pendingMutex;
	static bool s_only = false;
	static uint64 s_first = 16, s_every = 256;
	static std::string s_csvPath;
	static Clock::time_point s_lastReport;
	static std::atomic<uint64> s_nativeSince{ 0 };       // steady ns when a native run began, 0 if none
	static std::atomic<uint32> s_nativeFunc{ 0 };
	static uint64 s_detailed = 0;                         // mismatches reported in detail so far
	static jmp_buf s_nativeJmp;                           // RunNative's way out of a native fault
	static uint32 s_faultEa, s_faultTarget;

	static std::unordered_set<uint32> ParseAddresses(const char* env)
	{
		std::unordered_set<uint32> out;
		const char* s = getenv(env);
		while (s && *s)
		{
			char* end;
			uint32 a = (uint32)strtoul(s, &end, 16);
			if (end == s)
				Fatal("%s: can't parse '%s'", env, s);
			out.insert(a);
			s = *end == ',' ? end + 1 : end;
		}
		return out;
	}

	static uint64 EnvU64(const char* name, uint64 def)
	{
		const char* s = getenv(name);
		return (s && *s) ? strtoull(s, nullptr, 0) : def;
	}

	// the store decoder must find exactly the stores the generator counted (a decoding cross-check)
	static void CheckStoreDecoder()
	{
		std::unordered_map<std::string, uint64> counts;
		for (size_t i = 0; i < g_funcCount; i++)
		{
			const RecompFunc& f = g_funcTable[i];
			if (f.flags & kRecompSynthetic)
				continue;
			for (uint32 ea = f.address; ea < f.end; ea += 4)
				if (const char* n = DecodeStore(memory_readU32(ea)).name)
					counts[n]++;
		}
		uint64 total = 0;
		for (size_t i = 0; i < g_storeCensusCount; i++)
		{
			const RecompStoreCount& c = g_storeCensus[i];
			uint64 got = counts.count(c.mnemonic) ? counts[c.mnemonic] : 0;
			if (got != c.count)
				Fatal("store decoder finds %llu %s in guest memory, the generator counted %u",
					(unsigned long long)got, c.mnemonic, c.count);
			counts.erase(c.mnemonic);
			total += got;
		}
		if (!counts.empty())
			Fatal("store decoder finds %s, which the generator doesn't list", counts.begin()->first.c_str());
		Log("store decoder agrees with the generator's census: %llu stores", (unsigned long long)total);
	}

	static void Watchdog()
	{
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
			uint64 since = s_nativeSince.load();
			uint64 now = Clock::now().time_since_epoch().count();
			if (since && now - since > (uint64)kWatchdogSeconds * 1'000'000'000ull)
				Fatal("native run of f_%08X hasn't returned after %d s (a loop waiting on another thread?); "
					"exclude it with WWHD_DIFF_EXCLUDE", s_nativeFunc.load(), kWatchdogSeconds);
		}
	}

	static void AtExit()
	{
		DiffReport(true);
	}

	bool DiffInit()
	{
		const char* mode = getenv("WWHD_NATIVE");
		if (!mode || strcmp(mode, "diff") != 0)
			return false;
		auto only = ParseAddresses("WWHD_DIFF_ONLY");
		auto exclude = ParseAddresses("WWHD_DIFF_EXCLUDE");
		s_only = !only.empty();
		s_first = EnvU64("WWHD_DIFF_FIRST", 16);
		s_every = EnvU64("WWHD_DIFF_EVERY", 256);
		CheckStoreDecoder();
		s_eligible.assign(g_codeWords, 0);
		s_stats.assign(g_funcCount, {});
		// a function whose native code would reach patched code (itself or a callee, transitively)
		// can't match the interpreter, which runs the patched code (D10)
		std::vector<std::vector<uint32>> callers(g_funcCount);
		for (size_t i = 0; i < g_funcCount; i++)
			for (uint32 k = 0; k < g_funcTable[i].calleeCount; k++)
				callers[g_callees[g_funcTable[i].calleeFirst + k]].push_back((uint32)i);
		std::vector<uint8> tainted(g_patched);
		std::vector<uint32> work;
		for (size_t i = 0; i < g_funcCount; i++)
			if (tainted[i])
				work.push_back((uint32)i);
		while (!work.empty())
		{
			uint32 f = work.back();
			work.pop_back();
			for (uint32 c : callers[f])
				if (!tainted[c])
				{
					tainted[c] = 1;
					work.push_back(c);
				}
		}
		size_t eligible = 0, patchedPure = 0;
		for (size_t i = 0; i < g_funcCount; i++)
		{
			const RecompFunc& f = g_funcTable[i];
			if (!(f.flags & kRecompPure))
				continue;
			if (tainted[i])
			{
				patchedPure++;
				continue;
			}
			if (exclude.count(f.address) || (s_only && !only.count(f.address)))
				continue;
			s_eligible[(f.address - g_codeBase) / 4] = 1;
			eligible++;
		}
		for (uint32 a : only)
			if (FuncIndexAt(a) < 0)
				Fatal("WWHD_DIFF_ONLY: %08X is not a function entry", a);
		if (const char* log = getenv("WWHD_RT_LOG"); log && *log)
			s_csvPath = std::string(log) + ".funcs.csv";
		s_lastReport = Clock::now();
		std::thread(Watchdog).detach();
		at_quick_exit(AtExit);                          // the trace's exit-at-frame (patch 0011)
		atexit(AtExit);
		if (s_only)
			Log("diff mode: %zu function(s), every call", eligible);
		else
			Log("diff mode: %zu pure functions (%zu that reach patched code are skipped), first %llu calls each, then every %llu-th",
				eligible, patchedPure, (unsigned long long)s_first, (unsigned long long)s_every);
		return true;
	}

	bool DiffInNative()
	{
		return s_nativeSince.load(std::memory_order_relaxed) != 0;
	}

	void DiffNativeFault(uint32 ea, uint32 target)
	{
		s_faultEa = ea;
		s_faultTarget = target;
		longjmp(s_nativeJmp, 1);
	}

	// run the function natively from the current state, record what it did, and undo it; false if
	// the native run faulted (it is undone all the same)
	static bool RunNative(PPCInterpreter_t* hCPU, const RecompFunc& f, Sample& s)
	{
		PPCInterpreter_t saved = *hCPU;
		s_journal.clear();
		s_journalBytes.clear();
		s_nativeFunc = f.address;
		s_nativeSince = Clock::now().time_since_epoch().count();
		g_rtJournalOn = true;
		hCPU->remainingCycles = INT32_MAX;             // never yields; the ticks it spends are counted
		volatile bool ok = true;
		if (setjmp(s_nativeJmp) == 0)
			f.fn(hCPU);
		else
			ok = false;
		g_rtJournalOn = false;
		s_nativeSince = 0;
		s.nativeCycles = (uint64)((sint64)INT32_MAX - hCPU->remainingCycles);
		Capture(s.native, hCPU);
		for (const JournalEntry& j : s_journal)
			for (uint32 k = 0; k < j.size; k++)
				s.nativeWrites.emplace_back(j.ea + k, memory_base[j.ea + k]);
		Canonical(s.nativeWrites);                      // (values are already final)
		for (auto j = s_journal.rbegin(); j != s_journal.rend(); ++j)
			memcpy(memory_base + j->ea, s_journalBytes.data() + j->offset, j->size);
		*hCPU = saved;
		return ok;
	}

	static Sample* MaybeStart(PPCInterpreter_t* hCPU, uint32 ip)
	{
		sint32 fi = FuncIndexAt(ip);
		FuncStats& st = s_stats[fi];
		uint64 n = ++st.calls;
		if (!s_only && n > s_first && (s_every == 0 || n % s_every != 0))
			return nullptr;
		Sample* s = new Sample;
		s->func = fi;
		s->call = n;
		Capture(s->entry, hCPU);
		st.samples++;
		if (!RunNative(hCPU, g_funcTable[fi], *s))
		{
			st.nativeFault++;
			if (st.reported++ < 3)
				Log("NATIVE FAULT f_%08X call %llu: rt_bad_branch at %08X to %08X (LR %08X, r3-r6 %08X %08X %08X %08X)",
					g_funcTable[fi].address, (unsigned long long)n, s_faultEa, s_faultTarget, s->entry.LR,
					s->entry.gpr[3], s->entry.gpr[4], s->entry.gpr[5], s->entry.gpr[6]);
			delete s;
			return nullptr;
		}
		s->ret = s->native.LR & ~3u;                    // where the native run left the guest
		s->sp = s->native.gpr[1];
		return s;
	}

	static bool SameFloat(uint64 a, uint64 b, bool& nan)
	{
		if (a == b)
			return true;
		double x, y;
		memcpy(&x, &a, 8);
		memcpy(&y, &b, 8);
		if (std::isnan(x) && std::isnan(y))   // x86 NaN payload propagation (design, open question 4)
		{
			nan = true;
			return true;
		}
		return false;
	}

	// differing registers as text ("" if none)
	static std::string RegDiff(const Regs& n, const Regs& i, bool& nan)
	{
		std::string out;
		auto add = [&](const char* name, int idx, uint64 nv, uint64 iv) {
			char buf[96];
			snprintf(buf, sizeof(buf), " %s%s native=%llX interp=%llX", name, idx >= 0 ? std::to_string(idx).c_str() : "",
				(unsigned long long)nv, (unsigned long long)iv);
			out += buf;
		};
		for (int r = 0; r < 32; r++)
			if (n.gpr[r] != i.gpr[r]) add("r", r, n.gpr[r], i.gpr[r]);
		for (int r = 0; r < 32; r++)
		{
			if (!SameFloat(n.fpr[r][0], i.fpr[r][0], nan)) add("f", r, n.fpr[r][0], i.fpr[r][0]);
			if (!SameFloat(n.fpr[r][1], i.fpr[r][1], nan)) add("ps1_f", r, n.fpr[r][1], i.fpr[r][1]);
		}
		for (int b = 0; b < 32; b++)
			if (n.cr[b] != i.cr[b]) add("crbit", b, n.cr[b], i.cr[b]);
		if (n.fpscr != i.fpscr) add("fpscr", -1, n.fpscr, i.fpscr);
		if (n.xer_ca != i.xer_ca) add("ca", -1, n.xer_ca, i.xer_ca);
		if (n.xer_so != i.xer_so) add("so", -1, n.xer_so, i.xer_so);
		if (n.xer_ov != i.xer_ov) add("ov", -1, n.xer_ov, i.xer_ov);
		if (n.XER != i.XER) add("xer", -1, n.XER, i.XER);
		if (n.LR != i.LR) add("lr", -1, n.LR, i.LR);
		if (n.CTR != i.CTR) add("ctr", -1, n.CTR, i.CTR);
		for (int g = 0; g < 8; g++)
			if (n.UGQR[g] != i.UGQR[g]) add("gqr", g, n.UGQR[g], i.UGQR[g]);
		if (n.resAddr != i.resAddr) add("resaddr", -1, n.resAddr, i.resAddr);
		if (n.resValue != i.resValue) add("resval", -1, n.resValue, i.resValue);
		return out;
	}

	static std::string MemDiff(const Bytes& n, const Bytes& i, size_t& count)
	{
		std::string out;
		size_t a = 0, b = 0;
		count = 0;
		auto add = [&](uint32 addr, int nv, int iv) {
			if (++count > 8)
				return;
			char buf[64];
			snprintf(buf, sizeof(buf), " [%08X] native=%s interp=%s", addr,
				nv < 0 ? "-" : fmt::format("{:02X}", nv).c_str(), iv < 0 ? "-" : fmt::format("{:02X}", iv).c_str());
			out += buf;
		};
		while (a < n.size() || b < i.size())
		{
			if (b == i.size() || (a < n.size() && n[a].first < i[b].first)) { add(n[a].first, n[a].second, -1); a++; }
			else if (a == n.size() || i[b].first < n[a].first) { add(i[b].first, -1, i[b].second); b++; }
			else { if (n[a].second != i[b].second) add(n[a].first, n[a].second, i[b].second); a++; b++; }
		}
		return out;
	}

	static void Finish(PPCInterpreter_t* hCPU, Sample& s)
	{
		FuncStats& st = s_stats[s.func];
		Regs interp;
		Capture(interp, hCPU);
		Canonical(s.interpWrites);
		bool nan = false;
		std::string regs = RegDiff(s.native, interp, nan);
		if (s.nativeCycles != s.steps)
			regs += fmt::format(" cycles native={} interp={}", s.nativeCycles, s.steps);
		size_t memCount = 0;
		std::string mem = MemDiff(s.nativeWrites, s.interpWrites, memCount);
		st.nanTolerated += nan;
		if (s.preempted)
			st.preempted++;
		if (regs.empty() && memCount == 0)
		{
			st.ok++;
			return;
		}
		if (s.preempted)
			st.preemptedMismatch++;
		else
			st.mismatch++;
		if (st.reported++ < 3 && s_detailed++ < 500)
		{
			const RecompFunc& f = g_funcTable[s.func];
			Log("MISMATCH%s f_%08X call %llu (LR %08X, r3-r6 %08X %08X %08X %08X, %llu instructions, %zu/%zu bytes stored):%s%s%s",
				s.preempted ? " (preempted)" : "", f.address, (unsigned long long)s.call, s.entry.LR, s.entry.gpr[3],
				s.entry.gpr[4], s.entry.gpr[5], s.entry.gpr[6], (unsigned long long)s.steps, s.nativeWrites.size(),
				s.interpWrites.size(), regs.c_str(), memCount ? " mem:" : "", mem.c_str());
			if (memCount > 8)
				Log("  ... %zu differing bytes in all", memCount);
		}
	}

	static void Drop(Sample* s, bool escaped, uint32 ip)
	{
		FuncStats& st = s_stats[s->func];
		(escaped ? st.escaped : st.runaway)++;
		if (st.reported++ < 3)
			Log("%s f_%08X call %llu at %08X after %llu instructions: not checked", escaped ? "ESCAPED" : "RUNAWAY",
				g_funcTable[s->func].address, (unsigned long long)s->call, ip, (unsigned long long)s->steps);
		delete s;
	}

	void DiffExecute(PPCInterpreter_t* hCPU)
	{
		Sample* s = nullptr;
		{
			std::unique_lock _l(s_pendingMutex);
			if (auto it = s_pending.find(hCPU); it != s_pending.end())
			{
				s = it->second;
				s_pending.erase(it);
			}
		}
		if (Clock::now() - s_lastReport > std::chrono::seconds(30))
			DiffReport(false);

		while ((--hCPU->remainingCycles) >= 0)
		{
			uint32 ip = hCPU->instructionPointer;
			if (s && ip == s->ret && hCPU->gpr[1] == s->sp)
			{
				Finish(hCPU, *s);
				delete s;
				s = nullptr;
			}
			if (!s)
			{
				uint32 w = (ip - g_codeBase) >> 2;
				if (w < g_codeWords && s_eligible[w]) [[unlikely]]
					s = MaybeStart(hCPU, ip);
				if (!s)
				{
					PPCInterpreterSlim_executeInstruction(hCPU);
					continue;
				}
			}
			// inside a checked call: execute, recording what the instruction stores
			uint32 word = memory_readU32(ip);
			if ((word >> 26) == 1 || ((ip - g_codeBase) >> 2) >= g_codeWords)
			{
				Drop(s, true, ip);
				s = nullptr;
				PPCInterpreterSlim_executeInstruction(hCPU);
				continue;
			}
			StoreInsn st = DecodeStore(word);
			uint32 ea = 0, size = st.name ? StoreRange(hCPU, word, st, ea) : 0;
			PPCInterpreterSlim_executeInstruction(hCPU);
			if (size && (!st.conditional || hCPU->cr[CR_BIT_EQ]))
				for (uint32 k = 0; k < size; k++)
					s->interpWrites.emplace_back(ea + k, memory_base[ea + k]);
			if (++s->steps > kRunawaySteps)
			{
				Drop(s, false, hCPU->instructionPointer);
				s = nullptr;
			}
		}
		if (s)
		{
			s->preempted++;
			std::unique_lock _l(s_pendingMutex);
			s_pending[hCPU] = s;
		}
	}

	void DiffReport(bool final)
	{
		FuncStats t;
		size_t called = 0, checked = 0, clean = 0, failing = 0;
		for (const FuncStats& st : s_stats)
		{
			t.calls += st.calls; t.samples += st.samples; t.ok += st.ok; t.mismatch += st.mismatch;
			t.preempted += st.preempted; t.preemptedMismatch += st.preemptedMismatch; t.escaped += st.escaped;
			t.runaway += st.runaway; t.nativeFault += st.nativeFault; t.nanTolerated += st.nanTolerated;
			called += st.calls != 0;
			checked += (st.ok + st.mismatch + st.preemptedMismatch) != 0;
			clean += st.ok != 0 && st.mismatch == 0 && st.preemptedMismatch == 0 && st.escaped == 0 && st.nativeFault == 0
				&& st.runaway == 0;
			failing += st.mismatch != 0 || st.escaped != 0 || st.nativeFault != 0 || st.runaway != 0;
		}
		size_t pending;
		{
			std::unique_lock _l(s_pendingMutex);
			pending = s_pending.size();
		}
		Log("diff%s: pure calls %llu, checked %llu: ok %llu, MISMATCH %llu, preempted %llu (of which mismatched %llu), "
			"escaped %llu, runaway %llu, native faults %llu, NaN-tolerated %llu, pending %zu; functions called %zu, "
			"checked %zu, clean %zu, failing %zu",
			final ? " (final)" : "", (unsigned long long)t.calls, (unsigned long long)(t.ok + t.mismatch + t.preemptedMismatch),
			(unsigned long long)t.ok, (unsigned long long)t.mismatch, (unsigned long long)t.preempted,
			(unsigned long long)t.preemptedMismatch, (unsigned long long)t.escaped, (unsigned long long)t.runaway,
			(unsigned long long)t.nativeFault, (unsigned long long)t.nanTolerated, pending, called, checked, clean, failing);
		s_lastReport = Clock::now();
		if (!final || s_csvPath.empty())
			return;
		if (FILE* f = fopen(s_csvPath.c_str(), "w"))
		{
			fputs("address,calls,samples,ok,mismatch,preempted,preempted_mismatch,escaped,runaway,native_fault,nan_tolerated\n", f);
			for (size_t i = 0; i < s_stats.size(); i++)
			{
				const FuncStats& st = s_stats[i];
				if (st.calls)
					fprintf(f, "%08X,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", g_funcTable[i].address,
						(unsigned long long)st.calls, (unsigned long long)st.samples, (unsigned long long)st.ok,
						(unsigned long long)st.mismatch, (unsigned long long)st.preempted,
						(unsigned long long)st.preemptedMismatch, (unsigned long long)st.escaped,
						(unsigned long long)st.runaway, (unsigned long long)st.nativeFault, (unsigned long long)st.nanTolerated);
			}
			fclose(f);
		}
	}
}
