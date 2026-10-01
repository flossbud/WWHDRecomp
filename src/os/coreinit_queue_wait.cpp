// For the game's task loop in real time (src/overrides/task_loop.cpp, design D19): looking at a message
// queue and waiting for a message to arrive without taking one. Through coreinit's own API and
// structures, so it works over our scheduler fork and Cemu's alike; never used with the virtual clock.
#include "os.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"

namespace wwhd::os
{
	QueueView ViewQueue(uint32 queue, uint32 word)
	{
		using namespace coreinit;
		OSMessageQueue* q = MEMPTR<OSMessageQueue>(queue).GetPtr();
		__OSLockScheduler(q);
		QueueView v{ q->usedCount, 0, true };
		const uint32 count = q->msgCount, first = q->firstIndex;
		for (uint32 i = 0; i < v.used && count; i++)
		{
			uint32 m = q->msgArray.GetPtr()[(first + i) % count].message;
			v.allSame &= m == word;
			v.last = m;
		}
		__OSUnlockScheduler(q);
		return v;
	}

	namespace
	{
		struct QueueWait
		{
			OSThread_t* thread;
			coreinit::OSThreadQueue* threadQueue;
		};

		void QueueWaitTimeout(uint64 currentTick, void* context)
		{
			QueueWait* w = (QueueWait*)context;
			if (w->thread->state == OSThread_t::THREAD_STATE::STATE_WAITING)
				w->threadQueue->cancelWait(w->thread);
		}
	}

	// The thread waits among the queue's receivers (OSSendMessage wakes the first, and nothing else
	// looks at them), with a host alarm for the timeout, as OSWaitEventWithTimeout does.
	bool WaitQueueChange(uint32 queue, uint32 used, uint64 timeoutNs)
	{
		using namespace coreinit;
		OSMessageQueue* q = MEMPTR<OSMessageQueue>(queue).GetPtr();
		__OSLockScheduler(q);
		if (q->usedCount == used)
		{
			QueueWait w{ OSGetCurrentThread(), &q->threadQueueReceive };
			OSHostAlarm* alarm = OSHostAlarmCreate(OSGetTime() + EspressoTime::ConvertNsToTimerTicks(timeoutNs), 0, QueueWaitTimeout, &w);
			q->threadQueueReceive.queueAndWait(w.thread);
			OSHostAlarmDestroy(alarm);
		}
		bool changed = q->usedCount != used;
		__OSUnlockScheduler(q);
		return changed;
	}
}
