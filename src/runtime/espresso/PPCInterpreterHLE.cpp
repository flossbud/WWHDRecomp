// PPCInterpreterHLE.cpp (docs/recompiler-design.md D18): the HLE table and dispatch, and the
// reference's call tracer. Cemu's src/Cafe/HW/Espresso/Interpreter/PPCInterpreterHLE.cpp (Mozilla
// Public License 2.0), forked: wwhd-null links this in place of Cemu's object, under Cemu's names
// (so Cemu's code that calls it reaches ours). Route traces, GPU commands and sound must stay as
// Cemu's (tools/reference/stream_check.sh).
#include "../PPCState.h"
#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include <zstd.h>

std::unordered_set<std::string> s_unsupportedHLECalls;

void PPCInterpreter_handleUnsupportedHLECall(PPCInterpreter_t* hCPU)
{
	const char* libFuncName = (char*)memory_getPointerFromVirtualOffset(hCPU->instructionPointer + 8);
	std::string tempString = fmt::format("Unsupported lib call: {}", libFuncName);
	if (s_unsupportedHLECalls.find(tempString) == s_unsupportedHLECalls.end())
	{
		cemuLog_log(LogType::UnsupportedAPI, "{}", tempString);
		s_unsupportedHLECalls.emplace(tempString);
	}
	hCPU->gpr[3] = 0;
	PPCInterpreter_nextInstruction(hCPU);
}

static constexpr size_t HLE_TABLE_CAPACITY = 0x4000;
HLECALL s_ppcHleTable[HLE_TABLE_CAPACITY]{};
std::string s_ppcHleNames[HLE_TABLE_CAPACITY]; // wwhd-reference: for the HLE call tracer
sint32 s_ppcHleTableWriteIndex = 0;
std::mutex s_ppcHleTableMutex;

HLEIDX PPCInterpreter_registerHLECall(HLECALL hleCall, std::string hleName)
{
	std::unique_lock _l(s_ppcHleTableMutex);
	if (s_ppcHleTableWriteIndex >= HLE_TABLE_CAPACITY)
	{
		cemuLog_log(LogType::Force, "HLE table is full");
		cemu_assert(false);
	}
	for (sint32 i = 0; i < s_ppcHleTableWriteIndex; i++)
	{
		if (s_ppcHleTable[i] == hleCall)
		{
			return i;
		}
	}
	cemu_assert(s_ppcHleTableWriteIndex < HLE_TABLE_CAPACITY);
	s_ppcHleTable[s_ppcHleTableWriteIndex] = hleCall;
	s_ppcHleNames[s_ppcHleTableWriteIndex] = hleName;
	HLEIDX funcIndex = s_ppcHleTableWriteIndex;
	s_ppcHleTableWriteIndex++;
	return funcIndex;
}

HLECALL PPCInterpreter_getHLECall(HLEIDX funcIndex)
{
	if (funcIndex < 0 || funcIndex >= HLE_TABLE_CAPACITY)
		return nullptr;
	return s_ppcHleTable[funcIndex];
}

std::mutex s_hleLogMutex;

// wwhd-reference: with the virtual clock, an OS call must cost guest time like it does on hardware
// (a few hundred cycles), or a polling loop built from OS calls (e.g. WWHD's OSSendMessage /
// OSReceiveMessage ping-pong, ~65k round trips per frame) advances guest time almost not at all
// while burning host time. Charged to the thread's timeslice, so the virtual clock sees it too.
constexpr sint32 kVirtualHLECallCycles = 300;

