// write_watch_test.cpp: unit tests for src/runtime/write_watch.{h,cpp}, without the game or Cemu.
// Build and run: src/runtime/tests/run.sh. Each check prints "ok" or "FAIL"; the exit status is the FAIL count.
#include "../write_watch.h"
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

namespace ww = wwhd::rt::write_watch;

static int s_fails;
static void Check(bool ok, const char* what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		s_fails++;
}

static long P;                                 // the page size
static char* R;                                // the watched region: kPages pages
constexpr int kPages = 64;
static char* Pg(int i) { return R + i * P; }

// the "previous handler" (Cemu's crash handler's stand-in): records the address and jumps back
static sigjmp_buf s_jmp;
static std::atomic<int> s_oldCalls;
static void* s_oldAddr;
static void OldHandler(int, siginfo_t* info, void*)
{
	s_oldCalls++;
	s_oldAddr = info->si_addr;
	siglongjmp(s_jmp, 1);
}

static bool FaultsToOld(volatile char* at)
{
	int before = s_oldCalls;
	if (sigsetjmp(s_jmp, 1) == 0)
	{
		*at = 1;
		return false;
	}
	return s_oldCalls == before + 1 && s_oldAddr == (void*)at;
}

// a child process: runs f, returns its wait status
template <class F> static int InChild(F f)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0)
	{
		rlimit no{0, 0};
		setrlimit(RLIMIT_CORE, &no);           // no core files from the crash tests
		f();
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return st;
}

// ---- fibers ------------------------------------------------------------------------------------------
// A fiber with a small stack and a guard page below it. It writes to two protected pages near the bottom
// of its stack, the first on one host thread, the second after it moved to another.
constexpr size_t kFiberStack = 16 * 1024;
static uintptr_t s_fiberLo;
static ucontext_t s_fiberCtx, *s_back;
static volatile char* s_fiberTarget;
static pid_t s_fiberTids[2];

static void Deep(volatile char* target, int depth)
{
	volatile char buf[200];
	buf[0] = (char)depth;
	if ((uintptr_t)__builtin_frame_address(0) - s_fiberLo > 1500)
		Deep(target, depth + 1);
	else
		*target = buf[0];                      // < 1.5 KB of stack left: a signal frame wouldn't fit here
	asm volatile("" ::"r"(buf) : "memory");
}

static void FiberMain()
{
	Deep(s_fiberTarget, 0);
	s_fiberTids[0] = gettid();
	swapcontext(&s_fiberCtx, s_back);          // park; resumed on another host thread
	Deep(s_fiberTarget, 0);
	s_fiberTids[1] = gettid();
	swapcontext(&s_fiberCtx, s_back);
}

