// Null GPU (M0b.2): stands in for Cemu's Latte, which is not linked. Cemu's gx2 and TCL (the OS
// side) are kept: they write PM4 command buffers exactly as the reference does, and this file is
// the command processor that consumes them. It keeps the register file (LatteGPUState, the input
// a real backend would translate) and performs everything the guest can observe: register
// shadowing into context-state memory, memory writes, fences and semaphores, retire timestamps,
// timer samples, bottom-of-pipe callbacks, swap/flip bookkeeping and vsync. It draws nothing.
//
// Derived from Cemu (src/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp, LatteThread.cpp,
// LatteTiming.cpp, including the wwhd-reference virtual-clock change); Mozilla Public License 2.0.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include "../os/tcl/tcl_host.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/CafeSystem.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "util/helpers/helpers.h"
#include "vk/renderer.h"
#include <map>
#include <set>

LatteGPUState_t LatteGPUState = {};
std::unique_ptr<Renderer> g_renderer;      // stays null: nothing renders
extern std::atomic_bool g_isGPUInitFinished;

namespace
{
	std::atomic_bool s_running = false;
	std::atomic_bool s_initDone = false;
	std::thread s_thread;
	std::mutex s_stateMutex;
	bool s_predicationActive = false;

	[[noreturn]] void threadExit()
	{
		std::memset((void*)&LatteGPUState, 0, sizeof(LatteGPUState));
		pthread_exit(nullptr);
	}

	uint32be* phys(uint32 physAddr) { return (uint32be*)memory_getPointerFromPhysicalOffset(physAddr); }
}

// ---- vsync (LatteTiming.cpp) --------------------------------------------------------------------
static sint32 s_customVsyncFrequency = -1;

static HRTick timeBetweenVSync()
{
	HRTick tick = HighResolutionTimer::getFrequency();
	if (s_customVsyncFrequency > 0)
		return tick / (uint64)s_customVsyncFrequency;
	return tick * 1000ull / 1002ull / 60ull;
}

void LatteTiming_setCustomVsyncFrequency(sint32 frequency) { s_customVsyncFrequency = frequency; }
void LatteTiming_disableCustomVsyncFrequency() { s_customVsyncFrequency = -1; }
bool LatteTiming_getCustomVsyncFrequency(sint32& f)
{
	if (s_customVsyncFrequency <= 0)
		return false;
	f = s_customVsyncFrequency;
	return true;
}

void LatteTiming_signalVsync()
{
	static uint32 s_vsyncIntervalCounter = 0;
	if (!LatteGPUState.gx2InitCalled)
		return;
	s_vsyncIntervalCounter++;
	uint32 swapInterval = LatteGPUState.sharedArea ? (uint32)LatteGPUState.sharedArea->swapInterval : 1;
	if (s_vsyncIntervalCounter >= swapInterval)
	{
		// the reference's per-title flip workaround is for another game; WWHD takes the common path
		if (LatteGPUState.sharedArea && LatteGPUState.flipRequestCount > 0)
		{
			LatteGPUState.flipRequestCount.fetch_sub(1);
			LatteGPUState.sharedArea->flipExecuteCountBE = _swapEndianU32(_swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE) + 1);
		}
		GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::FLIP);
		s_vsyncIntervalCounter = 0;
	}
	GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::VSYNC);
}

// Host-timed vsync when the virtual clock is off (with it, the CPU scheduler raises vsync).
static void handleTimedVsync()
{
	if (PPCTimer_isVirtualClock())
		return;
	uint64 now = HighResolutionTimer::now().getTick();
	if (now < LatteGPUState.timer_nextVSync)
		return;
	LatteTiming_signalVsync();
	uint64 period = timeBetweenVSync();
	uint64 missed = (now - LatteGPUState.timer_nextVSync) / period;
	LatteGPUState.timer_nextVSync += period * (missed >= 2 ? missed + 1 : 1);
}

