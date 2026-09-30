// A sampling profiler for wwhd-null (WWHD_PROFILE=path): where host CPU time goes, to design the
// scheduler's real-time mode on numbers (docs/recompiler-design.md D18).
//
// A CPU-time timer (ITIMER_PROF, WWHD_PROFILE_HZ per CPU-second, default 997) interrupts whichever
// thread is running; the handler records its thread id, the interrupted instruction and, unless
// WWHD_PROFILE_STACKS=0, the return addresses above it (glibc's unwinder; Cemu's code keeps no frame
// pointers). Samples go into a buffer allocated up front; at exit (normal or quick_exit, which the
// trace's exit frame uses) the file gets the program's load address, every thread's name and CPU
// time from /proc, every loaded object's executable segments, and the samples as hex addresses. tools/profile_report.py symbolizes and sums
// them. Nothing here touches guest state: traces are the same with the profiler on.
#include "runtime.h"
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
#include <ucontext.h>
#include <unistd.h>

namespace wwhd::rt
{
	namespace
	{
		constexpr int kDepth = 10;                 // the interrupted instruction and 9 callers
		constexpr uint64_t kCapacity = 1 << 20;    // samples (~96 MB)

		struct Sample
		{
			uint32_t tid;
			uint32_t depth;
			uintptr_t pc[kDepth];
		};

		Sample* s_samples = nullptr;
		std::atomic<uint64_t> s_next{ 0 };
		bool s_stacks = true;
		const char* s_path = nullptr;
		int s_hz = 997;
		std::atomic<bool> s_written{ false };

		void OnSample(int, siginfo_t*, void* context)
		{
			uint64_t i = s_next.fetch_add(1, std::memory_order_relaxed);
			if (i >= kCapacity)
				return;
			Sample& s = s_samples[i];
			s.tid = (uint32_t)syscall(SYS_gettid);
			s.pc[0] = (uintptr_t)((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP];
			s.depth = 1;
			if (s_stacks)
			{
				// [0] this handler, [1] the signal trampoline, [2] the interrupted function, [3..] its callers
				void* frames[kDepth + 2];
				int n = backtrace(frames, kDepth + 2);
				for (int f = 3; f < n && s.depth < (uint32_t)kDepth; f++)
					s.pc[s.depth++] = (uintptr_t)frames[f];
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
			uint64_t n = std::min<uint64_t>(s_next.load(), kCapacity);
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
				const Sample& s = s_samples[i];
				fprintf(f, "s %u", s.tid);
				for (uint32_t k = 0; k < s.depth; k++)
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
		s_samples = (Sample*)calloc(kCapacity, sizeof(Sample));
		if (!s_samples)
			return;
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
