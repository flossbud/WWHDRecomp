// Save states (savestate.h): the save point, the file, the threads. The modules' host records are theirs
// (WWHD_STATE_RECORD in each file); this file holds what's common: parking the main thread, coming to rest, guest memory,
// rebuilding the game's threads, and the probe WWHD_STATE_CENSUS=SWAP[,SWAP...] (at the first whole tick from each swap,
// every guest thread's state, the host alarms pending and the files open, in the log).
//
// At rest: no guest thread on a core, the GPU has retired everything submitted, and each guest thread resumable from what
// guest memory holds:
//  - waiting in an OS call (srr0 its trampoline, LR in the game's code): a load runs the call again when it's woken (its
//    arguments are still in its registers; OSReceiveMessage, the only one the game's threads wait in at a frame's top,
//    loops back to the same check when woken), without the cycles its first run was charged;
//  - a system thread (its body the OS layer's: srr0 and LR both trampolines) waiting in that body: a load keeps the
//    fresh process's own, waiting in the same place (the game creates every thread by the title: the same 32 at the
//    same addresses at swap 60 and 1200);
//  - ready in the game's code (srr0 the next instruction: exact in wwhd-null, a loop header or function entry in
//    wwhd-play) with no callback from the OS layer under it (host frames the context doesn't hold): a load interprets
//    the rest of its function (the dispatcher's fallback), then native code goes on. OliveOperationMgrThread polls its
//    queue without waiting, so "every thread waiting" never comes;
//  - the parked main thread (srr0 fw_procFrame: a load runs the frame from its entry).
//
// The file: "WWHDSTAT", a version, the records (zstd), then guest memory: every mapped range in 64 KiB blocks, the
// zero ones only flagged (a load writes a block only where this process's differs: untouched pages stay unbacked).
#include "savestate.h"
#include "os.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/gx2/GX2_Command.h"
#include "Cafe/HW/MMU/MMU.h"
#include "../runtime/write_watch.h"
#include <zstd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

extern MPTR activeThread[256];
extern sint32 activeThreadCount;
extern std::string s_ppcHleNames[];
extern sint32 s_ppcHleTableWriteIndex;

namespace coreinit
{
	bool __OSNoThreadOnACore();                    // coreinit_Thread.cpp
	PPCInterpreter_t* __OSHostThreadInstance(OSThread_t* thread);
	void __OSReplaceHostThread(OSThread_t* thread);
	sint32 __OSResumeThreadInternal(OSThread_t* thread, sint32 resumeCount);
	void __OSHostAlarmsForEach(void (*fn)(uint64, uint64, void*, void*, OSAlarm_t*, void*), void* user);
}
namespace iosu::fsa
{
	void CountOpen(uint32& files, uint32& dirs);
}
namespace wwhd::rt
{
	bool InCallback(PPCInterpreter_t* hCPU);
}

namespace wwhd::state
{
	namespace
	{
		constexpr uint32 kSavePoint = 0x0274C264;      // fw_procFrame: where the parked main thread resumes
		constexpr uint32 kVersion = 2;                  // 2: each record framed with its length
		constexpr uint32 kBlock = 0x10000;

		enum class Action { None, Census, Save, Load };
		std::atomic<OSThread_t*> s_parked{ nullptr };   // the main thread, parked at the save point (Idle takes it)
		Action s_action = Action::None;
		uint64 s_parkedAt = 0;                          // when (guest time): the game gets half a second to come to rest
		std::unordered_map<MPTR, uint32> s_busy;        // while parked: how often each thread kept the game from rest (a probe)
		bool s_loadDone = false;                        // the load asked for (WWHD_STATE_LOAD) is over: done or given up
		std::mutex s_slotLock;
		std::string s_dir;                              // portable/states
		int s_request = 0;                              // the menu's: n > 0 save to slot n, n < 0 load slot -n
		std::string s_status;
		std::string s_slotPath;                         // the slot the menu's request is for (the main thread: AtFrameTop, Idle)
		bool s_fromMenu = false;

		std::string SlotPath(int slot)
		{
			return s_dir + "/slot" + std::to_string(slot) + ".state";
		}
		void SetStatus(std::string text)
		{
			std::lock_guard lock(s_slotLock);
			s_status = std::move(text);
		}
		uint32 s_loadedAt = 0;                          // this process's swap when it loaded (the log)
		uint32 s_loadTries = 0;                         // save points a load has waited at for this process to come as far
		std::unordered_map<OSThread_t*, bool> s_exact;  // each thread's last switch-out: exact (NoteSwitchOut; scheduler lock)
		std::mutex s_rerunLock;
		std::set<MPTR> s_rerun;                         // threads a load built anew that wait in an OS call (FiberStart)