// ---- G0 draw statistics (docs/recompiler-design.md D15) -----------------------------------------
// WWHD_GPU_STATS=path collects, at every draw, the register state a Vulkan backend would have to
// translate: shader programs (by content), render-target and depth formats and sizes, MSAA, HiZ,
// geometry shaders, stream-out, primitive types and special colour ops. The report goes to path
// (histograms) and path.variants.csv (each pipeline x target-format combination and its draw
// count) at exit, including the trace's exit-at-frame. It only reads state.
// WWHD_GPU_DUMP=dir (G1) also writes each program once, as dir/<hash>.<fs|vs|gs|ps> (the bytes
// the GPU reads), and for each new variant the whole register file at its first draw,
// dir/variant_<n>.regs (LATTE_MAX_REGISTER host-order words), listed in dir/variants.csv. That is
// everything Cemu's shader decompiler reads, so tools/shaders can translate exactly what the
// reference would.
namespace gpustats
{
	// context registers without a define in RegDefines.h (index = byte offset / 4, see LatteReg.h)
	constexpr uint32 kCB_TARGET_MASK = 0xA08E, kCB_COLOR_CONTROL = 0xA202, kVGT_GS_MODE = 0xA290,
		kCB_COLOR0_BASE = mmCB_COLOR0_BASE, kCB_COLOR0_SIZE = mmCB_COLOR0_SIZE, kCB_COLOR0_INFO = mmCB_COLOR0_INFO;

	struct Program { uint32 size; uint64 fingerprint, hash; };

	std::mutex s_mutex;
	std::string s_path;
	uint64 s_draws[3];
	std::map<std::string, uint64> s_hist;                       // "what=value" -> draws
	std::unordered_map<uint32, Program> s_programs;              // by address
	std::set<uint64> s_distinct[4];                              // program hashes seen, per stage (fs vs gs ps)
	std::map<std::string, uint64> s_variants;                    // csv row -> draws
	std::string s_dumpDir;
	std::set<uint64> s_dumped;
	uint32 s_dumpedVariants = 0;

	void DumpProgram(uint32 startReg, uint64 hash, const char* stage)
	{
		if (!hash || !s_dumped.insert(hash).second)
			return;
		uint32 addr = LatteGPUState.contextRegister[startReg] << 8, size = LatteGPUState.contextRegister[startReg + 1] << 3;
		if (FILE* f = fopen(fmt::format("{}/{:016x}.{}", s_dumpDir, hash, stage).c_str(), "wb"))
		{
			fwrite(memory_getPointerFromPhysicalOffset(addr), 1, size, f);
			fclose(f);
		}
	}

	void DumpVariant(const std::string& key, uint32 gsMode, uint64 fs, uint64 vs, uint64 gs, uint64 ps)
	{
		uint32 n = s_dumpedVariants++;
		if (FILE* f = fopen(fmt::format("{}/variant_{}.regs", s_dumpDir, n).c_str(), "wb"))
		{
			fwrite(LatteGPUState.contextRegister, 4, LATTE_MAX_REGISTER, f);
			fclose(f);
		}
		DumpProgram(mmSQ_PGM_START_FS, fs, "fs");
		DumpProgram(gsMode ? mmSQ_PGM_START_ES : mmSQ_PGM_START_VS, vs, "vs");
		DumpProgram(mmSQ_PGM_START_GS, gs, "gs");
		DumpProgram(mmSQ_PGM_START_PS, ps, "ps");
		if (FILE* f = fopen((s_dumpDir + "/variants.csv").c_str(), "a"))
		{
			if (n == 0)
				fputs("variant,fetch,vertex,geometry,pixel,targets,msaa_log2\n", f);
			fprintf(f, "%u,%s\n", n, key.c_str());
			fclose(f);
		}
	}

	uint64 Fnv(const uint8* p, uint32 n)
	{
		uint64 h = 0xCBF29CE484222325ull;
		for (uint32 i = 0; i < n; i++)
			h = (h ^ p[i]) * 0x100000001B3ull;
		return h;
	}

	// content hash of the program at a register pair (START, START+1 = size in 8-byte units)
	uint64 ProgramHash(uint32 startReg)
	{
		uint32 addr = LatteGPUState.contextRegister[startReg] << 8, size = LatteGPUState.contextRegister[startReg + 1] << 3;
		if (addr == 0 || size == 0)
			return 0;
		const uint8* code = memory_getPointerFromPhysicalOffset(addr);
		uint64 fp;
		memcpy(&fp, code, 8);
		fp ^= (uint64)size << 32;
		Program& pr = s_programs[addr];
		if (pr.size != size || pr.fingerprint != fp)              // new, or another program loaded there
			pr = { size, fp, Fnv(code, size) };
		return pr.hash;
	}