// wwhd-reference: HLE call tracer. CEMU_HLE_TRACE=<file.zst> records every OS library call made by
// the game as compact binary records; CEMU_HLE_TRACE_FILTER=gx2.,coreinit.OSGetTime limits it to
// names starting with any listed prefix; CEMU_HLE_TRACE_EXIT_FRAME=N ends the trace and exits Cemu
// when frame N begins, so repeated runs cover exactly the same span. The stream is zstd-compressed and flushed every 4096
// records, so a killed process still leaves a readable trace. Record layout (little endian):
//   'N' u16 index, u16 length, name                   - first time an index appears
//   'C' u32 frame, u16 index, u8 core, u8 0, u32 thread, u32 lr, u32 r3..r10, f32 f1..f8
// frame counts calls to gx2.GX2SwapScanBuffers. See tools/reference/hle_trace.py for a reader.
namespace HLETrace
{
	static std::mutex s_mutex;
	static FILE* s_file = nullptr;
	static ZSTD_CCtx* s_cctx = nullptr;
	static bool s_initialized = false;
	static std::vector<uint8> s_in, s_out;
	static std::vector<uint8> s_state; // per index: 0 unknown, 1 traced, 2 filtered out
	static uint32 s_frame = 0;
	static uint32 s_sinceFlush = 0;

	static void write(ZSTD_EndDirective mode)
	{
		ZSTD_inBuffer in{ s_in.data(), s_in.size(), 0 };
		bool done;
		do
		{
			ZSTD_outBuffer out{ s_out.data(), s_out.size(), 0 };
			size_t remaining = ZSTD_compressStream2(s_cctx, &out, &in, mode);
			fwrite(s_out.data(), 1, out.pos, s_file);
			done = (mode == ZSTD_e_continue) ? (in.pos == in.size) : (remaining == 0);
		} while (!done);
		s_in.clear();
		if (mode != ZSTD_e_continue)
			fflush(s_file);
	}

	// wwhd: whether a trace is wanted at all, known before the first call: without one, record() and
	// event() return without taking the mutex (they used to lock it on every OS call to find out)
	static const bool s_wanted = [] { const char* p = getenv("CEMU_HLE_TRACE"); return p && *p; }();

	static void finish()
	{
		std::unique_lock _l(s_mutex);
		if (s_file)
		{
			write(ZSTD_e_end);
			fclose(s_file);
			s_file = nullptr;
		}
	}

	static bool init()
	{
		s_initialized = true;
		const char* path = getenv("CEMU_HLE_TRACE");
		if (!path || !*path)
			return false;
		s_file = fopen(path, "wb");
		if (!s_file)
			return false;
		s_cctx = ZSTD_createCCtx();
		ZSTD_CCtx_setParameter(s_cctx, ZSTD_c_compressionLevel, 3);
		s_out.resize(ZSTD_CStreamOutSize());
		s_state.assign(HLE_TABLE_CAPACITY, 0);
		const char magic[8] = { 'H','L','E','T','R','C','0','1' };
		s_in.insert(s_in.end(), magic, magic + 8);
		atexit(finish);
		return true;
	}

	template<typename T> static void put(T v)
	{
		const uint8* p = (const uint8*)&v;
		s_in.insert(s_in.end(), p, p + sizeof(T));
	}

	static bool wanted(const std::string& name)
	{
		const char* filter = getenv("CEMU_HLE_TRACE_FILTER");
		if (!filter || !*filter)
			return true;
		std::string_view rest(filter);
		while (!rest.empty())
		{
			size_t comma = rest.find(',');
			std::string_view prefix = rest.substr(0, comma);
			if (!prefix.empty() && name.compare(0, prefix.size(), prefix) == 0)
				return true;
			if (comma == std::string_view::npos)
				break;
			rest.remove_prefix(comma + 1);
		}
		return false;
	}

	// Synthetic scheduler records (sched.*) share the call-record format: gpr[0..] carry the values,
	// thread/core identify the context. Indices are taken from the top of the HLE table.
	void event(uint32 index, const char* name, uint32 core, uint32 thread, std::initializer_list<uint32> values)
	{
		if (!s_wanted)
			return;
		std::unique_lock _l(s_mutex);
		if (!s_initialized && !init())
			return;
		if (!s_file || index >= HLE_TABLE_CAPACITY)
			return;
		if (s_ppcHleNames[index].empty())
			s_ppcHleNames[index] = name;
		if (s_state[index] == 0)
		{
			s_state[index] = wanted(s_ppcHleNames[index]) ? 1 : 2;
			if (s_state[index] == 1)
			{
				put<uint8>('N'); put<uint16>((uint16)index); put<uint16>((uint16)s_ppcHleNames[index].size());
				s_in.insert(s_in.end(), s_ppcHleNames[index].begin(), s_ppcHleNames[index].end());
			}
		}
		if (s_state[index] != 1)
			return;
		put<uint8>('C'); put<uint32>(s_frame); put<uint16>((uint16)index); put<uint8>((uint8)core); put<uint8>(0);
		put<uint32>(thread); put<uint32>(0);
		uint32 regs[8]{};
		size_t i = 0;
		for (uint32 v : values)
			if (i < 8) regs[i++] = v;
		for (uint32 r : regs) put<uint32>(r);
		for (sint32 f = 0; f < 8; f++) put<float>(0.0f);
		write(ZSTD_e_continue);
	}