		std::map<std::string, RecordFn>& Records()
		{
			static std::map<std::string, RecordFn> records;
			return records;
		}

		std::vector<uint32> Swaps(const char* env)
		{
			std::vector<uint32> v;
			if (const char* e = getenv(env))
				for (const char* p = e; *p;)
				{
					char* end;
					const unsigned long n = strtoul(p, &end, 0);
					if (end == p)
						break;
					v.push_back((uint32)n);
					p = *end == ',' ? end + 1 : end;
				}
			return v;
		}

		// WWHD_STATE_SAVE=SWAP:PATH
		struct SaveRequest
		{
			uint32 swap = ~0u;
			std::string path;
		};
		const SaveRequest& TheSaveRequest()
		{
			static const SaveRequest r = [] {
				SaveRequest r;
				if (const char* e = getenv("WWHD_STATE_SAVE"))
					if (const char* colon = strchr(e, ':'))
						r.swap = (uint32)strtoul(e, nullptr, 0), r.path = colon + 1;
				return r;
			}();
			return r;
		}
		// WWHD_STATE_LOAD_AT=SWAP:PATH (a test: statetest.sh STATE_INPLACE): in play, at the first whole tick from SWAP, a
		// load in place as the debug menu's
		const SaveRequest& TheLoadAtRequest()
		{
			static const SaveRequest r = [] {
				SaveRequest r;
				if (const char* e = getenv("WWHD_STATE_LOAD_AT"))
					if (const char* colon = strchr(e, ':'))
						r.swap = (uint32)strtoul(e, nullptr, 0), r.path = colon + 1;
				return r;
			}();
			return r;
		}
		const std::string& TheLoadPath()
		{
			static const std::string path = [] { const char* e = getenv("WWHD_STATE_LOAD"); return std::string(e ? e : ""); }();
			return path;
		}

		bool InTrampolines(uint32 addr) { return addr >= 0x00E00000 && addr < 0x01000000; }
		bool InText(uint32 addr) { return addr >= 0x02000000 && addr < 0x10000000; }

		// the OS function an address is the trampoline of (an HLE opcode: primary opcode 1, the table's index below), or -1
		sint32 HleIndex(uint32 addr)
		{
			if (!InTrampolines(addr) || !memory_isAddressRangeAccessible(addr, 4))
				return -1;
			const uint32 op = memory_read<uint32>(addr);
			if ((op >> 26) != 1 || (op & 0xFFFF) >= (uint32)s_ppcHleTableWriteIndex)
				return -1;
			return (sint32)(op & 0xFFFF);
		}
		std::string HleName(uint32 addr)
		{
			const sint32 i = HleIndex(addr);
			return i < 0 ? std::string() : s_ppcHleNames[i];
		}

		const char* StateName(OSThread_t::THREAD_STATE s)
		{
			switch (s)
			{
			case OSThread_t::THREAD_STATE::STATE_NONE: return "none";
			case OSThread_t::THREAD_STATE::STATE_READY: return "ready";
			case OSThread_t::THREAD_STATE::STATE_RUNNING: return "running";
			case OSThread_t::THREAD_STATE::STATE_WAITING: return "waiting";
			case OSThread_t::THREAD_STATE::STATE_MORIBUND: return "moribund";
			default: return "?";
			}
		}

		bool SystemThread(OSThread_t* t, OSThread_t* parked)
		{
			return t != parked && InTrampolines(t->context.srr0) && InTrampolines(_swapEndianU32(t->context.lr));
		}

		// how a thread resumes after a load (the top of this file), or why it can't yet (nullptr: it can)
		const char* NotResumable(OSThread_t* t, OSThread_t* parked)
		{
			using S = OSThread_t::THREAD_STATE;
			const uint32 srr0 = t->context.srr0;
			if (t == parked || t->state == S::STATE_NONE || t->state == S::STATE_MORIBUND)
				return nullptr;
			if (t->state == S::STATE_RUNNING)
				return "on a core";
			if (t->state == S::STATE_WAITING)
				return HleIndex(srr0) >= 0 ? nullptr : "waiting outside an OS call";
			if (!InText(srr0))
				return SystemThread(t, parked) ? "a system thread not waiting" : "ready in an OS call";
			if (auto it = s_exact.find(t); it == s_exact.end() || !it->second)
				return "stopped in host code (its IP is stale)";
			PPCInterpreter_t* cpu = coreinit::__OSHostThreadInstance(t);
			if (cpu && wwhd::rt::InCallback(cpu))
				return "in a callback from the OS layer";
			return nullptr;
		}

