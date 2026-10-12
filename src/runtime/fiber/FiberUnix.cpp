// FiberUnix.cpp (docs/recompiler-design.md D18): fibers: each guest thread's stack, and switching
// between them. Cemu's src/util/Fiber/FiberUnix.cpp (Mozilla Public License 2.0), forked: wwhd-null
// links this in place of Cemu's object, under Cemu's names (so Cemu's code that calls it reaches
// ours). Route traces, GPU commands and sound must stay as Cemu's
// (tools/reference/stream_check.sh).
#include "Fiber.h"
#include <atomic>
#include <cstring>

thread_local Fiber* sCurrentFiber{};

#if (defined(__x86_64__) || (defined(__aarch64__) && !defined(__APPLE__))) && !defined(_WIN32)
// wwhd: our own context switch (docs/recompiler-design.md D19) in place of swapcontext, which saves
// and restores the signal mask with a system call on every switch (2.9% of the CPU thread) and is
// deprecated on macOS. It keeps what the System V ABI says a call preserves: rbx, rbp, r12-r15, the
// MXCSR control bits and the x87 control word; the stack pointer is the fiber's context. On arm64
// (Linux and Android, whose libc has no ucontext) what AAPCS64 says a call preserves: x19-x28, the
// frame pointer and link register, d8-d15, and FPCR (flush to zero and rounding).
struct FiberContext
{
	void* sp = nullptr;          // where wwhd_fiber_switch left this fiber's registers
};

extern "C" void wwhd_fiber_switch(void** save, void* load);   // save: the leaving fiber's sp slot
extern "C" void wwhd_fiber_start();                          // a new fiber's first "return" lands here

#if defined(__x86_64__)
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
#else
asm(R"(
	.text
	.p2align 4
	.globl wwhd_fiber_switch
	.type wwhd_fiber_switch, %function
wwhd_fiber_switch:
	.cfi_startproc
	sub sp, sp, #176
	stp x19, x20, [sp, #0]
	stp x21, x22, [sp, #16]
	stp x23, x24, [sp, #32]
	stp x25, x26, [sp, #48]
	stp x27, x28, [sp, #64]
	stp x29, x30, [sp, #80]
	stp d8, d9, [sp, #96]
	stp d10, d11, [sp, #112]
	stp d12, d13, [sp, #128]
	stp d14, d15, [sp, #144]
	mrs x9, fpcr
	str x9, [sp, #160]
	mov x9, sp
	str x9, [x0]
	mov sp, x1
	ldr x9, [sp, #160]
	msr fpcr, x9
	ldp d14, d15, [sp, #144]
	ldp d12, d13, [sp, #128]
	ldp d10, d11, [sp, #112]
	ldp d8, d9, [sp, #96]
	ldp x29, x30, [sp, #80]
	ldp x27, x28, [sp, #64]
	ldp x25, x26, [sp, #48]
	ldp x23, x24, [sp, #32]
	ldp x21, x22, [sp, #16]
	ldp x19, x20, [sp, #0]
	add sp, sp, #176
	ret
	.cfi_endproc
	.size wwhd_fiber_switch, .-wwhd_fiber_switch
	.globl wwhd_fiber_switch_end
wwhd_fiber_switch_end:

	.p2align 4
	.globl wwhd_fiber_start
	.type wwhd_fiber_start, %function
wwhd_fiber_start:
	.cfi_startproc
	.cfi_undefined x30
	mov x0, x20
	mov x29, #0
	blr x19
	brk #0
	.cfi_endproc
	.size wwhd_fiber_start, .-wwhd_fiber_start
)");
#endif

Fiber::Fiber(void(*FiberEntryPoint)(void* userParam), void* userParam, void* privateData) : m_privateData(privateData)
{
	const size_t stackSize = 2 * 1024 * 1024;
	m_stackPtr = malloc(stackSize);
	// the frame wwhd_fiber_switch pops the first time it switches here, so that it "returns" into
	// wwhd_fiber_start with the entry point in r12 and its parameter in r13 (arm64: x19 and x20);
	// floating point starts in this thread's modes
	uintptr_t top = ((uintptr_t)m_stackPtr + stackSize) & ~(uintptr_t)15;
#if defined(__x86_64__)
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
#else
	uint64* frame = (uint64*)(top - 176);
	memset(frame, 0, 176);
	frame[0] = (uint64)FiberEntryPoint;             // x19
	frame[1] = (uint64)userParam;                   // x20
	frame[11] = (uint64)&wwhd_fiber_start;          // x30, where ret goes
	uint64 fpcr;
	asm volatile("mrs %0, fpcr" : "=r"(fpcr));
	frame[20] = fpcr;
#endif
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
#include <ucontext.h>

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
