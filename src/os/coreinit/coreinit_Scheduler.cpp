// coreinit_Scheduler.cpp (docs/recompiler-design.md D18): the scheduler lock and interrupt masks.
// Cemu's src/Cafe/OS/libs/coreinit/coreinit_Scheduler.cpp (Mozilla Public License 2.0), forked:
// wwhd-null links this in place of Cemu's object, under Cemu's names (so Cemu's code that calls it
// reaches ours). Route traces, GPU commands and sound must stay as Cemu's
// (tools/reference/stream_check.sh).
#include "Cafe/OS/common/OSCommon.h"
#include "coreinit_Scheduler.h"

thread_local sint32 s_schedulerLockCount = 0;   // this host thread's holds (it may hold it across fiber switches)

// The count through a call on arm64, as Cemu's TLS_WORKAROUND_NOINLINE does for the current instance: a fiber that
// locks, switches and unlocks (PPCCore_switchToScheduler) may come back on another host thread, and arm64 code reads
// the thread pointer once a function, so the unlock counted on the first thread's hold and the lock stayed held (the
// device's real-time build on 3 host threads, its first frame). The empty asm keeps the call from being taken as pure.
#if defined(__aarch64__)
__attribute__((noinline)) static sint32& LockCount()
{
	sint32* p = &s_schedulerLockCount;
	asm volatile("" : "+r"(p));
	return *p;
}
#else
static inline sint32& LockCount() { return s_schedulerLockCount; }
#endif

// wwhd: the scheduler lock, inline, in place of a recursive pthread mutex (a PLT call and an owner
// check in libc on every OS call that touches the scheduler: ~6% of the CPU thread, design doc
// "Profile of the native build"). Three states, as a futex lock: free, held, held with waiters;
// recursion per host thread comes from s_schedulerLockCount, as the pthread mutex gave it.
static std::atomic<uint32> s_schedulerLock{ 0 };  // 0 free, 1 held, 2 held and a thread waits

// The wait and the wake. libc++'s (Android) atomic wait on a uint32 sleeps on a slot of a shared table, not on the
// atomic, and its notify_one wakes one sleeper of the slot: a thread waiting on another atomic there takes the wake and
// ours sleeps on, the lock free (seen on the device: the GPU thread and a core both asleep at state 2). A futex on the
// lock word itself there; libstdc++'s wait (x86) wakes every sleeper of its slot.
#if defined(__ANDROID__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
static inline void SchedulerLockSleep()
{
	syscall(SYS_futex, reinterpret_cast<uint32*>(&s_schedulerLock), FUTEX_WAIT_PRIVATE, 2u, nullptr, nullptr, 0);
}
static inline void SchedulerLockWake()
{
	syscall(SYS_futex, reinterpret_cast<uint32*>(&s_schedulerLock), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
}
#else
static inline void SchedulerLockSleep() { s_schedulerLock.wait(2, std::memory_order_relaxed); }
static inline void SchedulerLockWake() { s_schedulerLock.notify_one(); }
#endif

static inline void SchedulerLockAcquire()
{
	uint32 c = 0;
	if (s_schedulerLock.compare_exchange_strong(c, 1, std::memory_order_acquire)) [[likely]]
		return;
	if (c != 2)
		c = s_schedulerLock.exchange(2, std::memory_order_acquire);
	while (c != 0)
	{
		SchedulerLockSleep();
		c = s_schedulerLock.exchange(2, std::memory_order_acquire);
	}
}

static inline void SchedulerLockRelease()
{
	if (s_schedulerLock.exchange(0, std::memory_order_release) == 2) [[unlikely]]
		SchedulerLockWake();
}

void __OSLockScheduler(void* obj)
{
	if (LockCount() == 0)
		SchedulerLockAcquire();
	LockCount()++;
	cemu_assert_debug(LockCount() <= 1); // >= 2 should not happen. Scheduler lock does not allow recursion
}

bool __OSHasSchedulerLock()
{
	return LockCount() > 0;
}

bool __OSTryLockScheduler(void* obj)
{
	if (LockCount() == 0)
	{
		uint32 c = 0;
		if (!s_schedulerLock.compare_exchange_strong(c, 1, std::memory_order_acquire))
			return false;
	}
	LockCount()++;
	return true;
}

void __OSUnlockScheduler(void* obj)
{
	LockCount()--;
	cemu_assert_debug(LockCount() >= 0);
	if (LockCount() == 0)
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