		bool AtRest(OSThread_t* parked)
		{
			if (!coreinit::__OSNoThreadOnACore() || GX2::GX2GetRetiredTimeStamp() < GX2::GX2GetLastSubmittedTimeStamp())
				return false;
			bool rest = true;
			for (sint32 i = 0; i < activeThreadCount; i++)
				if (NotResumable(MEMPTR<OSThread_t>(activeThread[i]).GetPtr(), parked))
				{
					s_busy[activeThread[i]]++;
					rest = false;
				}
			return rest;
		}

		void Census(OSThread_t* parked, const char* what)
		{
			uint32 files = 0, dirs = 0;
			iosu::fsa::CountOpen(files, dirs);
			cemuLog_log(LogType::Force, "wwhd state census{}: swap {}, {} threads, {} OS calls registered, {} files and {} directories open, now {}",
				what, wwhd::os::SwapCount(), activeThreadCount, s_ppcHleTableWriteIndex, files, dirs, coreinit::OSGetTime());
			coreinit::__OSHostAlarmsForEach([](uint64 fire, uint64 period, void* callback, void* context, coreinit::OSAlarm_t* guest, void*) {
				cemuLog_log(LogType::Force, "wwhd state census: host alarm at {} period {} callback {} context {}{}", fire, period, callback, context,
					guest ? fmt::format(" (guest alarm {:08x})", MEMPTR<coreinit::OSAlarm_t>(guest).GetMPTR()) : std::string());
			}, nullptr);
			for (sint32 i = 0; i < activeThreadCount; i++)
			{
				OSThread_t* t = MEMPTR<OSThread_t>(activeThread[i]).GetPtr();
				const uint32 srr0 = t->context.srr0, lr = _swapEndianU32(t->context.lr);
				const std::string srrHle = HleName(srr0), lrHle = HleName(lr);
				const char* name = t->threadName ? t->threadName.GetPtr() : "";
				const char* why = NotResumable(t, parked);
				const auto busy = s_busy.find(activeThread[i]);
				cemuLog_log(LogType::Force, "wwhd state census: thread {:08x} \"{}\"{} {} suspend {} prio {} affinity {} type {}: srr0 {:08x}{} lr {:08x}{} "
					"r1 {:08x} wait queue {:08x} mutex {:08x} fast mutex {:08x} entry {:08x}{}{}",
					activeThread[i], name, t == parked ? " (parked at the save point)" : "", StateName(t->state), (sint32)t->suspendCounter,
					(sint32)t->effectivePriority, (uint32)t->context.affinity, (uint32)t->type.value(), srr0, srrHle.empty() ? "" : " (" + srrHle + ")",
					lr, lrHle.empty() ? "" : " (" + lrHle + ")", _swapEndianU32(t->context.gpr[1]), t->currentWaitQueue.GetMPTR(),
					t->waitingForMutex.GetMPTR(), t->waitingForFastMutex.GetMPTR(), t->entrypoint.GetMPTR(),
					why ? fmt::format(": NOT RESUMABLE ({})", why) : std::string(),
					busy == s_busy.end() ? std::string() : fmt::format(", kept the game from rest at {} checks", busy->second));
			}
		}

		// ---- the threads ---------------------------------------------------------------------------------------
		struct ThreadEntry
		{
			MPTR thread;
			uint32 kind;                                // 0 game, 1 system, 2 the parked main thread
			uint32 srr0, waitQueue, state;
		};

		std::vector<ThreadEntry> ThreadTable(OSThread_t* parked)
		{
			std::vector<ThreadEntry> v;
			for (sint32 i = 0; i < activeThreadCount; i++)
			{
				OSThread_t* t = MEMPTR<OSThread_t>(activeThread[i]).GetPtr();
				v.push_back({ activeThread[i], t == parked ? 2u : SystemThread(t, parked) ? 1u : 0u, t->context.srr0,
					t->currentWaitQueue.GetMPTR(), (uint32)t->state.value() });
			}
			return v;
		}

		// ---- the file ------------------------------------------------------------------------------------------
		bool WriteAll(FILE* f, const void* p, size_t n) { return fwrite(p, 1, n, f) == n; }
		bool ReadAll(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }

