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
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/CafeSystem.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "util/helpers/helpers.h"

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
		case IT_HLE_TRIGGER_SCANBUFFER_SWAP: LatteGPUState.frameCounter++; break;
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
		// nothing to observe without a renderer
		case IT_DRAW_INDEX_2: case IT_DRAW_INDEX_AUTO: case IT_DRAW_INDEX_IMMD: case IT_SURFACE_SYNC:
		case IT_HLE_CLEAR_COLOR_DEPTH_STENCIL: case IT_HLE_COPY_SURFACE_NEW:
		case IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER: case IT_HLE_SYNC_ASYNC_OPERATIONS:
			break;
		default:
			cemuLog_logOnce(LogType::Force, "null GPU: unknown PM4 packet {:02x}", op);
			break;
		}
	}

	// type-0 packets: only the two GX2 timestamp registers occur
	void type0(uint32 header)
	{
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

	uint32 ringWord()
	{
		uint32 w;
		for (;;)
		{
			if (TCL::TCLGPUReadRBWord(w))
				return w;
			if (!s_running)
				threadExit();
			handleTimedVsync();
			std::this_thread::yield();
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

// GX2CopySurface's CPU-copy path goes through the reference's GPU texture cache, which a null GPU
// doesn't have. Logged so a trace divergence here is easy to spot.
void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam&, const LatteSurfaceCopyParam&, const LatteSurfaceCopyRect&)
{
	cemuLog_logOnce(LogType::Force, "null GPU: GX2CopySurface CPU copy skipped");
}
void LatteAsyncCommands_waitUntilAllProcessed() {}

// Only reached through GX2 tiling apertures, which WWHD does not use.
void LatteTextureLoader_begin(LatteTextureLoaderCtx*, uint32, uint32, MPTR, MPTR, Latte::E_GX2SURFFMT, Latte::E_DIM, uint32, uint32, uint32, uint32, uint32, Latte::E_HWTILEMODE, uint32)
{
	cemu_assert_unimplemented();
	abort();
}
uint8* LatteTextureLoader_GetInput(LatteTextureLoaderCtx*, sint32, sint32)
{
	abort();
}

// Referenced by Cemu's main() for a Vulkan driver probe that belongs to Latte's renderer.
int BreathOfTheWildChildProcessMain() { return 0; }

// No Vulkan in this build (only logging asks), and no renderer to take screenshots
// (GX2's screenshot hook checks g_renderer first).
bool g_vulkan_available = false;
void Renderer::RequestScreenshot(ScreenshotSaveFunction) { }