static void MakeFiber()
{
	char* mem = (char*)mmap(nullptr, kFiberStack + P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mprotect(mem, P, PROT_NONE);
	s_fiberLo = (uintptr_t)mem + P;
	getcontext(&s_fiberCtx);
	s_fiberCtx.uc_stack.ss_sp = mem + P;
	s_fiberCtx.uc_stack.ss_size = kFiberStack;
	s_fiberCtx.uc_link = nullptr;
	makecontext(&s_fiberCtx, FiberMain, 0);
}

static void RunFiberOnce(bool threadInit, volatile char* target)
{
	if (threadInit)
		ww::ThreadInit();
	ucontext_t here;
	s_back = &here;
	s_fiberTarget = target;
	swapcontext(&here, &s_fiberCtx);
}

int main()
{
	P = sysconf(_SC_PAGESIZE);
	setenv("WWHD_WRITE_WATCH", "1", 1);

	// a fault with no handler before ours still stops the process with SIGSEGV (the default action)
	{
		int st = InChild([] {
			char* r = (char*)mmap(nullptr, 4 * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			ww::Init(r, 2 * P);
			mprotect(r + 3 * P, P, PROT_NONE);
			*(volatile char*)(r + 3 * P) = 1;  // outside the region
		});
		Check(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "chain to SIG_DFL: the process dies of SIGSEGV");
	}
	// a crash handler that exits (Cemu's, with crash dumps off, ends in _Exit(1)) still gets the fault
	{
		int st = InChild([] {
			struct sigaction crash{};
			crash.sa_sigaction = [](int, siginfo_t*, void*) { _exit(42); };
			crash.sa_flags = SA_SIGINFO;
			sigemptyset(&crash.sa_mask);
			sigaction(SIGSEGV, &crash, nullptr);
			char* r = (char*)mmap(nullptr, 4 * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			ww::Init(r, 2 * P);
			ww::Protect(r, P);
			r[0] = 1;                                  // ours: handled, not passed on
			mprotect(r + 3 * P, P, PROT_NONE);
			*(volatile char*)(r + 3 * P) = 1;        // not ours
		});
		Check(WIFEXITED(st) && WEXITSTATUS(st) == 42, "chain to an exiting crash handler: its exit status");
	}
	// control: a thread without ThreadInit faulting near the bottom of a fiber's stack can't take the signal
	// there (so the fiber test below really needs the alternate stack)
	{
		int st = InChild([] {
			char* r = (char*)mmap(nullptr, 2 * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			ww::Init(r, 2 * P);
			ww::Protect(r, P);
			MakeFiber();
			std::thread t([&] { RunFiberOnce(false, r); });
			t.join();
			_exit(0);
		});
		if (WIFSIGNALED(st))
			Check(WTERMSIG(st) == SIGSEGV, "control: without a sigaltstack, a fault deep in a fiber kills the process");
		else
			printf("note: control: the signal frame fit in the fiber's last 1.5 KB here; the fiber test proves less\n");
	}

	// the stand-in for Cemu's crash handler, installed before Init as Cemu's is
	struct sigaction old{};
	old.sa_sigaction = OldHandler;
	old.sa_flags = SA_SIGINFO;
	sigemptyset(&old.sa_mask);
	sigaction(SIGSEGV, &old, nullptr);

	char* mem = (char*)mmap(nullptr, (kPages + 2) * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	R = mem + P;                                 // a page on each side, outside the region
	Check(!ww::Active() && !ww::Protect(R, P), "before Init: inactive, Protect refuses");
	Check(ww::Init(R, kPages * P) && ww::Active(), "Init");
	Check(ww::Init(R, kPages * P), "Init again: true, no change");

	// ---- one thread --------------------------------------------------------------------------------
	{
		uint32_t m = ww::Mark();
		Check(ww::Protect(Pg(2), 3 * P), "Protect pages 2-4");
		Check(!ww::WrittenSince(Pg(2), 3 * P, m), "nothing written yet");
		ww::Stats s0 = ww::GetStats();
		Pg(3)[100] = 42;
		ww::Stats s1 = ww::GetStats();
		Check(Pg(3)[100] == 42, "the store went through");
		Check(s1.faults == s0.faults + 1 && s1.chained == s0.chained, "one fault, handled, not chained");
		Check(s1.onAltStack == s0.onAltStack + 1, "the handler ran on the alternate stack");
		Check(ww::WrittenSince(Pg(3), P, m) && ww::WrittenSince(Pg(2), 3 * P, m), "page 3 stamped after the mark");
		Check(!ww::WrittenSince(Pg(2), P, m) && !ww::WrittenSince(Pg(4), P, m), "pages 2 and 4 not");
		Check(ww::WrittenSince(Pg(3) + P - 1, 2, m), "a range straddling pages 3-4");
		Pg(3)[101] = 43;
		Check(ww::GetStats().faults == s1.faults, "a second write to page 3: no fault");
		Pg(2)[0] = 1;
		Pg(4)[P - 1] = 1;
		Check(ww::GetStats().faults == s1.faults + 2, "pages 2 and 4: a fault each");
		uint32_t m2 = ww::Mark();
		Pg(4)[0] = 2;
		Check(!ww::WrittenSince(Pg(2), 3 * P, m2), "unprotected pages: no new stamp (protect again to watch)");
		Check(ww::LastStamp(Pg(2), 3 * P) > m && ww::LastStamp(Pg(10), P) == 0, "LastStamp");
	}
	// Unprotect stamps (a neighbour's watcher must see it) and leaves the page writable
	{
		ww::Protect(Pg(6), P);
		uint32_t m = ww::Mark();
		ww::Unprotect(Pg(6), P);
		Check(ww::WrittenSince(Pg(6), P, m), "Unprotect stamps");
		uint64_t f = ww::GetStats().faults;
		Pg(6)[0] = 1;
		Check(ww::GetStats().faults == f, "Unprotect: writable, no fault");
	}
	// faults the watcher doesn't own reach the old handler
	{
		mprotect(mem, P, PROT_NONE);
		Check(FaultsToOld(mem), "a fault below the region reaches the old handler");
		mprotect(R + kPages * P, P, PROT_NONE);
		Check(FaultsToOld(R + kPages * P), "a fault above the region reaches the old handler");
		mprotect(Pg(20), P, PROT_READ);          // read-only, but not by us
		Check(FaultsToOld(Pg(20) + 5), "a read-only page inside the region we never protected: old handler");
		mprotect(Pg(20), P, PROT_READ | PROT_WRITE);
		Check(FaultsToOld((char*)8), "a null-ish pointer: old handler");
	}
	// a kernel write into a protected page fails; inside a HostWrite scope it works and is stamped
	{
		int fds[2];
		pipe(fds);
		ww::Protect(Pg(8), P);
		write(fds[1], "abcd", 4);
		ssize_t r = read(fds[0], Pg(8), 4);
		Check(r == -1 && errno == EFAULT, "read() into a protected page: EFAULT (why HostWrite exists)");
		uint32_t m = ww::Mark();
		uint32_t mDuring;
		{
			ww::HostWrite hw(Pg(8), 4);
			Check(ww::WrittenSince(Pg(8), P, m), "HostWrite stamps at its start");
			r = read(fds[0], Pg(8), 4);
			Check(r == 4 && memcmp(Pg(8), "abcd", 4) == 0, "read() inside HostWrite");
			mDuring = ww::Mark();                // a watcher marking and protecting during the scope
			ww::Protect(Pg(8), P);
			write(fds[1], "efgh", 4);
			r = read(fds[0], Pg(8), 4);
			Check(r == 4, "Protect skips a page inside a HostWrite scope");
		}
		Check(ww::WrittenSince(Pg(8), P, mDuring), "HostWrite stamps again at its end");
		uint64_t f = ww::GetStats().faults;
		{
			ww::HostWrite a(Pg(9), P), b(Pg(9) + 10, 10);    // nested
		}
		Check(ww::Protect(Pg(9), P), "after nested scopes end, the page can be protected");
		Pg(9)[0] = 1;
		Check(ww::GetStats().faults == f + 1, "and faults again");
		close(fds[0]);
		close(fds[1]);
	}

	// ---- several host threads ----------------------------------------------------------------------
	// Each round the main thread marks and protects pages 16-47; each thread writes its own page and a page
	// shared by all (the race: one thread's fault makes it writable while the others wait for the lock);
	// pages no one wrote must carry no stamp after the mark.
	{
		constexpr int kThreads = 8, kRounds = 300, kFirst = 16, kCount = 32;
		pthread_barrier_t bar;
		pthread_barrier_init(&bar, nullptr, kThreads + 1);
		ww::Stats s0 = ww::GetStats();
		std::vector<std::thread> ts;
		for (int t = 0; t < kThreads; t++)
			ts.emplace_back([&, t] {
				ww::ThreadInit();
				for (int r = 0; r < kRounds; r++)
				{
					pthread_barrier_wait(&bar);              // protected
					int own = kFirst + 1 + ((t + r) % (kCount - 2) / 2) * 2;   // odd pages, may repeat
					Pg(own)[t] = (char)r;
					Pg(kFirst)[t] = (char)r;                 // shared
					pthread_barrier_wait(&bar);              // written
				}
			});
		bool stamped = true, clean = true;
		for (int r = 0; r < kRounds; r++)
		{
			uint32_t m = ww::Mark();
			ww::Protect(Pg(kFirst), kCount * P);
			pthread_barrier_wait(&bar);
			pthread_barrier_wait(&bar);
			stamped &= ww::WrittenSince(Pg(kFirst), P, m);
			for (int t = 0; t < kThreads; t++)
				stamped &= ww::WrittenSince(Pg(kFirst + 1 + ((t + r) % (kCount - 2) / 2) * 2), P, m);
			for (int i = kFirst + 2; i < kFirst + kCount; i += 2)
				clean &= !ww::WrittenSince(Pg(i), P, m);        // even pages: nobody writes them
		}
		for (auto& t : ts)
			t.join();
		ww::Stats s1 = ww::GetStats();
		Check(stamped, "threads: every written page stamped after its round's mark");
		Check(clean, "threads: no page stamped that no one wrote");
		Check(s1.chained == s0.chained, "threads: no fault chained");
		Check(s1.onAltStack - s0.onAltStack == s1.faults - s0.faults, "threads: every fault on an alternate stack");
		printf("      threads: %llu faults, %llu raced\n", (unsigned long long)(s1.faults - s0.faults),
			(unsigned long long)(s1.raced - s0.raced));
		bool values = true;
		for (int t = 0; t < kThreads; t++)
			values &= Pg(kFirst)[t] == (char)(kRounds - 1);
		Check(values, "threads: the stores all landed");
	}

	// ---- the watcher's invariant under load ----------------------------------------------------------
	// Four writers store into pages 52-59 nonstop, a fifth thread unprotects some of them, while a re-protector loops: mark, Protect, copy the bytes; later,
	// compare the bytes with the copy FIRST, then ask WrittenSince. Bytes that differ while WrittenSince says no
	// would be a missed write (a stamp stored after its page opened gives some).
	{
		constexpr int kFirst = 52, kCount = 8;
		std::atomic<bool> stop{false};
		std::vector<std::thread> ws;
		for (int t = 0; t < 4; t++)
			ws.emplace_back([&, t] {
				ww::ThreadInit();
				uint32_t x = 0x9E3779B9u * (t + 1);
				while (!stop.load(std::memory_order_relaxed))
				{
					x ^= x << 13, x ^= x >> 17, x ^= x << 5;
					((volatile char*)Pg(kFirst + x % kCount))[(x >> 8) % P] = (char)x;
					if ((x & 63) == 0)
						std::this_thread::yield();
				}
			});
		ws.emplace_back([&] {                        // and pages opened by Unprotect, which writers then hit
			uint32_t x = 12345;
			while (!stop.load(std::memory_order_relaxed))
			{
				x ^= x << 13, x ^= x >> 17, x ^= x << 5;
				ww::Unprotect(Pg(kFirst + x % kCount), P);
				for (int i = 0; i < 2000; i++)
					asm volatile("" ::: "memory");
			}
		});
		std::vector<char> copy(kCount * P);
		uint64_t rounds = 0, missed = 0, changed = 0;
		auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < until)
		{
			uint32_t m = ww::Mark();
			ww::Protect(Pg(kFirst), kCount * P);
			memcpy(copy.data(), Pg(kFirst), copy.size());
			// each page checked on its own until all were written: a page's first write after the Protect is the
			// moment a late stamp would show, and the checks keep landing near it
			for (int k = 0; k < 400; k++)
			{
				int open = 0;
				for (int i = 0; i < kCount; i++)
				{
					bool differs = memcmp(&copy[i * P], Pg(kFirst + i), P) != 0;
					bool written = ww::WrittenSince(Pg(kFirst + i), P, m);
					missed += differs && !written;
					changed += differs;
					open += written;
				}
				if (open == kCount)
					break;
			}
			rounds++;
		}
		stop = true;
		for (auto& t : ws)
			t.join();
		printf("      invariant: %llu rounds, %llu page checks with changed bytes, %llu missed\n", (unsigned long long)rounds,
			(unsigned long long)changed, (unsigned long long)missed);
		Check(missed == 0 && changed > 0, "invariant: bytes changed after Protect => WrittenSince, 4 writers and an unprotector");
	}

	// ---- what a fault costs ---------------------------------------------------------------------------
	{
		constexpr int kN = 20000;
		uint64_t f0 = ww::GetStats().faults;
		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < kN; i++)
		{
			ww::Protect(Pg(60), P);
			Pg(60)[i % P] = 1;
		}
		auto t1 = std::chrono::steady_clock::now();
		for (int i = 0; i < kN; i++)
		{
			ww::Protect(Pg(61), P);
			ww::Unprotect(Pg(61), P);
		}
		auto t2 = std::chrono::steady_clock::now();
		double both = std::chrono::duration<double, std::micro>(t1 - t0).count() / kN;
		double noFault = std::chrono::duration<double, std::micro>(t2 - t1).count() / kN;
		Check(ww::GetStats().faults - f0 == kN, "cost: one fault per protect-and-write");
		printf("      cost: protect + faulting write %.2f us, protect + unprotect %.2f us: a fault ~%.2f us\n", both,
			noFault, both - noFault / 2);
	}

	// ---- a fiber moving between host threads -------------------------------------------------------
	{
		MakeFiber();
		ww::Stats s0 = ww::GetStats();
		uint32_t m = ww::Mark();
		ww::Protect(Pg(50), 2 * P);
		std::thread a([] { RunFiberOnce(true, Pg(50)); });
		a.join();                                 // thread a is gone (its alternate stack too)
		std::thread b([] { RunFiberOnce(true, Pg(51)); });
		b.join();
		ww::Stats s1 = ww::GetStats();
		Check(s_fiberTids[0] && s_fiberTids[1] && s_fiberTids[0] != s_fiberTids[1], "fiber: ran on two host threads");
		Check(ww::WrittenSince(Pg(50), P, m) && ww::WrittenSince(Pg(51), P, m), "fiber: both pages stamped");
		Check(s1.faults == s0.faults + 2 && s1.onAltStack == s0.onAltStack + 2,
			"fiber: two faults, each on its host thread's alternate stack, near the bottom of the fiber's stack");
	}

	printf("%s: %d failed\n", s_fails ? "FAIL" : "ok", s_fails);
	return s_fails;
}