	std::string SurfaceSize(uint32 sizeReg)
	{
		uint32 pitch = ((sizeReg & 0x3FF) + 1) * 8, slice = ((sizeReg >> 10) & 0xFFFFF) + 1;
		return fmt::format("{}x{}", pitch, slice * 64 / pitch);
	}

	void Draw(uint32 op)
	{
		const uint32* r = LatteGPUState.contextRegister;
		std::unique_lock _l(s_mutex);
		s_draws[op == IT_DRAW_INDEX_2 ? 0 : op == IT_DRAW_INDEX_AUTO ? 1 : 2]++;
		auto hist = [](const std::string& k) { s_hist[k]++; };
		uint32 gsMode = r[kVGT_GS_MODE] & 3;
		hist(fmt::format("gs_mode={}", gsMode));
		hist(fmt::format("streamout_en={}", r[mmVGT_STRMOUT_EN] & 1));
		hist(fmt::format("msaa_log2_samples={}", r[mmPA_SC_AA_CONFIG] & 3));
		hist(fmt::format("primitive_type={:#x}", r[mmVGT_PRIMITIVE_TYPE]));
		hist(fmt::format("cb_special_op={}", (r[kCB_COLOR_CONTROL] >> 4) & 7));
		// shaders, as Cemu's LatteShader picks them (with a GS, the vertex shader is ES)
		uint64 fs = ProgramHash(mmSQ_PGM_START_FS);
		uint64 vs = ProgramHash(gsMode ? mmSQ_PGM_START_ES : mmSQ_PGM_START_VS);
		uint64 gs = gsMode ? ProgramHash(mmSQ_PGM_START_GS) : 0;
		uint64 ps = ProgramHash(mmSQ_PGM_START_PS);
		s_distinct[0].insert(fs); s_distinct[1].insert(vs); s_distinct[2].insert(gs); s_distinct[3].insert(ps);
		// render targets the pixel shader writes (CB_TARGET_MASK), then depth
		std::string targets;
		uint32 mask = r[kCB_TARGET_MASK];
		for (uint32 t = 0; t < 8; t++)
		{
			if (((mask >> (4 * t)) & 0xF) == 0 || r[kCB_COLOR0_BASE + t] == 0)
				continue;
			uint32 info = r[kCB_COLOR0_INFO + t];
			std::string fmtName = fmt::format("fmt{:#x}/num{}", (info >> 2) & 0x3F, (info >> 12) & 7);
			targets += fmt::format("c{}:{} ", t, fmtName);
			hist(fmt::format("color_target={} {} array_mode={}", SurfaceSize(r[kCB_COLOR0_SIZE + t]), fmtName, (info >> 8) & 0xF));
		}
		// Cemu's GX2SetDepthBuffer writes the depth image's address into DB_HTILE_DATA_BASE and 0 into
		// DB_DEPTH_BASE, and encodes no HiZ (Latte reads it the same way): a Cemu dialect of the
		// registers that a backend on this register file inherits
		uint32 dbInfo = r[mmDB_DEPTH_INFO];
		bool depth = r[mmDB_HTILE_DATA_BASE] != 0;
		if (depth)
		{
			targets += fmt::format("d:fmt{}", dbInfo & 7);
			hist(fmt::format("depth_target={} fmt{} array_mode={} tile_surface(htile)={}", SurfaceSize(r[mmDB_DEPTH_SIZE]),
				dbInfo & 7, (dbInfo >> 15) & 0xF, (dbInfo >> 25) & 1));
			hist(fmt::format("db_htile_surface={:#x} db_render_override={:#x}", r[mmDB_HTILE_SURFACE], r[mmDB_RENDER_OVERRIDE]));
		}
		std::string key = fmt::format("{:016x},{:016x},{:016x},{:016x},{},{}", fs, vs, gs, ps, targets, r[mmPA_SC_AA_CONFIG] & 3);
		auto [variant, isNew] = s_variants.try_emplace(key, 0);
		variant->second++;
		if (isNew && !s_dumpDir.empty())
			DumpVariant(key, gsMode, fs, vs, gs, ps);
	}

