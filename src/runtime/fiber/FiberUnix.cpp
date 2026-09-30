// FiberUnix.cpp (docs/recompiler-design.md D18): fibers: each guest thread's stack, and switching
// between them. Cemu's src/util/Fiber/FiberUnix.cpp (Mozilla Public License 2.0), forked: wwhd-null
// links this in place of Cemu's object, at its position in the link (so its guest-memory slots keep
// their addresses) and under Cemu's names (so Cemu's code that calls it reaches ours). Route
// traces, GPU commands and sound must stay as Cemu's (tools/reference/stream_check.sh).
#include "Fiber.h"
#include <ucontext.h>
#include <atomic>

thread_local Fiber* sCurrentFiber{};

#if defined(__x86_64__) && !defined(_WIN32)
// wwhd: our own context switch (docs/recompiler-design.md D19) in place of swapcontext, which saves
// and restores the signal mask with a system call on every switch (2.9% of the CPU thread) and is
// deprecated on macOS. It keeps what the System V ABI says a call preserves: rbx, rbp, r12-r15, the
// MXCSR control bits and the x87 control word; the stack pointer is the fiber's context.
struct FiberContext
{
	void* sp = nullptr;          // where wwhd_fiber_switch left this fiber's registers
};

extern "C" void wwhd_fiber_switch(void** save, void* load);   // save: the leaving fiber's sp slot
extern "C" void wwhd_fiber_start();                          // a new fiber's first "return" lands here

asm(R"(
	.text
	.p2align 4
	.globl wwhd_fiber_switch
	.type wwhd_fiber_switch, @function
wwhd_fiber_switch:
	.cfi_startproc
	pushq %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	subq $8, %rsp
	stmxcsr (%rsp)
	fnstcw 4(%rsp)
	movq %rsp, (%rdi)
	movq %rsi, %rsp
	ldmxcsr (%rsp)
	fldcw 4(%rsp)
	addq $8, %rsp
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	popq %rbx
	popq %rbp
	ret
	.cfi_endproc
	.size wwhd_fiber_switch, .-wwhd_fiber_switch
	.globl wwhd_fiber_switch_end
wwhd_fiber_switch_end:

	.p2align 4
	.globl wwhd_fiber_start
	.type wwhd_fiber_start, @function
wwhd_fiber_start:
	.cfi_startproc
	.cfi_undefined rip
	movq %r13, %rdi
	andq $-16, %rsp
	callq *%r12
	ud2
	.cfi_endproc
	.size wwhd_fiber_start, .-wwhd_fiber_start
)");

Fiber::Fiber(void(*FiberEntryPoint)(void* userParam), void* userParam, void* privateData) : m_privateData(privateData)
{
	const size_t stackSize = 2 * 1024 * 1024;
	m_stackPtr = malloc(stackSize);
	// the frame wwhd_fiber_switch pops the first time it switches here, so that it "returns" into
	// wwhd_fiber_start with the entry point in r12 and its parameter in r13; floating point starts
	// in this thread's modes
	uintptr_t top = ((uintptr_t)m_stackPtr + stackSize) & ~(uintptr_t)15;
	uint64* frame = (uint64*)(top - 9 * 8);
	uint32 mxcsr;
	uint16 fcw;
	asm volatile("stmxcsr %0" : "=m"(mxcsr));
	asm volatile("fnstcw %0" : "=m"(fcw));
	frame[0] = mxcsr | (uint64)fcw << 32;
	frame[1] = 0;                                   // r15
	frame[2] = 0;                                   // r14
	frame[3] = (uint64)userParam;                   // r13
	frame[4] = (uint64)FiberEntryPoint;             // r12
	frame[5] = 0;                                   // rbx
	frame[6] = 0;                                   // rbp
	frame[7] = (uint64)&wwhd_fiber_start;           // return address
	frame[8] = 0;
	FiberContext* ctx = new FiberContext;
	ctx->sp = frame;
	this->m_implData = ctx;
}

Fiber::Fiber(void* privateData) : m_privateData(privateData)
{
	this->m_implData = new FiberContext;            // the thread's own stack: saved at its first switch
	m_stackPtr = nullptr;
}

Fiber::~Fiber()
{
	if(m_stackPtr)
		free(m_stackPtr);
	delete (FiberContext*)m_implData;
}

void Fiber::Switch(Fiber& targetFiber)
{
	Fiber* leavingFiber = sCurrentFiber;
	if (leavingFiber == &targetFiber)
		return;                                     // swapcontext to itself changed nothing either
	sCurrentFiber = &targetFiber;
	std::atomic_thread_fence(std::memory_order_seq_cst);
	wwhd_fiber_switch(&((FiberContext*)leavingFiber->m_implData)->sp, ((FiberContext*)targetFiber.m_implData)->sp);
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

#else  // Cemu's, on ucontext

Fiber::Fiber(void(*FiberEntryPoint)(void* userParam), void* userParam, void* privateData) : m_privateData(privateData)
{
	ucontext_t* ctx = (ucontext_t*)malloc(sizeof(ucontext_t));
	
	const size_t stackSize = 2 * 1024 * 1024;
	m_stackPtr = malloc(stackSize);

	getcontext(ctx);
	ctx->uc_stack.ss_sp = m_stackPtr;
	ctx->uc_stack.ss_size = stackSize;
	ctx->uc_link = &ctx[0];
#ifdef __arm64__
	// https://www.man7.org/linux/man-pages/man3/makecontext.3.html#NOTES
	makecontext(ctx, (void(*)())FiberEntryPoint, 2, (uint64) userParam >> 32, userParam);
#else
	makecontext(ctx, (void(*)())FiberEntryPoint, 1, userParam);
#endif
	this->m_implData = (void*)ctx;
}

Fiber::Fiber(void* privateData) : m_privateData(privateData)
{
	ucontext_t* ctx = (ucontext_t*)malloc(sizeof(ucontext_t));
	getcontext(ctx);
	this->m_implData = (void*)ctx;
	m_stackPtr = nullptr;
}

Fiber::~Fiber()
{
	if(m_stackPtr)
		free(m_stackPtr);
	free(m_implData);
}

void Fiber::Switch(Fiber& targetFiber)
{
    Fiber* leavingFiber = sCurrentFiber;
    sCurrentFiber = &targetFiber;
	std::atomic_thread_fence(std::memory_order_seq_cst);
	swapcontext((ucontext_t*)(leavingFiber->m_implData), (ucontext_t*)(targetFiber.m_implData));
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

#endif

Fiber* Fiber::PrepareCurrentThread(void* privateData)
{
	cemu_assert_debug(sCurrentFiber == nullptr);
    sCurrentFiber = new Fiber(privateData);
	return sCurrentFiber;
}

void* Fiber::GetFiberPrivateData()
{
	return sCurrentFiber->m_privateData;
}
