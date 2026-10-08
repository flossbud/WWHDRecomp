// gpu_timing.cpp: WWHD_GPU_TIMING=1, where the GPU's time goes (docs/research/gpu-plan.md item 1). A timestamp is
// written (bottom of pipe: when everything recorded before it has finished) wherever the command buffer turns to
// another kind of work: a render pass begins or ends, a texture uploads, a surface is copied, grown or cleared, the
// scan buffer is copied, the window's image is drawn. Each span between two timestamps is the work of the first one's
// kind (and, for passes, its targets: "1280x720 c1 f37 d" is a pass into one colour target of VkFormat 37 and a
// depth target), so the spans add up to the command buffer's GPU time; "other" is the barriers and the gaps between
// the marked work. With the renderer's barriers waiting for everything (Transition), the GPU runs the spans one after
// another, so each is that work's own time. The results of a command buffer are read when its slot is recorded again
// (its fence waited for). Every WWHD_GPU_TIMING_EVERY (600) frames a line of milliseconds a frame by kind, every sixth
// line the passes that took the most, and at exit both over the whole run. Off: one predictable branch a mark.
#include "renderer_internal.h"
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace wwhd::gpu::timing
{
	namespace
	{
		constexpr uint32 kQueries = 8192;                        // per command buffer; marks past it are dropped
		constexpr const char* kKindNames[] = { "other", "pass", "upload", "copy", "mips", "grow", "reset", "clear", "scan",
			"present" };
		static_assert(std::size(kKindNames) == (size_t)Kind::Count);

		struct Rec { Kind kind; uint32 label; };
		struct Slot
		{
			VkQueryPool pool = VK_NULL_HANDLE;
			std::vector<Rec> recs;
		};
		Slot s_slots[2];
		bool s_on = false;
		double s_period = 1.0;                                    // nanoseconds a tick
		uint64 s_mask = ~0ull;                                    // the queue's valid timestamp bits
		uint32 s_every = 600;

		std::vector<std::string> s_labels{ "" };
		std::unordered_map<std::string, uint32> s_labelIds{ { "", 0 } };
		struct Sum { double ns = 0; uint64 count = 0; };
		std::vector<Sum> s_labelLine{ Sum{} }, s_labelRun{ Sum{} };   // per label: since the last top line, the run
		double s_kindLine[(size_t)Kind::Count]{}, s_kindRun[(size_t)Kind::Count]{};
		uint32 s_framesLine = 0, s_framesTop = 0, s_framesRun = 0, s_lines = 0;
		uint64 s_dropped = 0, s_unread = 0;
		std::vector<uint64> s_results;
		std::mutex s_mutex;                                       // the exit's summary runs on another thread

		uint32 LabelId(std::string_view label)
		{
			if (label.empty())
				return 0;
			std::lock_guard lock(s_mutex);
			auto it = s_labelIds.find(std::string(label));
			if (it != s_labelIds.end())
				return it->second;
			const uint32 id = (uint32)s_labels.size();
			s_labels.emplace_back(label);
			s_labelIds.emplace(s_labels.back(), id);
			s_labelLine.emplace_back();
			s_labelRun.emplace_back();
			return id;
		}

		// the slot's last recording, its fence already waited for
		void Collect(Slot& slot)
		{
			const uint32 n = (uint32)slot.recs.size();
			if (n < 2)
				return;
			std::lock_guard lock(s_mutex);
			s_results.resize(n);
			if (vkGetQueryPoolResults(s.device, slot.pool, 0, n, n * sizeof(uint64), s_results.data(), sizeof(uint64),
				VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
			{
				s_unread++;
				return;
			}
			for (uint32 i = 0; i + 1 < n; i++)
			{
				const double ns = (double)((s_results[i + 1] - s_results[i]) & s_mask) * s_period;
				const Rec& r = slot.recs[i];
				s_kindLine[(size_t)r.kind] += ns;
				s_kindRun[(size_t)r.kind] += ns;
				if (r.label)
				{
					s_labelLine[r.label].ns += ns, s_labelLine[r.label].count++;
					s_labelRun[r.label].ns += ns, s_labelRun[r.label].count++;
				}
			}
		}

		std::string KindsLine(const double* kinds, uint32 frames)
		{
			double total = 0;
			for (size_t k = 0; k < (size_t)Kind::Count; k++)
				total += kinds[k];
			std::string line = fmt::format("{:.2f} ms a frame:", total / 1e6 / std::max(frames, 1u));
			for (size_t k = 0; k < (size_t)Kind::Count; k++)
				if (kinds[k] > 0)
					line += fmt::format(" {} {:.2f}", kKindNames[k], kinds[k] / 1e6 / std::max(frames, 1u));
			return line;
		}

		std::string TopLine(const std::vector<Sum>& sums, uint32 frames, size_t top)
		{
			std::vector<uint32> ids;
			for (uint32 i = 1; i < (uint32)sums.size(); i++)
				if (sums[i].count)
					ids.push_back(i);
			std::sort(ids.begin(), ids.end(), [&](uint32 a, uint32 b) { return sums[a].ns > sums[b].ns; });
			std::string line;
			for (size_t i = 0; i < std::min(top, ids.size()); i++)
				line += fmt::format("{}{} {:.2f} ms ({:.1f}x)", i ? ", " : "", s_labels[ids[i]],
					sums[ids[i]].ns / 1e6 / std::max(frames, 1u), (double)sums[ids[i]].count / std::max(frames, 1u));
			return line;
		}

		void Summary()
		{
			if (!s_on || !s_framesRun)
				return;
			std::lock_guard lock(s_mutex);
			Log(fmt::format("gpu timing: the run, {} frames: {}{}", s_framesRun, KindsLine(s_kindRun, s_framesRun),
				s_dropped || s_unread ? fmt::format(" ({} marks dropped, {} buffers unread)", s_dropped, s_unread) : ""));
			Log(fmt::format("gpu timing: the run's passes, a frame: {}", TopLine(s_labelRun, s_framesRun, 30)));
		}
	}

	bool On()
	{
		return s_on;
	}

	void Init()
	{
		const char* e = getenv("WWHD_GPU_TIMING");
		if (!e || atoi(e) == 0)
			return;
		uint32 count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &count, families.data());
		const uint32 bits = s.queueFamily < count ? families[s.queueFamily].timestampValidBits : 0;
		if (!bits)
		{
			Log("gpu timing: the queue has no timestamps (WWHD_GPU_TIMING ignored)");
			return;
		}
		s_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
		s_period = s.props.limits.timestampPeriod;
		if (const char* every = getenv("WWHD_GPU_TIMING_EVERY"); every && atoi(every) > 0)
			s_every = (uint32)atoi(every);
		VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qi.queryCount = kQueries;
		for (uint32 i = 0; i < (s.cmds[1] ? 2u : 1u); i++)
		{
			Check(vkCreateQueryPool(s.device, &qi, nullptr, &s_slots[i].pool), "vkCreateQueryPool");
			s_slots[i].recs.reserve(kQueries);
		}
		s_on = true;
		atexit(Summary);
		at_quick_exit(Summary);                                   // WWHD_EXIT_FRAME's runs
		Log(fmt::format("gpu timing: on ({} timestamp bits, {} ns a tick), a line every {} frames (WWHD_GPU_TIMING)", bits,
			s_period, s_every));
		Begin();                                                  // the first command buffer is already recording
	}

	void Begin()
	{
		if (!s_on)
			return;
		Slot& slot = s_slots[s.slot];
		Collect(slot);
		slot.recs.clear();
		vkCmdResetQueryPool(s.cmd, slot.pool, 0, kQueries);
		Mark(Kind::Other);
	}

	void Mark(Kind kind, std::string_view label)
	{
		if (!s_on)
			return;
		Slot& slot = s_slots[s.slot];
		if (slot.recs.size() >= kQueries)
		{
			s_dropped++;
			return;
		}
		vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.pool, (uint32)slot.recs.size());
		slot.recs.push_back({ kind, LabelId(label) });
	}

	void Frame()
	{
		if (!s_on)
			return;
		s_framesRun++, s_framesTop++;
		if (++s_framesLine < s_every)
			return;
		std::lock_guard lock(s_mutex);
		Log(fmt::format("gpu timing: frames {}-{}: {}", s_framesRun - s_framesLine + 1, s_framesRun, KindsLine(s_kindLine, s_framesLine)));
		std::fill(std::begin(s_kindLine), std::end(s_kindLine), 0.0);
		s_framesLine = 0;
		if (++s_lines % 6 == 0)
		{
			Log(fmt::format("gpu timing: passes, a frame over the last {} frames: {}", s_framesTop, TopLine(s_labelLine, s_framesTop, 12)));
			std::fill(s_labelLine.begin(), s_labelLine.end(), Sum{});
			s_framesTop = 0;
		}
	}
}
