// write_watch.cpp: page write-protection with stamps (write_watch.h; docs/research/texture-tracking.md).
//
// Each page of the region has a stamp (the sequence number of the last time it became writable or was written by
// a HostWrite), a state (untouched by us, protected by us, made writable again by us) and a pin count (HostWrite
// scopes on it). The state and pins change only under one spinlock, and the mprotect that goes with a change is
// made inside it, so "state == protected" always means the page is read-only. The handler takes that lock too:
// a thread can't fault while it holds it (nothing under it touches guest memory), so it can't deadlock on itself.
// Faults are rare (the rival saw about 2 a frame), so one lock is enough.
#include "write_watch.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

namespace wwhd::rt::write_watch
{
	namespace
	{
		enum : uint8_t { kUntouched = 0, kProtected = 1, kWritable = 2 };

		struct Page
		{
			std::atomic<uint32_t> stamp;
			uint8_t state;
			uint8_t pad;
			std::atomic<uint16_t> pins;              // changed under the lock; read without it by WrittenSince
		};
		static_assert(sizeof(Page) == 8);

		constexpr size_t kAltStackSize = 256 * 1024;
		constexpr uint32_t kRaceRetries = 1000;      // a writable page faulting this often from one thread isn't a race

		// set by Init before the handler goes in, then read-only
		uintptr_t s_base, s_size;                    // s_size 0: inactive (every fault chains)
		unsigned s_shift;                            // log2 of the page size
		Page* s_pages;
		struct sigaction s_oldSegv, s_oldBus;

		std::atomic<bool> s_active{false};
		std::atomic<uint32_t> s_seq{0};
		std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
		std::atomic<uint64_t> s_faults, s_raced, s_onAlt, s_chained, s_hostWrites, s_protects;

		// read in the handler: initial-exec, so no lazy TLS allocation can happen in it (were this ever in a .so)
#define WW_TLS thread_local __attribute__((tls_model("initial-exec")))
		WW_TLS uintptr_t t_altLo, t_altHi;           // this thread's ThreadInit stack
		WW_TLS uintptr_t t_raceAddr;
		WW_TLS uint32_t t_raceCount;

		void Lock()
		{
			while (s_lock.test_and_set(std::memory_order_acquire))
			{
#if defined(__x86_64__) || defined(__i386__)
				__builtin_ia32_pause();
#elif defined(__aarch64__)
				asm volatile("yield");
#endif
			}
		}
		void Unlock() { s_lock.clear(std::memory_order_release); }

		uint32_t NextStamp() { return s_seq.fetch_add(1, std::memory_order_acq_rel) + 1; }

		// the pages [first, last] of the region covering [p, p + n), clipped; false if none
		bool Pages(const void* p, size_t n, size_t& first, size_t& last)
		{
			if (!s_active.load(std::memory_order_acquire) || n == 0)
				return false;
			uintptr_t lo = (uintptr_t)p, hi = lo + n;     // [lo, hi)
			if (hi < lo || hi <= s_base || lo >= s_base + s_size)
				return false;
			lo = lo < s_base ? s_base : lo;
			hi = hi > s_base + s_size ? s_base + s_size : hi;
			first = (lo - s_base) >> s_shift;
			last = (hi - 1 - s_base) >> s_shift;
			return true;
		}

		bool ProtectRun(size_t first, size_t count, int prot)
		{
			void* at = (void*)(s_base + (first << s_shift));
			return mprotect(at, count << s_shift, prot) == 0;
		}

		void Say(const char* msg)
		{
			ssize_t r = write(2, msg, strlen(msg));
			(void)r;
		}

		void Chain(int sig, siginfo_t* info, void* uctx)
		{
			s_chained.fetch_add(1, std::memory_order_relaxed);
			const struct sigaction& old = sig == SIGBUS ? s_oldBus : s_oldSegv;
			if (old.sa_flags & SA_SIGINFO)
			{
				if (old.sa_sigaction)
				{
					old.sa_sigaction(sig, info, uctx);
					return;
				}
			}
			else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN)
			{
				old.sa_handler(sig);
				return;
			}
			// no handler before ours: the default action. Put it back and return: the access faults again
			// and the process stops as it would have without us (a core dump).
			struct sigaction dfl{};
			dfl.sa_handler = SIG_DFL;
			sigemptyset(&dfl.sa_mask);
			sigaction(sig, &dfl, nullptr);
		}

