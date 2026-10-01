// coreinit_MessageQueue.cpp (docs/recompiler-design.md D18): message queues. Cemu's
// src/Cafe/OS/libs/coreinit/coreinit_MessageQueue.cpp (Mozilla Public License 2.0), forked: wwhd-
// null links this in place of Cemu's object, at its position in the link (so its guest-memory slots
// keep their addresses) and under Cemu's names (so Cemu's code that calls it reaches ours). Route
// traces, GPU commands and sound must stay as Cemu's (tools/reference/stream_check.sh).
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "../os.h"

namespace coreinit
{
	// wwhd: x % count without the division when x is already below it (a ring index usually is): the
	// same result for every x; the queue ops run ~2.3 million times a second on the save route
	static inline uint32 RingIndex(uint32 x, uint32 count)
	{
		return x < count ? x : x % count;
	}

	void UpdateSystemMessageQueue();
	void HandleReceivedSystemMessage(OSMessage* msg);

	SysAllocator<OSMessageQueue> g_systemMessageQueue;
	SysAllocator<OSMessage, 16> _systemMessageQueueArray;

	void OSInitMessageQueueEx(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount, void* userData)
	{
		msgQueue->magic = 'mSgQ';
		msgQueue->userData = userData;
		msgQueue->msgArray = msgArray;
		msgQueue->msgCount = msgCount;
		msgQueue->firstIndex = 0;
		msgQueue->usedCount = 0;
		msgQueue->ukn08 = 0;
		OSInitThreadQueueEx(&msgQueue->threadQueueReceive, msgQueue);
		OSInitThreadQueueEx(&msgQueue->threadQueueSend, msgQueue);
	}

	void OSInitMessageQueue(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount)
	{
		OSInitMessageQueueEx(msgQueue, msgArray, msgCount, nullptr);
	}

	bool OSReceiveMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags)
	{
		bool isSystemMessageQueue = (msgQueue == g_systemMessageQueue);
		if(isSystemMessageQueue)
			UpdateSystemMessageQueue();
		__OSLockScheduler(msgQueue);
		while (msgQueue->usedCount == (uint32be)0)
		{
			if ((flags & OS_MESSAGE_BLOCK))
			{
				msgQueue->threadQueueReceive.queueAndWait(OSGetCurrentThread());
			}
			else
			{
				__OSUnlockScheduler(msgQueue);
				return false;
			}
		}
		// copy message
		sint32 messageIndex = msgQueue->firstIndex;
		OSMessage* readMsg = &(msgQueue->msgArray[messageIndex]);
		memcpy(msg, readMsg, sizeof(OSMessage));
		msgQueue->firstIndex = RingIndex((uint32)msgQueue->firstIndex + 1, (uint32)(msgQueue->msgCount));
		msgQueue->usedCount = (uint32)msgQueue->usedCount - 1;
		// wake up any thread waiting to add a message
		if (!msgQueue->threadQueueSend.isEmpty())
			msgQueue->threadQueueSend.wakeupSingleThreadWaitQueue(true);
		__OSUnlockScheduler(msgQueue);
		if(isSystemMessageQueue)
			HandleReceivedSystemMessage(msg);
		return true;
	}

	bool OSPeekMessage(OSMessageQueue* msgQueue, OSMessage* msg)
	{
		__OSLockScheduler(msgQueue);
		if ((msgQueue->usedCount == (uint32be)0))
		{
			__OSUnlockScheduler(msgQueue);
			return false;
		}
		// copy message
		sint32 messageIndex = msgQueue->firstIndex;
		if (msg)
		{
			OSMessage* readMsg = &(msgQueue->msgArray[messageIndex]);
			memcpy(msg, readMsg, sizeof(OSMessage));
		}
		__OSUnlockScheduler(msgQueue);
		return true;
	}

	sint32 OSSendMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags)
	{
		__OSLockScheduler();
		while (msgQueue->usedCount >= msgQueue->msgCount)
		{
			if ((flags & OS_MESSAGE_BLOCK))
			{
				msgQueue->threadQueueSend.queueAndWait(OSGetCurrentThread());																  
			}
			else
			{
				__OSUnlockScheduler();
				return 0;
			}
		}
		// add message
		if ((flags & OS_MESSAGE_HIGH_PRIORITY))
		{
			// decrease firstIndex
			sint32 newFirstIndex = (sint32)((sint32)msgQueue->firstIndex + (sint32)msgQueue->msgCount - 1) % (sint32)msgQueue->msgCount;
			msgQueue->firstIndex = newFirstIndex;
			// insert message at new first index
			msgQueue->usedCount = (uint32)msgQueue->usedCount + 1;
			OSMessage* newMsg = &(msgQueue->msgArray[newFirstIndex]);
			memcpy(newMsg, msg, sizeof(OSMessage));
		}
		else
		{
			sint32 messageIndex = RingIndex((uint32)(msgQueue->firstIndex + msgQueue->usedCount), (uint32)msgQueue->msgCount);
			msgQueue->usedCount = (uint32)msgQueue->usedCount + 1;
			OSMessage* newMsg = &(msgQueue->msgArray[messageIndex]);
			memcpy(newMsg, msg, sizeof(OSMessage));
		}
		// wake up any thread waiting to read a message
		if (!msgQueue->threadQueueReceive.isEmpty())
			msgQueue->threadQueueReceive.wakeupSingleThreadWaitQueue(true);
		__OSUnlockScheduler();
		return 1;
	}

	OSMessageQueue* OSGetSystemMessageQueue()
	{
		return g_systemMessageQueue.GetPtr();
	}

	void InitializeMessageQueue()
	{
		OSInitMessageQueue(g_systemMessageQueue.GetPtr(), _systemMessageQueueArray.GetPtr(), _systemMessageQueueArray.GetCount());

		cafeExportRegister("coreinit", OSInitMessageQueueEx, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSInitMessageQueue, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSReceiveMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSPeekMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSSendMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetSystemMessageQueue, LogType::CoreinitThread);
	}
};

// wwhd: for the game's task loop in real time (src/overrides/task_loop.cpp, design D19). Never used
// with the virtual clock.
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
