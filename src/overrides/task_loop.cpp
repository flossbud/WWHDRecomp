// f_0275FFCC: the game's task message loop (docs/recompiler-design.md D9, D19).
//
// Some of the game's work runs as tasks: objects with an OSMessageQueue at task+32, each on a guest
// thread of its own. The thread's entry, f_02760ACC, stores the task in a thread-specific slot and
// runs this loop, which takes messages (f_02760374: OSReceiveMessage, blocking unless task+116 says
// otherwise) until the task's stop message (task+120) and hands each other one to the task's handler
// (vtable at task+12, slot 124): handler(task, message).
//
// One task ticks: after every message, its handler posts message 5 to its own queue (non-blocking),
// and handling a 5 does nothing while the object waits for work. On the Wii U that thread spins:
// an OSSendMessage and an OSReceiveMessage a round, 35,000 rounds a frame on the save route (69
// million over it); in real time it kept the host's scheduler thread busy all the time.
//
// With the fast paths (real time only) the loop is this one: the game's instructions, plus a watch
// on each handler call (wwhd::rt::QuietBegin). A call that wrote nothing but the dead stack below
// this frame, made one OS call, left the queue one message longer with its own message at the end,
// and never left the core, had no effect but posting that message back. Handling it again reads
// the same memory and does the same nothing, until something else writes memory, a message arrives
// or time passes. So when the loop takes that message again and the queue holds nothing else, the
// thread first waits until a message arrives or a millisecond has passed (the scheduler's own
// polling interval), and the host thread sleeps if no other guest thread can run. To the game it
// looks as if the thread had been preempted for a while: the messages, their order and every write
// are the same; something only memory or the clock tells the handler is seen at most 1 ms later.
// With the virtual clock (every check) the game's loop runs, orig_f_0275FFCC.
#include "override.h"

namespace
{
	constexpr uint32 kQueue = 32;                  // the task's OSMessageQueue (f_02760374 adds 4 to task+28)
	constexpr uint64 kIdleWaitNs = 1000000;        // 1 ms: the scheduler's idle loop polls as often

	// 0275FFE0-0275FFE8 and 02760014-0276001C: r3 = f_02760374(task + 28, task[116]), returning to lr
	inline void Receive(PPCInterpreter_t* __restrict ctx, uint32 lr)
	{
		GPR(4) = rd32(GPR(31) + 116);
		GPR(3) = GPR(31) + 28;
		ctx->spr.LR = lr;
		f_02760374(ctx);
	}
}

void f_0275FFCC(PPCInterpreter_t* __restrict ctx)
{
	if (!wwhd::rt::FastPaths())
		[[clang::musttail]] return orig_f_0275FFCC(ctx);
	GPR(0) = ctx->spr.LR;                                  // mflr r0
	wr32(GPR(1) - 16, GPR(1));                             // stwu r1, -16(r1)
	GPR(1) -= 16;
	wr32(GPR(1) + 12, GPR(31));                            // stw r31, 12(r1)
	GPR(31) = GPR(3);                                      // mr r31, r3: the task
	wr32(GPR(1) + 20, GPR(0));                             // stw r0, 20(r1)
	Receive(ctx, 0x0275FFECu);
	GPR(10) = rd32(GPR(31) + 120);                         // the stop message
	cr_compare<uint32>(ctx, 0, GPR(3), GPR(10));
	uint32 stop = GPR(10);
	bool idle = false;                                     // the last call only posted its message back
	uint32 idleMessage = 0;
	while (GPR(3) != stop)
	{
		const uint32 message = GPR(3), queue = GPR(31) + kQueue;
		if (idle && message == idleMessage)
		{
			wwhd::os::QueueView v = wwhd::os::ViewQueue(queue, message);
			if (v.allSame)
				wwhd::rt::CountIdleWait(wwhd::os::WaitQueueChange(queue, v.used, kIdleWaitNs));
		}
		GPR(12) = rd32(GPR(31) + 12);                      // lwz r12, 12(r31): the vtable
		GPR(0) = rd32(GPR(12) + 124);                      // lwz r0, 124(r12): the handler
		GPR(4) = GPR(3);                                   // mr r4, r3
		ctx->spr.CTR = GPR(0);                             // mtctr r0
		GPR(3) = GPR(31);                                  // mr r3, r31
		ctx->spr.LR = 0x02760014u;                         // bctrl
		const uint32 before = wwhd::os::ViewQueue(queue, message).used;
		// dead once the handler returns: the stack below this frame, and 4(r1), where callees save
		// their return address (the EABI's LR save word, which this function never reads)
		const uint64 watch = wwhd::rt::QuietBegin(ctx, wwhd::os::CurrentThreadStackEnd(), GPR(1) + 8);
		RT_CALL_CTR();
		const sint32 osCalls = wwhd::rt::QuietEnd(watch);
		const wwhd::os::QueueView after = wwhd::os::ViewQueue(queue, message);
		idle = osCalls == 1 && after.used == before + 1 && after.last == message;
		idleMessage = message;
		Receive(ctx, 0x02760020u);
		GPR(0) = rd32(GPR(31) + 120);                      // lwz r0, 120(r31)
		cr_compare<uint32>(ctx, 0, GPR(3), GPR(0));
		stop = GPR(0);
	}
	GPR(0) = rd32(GPR(1) + 20);                            // lwz r0, 20(r1)
	GPR(31) = rd32(GPR(1) + 12);                           // lwz r31, 12(r1)
	ctx->spr.LR = GPR(0);                                  // mtlr r0
	GPR(1) += 16;                                          // addi r1, r1, 16; blr
}
