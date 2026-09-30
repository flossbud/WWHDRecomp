// coreinit_Scheduler.cpp (docs/recompiler-design.md D18): the scheduler lock and interrupt masks.
// Cemu's src/Cafe/OS/libs/coreinit/coreinit_Scheduler.cpp (Mozilla Public License 2.0), forked:
// wwhd-null links this in place of Cemu's object, at its position in the link (so its guest-memory
// slots keep their addresses) and under Cemu's names (so Cemu's code that calls it reaches ours).
// Route traces, GPU commands and sound must stay as Cemu's (tools/reference/stream_check.sh).
#include "Cafe/OS/common/OSCommon.h"
#include "coreinit_Scheduler.h"

thread_local sint32 s_schedulerLockCount = 0;   // this host thread's holds (it may hold it across fiber switches)

// wwhd: the scheduler lock, inline, in place of a recursive pthread mutex (a PLT call and an owner
// check in libc on every OS call that touches the scheduler: ~6% of the CPU thread, design doc
// "Profile of the native build"). Three states, as a futex lock: free, held, held with waiters;
// recursion per host thread comes from s_schedulerLockCount, as the pthread mutex gave it.
static std::atomic<uint32> s_schedulerLock{ 0 };  // 0 free, 1 held, 2 held and a thread waits

static inline void SchedulerLockAcquire()
{
	uint32 c = 0;
	if (s_schedulerLock.compare_exchange_strong(c, 1, std::memory_order_acquire)) [[likely]]
		return;
	if (c != 2)
		c = s_schedulerLock.exchange(2, std::memory_order_acquire);
	while (c != 0)
	{
		s_schedulerLock.wait(2, std::memory_order_relaxed);
		c = s_schedulerLock.exchange(2, std::memory_order_acquire);
	}
}

static inline void SchedulerLockRelease()
{
	if (s_schedulerLock.exchange(0, std::memory_order_release) == 2) [[unlikely]]
		s_schedulerLock.notify_one();
}

void __OSLockScheduler(void* obj)
{
	if (s_schedulerLockCount == 0)
		SchedulerLockAcquire();
	s_schedulerLockCount++;
	cemu_assert_debug(s_schedulerLockCount <= 1); // >= 2 should not happen. Scheduler lock does not allow recursion
}

bool __OSHasSchedulerLock()
{
	return s_schedulerLockCount > 0;
}

bool __OSTryLockScheduler(void* obj)
{
	if (s_schedulerLockCount == 0)
	{
		uint32 c = 0;
		if (!s_schedulerLock.compare_exchange_strong(c, 1, std::memory_order_acquire))
			return false;
	}
	s_schedulerLockCount++;
	return true;
}

void __OSUnlockScheduler(void* obj)
{
	s_schedulerLockCount--;
	cemu_assert_debug(s_schedulerLockCount >= 0);
	if (s_schedulerLockCount == 0)
		SchedulerLockRelease();
}

namespace coreinit
{
	uint32 OSIsInterruptEnabled()
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		if (hCPU == nullptr)
			return 0;

		return hCPU->coreInterruptMask;
	}

	// disables interrupts and scheduling
	uint32 OSDisableInterrupts()
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		if (hCPU == nullptr)
			return 0;
		uint32 prevInterruptMask = hCPU->coreInterruptMask;
		if (hCPU->coreInterruptMask != 0)
		{
			// we have no efficient method to turn off scheduling completely, so instead we just increase the remaining cycles
			if (hCPU->remainingCycles >= 0x40000000)
				cemuLog_log(LogType::Force, "OSDisableInterrupts(): Warning - Interrupts already disabled but the mask was still set? remCycles {:08x} LR {:08x}", hCPU->remainingCycles, hCPU->spr.LR);
			hCPU->remainingCycles += 0x40000000;
		}
		hCPU->coreInterruptMask = 0;
		return prevInterruptMask;
	}

	uint32 OSRestoreInterrupts(uint32 interruptMask)
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		if (hCPU == nullptr)
			return 0;
		uint32 prevInterruptMask = hCPU->coreInterruptMask;
		if (hCPU->coreInterruptMask == 0 && interruptMask != 0)
		{
			hCPU->remainingCycles -= 0x40000000;
		}
		hCPU->coreInterruptMask = interruptMask;
		return prevInterruptMask;
	}

	uint32 OSEnableInterrupts()
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		uint32 prevInterruptMask = hCPU->coreInterruptMask;
		OSRestoreInterrupts(1);
		return prevInterruptMask;
	}

	void InitializeSchedulerLock()
	{
		cafeExportRegister("coreinit", __OSLockScheduler, LogType::Placeholder);
		cafeExportRegister("coreinit", __OSUnlockScheduler, LogType::Placeholder);

		cafeExportRegister("coreinit", OSDisableInterrupts, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSEnableInterrupts, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSRestoreInterrupts, LogType::CoreinitThread);
	}
};