	void Report()
	{
		std::unique_lock _l(s_mutex);
		FILE* f = fopen(s_path.c_str(), "w");
		if (!f)
			return;
		uint64 total = s_draws[0] + s_draws[1] + s_draws[2];
		std::set<std::string> pipelines;
		for (auto& [k, n] : s_variants)
			pipelines.insert(k.substr(0, 4 * 17 - 1));
		fprintf(f, "draws %llu (DRAW_INDEX_2 %llu, DRAW_INDEX_AUTO %llu, DRAW_INDEX_IMMD %llu)\n", (unsigned long long)total,
			(unsigned long long)s_draws[0], (unsigned long long)s_draws[1], (unsigned long long)s_draws[2]);
		fprintf(f, "distinct programs by content: fetch %zu, vertex %zu, geometry %zu, pixel %zu\n", s_distinct[0].size(),
			s_distinct[1].size(), s_distinct[2].size() - s_distinct[2].count(0), s_distinct[3].size());
		fprintf(f, "pipelines (fetch, vertex, geometry, pixel): %zu; variants (pipeline x target formats x MSAA): %zu\n",
			pipelines.size(), s_variants.size());
		for (auto& [k, n] : s_hist)
			fprintf(f, "%s: %llu\n", k.c_str(), (unsigned long long)n);
		fclose(f);
		if (FILE* v = fopen((s_path + ".variants.csv").c_str(), "w"))
		{
			fputs("fetch,vertex,geometry,pixel,targets,msaa_log2,draws\n", v);
			for (auto& [k, n] : s_variants)
				fprintf(v, "%s,%llu\n", k.c_str(), (unsigned long long)n);
			fclose(v);
		}
	}

	bool Init()
	{
		if (const char* dump = getenv("WWHD_GPU_DUMP"); dump && *dump)
			s_dumpDir = dump;
		const char* path = getenv("WWHD_GPU_STATS");
		if (!path || !*path)
			return false;
		s_path = path;
		at_quick_exit(Report);
		atexit(Report);
		return true;
	}
	const bool s_enabled = Init();
}

// WWHD_GPU_STREAM=path writes, per frame (swap), a hash of every command packet the GPU executes: the
// flattened stream, indirect buffers followed rather than hashed by address, so two builds that send
// the same commands match even if they split them into buffers differently. Our gx2 (D18) must send
// exactly what Cemu's did. Lines: frame, hash, packets.
namespace streamhash
{
	FILE* s_file = nullptr;
	uint64 s_hash = 0xCBF29CE484222325ull, s_packets = 0;
	uint32 s_frame = 0;

	bool Init()
	{
		const char* path = getenv("WWHD_GPU_STREAM");
		if (!path || !*path)
			return false;
		s_file = fopen(path, "w");
		return s_file != nullptr;
	}
	const bool s_enabled = Init();

	inline void Word(uint32 w)
	{
		s_hash = (s_hash ^ w) * 0x100000001B3ull;
	}

	void Packet(uint32 header, const uint32be* body, uint32 nWords)
	{
		Word(header);
		for (uint32 i = 0; i < nWords; i++)
			Word(body[i]);
		s_packets++;
	}

	void Frame()
	{
		fprintf(s_file, "%u %016llx %llu\n", s_frame++, (unsigned long long)s_hash, (unsigned long long)s_packets);
		fflush(s_file);
		s_hash = 0xCBF29CE484222325ull;
		s_packets = 0;
	}
}

// ---- command processing (LatteCommandProcessor.cpp) --------------------------------------------
namespace
{
	template<uint32 Base>
	void setRegisters(const uint32be* p, uint32 nWords)
	{
		uint32 index = Base + (uint32)p[0];
		uint32 count = nWords - 1;
		const bool shadow = LatteGPUState.contextControl0 == 0x80000077;
		for (uint32 i = 0; i < count; i++)
		{
			uint32 v = p[1 + i];
			if (shadow)
			{
				MPTR a = LatteGPUState.contextRegisterShadowAddr[index + i];
				if (a)
					*(uint32*)(memory_base + a) = _swapEndianU32(v);
			}
			LatteGPUState.contextRegister[index + i] = v;
		}
		if constexpr (Base == LATTE_REG_BASE_CONTEXT)
		{
			if (index <= mmSQ_VTX_SEMANTIC_CLEAR && index + count >= mmSQ_VTX_SEMANTIC_CLEAR)
				for (uint32 i = 0; i < 32; i++)
					LatteGPUState.contextRegister[mmSQ_VTX_SEMANTIC_0 + i] = 0xFF;
		}
	}