		void OnFault(int sig, siginfo_t* info, void* uctx)
		{
			int savedErrno = errno;
			uintptr_t a = (uintptr_t)info->si_addr;
			if (a - s_base < s_size)                     // s_size 0 while inactive
			{
				size_t pg = (a - s_base) >> s_shift;
				Page& page = s_pages[pg];
				Lock();
				uint8_t state = page.state;
				if (state == kProtected)
				{
					page.stamp.store(NextStamp(), std::memory_order_release);
					bool ok = ProtectRun(pg, 1, PROT_READ | PROT_WRITE);
					if (ok)
						page.state = kWritable;
					Unlock();
					if (ok)
					{
						s_faults.fetch_add(1, std::memory_order_relaxed);
						uintptr_t sp = (uintptr_t)&pg;
						if (sp - t_altLo < t_altHi - t_altLo)
							s_onAlt.fetch_add(1, std::memory_order_relaxed);
						t_raceAddr = 0;
						errno = savedErrno;
						return;                          // the store re-executes on a writable page
					}
					Say("wwhd: write watch: mprotect failed in the fault handler\n");
				}
				else
				{
					Unlock();
					// made writable by another thread between this fault and the lock: retry the store.
					// The same address faulting again and again means it isn't writable after all (Cemu
					// decommitted it, say): that one goes on to the old handler.
					if (state == kWritable)
					{
						if (a != t_raceAddr)
						{
							t_raceAddr = a;
							t_raceCount = 0;
						}
						if (++t_raceCount < kRaceRetries)
						{
							s_raced.fetch_add(1, std::memory_order_relaxed);
							errno = savedErrno;
							return;
						}
					}
				}
			}
			errno = savedErrno;
			Chain(sig, info, uctx);
		}