		std::vector<MMURange*> GuestRanges()
		{
			std::vector<MMURange*> v;
			for (MMURange* r : memory_getMMURanges())
				if (r->isMapped() && r->getSize())
					v.push_back(r);
			return v;
		}

		bool ZeroBlock(const uint8* p, size_t n)
		{
			const uint64* q = (const uint64*)p;
			for (size_t i = 0; i < n / 8; i++)
				if (q[i])
					return false;
			return true;
		}

		// guest memory: the ranges (base and size each), then per range a flag a block (0 zero) and the non-zero blocks, zstd
		bool SaveMemory(FILE* f, uint64& raw, uint64& packed)
		{
			const auto ranges = GuestRanges();
			uint32 n = (uint32)ranges.size();
			bool ok = WriteAll(f, &n, 4);
			for (MMURange* r : ranges)
			{
				const uint32 base = r->getBase(), size = r->getSize();
				ok = ok && WriteAll(f, &base, 4) && WriteAll(f, &size, 4);
			}
			if (!ok)
				return false;
			ZSTD_CCtx* cctx = ZSTD_createCCtx();
			ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1);
			std::vector<uint8> out(ZSTD_CStreamOutSize());
			for (MMURange* r : ranges)
			{
				const uint32 base = r->getBase(), size = r->getSize();
				const uint8* mem = memory_base + base;
				const uint32 blocks = (size + kBlock - 1) / kBlock;
				std::vector<uint8> flags(blocks);
				for (uint32 b = 0; b < blocks; b++)
					flags[b] = ZeroBlock(mem + (size_t)b * kBlock, std::min<size_t>(kBlock, size - (size_t)b * kBlock)) ? 0 : 1;
				ok = ok && WriteAll(f, flags.data(), flags.size());
				const long lenAt = ftell(f);
				uint64 len = 0;
				ok = ok && WriteAll(f, &len, 8);
				ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
				for (uint32 b = 0; ok && b < blocks; b++)
				{
					if (!flags[b])
						continue;
					const size_t bn = std::min<size_t>(kBlock, size - (size_t)b * kBlock);
					raw += bn;
					ZSTD_inBuffer in{ mem + (size_t)b * kBlock, bn, 0 };
					while (ok && in.pos < in.size)
					{
						ZSTD_outBuffer o{ out.data(), out.size(), 0 };
						if (ZSTD_isError(ZSTD_compressStream2(cctx, &o, &in, ZSTD_e_continue)))
							ok = false;
						ok = ok && WriteAll(f, out.data(), o.pos);
						len += o.pos;
					}
				}
				for (size_t left = 1; ok && left;)
				{
					ZSTD_inBuffer in{ nullptr, 0, 0 };
					ZSTD_outBuffer o{ out.data(), out.size(), 0 };
					left = ZSTD_compressStream2(cctx, &o, &in, ZSTD_e_end);
					if (ZSTD_isError(left))
						ok = false;
					ok = ok && WriteAll(f, out.data(), o.pos);
					len += o.pos;
				}
				const long endAt = ftell(f);
				ok = ok && fseek(f, lenAt, SEEK_SET) == 0 && WriteAll(f, &len, 8) && fseek(f, endAt, SEEK_SET) == 0;
				packed += len;
			}
			ZSTD_freeCCtx(cctx);
			return ok;
		}

		// a load's first step: the state's ranges, each mapped so here (the same boot); nothing written yet
		bool LoadRanges(FILE* f, std::vector<std::pair<uint32, uint32>>& ranges, std::string& error)
		{
			uint32 n = 0;
			if (!ReadAll(f, &n, 4) || n > 64)
				return error = "no memory in the state", false;
			ranges.resize(n);
			const auto here = GuestRanges();
			for (auto& [base, size] : ranges)
			{
				if (!ReadAll(f, &base, 4) || !ReadAll(f, &size, 4))
					return error = "no memory in the state", false;
				bool mapped = false;
				for (MMURange* r : here)
					mapped |= r->getBase() == base && r->getSize() == size;
				if (!mapped)
					return error = fmt::format("guest range {:08x}+{:x} isn't mapped so here", base, size), false;
			}
			return true;
		}

