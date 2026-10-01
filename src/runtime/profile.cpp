// A sampling profiler for wwhd-null (WWHD_PROFILE=path): where host CPU time goes, to design the
// scheduler's real-time mode on numbers (docs/recompiler-design.md D18).
//
// A CPU-time timer (ITIMER_PROF, WWHD_PROFILE_HZ per CPU-second, default 997) interrupts whichever
// thread is running; the handler records its thread id, the interrupted instruction and, unless
// WWHD_PROFILE_STACKS=0, the return addresses above it (WWHD_PROFILE_DEPTH addresses, default 10, up
// to 64: deeper stacks show guest call chains whole): unwound by glibc when the thread was in our
// program's own code (Cemu's code keeps no frame pointers). Elsewhere (libc, the vDSO, the middle of
// a fiber switch) unwinding can fault, so it takes just the first address of our program's code
// found near the stack pointer, read with process_vm_readv, which cannot fault: the likely caller. Samples go into a buffer allocated up front; at exit (normal or quick_exit, which the
// trace's exit frame uses) the file gets the program's load address, every thread's name and CPU
// time from /proc, every loaded object's executable segments, and the samples as hex addresses. tools/profile_report.py symbolizes and sums
// them. Nothing here touches guest state: traces are the same with the profiler on.
#include "runtime.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cstdint>
#include <dirent.h>
#include <execinfo.h>
#include <link.h>
#include <string>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__x86_64__) && !defined(_WIN32)
// src/runtime/fiber/FiberUnix.cpp; weak: without our forks (WWHD_FORKS=OFF) both are 0, and no pc is in between
extern "C" __attribute__((weak)) char wwhd_fiber_switch[], wwhd_fiber_switch_end[];
#endif

namespace wwhd::rt
{
	namespace
	{
		constexpr int kMaxDepth = 64;
		constexpr uint64_t kBufferWords = 12 << 20;   // ~96 MB: 1M samples of depth 10

		// a sample is a record of 2 + s_depth words: thread id, depth, the interrupted instruction and
		// its callers
		struct Sample
		{
			uintptr_t tid;
			uintptr_t depth;
			uintptr_t pc[];                        // s_depth of them
		};

		int s_depth = 10;                          // the interrupted instruction and 9 callers
		uint64_t s_capacity = 0;                   // samples
		uintptr_t* s_buffer = nullptr;
		Sample& At(uint64_t i) { return *(Sample*)(s_buffer + i * (2 + s_depth)); }
		std::atomic<uint64_t> s_next{ 0 };
		bool s_stacks = true;
		const char* s_path = nullptr;
		int s_hz = 997;
		std::atomic<bool> s_written{ false };
		uintptr_t s_textLo = 0, s_textHi = 0;      // our program's executable segment(s)

		bool InText(uintptr_t a) { return a >= s_textLo && a < s_textHi; }

		void OnSample(int, siginfo_t*, void* context)
		{
			uint64_t i = s_next.fetch_add(1, std::memory_order_relaxed);
			if (i >= s_capacity)
				return;
			Sample& s = At(i);
			const greg_t* regs = ((ucontext_t*)context)->uc_mcontext.gregs;
			s.tid = (uintptr_t)syscall(SYS_gettid);
			s.pc[0] = (uintptr_t)regs[REG_RIP];
			s.depth = 1;
			if (!s_stacks)
				return;
#if defined(__x86_64__) && !defined(_WIN32)
			// in the middle of a fiber switch the stack pointer belongs to either fiber: no unwinding
			const bool switching = s.pc[0] >= (uintptr_t)wwhd_fiber_switch && s.pc[0] < (uintptr_t)wwhd_fiber_switch_end;
#else
			const bool switching = false;
#endif
			if (InText(s.pc[0]) && !switching)
			{
				// [0] this handler, [1] the signal trampoline, [2] the interrupted function, [3..] its callers
				void* frames[kMaxDepth + 2];
				int n = backtrace(frames, s_depth + 2);
				for (int f = 3; f < n && s.depth < (uintptr_t)s_depth; f++)
					s.pc[s.depth++] = (uintptr_t)frames[f];
				return;
			}
			uintptr_t words[16];
			iovec local{ words, sizeof(words) }, remote{ (void*)regs[REG_RSP], sizeof(words) };
			if (s_depth > 1 && process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)sizeof(words))
				for (uintptr_t w : words)
					if (InText(w))
					{
						s.pc[s.depth++] = w;
						break;
					}
		}

		uintptr_t LoadAddress()
		{
			uintptr_t base = 0;
			dl_iterate_phdr([](dl_phdr_info* info, size_t, void* out) {
				*(uintptr_t*)out = info->dlpi_addr;    // the first object is the program itself
				return 1;
			}, &base);
			return base;
		}

