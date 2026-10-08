// write_watch.h: which guest pages were written since a mark, by page write-protection (deck-plan.md item 6,
// docs/research/texture-tracking.md; the rival's runtime/src/write_watch.{h,cpp}, rival-study.md §4.3).
//
// A watcher (the render thread's texture cache) takes a mark, protects a range read-only and hashes it. The first
// write to a protected page, from any thread (the recompiled code, Cemu's HLE memcpy, the GX2 thread), faults once:
// the handler stamps the page with a new sequence number and makes it writable again, and the store re-executes.
// Later, "was any page of the range stamped after my mark?" is an array lookup per page; a whole hash only when yes.
// Kernel writes into guest memory (read(), fread()) don't fault, they fail with EFAULT: such a write goes inside a
// HostWrite scope, which unprotects and stamps first and stamps again at its end.
//
// Real time only and off by default: Init does nothing unless WWHD_WRITE_WATCH=1. The checks build (the virtual
// clock) hashes every texture whole and never calls it, so it can't change a trace or a capture.
//
// The SIGSEGV handler (SIGBUS too on macOS) is installed by Init after Cemu's crash handler, so it runs first; a
// fault it doesn't own (outside the watched region, or on a page it never protected) goes on to the handler that
// was there before, so crashes still dump. Each host thread that runs guest code calls ThreadInit once: a
// sigaltstack, so the handler doesn't run on a fiber's stack. Guest threads are fibers and can move between host
// threads; the handler keeps no per-fiber state and each host thread has its own alternate stack, so a fiber may
// fault on any of them.
#pragma once
#include <cstddef>
#include <cstdint>

namespace wwhd::rt::write_watch
{
	// WWHD_WRITE_WATCH=1 (read once)
	bool Enabled();

	// Watch [base, base + size) (guest memory: memory_base and its 4 GB reservation). Installs the handler and
	// calls ThreadInit for the calling thread. Call after Cemu's ExceptionHandler_Init. Returns false (and does
	// nothing) when disabled or when the tables can't be allocated; once it returned true, later calls return
	// true and change nothing.
	bool Init(void* base, size_t size);
	bool Active();

	// This host thread's alternate signal stack (256 KB: room for Cemu's crash handler when a fault is chained to
	// it). Idempotent, nothing when disabled; freed when the thread exits. Call at the start of every host thread
	// that runs guest code.
	void ThreadInit();

	// A sequence number: every stamp after it is greater.
	uint32_t Mark();

	// Make the pages covering [p, p + n) read-only (page-rounded: a neighbour on the same page faults too, one
	// stamp). Pages inside a HostWrite scope stay writable (they get the scope's end stamp instead). Order for a
	// watcher: m = Mark(); if (Protect()) hash; later WrittenSince(m): a write racing the hash faults and is seen.
	// False: not active, outside the region, or mprotect failed (vm.max_map_count): hash whole as without it.
	// The pages must be committed read-write guest memory (the handler and Unprotect make them read-write).
	bool Protect(const void* p, size_t n);
	// Writable again. Every way a protected page becomes writable (a fault, this, HostWrite) stamps it, so a
	// watcher of a neighbouring range on the same page re-hashes once rather than missing a later write.
	void Unprotect(const void* p, size_t n);

	// Was any page of [p, p + n) stamped after mark m? Ranges outside the region: always true (no knowledge).
	bool WrittenSince(const void* p, size_t n, uint32_t m);
	// The newest stamp on [p, p + n) (0: never stamped).
	uint32_t LastStamp(const void* p, size_t n);

	// A host-side write into guest memory that can't fault (a kernel read into it): the range's pages are
	// unprotected and stamped now, kept writable while the scope lives, and stamped again when it ends. Nests,
	// and works from any thread. Without Init (or outside the region) it does nothing.
	class HostWrite
	{
	public:
		HostWrite(void* p, size_t n);
		~HostWrite();
		HostWrite(const HostWrite&) = delete;
		HostWrite& operator=(const HostWrite&) = delete;
	private:
		void* m_p;
		size_t m_n;
	};

	struct Stats
	{
		uint64_t faults;           // writes to a protected page, handled
		uint64_t raced;            // faults on a page another thread had just made writable (retried)
		uint64_t onAltStack;       // handled faults that ran on a ThreadInit stack
		uint64_t chained;          // faults passed on to the previous handler
		uint64_t hostWrites;       // HostWrite scopes
		uint64_t protects;         // mprotect calls made read-only
	};
	Stats GetStats();
}
