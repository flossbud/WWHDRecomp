// The shader cache on disk (docs/recompiler-design.md D20), in portable/shaderCache/wwhd:
// * shaders.bin: every shader the renderer has translated (SPIR-V and what the draws need of the
//   decompiler's analysis), and pipelines.bin: every pipeline it has built (its Vulkan state and the
//   keys of its two shaders). Both are the same on every machine and are appended to as the game
//   shows something new. They hold translations of the game's shaders: they stay on the player's
//   machine, never in git.
// * vulkan-<vendor>-<device>.bin: the driver's pipeline cache (VkPipelineCache), used only if its
//   header says this device and driver made it, and rewritten now and then (write, then rename).
// Records are length, bytes, checksum; a file whose header doesn't match kVersion is started over,
// and a torn record at the end (a crash, or a phone killing the app) is cut off.
#include "renderer_internal.h"
#include "config/ActiveSettings.h"
#include <fstream>

namespace wwhd::gpu::cache
{
	namespace
	{
		// Bump when what a record holds changes meaning: the decompiler, its options, glslang's
		// options, or the fields draw.cpp writes.
		constexpr uint32 kVersion = 1;
		constexpr char kMagic[8] = { 'W', 'W', 'H', 'D', 'S', 'C', 'A', 'C' };

		uint64 Checksum(const uint8* p, size_t n)
		{
			uint64 h = 0xCBF29CE484222325ull;
			for (size_t i = 0; i < n; i++)
				h = (h ^ p[i]) * 0x100000001B3ull;
			return h;
		}

		struct RecordFile
		{
			fs::path path;
			uint32 kind = 0;
			std::vector<std::vector<uint8>> records;               // as read at Open
			FILE* append = nullptr;
			std::mutex lock;

			void Open(const fs::path& p, uint32 k)
			{
				path = p;
				kind = k;
				uint64 good = 0;
				{
					std::ifstream in(path, std::ios::binary);
					char magic[8];
					uint32 version = 0, fileKind = 0;
					if (in.read(magic, 8) && in.read((char*)&version, 4) && in.read((char*)&fileKind, 4) &&
						!memcmp(magic, kMagic, 8) && version == kVersion && fileKind == kind)
					{
						good = 16;
						for (;;)
						{
							uint32 n = 0;
							uint64 sum = 0;
							if (!in.read((char*)&n, 4) || n > (64u << 20))
								break;
							std::vector<uint8> r(n);
							if (!in.read((char*)r.data(), n) || !in.read((char*)&sum, 8) || sum != Checksum(r.data(), n))
								break;
							records.push_back(std::move(r));
							good += 4 + n + 8;
						}
					}
				}
				std::error_code ec;
				if (good == 0)
				{
					fs::remove(path, ec);
					append = fopen(path.string().c_str(), "wb");
					if (append)
					{
						uint32 header[2] = { kVersion, kind };
						fwrite(kMagic, 1, 8, append);
						fwrite(header, 4, 2, append);
						fflush(append);
					}
				}
				else
				{
					if (fs::file_size(path, ec) != good)
						fs::resize_file(path, good, ec);                 // drop a torn record
					append = fopen(path.string().c_str(), "ab");
				}
				if (!append)
					Log(fmt::format("shader cache: can't write {}", path.string()));
			}

			void Add(std::span<const uint8> r)
			{
				std::lock_guard l(lock);
				if (!append)
					return;
				uint32 n = (uint32)r.size();
				uint64 sum = Checksum(r.data(), r.size());
				fwrite(&n, 4, 1, append);
				fwrite(r.data(), 1, r.size(), append);
				fwrite(&sum, 8, 1, append);
				fflush(append);
			}
		};

		RecordFile s_shaders, s_pipelines;
		fs::path s_driverPath;
		VkPipelineCache s_driver = VK_NULL_HANDLE;
		std::atomic<uint32> s_added{ 0 };                          // pipelines built since the driver cache was saved
		std::atomic<bool> s_saving{ false };
		std::atomic<sint64> s_lastSave{ 0 };                       // steady-clock seconds

		sint64 Now()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void WriteDriverCache()
		{
			size_t n = 0;
			if (!s_driver || vkGetPipelineCacheData(s.device, s_driver, &n, nullptr) != VK_SUCCESS || !n)
				return;
			std::vector<uint8> data(n);
			if (vkGetPipelineCacheData(s.device, s_driver, &n, data.data()) != VK_SUCCESS)
				return;
			fs::path tmp = s_driverPath;
			tmp += ".tmp";
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				out.write((const char*)data.data(), (std::streamsize)n);
				if (!out)
					return;
			}
			std::error_code ec;
			fs::rename(tmp, s_driverPath, ec);
		}
	}

	void Open()
	{
		fs::path dir = ActiveSettings::GetCachePath("shaderCache/wwhd");
		std::error_code ec;
		fs::create_directories(dir, ec);
		s_shaders.Open(dir / "shaders.bin", 1);
		s_pipelines.Open(dir / "pipelines.bin", 2);

		// the driver's cache, if this device and driver wrote it (the header Vulkan defines)
		s_driverPath = dir / fmt::format("vulkan-{:04x}-{:04x}.bin", s.props.vendorID, s.props.deviceID);
		std::vector<uint8> data;
		{
			std::ifstream in(s_driverPath, std::ios::binary);
			data.assign(std::istreambuf_iterator<char>(in), {});
		}
		struct Header { uint32 length, version, vendor, device; uint8 uuid[VK_UUID_SIZE]; };
		Header h{};
		if (data.size() >= sizeof(h))
			memcpy(&h, data.data(), sizeof(h));
		bool ours = data.size() >= sizeof(h) && h.version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h.vendor == s.props.vendorID &&
			h.device == s.props.deviceID && !memcmp(h.uuid, s.props.pipelineCacheUUID, VK_UUID_SIZE);
		VkPipelineCacheCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
		if (ours)
		{
			ci.initialDataSize = data.size();
			ci.pInitialData = data.data();
		}
		if (vkCreatePipelineCache(s.device, &ci, nullptr, &s_driver) != VK_SUCCESS)
			s_driver = VK_NULL_HANDLE;
		s_lastSave = Now();
		Log(fmt::format("shader cache: {} shaders, {} pipelines, driver cache {} ({})", s_shaders.records.size(),
			s_pipelines.records.size(), ours ? fmt::format("{} KB", data.size() / 1024) : std::string("empty"), dir.string()));
		// runs that end without closing the window (WWHD_EXIT_FRAME, the trace's exit frame)
		atexit(Save);
		at_quick_exit(Save);
	}

	const std::vector<std::vector<uint8>>& Shaders() { return s_shaders.records; }
	const std::vector<std::vector<uint8>>& Pipelines() { return s_pipelines.records; }
	void AddShader(std::span<const uint8> record) { s_shaders.Add(record); }

	void AddPipeline(std::span<const uint8> record)
	{
		s_pipelines.Add(record);
		s_added++;
	}

	VkPipelineCache Driver() { return s_driver; }

	void Save()
	{
		if (s_saving.exchange(true))
			return;
		WriteDriverCache();
		s_added = 0;
		s_lastSave = Now();
		s_saving = false;
	}

	// at most every 10 s, when pipelines were built since the last save, on a thread of its own
	// (the driver's cache is internally synchronised)
	void SaveNowAndThen()
	{
		if (!s_added || s_saving || Now() - s_lastSave < 10)
			return;
		std::thread([] { Save(); }).detach();
	}
}