		void Write()
		{
			if (s_written.exchange(true))
				return;
			itimerval off{};
			setitimer(ITIMER_PROF, &off, nullptr);
			FILE* f = fopen(s_path, "w");
			if (!f)
				return;
			uint64_t n = std::min<uint64_t>(s_next.load(), s_capacity);
			char exe[4096] = {};
			readlink("/proc/self/exe", exe, sizeof(exe) - 1);
			fprintf(f, "wwhd-profile 1\nexe %s\nbase %lx\nhz %d\nsamples %lu\n", exe, (unsigned long)LoadAddress(), s_hz,
				(unsigned long)n);
			// every thread: id, CPU time in clock ticks (user, system), name
			if (DIR* d = opendir("/proc/self/task"))
			{
				while (dirent* e = readdir(d))
				{
					if (e->d_name[0] == '.')
						continue;
					std::string stat = std::string("/proc/self/task/") + e->d_name + "/stat";
					if (FILE* sf = fopen(stat.c_str(), "r"))
					{
						char buf[1024] = {};
						size_t len = fread(buf, 1, sizeof(buf) - 1, sf);
						fclose(sf);
						buf[len] = 0;
						char* open = strchr(buf, '('), * close = strrchr(buf, ')');
						if (!open || !close)
							continue;
						std::string name(open + 1, close);
						unsigned long utime = 0, stime = 0;
						// after ") ": state and 12 more fields, then utime and stime (fields 14 and 15)
						sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime);
						fprintf(f, "thread %s %lu %lu %s\n", e->d_name, utime, stime, name.c_str());
					}
				}
				closedir(d);
			}
			fprintf(f, "ticks_per_second %ld\n", sysconf(_SC_CLK_TCK));
			// every loaded object's executable segments: start, end, load address, file
			dl_iterate_phdr([](dl_phdr_info* info, size_t, void* out) {
				const char* name = info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "exe";
				for (int i = 0; i < info->dlpi_phnum; i++)
				{
					const auto& ph = info->dlpi_phdr[i];
					if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X))
						fprintf((FILE*)out, "segment %lx %lx %lx %s\n", (unsigned long)(info->dlpi_addr + ph.p_vaddr),
							(unsigned long)(info->dlpi_addr + ph.p_vaddr + ph.p_memsz), (unsigned long)info->dlpi_addr, name);
				}
				return 0;
			}, f);
			for (uint64_t i = 0; i < n; i++)
			{
				const Sample& s = At(i);
				fprintf(f, "s %u", (unsigned)s.tid);
				for (uintptr_t k = 0; k < s.depth; k++)
					fprintf(f, " %lx", (unsigned long)s.pc[k]);
				fputc('\n', f);
			}
			fclose(f);
		}
	}

	void StartProfiler()
	{
		s_path = getenv("WWHD_PROFILE");
		if (!s_path || !*s_path)
			return;
		if (const char* hz = getenv("WWHD_PROFILE_HZ"))
			s_hz = std::max(1, atoi(hz));
		if (const char* stacks = getenv("WWHD_PROFILE_STACKS"))
			s_stacks = strcmp(stacks, "0") != 0;
		if (const char* depth = getenv("WWHD_PROFILE_DEPTH"))
			s_depth = std::clamp(atoi(depth), 1, kMaxDepth);
		s_capacity = kBufferWords / (2 + s_depth);
		s_buffer = (uintptr_t*)calloc(kBufferWords, sizeof(uintptr_t));
		if (!s_buffer)
			return;
		dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) {
			for (int i = 0; i < info->dlpi_phnum; i++)
			{
				const auto& ph = info->dlpi_phdr[i];
				if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X))
				{
					uintptr_t lo = info->dlpi_addr + ph.p_vaddr, hi = lo + ph.p_memsz;
					s_textLo = s_textLo ? std::min(s_textLo, lo) : lo;
					s_textHi = std::max(s_textHi, hi);
				}
			}
			return 1;                              // the first object is the program itself
		}, nullptr);
		void* warm[4];
		backtrace(warm, 4);                        // loads the unwinder now, not inside the handler
		struct sigaction sa{};
		sa.sa_sigaction = OnSample;
		sa.sa_flags = SA_SIGINFO | SA_RESTART;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGPROF, &sa, nullptr);
		atexit(Write);
		at_quick_exit(Write);
		itimerval t{};
		t.it_interval.tv_usec = t.it_value.tv_usec = 1000000 / s_hz;
		setitimer(ITIMER_PROF, &t, nullptr);
	}
}