	void record(PPCInterpreter_t* hCPU, uint32 index)
	{
		if (!s_wanted)
			return;
		std::unique_lock _l(s_mutex);
		if (!s_initialized && !init())
			return;
		if (!s_file || index >= HLE_TABLE_CAPACITY)
			return;
		const std::string& name = s_ppcHleNames[index];
		if (s_state[index] == 0)
		{
			s_state[index] = wanted(name) ? 1 : 2;
			if (s_state[index] == 1)
			{
				put<uint8>('N'); put<uint16>((uint16)index); put<uint16>((uint16)name.size());
				s_in.insert(s_in.end(), name.begin(), name.end());
			}
		}
		if (name == "gx2.GX2SwapScanBuffers")
		{
			s_frame++;
			static const char* exitFrame = getenv("CEMU_HLE_TRACE_EXIT_FRAME");
			if (exitFrame && *exitFrame && s_frame >= (uint32)atoi(exitFrame))
			{
				write(ZSTD_e_end);
				fclose(s_file);
				s_file = nullptr;
				cemuLog_log(LogType::Force, "HLE trace: reached frame {}, exiting", s_frame);
				std::quick_exit(0); // like _exit, but runs at_quick_exit handlers (the wwhd runtime's final report)
			}
		}
		if (s_state[index] != 1)
			return;
		put<uint8>('C');
		put<uint32>(s_frame);
		put<uint16>((uint16)index);
		put<uint8>((uint8)hCPU->spr.UPIR);
		put<uint8>(0);
		put<uint32>(memory_getVirtualOffsetFromPointer(coreinit::OSGetCurrentThread()));
		put<uint32>(hCPU->spr.LR);
		for (sint32 r = 3; r <= 10; r++)
			put<uint32>(hCPU->gpr[r]);
		for (sint32 f = 1; f <= 8; f++)
			put<float>((float)hCPU->fpr[f].fp0);
		write(ZSTD_e_continue);
		if (++s_sinceFlush >= 4096)
		{
			write(ZSTD_e_flush);
			s_sinceFlush = 0;
		}
	}
}

void PPCInterpreter_virtualHLE(PPCInterpreter_t* hCPU, unsigned int opcode)
{
	uint32 hleFuncId = opcode & 0xFFFF;
	if (hleFuncId == 0xFFD0) [[unlikely]]
	{
		s_hleLogMutex.lock();
		PPCInterpreter_handleUnsupportedHLECall(hCPU);
		s_hleLogMutex.unlock();
	}
	else
	{
		// os lib function
		auto hleCall = PPCInterpreter_getHLECall(hleFuncId);
		cemu_assert(hleCall);
		if (HLETrace::s_wanted) // wwhd: no call at all when there is no trace
			HLETrace::record(hCPU, hleFuncId);
		if (PPCTimer_isVirtualClock())
			hCPU->remainingCycles -= kVirtualHLECallCycles;
		hleCall(hCPU);
	}
}
// wwhd-reference: scheduler events for determinism debugging (see HLETrace::event)
void HLETrace_schedEvent(uint32 kind, uint32 core, uint32 thread, uint32 a, uint32 b, uint32 c, uint32 d, uint32 e)
{
	static const char* kNames[] = { "sched.slice", "sched.alarm", "sched.skip" };
	if (kind < 3)
		HLETrace::event(0x3FF0 + kind, kNames[kind], core, thread, { a, b, c, d, e });
}