		// then every range, each block written only where it differs
		bool LoadMemory(FILE* f, const std::vector<std::pair<uint32, uint32>>& ranges, std::string& error)
		{
			ZSTD_DCtx* dctx = ZSTD_createDCtx();
			std::vector<uint8> packed, block(kBlock);
			bool ok = true;
			for (auto [base, size] : ranges)
			{
				if (!ok)
					break;
				const uint32 blocks = (size + kBlock - 1) / kBlock;
				std::vector<uint8> flags(blocks);
				uint64 len = 0;
				ok = ReadAll(f, flags.data(), blocks) && ReadAll(f, &len, 8);
				packed.resize(len);
				ok = ok && ReadAll(f, packed.data(), len);
				ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
				ZSTD_inBuffer in{ packed.data(), packed.size(), 0 };
				uint8* mem = memory_base + base;
				// real time: the texture cache's watched pages writable and stamped (it hashes them again), rather than a
				// fault a page
				if (wwhd::rt::write_watch::Active())
					wwhd::rt::write_watch::Unprotect(mem, size);
				for (uint32 b = 0; ok && b < blocks; b++)
				{
					const size_t bn = std::min<size_t>(kBlock, size - (size_t)b * kBlock);
					uint8* dst = mem + (size_t)b * kBlock;
					if (!flags[b])
					{
						if (!ZeroBlock(dst, bn))
							memset(dst, 0, bn);
						continue;
					}
					ZSTD_outBuffer o{ block.data(), bn, 0 };
					while (ok && o.pos < bn)
					{
						const size_t before = o.pos;
						const size_t r2 = ZSTD_decompressStream(dctx, &o, &in);
						if (ZSTD_isError(r2) || (o.pos == before && in.pos == in.size))
							ok = false;
					}
					if (ok && memcmp(dst, block.data(), bn) != 0)
						memcpy(dst, block.data(), bn);
				}
				if (!ok && error.empty())
					error = fmt::format("guest range {:08x}: the state's data is damaged", base);
			}
			ZSTD_freeDCtx(dctx);
			return ok;
		}