	void loadRegisters(const uint32be* p, uint32 nWords, uint32 base)
	{
		if (nWords < 2 || (nWords & 1) != 0)
			return;
		MPTR shadowAddr = p[0];
		for (uint32 e = 0; e < (nWords - 2) / 2; e++)
		{
			uint32 reg = base + (uint32)p[2 + e * 2];
			uint32 count = p[3 + e * 2];
			for (uint32 f = 0; f < count; f++, reg++, shadowAddr += 4)
			{
				LatteGPUState.contextRegisterShadowAddr[reg] = shadowAddr;
				LatteGPUState.contextRegister[reg] = memory_read<uint32>(shadowAddr);
			}
		}
	}

	void waitRegMem(const uint32be* p)
	{
		uint32 op = p[0] & 7;
		if (!(p[0] & 0x10))
			return;                                       // register (not memory) waits: never emitted by GX2
		uint32be* fence = phys(p[1] & ~3u);
		uint32 value = p[3], mask = p[4];
		for (;;)
		{
			uint32 m = (uint32)*fence & mask;
			bool ok = op == 0 || op == 7 || (op == 1 && m < value) || (op == 2 && m <= value) || (op == 3 && m == value)
				|| (op == 4 && m != value) || (op == 5 && m >= value) || (op == 6 && m > value);
			if (ok)
				return;
			handleTimedVsync();
			if (!s_running)
				threadExit();
			std::this_thread::yield();
		}
	}

	void memWrite(const uint32be* p)
	{
		MPTR addr = p[0] & ~3u;
		if (addr == 0)
			return;
		uint32be* mem = phys(addr);
		uint32 mode = p[1], lo = p[2], hi = p[3];
		if (mode == 0x40000)
			stdx::atomic_ref<uint32be>(*mem).store(lo);
		else if (mode == 0x00000)
			stdx::atomic_ref<uint64be>(*(uint64be*)mem).store(((uint64)lo << 32) | hi);
		else if (mode == 0x20000)
			stdx::atomic_ref<uint64le>(*(uint64le*)mem).store(((uint64)hi << 32) | lo);
		else
			cemuLog_logOnce(LogType::Force, "null GPU: MEM_WRITE mode {:x} not handled", mode);
	}

	void eventWriteEOP(const uint32be* p)
	{
		uint32 w0 = p[0], addr = p[1], w2 = p[2];
		if (w0 == 0x504 && (w2 & 0x40000000))
			stdx::atomic_ref<uint64be>(*(uint64be*)phys(addr)).store(((uint64)(uint32)p[4] << 32) | (uint32)p[3]);
		TCL::TCLGPUNotifyNewRetirementTimestamp();
	}

	void memSemaphore(const uint32be* p)
	{
		auto* sem = _rawPtrToAtomic((uint64le*)phys(p[0]));
		uint32 signal = ((uint32)p[1] >> 29) & 7;
		if (signal == 6)
			sem->fetch_add(1);
		else if (signal == 7)
		{
			for (;;)
			{
				uint64le v = sem->load();
				if (v != 0 && sem->compare_exchange_strong(v, v - 1))
					return;
				if (!s_running)
					threadExit();
				std::this_thread::yield();
			}
		}
	}

	void streamoutBufferUpdate(const uint32be* p)
	{
		uint32 control = p[0], mode = (control >> 1) & 3, so = (control >> 8) & 3;
		if (mode == 0)
			LatteGPUState.contextRegister[mmVGT_STRMOUT_BUFFER_OFFSET_0 + 4 * so] = 0;
		else if (mode == 3)
			memory_writeU32(memory_physicalToVirtual(p[1]), LatteGPUState.contextRegister[mmVGT_STRMOUT_BUFFER_OFFSET_0 + 4 * so]);
	}

	void processBuffer(const uint32be* p, const uint32be* end, int depth);