		struct AltStack
		{
			void* mem = nullptr;
			stack_t old{};
			~AltStack()
			{
				if (!mem)
					return;
				stack_t cur{};
				sigaltstack(nullptr, &cur);
				if (cur.ss_sp == (char*)mem + getpagesize())     // still ours: put back what was there
				{
					stack_t restore = old;
					if (!restore.ss_sp || restore.ss_size == 0)
					{
						restore = stack_t{};
						restore.ss_flags = SS_DISABLE;
					}
					sigaltstack(&restore, nullptr);
				}
				t_altLo = t_altHi = 0;
				munmap(mem, kAltStackSize + getpagesize());
			}
		};
		thread_local AltStack t_alt;
	}

	bool Enabled()
	{
		static const bool on = [] {
			const char* e = getenv("WWHD_WRITE_WATCH");
			return e && strcmp(e, "1") == 0;
		}();
		return on;
	}

	bool Active() { return s_active.load(std::memory_order_acquire); }

	void ThreadInit()
	{
		if (!Enabled() || t_alt.mem)
			return;
		stack_t cur{};
		if (sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_size >= kAltStackSize)
		{
			t_altLo = (uintptr_t)cur.ss_sp;              // someone's already big enough: keep it
			t_altHi = t_altLo + cur.ss_size;
			return;
		}
		size_t guard = getpagesize();                    // a guard page below: an overflow faults, not corrupts
		void* mem = mmap(nullptr, kAltStackSize + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mem == MAP_FAILED)
			return;
		mprotect(mem, guard, PROT_NONE);
		stack_t ss{};
		ss.ss_sp = (char*)mem + guard;
		ss.ss_size = kAltStackSize;
		if (sigaltstack(&ss, &t_alt.old) != 0)
		{
			munmap(mem, kAltStackSize + guard);
			return;
		}
		if (t_alt.old.ss_flags & SS_DISABLE)
			t_alt.old = stack_t{};
		t_alt.mem = mem;
		t_altLo = (uintptr_t)ss.ss_sp;
		t_altHi = t_altLo + kAltStackSize;
	}

	bool Init(void* base, size_t size)
	{
		static std::mutex m;
		std::lock_guard<std::mutex> g(m);
		if (s_active.load())
			return true;
		if (!Enabled() || !base || size == 0)
			return false;
		long ps = sysconf(_SC_PAGESIZE);
		unsigned shift = 0;
		while ((1L << shift) < ps)
			shift++;
		if ((uintptr_t)base & (ps - 1))
		{
			fprintf(stderr, "wwhd: write watch: the region isn't page-aligned, off\n");
			return false;
		}
		size_t pages = (size + ps - 1) >> shift;
		// zero-filled on first touch: 8 bytes a page, 8 MB for 4 GB of 4 KB pages, mostly never touched
		void* t = mmap(nullptr, pages * sizeof(Page), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (t == MAP_FAILED)
		{
			fprintf(stderr, "wwhd: write watch: no memory for %zu pages' table, off\n", pages);
			return false;
		}
		s_pages = (Page*)t;
		s_base = (uintptr_t)base;
		s_shift = shift;
		s_size = pages << shift;

		ThreadInit();
		struct sigaction sa{};
		sa.sa_sigaction = OnFault;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
		sigemptyset(&sa.sa_mask);
		// the previous actions first, so the handler never sees them unset
		sigaction(SIGSEGV, nullptr, &s_oldSegv);
		sigaction(SIGBUS, nullptr, &s_oldBus);
		s_active.store(true, std::memory_order_release);
		sigaction(SIGSEGV, &sa, nullptr);
#if defined(__APPLE__)
		sigaction(SIGBUS, &sa, nullptr);                 // macOS reports a write to a read-only page as SIGBUS
#endif
		fprintf(stderr, "wwhd: write watch on: %zu pages of %ld bytes from %p\n", pages, ps, base);
		return true;
	}

	uint32_t Mark() { return s_seq.load(std::memory_order_acquire); }

	bool Protect(const void* p, size_t n)
	{
		size_t first, last;
		if (!Pages(p, n, first, last))
			return false;
		bool ok = true;
		Lock();
		for (size_t i = first; i <= last;)
		{
			if (s_pages[i].state == kProtected || s_pages[i].pins.load(std::memory_order_relaxed))
			{
				i++;
				continue;
			}
			size_t j = i;
			while (j <= last && s_pages[j].state != kProtected && !s_pages[j].pins.load(std::memory_order_relaxed))
				j++;
			if (ProtectRun(i, j - i, PROT_READ))
			{
				for (size_t k = i; k < j; k++)
					s_pages[k].state = kProtected;
				s_protects.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				// ENOMEM: too many mappings (vm.max_map_count). The pages stay writable, so they must look
				// changed after any mark taken before this, never unchanged
				ok = false;
				uint32_t stamp = NextStamp();
				for (size_t k = i; k < j; k++)
					s_pages[k].stamp.store(stamp, std::memory_order_release);
			}
			i = j;
		}
		Unlock();
		return ok;
	}

	namespace
	{
		// under the lock: make the protected pages of [first, last] writable, stamped
		void Release(size_t first, size_t last, uint32_t stamp)
		{
			for (size_t i = first; i <= last;)
			{
				if (s_pages[i].state != kProtected)
				{
					i++;
					continue;
				}
				size_t j = i;
				while (j <= last && s_pages[j].state == kProtected)
					j++;
				// the stamp before the page opens: a write landing right after the mprotect must find it already
				// stamped, or a watcher checking between the two would take changed bytes for unchanged
				for (size_t k = i; k < j; k++)
					s_pages[k].stamp.store(stamp, std::memory_order_release);
				if (ProtectRun(i, j - i, PROT_READ | PROT_WRITE))
					for (size_t k = i; k < j; k++)
						s_pages[k].state = kWritable;
				i = j;
			}
		}
	}

	void Unprotect(const void* p, size_t n)
	{
		size_t first, last;
		if (!Pages(p, n, first, last))
			return;
		Lock();
		Release(first, last, NextStamp());
		Unlock();
	}

	bool WrittenSince(const void* p, size_t n, uint32_t m)
	{
		size_t first, last;
		if (!Pages(p, n, first, last))
			return true;
		for (size_t i = first; i <= last; i++)
			if (s_pages[i].stamp.load(std::memory_order_acquire) > m || s_pages[i].pins.load(std::memory_order_acquire))
				return true;                             // pinned: a HostWrite is writing it now
		return false;
	}

	uint32_t LastStamp(const void* p, size_t n)
	{
		size_t first, last;
		uint32_t best = 0;
		if (Pages(p, n, first, last))
			for (size_t i = first; i <= last; i++)
			{
				uint32_t s = s_pages[i].stamp.load(std::memory_order_acquire);
				best = s > best ? s : best;
			}
		return best;
	}

	HostWrite::HostWrite(void* p, size_t n) : m_p(p), m_n(0)
	{
		size_t first, last;
		if (!Pages(p, n, first, last))
			return;
		m_n = n;
		s_hostWrites.fetch_add(1, std::memory_order_relaxed);
		Lock();
		uint32_t stamp = NextStamp();
		for (size_t i = first; i <= last; i++)
		{
			s_pages[i].pins.fetch_add(1, std::memory_order_acq_rel);
			s_pages[i].stamp.store(stamp, std::memory_order_release);
		}
		Release(first, last, stamp);
		Unlock();
	}

	HostWrite::~HostWrite()
	{
		size_t first, last;
		if (!m_n || !Pages(m_p, m_n, first, last))
			return;
		Lock();
		uint32_t stamp = NextStamp();                    // after the write: a watcher that marked during it re-hashes
		for (size_t i = first; i <= last; i++)
		{
			s_pages[i].stamp.store(stamp, std::memory_order_release);
			s_pages[i].pins.fetch_sub(1, std::memory_order_acq_rel);
		}
		Unlock();
	}

	Stats GetStats()
	{
		return Stats{s_faults.load(), s_raced.load(), s_onAlt.load(), s_chained.load(), s_hostWrites.load(),
			s_protects.load()};
	}
}