		bool Compress(const std::vector<uint8>& in, std::vector<uint8>& out)
		{
			out.resize(ZSTD_compressBound(in.size()));
			const size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3);
			if (ZSTD_isError(n))
				return false;
			out.resize(n);
			return true;
		}

		// the records: each module's in the order of their tags, each its tag, its length and its bytes (a load checks all
		// three: a record of another length is another build's layout, refused before it's read)
		void RunRecords(Archive& ar)
		{
			for (auto& [tag, fn] : Records())
			{
				std::string t = tag;
				ar.Str(t);
				if (ar.Loading() && ar.Ok() && t != tag)
				{
					ar.Fail(fmt::format("record {} where {} was expected (another build)", t, tag));
					return;
				}
				Archive sub(ar.Loading());
				std::vector<uint8> bytes;
				if (!ar.Loading())
				{
					fn(sub);
					if (!sub.Ok())
						return ar.Fail(fmt::format("record {}: {}", tag, sub.Error()));
					bytes = std::move(sub.Data());
				}
				ar.Vec(bytes);
				if (!ar.Loading() || !ar.Ok())
					continue;
				sub.Data() = std::move(bytes);
				fn(sub);
				if (!sub.Ok())
					return ar.Fail(fmt::format("record {}: {}", tag, sub.Error()));
				if (!sub.AtEnd())
					return ar.Fail(fmt::format("record {} is longer than this build's (another build)", tag));
			}
		}

		bool Save(OSThread_t* parked, const std::string& path)
		{
			const auto start = std::chrono::steady_clock::now();
			Archive ar(false);
			uint32 swap = wwhd::os::SwapCount();
			ar.Pod(swap);
			auto threads = ThreadTable(parked);
			ar.Vec(threads);
			ar.Expect(s_ppcHleTableWriteIndex, "the number of OS calls registered");
			RunRecords(ar);
			if (!ar.Ok())
			{
				cemuLog_log(LogType::Force, "wwhd state: couldn't save to {}: {}", path, ar.Error());
				return false;
			}
			std::vector<uint8> packed;
			FILE* f = fopen((path + ".tmp").c_str(), "wb");
			uint64 raw = 0, mem = 0;
			bool ok = f && Compress(ar.Data(), packed);
			const uint32 version = kVersion;
			uint64 recLen = ar.Data().size(), packedLen = packed.size();
			ok = ok && WriteAll(f, "WWHDSTAT", 8) && WriteAll(f, &version, 4) && WriteAll(f, &recLen, 8) && WriteAll(f, &packedLen, 8) &&
				WriteAll(f, packed.data(), packed.size());
			ok = ok && SaveMemory(f, raw, mem);
			if (f)
				ok = fclose(f) == 0 && ok;
			ok = ok && rename((path + ".tmp").c_str(), path.c_str()) == 0;
			const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			if (ok)
				cemuLog_log(LogType::Force, "wwhd state: saved swap {} to {} ({} threads, records {} bytes, memory {} MB in use as {} MB) in {:.2f} s",
					swap, path, threads.size(), recLen, raw >> 20, mem >> 20, secs);
			else
			{
				remove((path + ".tmp").c_str());
				cemuLog_log(LogType::Force, "wwhd state: couldn't save to {} (write failed)", path);
			}
			return ok;
		}

		// a load at start (WWHD_STATE_LOAD): the state's thread count and OS calls registered, read once, so the frames
		// before this process has come as far aren't parked for nothing (in real time the 33rd thread comes at ~swap 825)
		bool LoadLooksReady()
		{
			static const std::pair<sint32, sint32> want = [] {
				std::pair<sint32, sint32> w{ -1, -1 };
				FILE* f = fopen(TheLoadPath().c_str(), "rb");
				if (!f)
					return w;
				char magic[8];
				uint32 version = 0;
				uint64 recLen = 0, packedLen = 0;
				std::vector<uint8> packed;
				Archive ar(true);
				bool ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, "WWHDSTAT", 8) == 0 && fread(&version, 1, 4, f) == 4 &&
					fread(&recLen, 1, 8, f) == 8 && fread(&packedLen, 1, 8, f) == 8 && packedLen < (1u << 30) && recLen < (1u << 30);
				if (ok)
				{
					packed.resize(packedLen);
					ar.Data().resize(recLen);
					ok = fread(packed.data(), 1, packedLen, f) == packedLen &&
						ZSTD_decompress(ar.Data().data(), recLen, packed.data(), packedLen) == recLen;
				}
				fclose(f);
				if (!ok)
					return w;
				uint32 swap = 0;
				std::vector<ThreadEntry> threads;
				sint32 hle = 0;
				ar.Pod(swap);
				ar.Vec(threads);
				ar.Pod(hle);
				return ar.Ok() ? std::pair<sint32, sint32>{ (sint32)threads.size(), hle } : w;
			}();
			// a state that can't be read: try at once (Load says why)
			return want.first < 0 || (activeThreadCount == want.first && s_ppcHleTableWriteIndex == want.second);
		}

		// a load's outcome: done; not yet (this process hasn't come as far as the state's: the threads or the OS calls
		// registered differ; tried again at the next save point); refused (nothing changed); broken (past the point of no
		// return: the process is neither)
		enum class Loaded { Done, NotYet, Refused, Broken };

		Loaded Load(OSThread_t* parked, const std::string& path, std::string& error)
		{
			FILE* f = fopen(path.c_str(), "rb");
			if (!f)
				return error = "can't open " + path, Loaded::Refused;
			char magic[8];
			uint32 version = 0;
			uint64 recLen = 0, packedLen = 0;
			std::vector<uint8> packed;
			bool ok = ReadAll(f, magic, 8) && memcmp(magic, "WWHDSTAT", 8) == 0 && ReadAll(f, &version, 4) && version == kVersion &&
				ReadAll(f, &recLen, 8) && ReadAll(f, &packedLen, 8) && packedLen < (1u << 30) && recLen < (1u << 30);
			if (ok)
			{
				packed.resize(packedLen);
				ok = ReadAll(f, packed.data(), packedLen);
			}
			Archive ar(true);
			if (ok)
			{
				ar.Data().resize(recLen);
				ok = ZSTD_decompress(ar.Data().data(), recLen, packed.data(), packedLen) == recLen;
			}
			if (!ok)
			{
				fclose(f);
				return error = "not a state of this format (or damaged)", Loaded::Refused;
			}
			uint32 swap = 0;
			ar.Pod(swap);
			std::vector<ThreadEntry> saved;
			ar.Vec(saved);
			ar.Expect(s_ppcHleTableWriteIndex, "the number of OS calls registered");
			// the threads: the same set; the system threads waiting where the state's were (this process keeps its own)
			const auto here = ThreadTable(parked);
			std::map<MPTR, ThreadEntry> hereBy;
			for (auto& e : here)
				hereBy[e.thread] = e;
			for (auto& e : saved)
			{
				auto it = hereBy.find(e.thread);
				if (it == hereBy.end())
					ar.Fail(fmt::format("thread {:08x} doesn't exist here yet", e.thread));
				else if (e.kind == 1 && (it->second.kind != 1 || it->second.srr0 != e.srr0 || it->second.waitQueue != e.waitQueue))
					ar.Fail(fmt::format("system thread {:08x} waits elsewhere here", e.thread));
			}
			if (here.size() != saved.size())
				ar.Fail(fmt::format("{} threads here, {} in the state", here.size(), saved.size()));
			if (!ar.Ok())
			{
				fclose(f);
				return error = ar.Error(), Loaded::NotYet;
			}
			std::vector<std::pair<uint32, uint32>> ranges;
			if (!LoadRanges(f, ranges, error))
			{
				fclose(f);
				return Loaded::Refused;
			}
			// from here on the process is the state's: memory, then the records, then the game's threads anew
			ok = LoadMemory(f, ranges, error);
			fclose(f);
			if (!ok)
				return Loaded::Broken;
			RunRecords(ar);
			if (!ar.Ok())
				return error = ar.Error(), Loaded::Broken;
			{
				std::lock_guard lock(s_rerunLock);
				s_rerun.clear();
				for (auto& e : saved)
				{
					if (e.kind == 1)
						continue;
					coreinit::__OSReplaceHostThread(MEMPTR<OSThread_t>(e.thread).GetPtr());
					s_exact[MEMPTR<OSThread_t>(e.thread).GetPtr()] = true;   // its saved IP is where it goes on
					if (e.state == (uint32)OSThread_t::THREAD_STATE::STATE_WAITING)
						s_rerun.insert(e.thread);
				}
			}
			cemuLog_log(LogType::Force, "wwhd state: loaded {} at swap {} (the state's swap {}, {} threads, {} of them to run their OS call again)",
				path, s_loadedAt, swap, saved.size(), s_rerun.size());
			return Loaded::Done;
		}
	}

	Record::Record(const char* tag, RecordFn fn)
	{
		Records()[tag] = fn;
	}

	void SetDir(const std::string& portableDir)
	{
		std::lock_guard lock(s_slotLock);
		s_dir = portableDir + "/states";
	}

	void RequestSave(int slot)
	{
		std::lock_guard lock(s_slotLock);
		if (slot >= 1 && slot <= kSlots && !s_dir.empty())
			s_request = slot, s_status = "Saving to slot " + std::to_string(slot) + "...";
	}

	void RequestLoad(int slot)
	{
		std::lock_guard lock(s_slotLock);
		if (slot >= 1 && slot <= kSlots && !s_dir.empty())
			s_request = -slot, s_status = "Loading slot " + std::to_string(slot) + "...";
	}

	std::string SlotInfo(int slot)
	{
		std::error_code ec;
		std::string path;
		{
			std::lock_guard lock(s_slotLock);
			if (s_dir.empty())
				return "empty";
			path = SlotPath(slot);
		}
		const auto when = std::filesystem::last_write_time(path, ec);
		if (ec)
			return "empty";
		const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(when - std::filesystem::file_time_type::clock::now() +
			std::chrono::system_clock::now());
		const std::time_t t = std::chrono::system_clock::to_time_t(sys);
		char buf[32];
		std::strftime(buf, sizeof(buf), "%b %d %H:%M", std::localtime(&t));
		return buf;
	}

	std::string Status()
	{
		std::lock_guard lock(s_slotLock);
		return s_status;
	}

	void AtFrameTop(PPCInterpreter_t* ctx)
	{
		static const std::vector<uint32> census = Swaps("WWHD_STATE_CENSUS");
		static size_t nextCensus = 0;
		static bool saved = false;
		const uint32 swap = wwhd::os::SwapCount();
		Action action = Action::None;
		{
			std::lock_guard lock(s_slotLock);
			if (s_request)
			{
				s_slotPath = SlotPath(std::abs(s_request));
				action = s_request > 0 ? Action::Save : Action::Load;
				s_fromMenu = true;
				s_request = 0;
			}
		}
		static bool loadAtDone = false;
		if (action != Action::None)
			;
		else if (!loadAtDone && swap >= TheLoadAtRequest().swap)
		{
			loadAtDone = true;
			s_slotPath = TheLoadAtRequest().path;
			s_fromMenu = true;                         // in place, as the menu's: "not yet" is a refusal
			action = Action::Load;
		}
		else if (!s_loadDone && !TheLoadPath().empty())
		{
			if (!LoadLooksReady())
				return;
			action = Action::Load;
		}                     // the first save point (Load refuses until the threads are the state's)
		else if (!saved && swap >= TheSaveRequest().swap)
			action = Action::Save, saved = true;
		else if (nextCensus < census.size() && swap >= census[nextCensus])
		{
			while (nextCensus < census.size() && census[nextCensus] <= swap)
				nextCensus++;
			action = Action::Census;
		}
		if (action == Action::None)
			return;
		OSThread_t* self = coreinit::OSGetCurrentThread();
		ctx->instructionPointer = kSavePoint;          // its context's srr0: a load resumes it here, at the dispatcher
		s_parkedAt = coreinit::OSGetTime();
		s_busy.clear();
		s_action = action;
		s_parked.store(self, std::memory_order_release);
		coreinit::OSSuspendThread(self);               // Idle resumes it once the game is at rest (after a load, a fiber anew)
	}

	bool Idle()
	{
		OSThread_t* parked = s_parked.load(std::memory_order_acquire);
		if (!parked)
			return false;
		__OSLockScheduler();
		if (parked->suspendCounter == 0)
		{
			__OSUnlockScheduler();
			return false;
		}
		if (!AtRest(parked))
		{
			// never at rest (a thread that never comes to a resumable point): nothing saved or loaded, the game goes on
			const bool giveUp = coreinit::OSGetTime() - s_parkedAt > ESPRESSO_TIMER_CLOCK / 2;
			if (giveUp)
			{
				Census(parked, " (never at rest: nothing saved or loaded)");
				s_parked.store(nullptr, std::memory_order_relaxed);
				coreinit::__OSResumeThreadInternal(parked, 1);
			}
			__OSUnlockScheduler();
			return giveUp;
		}
		switch (s_action)
		{
		case Action::Census:
			Census(parked, "");
			break;
		case Action::Save:
			if (s_fromMenu)
			{
				std::error_code ec;
				std::filesystem::create_directories(s_dir, ec);
				SetStatus(Save(parked, s_slotPath) ? "Saved to " + std::filesystem::path(s_slotPath).filename().string() : "Couldn't save (see the log)");
			}
			else
				Save(parked, TheSaveRequest().path);
			break;
		case Action::Load:
		{
			std::string error;
			s_loadedAt = wwhd::os::SwapCount();
			const std::string path = s_fromMenu ? s_slotPath : TheLoadPath();
			const Loaded result = Load(parked, path, error);
			if (s_fromMenu)
			{
				// in play the threads and the OS calls are as far as any state's: "not yet" is a refusal
				SetStatus(result == Loaded::Done ? "Loaded " + std::filesystem::path(path).filename().string() : "Couldn't load: " + error);
				if (result == Loaded::NotYet)
				{
					cemuLog_log(LogType::Force, "wwhd state: couldn't load {}: {}", path, error);
					break;
				}
			}
			switch (result)
			{
			case Loaded::Done:
				s_loadDone = true;
				break;
			case Loaded::NotYet:
				// this process hasn't come as far as the state's yet: the next save point (a minute of frames at most)
				if (++s_loadTries < 3600)
					break;
				cemuLog_log(LogType::Force, "wwhd state: couldn't load {} by swap {}: {}", TheLoadPath(), s_loadedAt, error);
				s_loadDone = true;
				break;
			case Loaded::Refused:
				cemuLog_log(LogType::Force, "wwhd state: couldn't load {}: {}", TheLoadPath(), error);
				s_loadDone = true;
				break;
			case Loaded::Broken:
				cemuLog_log(LogType::Force, "wwhd state: couldn't load {}: {}; the process is half loaded: stopping", TheLoadPath(), error);
				exit(3);
			}
			break;
		}
		default:
			break;
		}
		s_fromMenu = false;
		s_parked.store(nullptr, std::memory_order_relaxed);
		coreinit::__OSResumeThreadInternal(parked, 1);
		__OSUnlockScheduler();
		return true;
	}

	void NoteSwitchOut(OSThread_t* thread, bool exact)
	{
		if (thread)
			s_exact[thread] = exact;
	}

	bool Pending()
	{
		return s_parked.load(std::memory_order_relaxed) != nullptr;
	}

	void FiberStart(PPCInterpreter_t* hCPU)
	{
		OSThread_t* t = coreinit::OSGetCurrentThread();
		if (!t)
			return;
		{
			std::lock_guard lock(s_rerunLock);
			if (s_rerun.empty() || !s_rerun.erase(memory_getVirtualOffsetFromPointer(t)))
				return;
		}
		const sint32 i = HleIndex(hCPU->instructionPointer);
		if (i >= 0)
			PPCInterpreter_getHLECall(i)(hCPU);         // the call again, uncharged: it returns to LR as the first one would have
	}
}