	// One type-3 packet. `body` points at its nWords payload words.
	void packet(uint32 op, const uint32be* body, uint32 nWords, int depth)
	{
		if (streamhash::s_enabled && op != IT_INDIRECT_BUFFER_PRIV)
			streamhash::Packet(op << 16 | nWords, body, nWords);
		switch (op)
		{
		case IT_SET_CONTEXT_REG: case IT_SET_ALL_CONTEXTS: setRegisters<LATTE_REG_BASE_CONTEXT>(body, nWords); break;
		case IT_SET_RESOURCE: setRegisters<LATTE_REG_BASE_RESOURCE>(body, nWords); break;
		case IT_SET_ALU_CONST: setRegisters<LATTE_REG_BASE_ALU_CONST>(body, nWords); break;
		case IT_SET_CTL_CONST: setRegisters<mmSQ_VTX_BASE_VTX_LOC>(body, nWords); break;
		case IT_SET_SAMPLER: setRegisters<LATTE_REG_BASE_SAMPLER>(body, nWords); break;
		case IT_SET_CONFIG_REG: setRegisters<LATTE_REG_BASE_CONFIG>(body, nWords); break;
		case IT_SET_LOOP_CONST: break;                     // ignored by Latte as well
		case IT_LOAD_CONFIG_REG: loadRegisters(body, nWords, LATTE_REG_BASE_CONFIG); break;
		case IT_LOAD_CONTEXT_REG: loadRegisters(body, nWords, LATTE_REG_BASE_CONTEXT); break;
		case IT_LOAD_ALU_CONST: loadRegisters(body, nWords, LATTE_REG_BASE_ALU_CONST); break;
		case IT_LOAD_LOOP_CONST: loadRegisters(body, nWords, LATTE_REG_BASE_LOOP_CONST); break;
		case IT_LOAD_RESOURCE: loadRegisters(body, nWords, LATTE_REG_BASE_RESOURCE); break;
		case IT_LOAD_SAMPLER: loadRegisters(body, nWords, LATTE_REG_BASE_SAMPLER); break;
		case IT_CONTEXT_CONTROL: LatteGPUState.contextControl0 = body[0]; LatteGPUState.contextControl1 = body[1]; break;
		case IT_INDEX_TYPE:
			LatteGPUState.contextNew.VGT_DMA_INDEX_TYPE.set_INDEX_TYPE((Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE)(uint32)body[0]);
			break;
		case IT_NUM_INSTANCES:
			LatteGPUState.contextNew.VGT_DMA_NUM_INSTANCES.set_NUM_INSTANCES(body[0] ? (uint32)body[0] : 1);
			break;
		case IT_SET_PREDICATION: s_predicationActive = ((uint32)body[1] >> 8) & 3; break;
		case IT_INDIRECT_BUFFER_PRIV:
		{
			uint32 size = body[2];
			if (size)
			{
				const uint32be* buf = phys(body[0]);
				processBuffer(buf, buf + size, depth + 1);
			}
			break;
		}
		case IT_WAIT_REG_MEM: waitRegMem(body); break;
		case IT_MEM_WRITE: memWrite(body); break;
		case IT_MEM_SEMAPHORE: memSemaphore(body); break;
		case IT_EVENT_WRITE_EOP: eventWriteEOP(body); break;
		case IT_STRMOUT_BUFFER_UPDATE: streamoutBufferUpdate(body); break;
		case IT_HLE_SAMPLE_TIMER: memory_writeU64((MPTR)(uint32)body[0], coreinit::OSGetSystemTime()); break;
		case IT_HLE_SPECIAL_STATE:
			if ((uint32)body[0] <= GX2_SPECIAL_STATE_COUNT)
				LatteGPUState.contextNew.GetSpecialStateValues()[(uint32)body[0]] = body[1];
			break;
		case IT_HLE_BOTTOM_OF_PIPE_CB:
		{
			MPTR addr = body[0];
			*phys(addr) = (uint32)body[1];
			*phys(addr + 4) = (uint32)body[2];
			GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::TIMESTAMP_BOTTOM);
			break;
		}
		case IT_HLE_REQUEST_SWAP_BUFFERS: LatteGPUState.flipRequestCount.fetch_add(1); break;
		case IT_HLE_TRIGGER_SCANBUFFER_SWAP:
			if (streamhash::s_enabled)
				streamhash::Frame();
			LatteGPUState.frameCounter++;
			if (wwhd::gpu::RendererOn())
				wwhd::gpu::RendererSwap();
			break;
		case IT_HLE_WAIT_FOR_FLIP:
		{
			uint32 flips = LatteGPUState.flipCounter;
			while (flips == LatteGPUState.flipCounter)
			{
				handleTimedVsync();
				if (!s_running)
					threadExit();
				std::this_thread::yield();
			}
			break;
		}
		case IT_HLE_BEGIN_OCCLUSION_QUERY: case IT_HLE_END_OCCLUSION_QUERY:
			cemuLog_logOnce(LogType::Force, "null GPU: occlusion queries are not answered");
			break;
		// the renderer (src/gpu/vk, WWHD_RENDER=vk) draws these; nothing guest-visible
		case IT_DRAW_INDEX_2: case IT_DRAW_INDEX_AUTO: case IT_DRAW_INDEX_IMMD:
			if (gpustats::s_enabled)
				gpustats::Draw(op);
			if (wwhd::gpu::RendererOn())
				wwhd::gpu::RendererDraw(op, body, nWords);
			break;
		case IT_HLE_CLEAR_COLOR_DEPTH_STENCIL:
			if (wwhd::gpu::RendererOn())
				wwhd::gpu::RendererClear(body, nWords);
			break;
		case IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER:
			if (wwhd::gpu::RendererOn())
				wwhd::gpu::RendererCopyToScanBuffer(body, nWords);
			break;
		case IT_HLE_COPY_SURFACE_NEW:
			if (wwhd::gpu::RendererOn())
				wwhd::gpu::RendererCopySurface(body, nWords);
			break;
		case IT_SURFACE_SYNC: case IT_HLE_SYNC_ASYNC_OPERATIONS:
			break;
		default:
			cemuLog_logOnce(LogType::Force, "null GPU: unknown PM4 packet {:02x}", op);
			break;
		}
	}

	// type-0 packets: only the two GX2 timestamp registers occur
	void type0(uint32 header)
	{
		if (streamhash::s_enabled)
			streamhash::Word(header);
		if ((header & 0xFFFF) == 0x304A)
			GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::TIMESTAMP_TOP);
	}

	void processBuffer(const uint32be* p, const uint32be* end, int depth)
	{
		if (depth > 16)
		{
			cemuLog_logOnce(LogType::Force, "null GPU: indirect buffers nested too deep");
			return;
		}
		while (p < end)
		{
			uint32 h = *p++;
			uint32 type = h >> 30;
			if (type == 3)
			{
				uint32 n = ((h >> 16) & 0x3FFF) + 1;
				packet((h >> 8) & 0xFF, p, n, depth);
				p += n;
			}
			else if (type == 0)
			{
				type0(h);
				p += ((h >> 16) & 0x3FFF) + 1;
			}
			// type 2: filler
		}
	}

	// The next word of the ring: after a few quick looks (a submission usually follows closely), the
	// thread sleeps until the CPU submits (os/tcl) instead of spinning, waking every millisecond for
	// host-timed vsync and shutdown.
	uint32 ringWord()
	{
		uint32 w;
		for (int spins = 0;; spins++)
		{
			if (TCL::TCLGPUReadRBWord(w))
				return w;
			if (!s_running)
				threadExit();
			handleTimedVsync();
			if (spins < 64)
				std::this_thread::yield();
			else
				TCL::TCLGPUWaitForCommands(std::chrono::microseconds(1000));
		}
	}

	void processRing()
	{
		uint32be tmp[0x4000];
		for (;;)
		{
			uint32 h = ringWord();
			uint32 type = h >> 30;
			if (type == 3)
			{
				uint32 n = ((h >> 16) & 0x3FFF) + 1;
				for (uint32 i = 0; i < n; i++)
					tmp[i] = ringWord();
				packet((h >> 8) & 0xFF, tmp, n, 0);
			}
			else if (type == 0)
			{
				type0(h);
				for (uint32 i = ((h >> 16) & 0x3FFF) + 1; i; i--)
					ringWord();
			}
		}
	}

	void threadEntry()
	{
		SetThreadName("NullGPU");
		LatteGPUState.timer_frequency = HighResolutionTimer::getFrequency();
		LatteGPUState.timer_bootUp = HighResolutionTimer::now().getTick();
		LatteGPUState.timer_nextVSync = LatteGPUState.timer_bootUp + timeBetweenVSync();
		LatteGPUState.glVendor = GLVENDOR_UNKNOWN;
		s_initDone = true;
		while (!CafeSystem::IsTitleRunning())
		{
			if (!s_running)
				threadExit();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		// Latte_LoadInitialRegisters
		LatteGPUState.contextNew.CB_TARGET_MASK.set_MASK(0xFFFFFFFF);
		LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.set_RESTART_INDEX(0xFFFFFFFF);
		LatteGPUState.contextNew.VGT_DMA_NUM_INSTANCES.set_NUM_INSTANCES(1);
		LatteGPUState.contextRegister[Latte::REGADDR::PA_CL_CLIP_CNTL] = 0;
		*(float*)&LatteGPUState.contextRegister[mmDB_DEPTH_CLEAR] = 1.0f;
		g_isGPUInitFinished = true;
		while (LatteGPUState.gx2InitCalled == 0)
		{
			if (!s_running)
				threadExit();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		processRing();
	}
}

// ---- the Latte entry points the rest of Cemu calls ----------------------------------------------
void Latte_Start()
{
	std::unique_lock lock(s_stateMutex);
	s_running = true;
	s_initDone = false;
	s_thread = std::thread(threadEntry);
	while (!s_initDone)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void Latte_Stop()
{
	std::unique_lock lock(s_stateMutex);
	if (!s_running)
		return;
	s_running = false;
	lock.unlock();
	s_thread.join();
}

bool Latte_GetStopSignal() { return !s_running; }

// The CPU wrote memory the GPU may read (dcbf/dcbst, DCFlushRange): nothing is cached here.
void LatteBufferCache_notifyDCFlush(MPTR, uint32) {}

void LatteRenderTarget_getScreenImageArea(sint32* x, sint32* y, sint32* w, sint32* h, sint32* fw, sint32* fh, bool)
{
	*x = *y = 0;
	*w = *fw = 1920;
	*h = *fh = 1080;
}

// GX2CopySurface's CPU copies (a linear-special surface on either side; WWHD makes 50 at boot).
// The reference queues them to its GPU thread (LatteSurfaceCopy_copySurfaceNew) and GX2CopySurface
// waits for them. From a linear-special source, and from any source its texture cache doesn't
// hold, it untiles and retiles in guest memory (LatteSurfaceCopy_CopyInRAM), which is done here in
// place. The one path not taken is the reference's readback of a GPU-rendered source into a
// linear-special destination: nothing here renders into guest memory.
void gx2SurfaceCopySoftware(uint8* inputData, sint32 surfSrcHeight, sint32 srcPitch, sint32 srcDepth, uint32 srcSlice,
	uint32 srcSwizzle, uint32 srcHwTileMode, uint8* outputData, sint32 surfDstHeight, sint32 dstPitch, sint32 dstDepth,
	uint32 dstSlice, uint32 dstSwizzle, uint32 dstHwTileMode, uint32 copyWidth, uint32 copyHeight, uint32 copyBpp);

void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	Latte::E_HWFMT dstHwFormat = Latte::GetHWFormat(dst.surfaceFormat);
	sint32 copyWidth = rect.width, copyHeight = rect.height;
	if (Latte::IsCompressedFormat(dstHwFormat))
	{
		copyWidth = (copyWidth + 3) / 4;
		copyHeight = (copyHeight + 3) / 4;
	}
	gx2SurfaceCopySoftware((uint8*)MEMPTR<void>(src.physDataAddr).GetPtr(), src.heightInTexels, src.pitch, 1, src.sliceIndex, src.swizzle,
		(uint32)src.tilemode, (uint8*)MEMPTR<void>(dst.physDataAddr).GetPtr(), dst.heightInTexels, dst.pitch, 1, dst.sliceIndex,
		dst.swizzle, (uint32)dst.tilemode, copyWidth, copyHeight, Latte::GetFormatBits(dstHwFormat));
}
void LatteAsyncCommands_waitUntilAllProcessed() {}

// Referenced by Cemu's main() for a Vulkan driver probe that belongs to Latte's renderer.
int BreathOfTheWildChildProcessMain() { return 0; }

// No Vulkan in this build (only logging asks), and no renderer to take screenshots
// (GX2's screenshot hook checks g_renderer first).
bool g_vulkan_available = false;
void Renderer::RequestScreenshot(ScreenshotSaveFunction) { }
