// The player's copy of the game (t-startscreen): see game_files.h.
#include "game_files.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "config/ActiveSettings.h"
#include <openssl/sha.h>
#include <SDL3/SDL_iostream.h>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include "build_id.inc"

namespace wwhd::game_files
{
	namespace
	{
		constexpr uint64 kTitleId = 0x0005000010143500ull;   // The Wind Waker HD, USA (orig/README.md)
		constexpr uint16 kVersion = 0;                         // v0: every address in this port is its
		// code/cking.rpx's sha256 (US v0, orig/README.md; cemu-project/title-checksums)
		constexpr const char* kRpxSha256 = "c4f0ab300542e0bfc462696850534e71db2ad02288a7eb55e5a4cd4062f16153";
		constexpr const char* kCheckMount = "/vol/wwhd_check/";

		std::mutex s_lock;                                     // one check at a time (its mount point is one)

		void Set(std::atomic<float>* p, float v)
		{
			if (p)
				p->store(v, std::memory_order_relaxed);
		}

		std::string RegionName(CafeConsoleRegion r)
		{
			const uint32 v = (uint32)r;
			if (v & (uint32)CafeConsoleRegion::USA)
				return "USA";
			if (v & (uint32)CafeConsoleRegion::EUR)
				return "EUR";
			if (v & (uint32)CafeConsoleRegion::JPN)
				return "JPN";
			return "";
		}

		fs::path RegistryFile()
		{
			return ActiveSettings::GetUserDataPath("game.txt");
		}

		// the copy as it is on disk: its size and time (a folder: its code/cking.rpx's), so a changed copy is checked again
		std::string Stamp(const std::string& path)
		{
			std::error_code ec;
			fs::path p = _utf8ToPath(path);
			if (fs::is_directory(p, ec))
				p /= "code/cking.rpx";
			const auto size = fs::file_size(p, ec);
			if (ec)
				return "";
			const auto time = fs::last_write_time(p, ec);
			if (ec)
				return "";
			return fmt::format("{} {}", size, (long long)time.time_since_epoch().count());
		}
	}

	Copy Check(const std::string& path, std::atomic<float>* progress, std::atomic<bool>* cancel, float from, float span)
	{
		std::lock_guard lock(s_lock);
		Copy c;
		c.path = path;
		auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
		auto Set = [&](std::atomic<float>* p, float v) { game_files::Set(p, from + span * v); };
		// reading the files (0 to .35): the format and the title's XML
		Set(progress, 0.02f);
		TitleInfo title{ _utf8ToPath(path) };
		if (!title.IsValid())
		{
			switch (title.GetInvalidReason())
			{
			case TitleInfo::InvalidReason::NO_DISC_KEY: c.why = "This disc image needs its key (keys.txt), which wasn't found."; break;
			case TitleInfo::InvalidReason::BAD_PATH_OR_INACCESSIBLE: c.why = "The file couldn't be opened."; break;
			default: c.why = "This isn't a Wii U game dump."; break;
			}
			return c;
		}
		c.version = title.GetAppTitleVersion();
		c.region = RegionName(title.GetMetaRegion());
		Set(progress, 0.35f);
		if (cancelled())
			return c.why = "cancelled", c;
		if (title.GetAppTitleId() != kTitleId)
		{
			const std::string name = title.GetMetaTitleName();
			c.why = (title.GetAppTitleId() & ~0xFFull) == (kTitleId & ~0xFFull) || name.find("Wind Waker") != std::string::npos
				? fmt::format("This is the {} release; WindSong runs the USA release for now.", c.region.empty() ? "other" : c.region)
				: "This isn't The Wind Waker HD.";
			return c;
		}
		if (c.version != kVersion)
		{
			c.why = fmt::format("This is version {}; WindSong runs version 0 (the disc's) for now.", c.version);
			return c;
		}
		// verifying (.35 to .75): code/cking.rpx's hash
		if (!title.Mount(kCheckMount, "code", FSC_PRIORITY_BASE))
		{
			c.why = "The game's code couldn't be read.";
			return c;
		}
		auto rpx = fsc_extractFile((std::string(kCheckMount) + "cking.rpx").c_str());
		title.Unmount(kCheckMount);
		if (!rpx || rpx->empty())
		{
			c.why = "The dump is incomplete (its code is missing).";
			return c;
		}
		Set(progress, 0.45f);
		SHA256_CTX sha;
		SHA256_Init(&sha);
		const size_t chunk = 1 << 20;
		static const int delay = getenv("WWHD_DEBUG_CHECK_DELAY") ? atoi(getenv("WWHD_DEBUG_CHECK_DELAY")) : 0;   // a test's: ms a MB
		for (size_t at = 0; at < rpx->size(); at += chunk)
		{
			if (delay > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(delay));
			SHA256_Update(&sha, rpx->data() + at, std::min(chunk, rpx->size() - at));
			Set(progress, 0.45f + 0.3f * (float)(at + chunk) / (float)rpx->size());
			if (cancelled())
				return c.why = "cancelled", c;
		}
		uint8 digest[SHA256_DIGEST_LENGTH];
		SHA256_Final(digest, &sha);
		std::string hex;
		for (uint8 b : digest)
			hex += fmt::format("{:02x}", b);
		if (hex != kRpxSha256)
		{
			c.why = "The game's code doesn't match the USA version 0 release (a modified or damaged dump).";
			return c;
		}
		// preparing (.75 to 1): nothing to copy (the copy is read in place)
		Set(progress, 1.0f);
		c.ok = true;
		return c;
	}

	namespace
	{
		// where a copy taken in goes: WWHD_COPY_IN's folder (a test's, any system), else the Android app's files
		std::string CopyInDir()
		{
			if (const char* d = getenv("WWHD_COPY_IN"); d && *d)
				return d;
#if defined(__ANDROID__)
			if (const char* home = getenv("WWHD_HOME"); home && *home)
				return home;
#endif
			return "";
		}

		// a ZArchive (.wua) ends with its footer: ..., its total size (8 bytes), version (4), magic (4), big-endian
		// (Cemu's dependencies/ZArchive, zarchivecommon.h). True also when the end can't be read (a stream that can't
		// seek): the check after the copy decides then
		bool LooksLikeWua(SDL_IOStream* in, Sint64 size)
		{
			uint8 tail[16];
			if (size < (Sint64)sizeof(tail) || SDL_SeekIO(in, size - (Sint64)sizeof(tail), SDL_IO_SEEK_SET) < 0)
				return true;
			const bool read = SDL_ReadIO(in, tail, sizeof(tail)) == sizeof(tail);
			SDL_SeekIO(in, 0, SDL_IO_SEEK_SET);
			if (!read)
				return true;
			auto be = [&](int at, int n) { uint64 v = 0; for (int i = 0; i < n; i++) v = v << 8 | tail[at + i]; return v; };
			return be(12, 4) == 0x169f52d6 && be(0, 8) == (uint64)size;
		}
	}

	bool CopiesIn()
	{
		return !CopyInDir().empty();
	}

	Copy CopyIn(const std::string& chosen, std::atomic<float>* progress, std::atomic<bool>* cancel)
	{
		static std::mutex s_copyLock;                  // one at a time: a cancelled copy removes its part before the next writes it
		std::lock_guard lock(s_copyLock);
		Copy c;
		c.path = chosen;
		auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };
		const fs::path dir = _utf8ToPath(CopyInDir()) / "game", into = dir / "wwhd.wua", part = dir / "wwhd-new.wua";
		std::error_code ec;
		fs::create_directories(dir, ec);
		Set(progress, 0.0f);
		SDL_IOStream* in = SDL_IOFromFile(chosen.c_str(), "rb");
		if (!in)
		{
			cemuLog_log(LogType::Force, "wwhd: start screen: couldn't open {} ({})", chosen, SDL_GetError());
			c.why = "The file couldn't be opened.";
			return c;
		}
		const Sint64 size = SDL_GetIOSize(in);
		if (!LooksLikeWua(in, size))
		{
			SDL_CloseIO(in);
			c.why = "This isn\xE2\x80\x99t a .wua file. WindSong takes the game here as one .wua (Cemu can make one from a dump).";
			return c;
		}
		if (const fs::space_info room = fs::space(dir, ec); !ec && size > 0 && room.available < (uintmax_t)size + (64u << 20))
		{
			SDL_CloseIO(in);
			c.why = fmt::format("There isn\xE2\x80\x99t room for it: it needs {:.1f} GB free, and there\xE2\x80\x99s {:.1f} GB.",
				(double)size / 1e9, (double)room.available / 1e9);
			return c;
		}
		cemuLog_log(LogType::Force, "wwhd: start screen: copying {} ({} bytes) to {}", chosen, size, _pathToUtf8(part));
		FILE* out = fopen(_pathToUtf8(part).c_str(), "wb");
		bool ok = out != nullptr;
		std::vector<char> buf(4 << 20);
		uint64 done = 0;
		while (ok)
		{
			const size_t n = SDL_ReadIO(in, buf.data(), buf.size());
			if (n == 0)
			{
				ok = SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF;
				break;
			}
			ok = fwrite(buf.data(), 1, n, out) == n;
			done += n;
			Set(progress, 0.35f * (size > 0 ? std::min(1.0f, (float)done / (float)size) : 0.5f));
			if (cancelled())
				break;
		}
		SDL_CloseIO(in);
		if (out && fclose(out) != 0)
			ok = false;
		if (!ok || cancelled() || (size > 0 && done != (uint64)size))
		{
			fs::remove(part, ec);
			c.why = cancelled() ? "cancelled" : "It couldn\xE2\x80\x99t be copied (is the storage full, or the file gone?).";
			return c;
		}
		Copy checked = Check(_pathToUtf8(part), progress, cancel, 0.35f, 0.65f);
		if (checked.ok)
		{
			fs::rename(part, into, ec);                // over the copy before, which a running game may still read (its inode stays)
			if (ec)
				checked.ok = false, checked.why = "It couldn\xE2\x80\x99t be put in place (" + ec.message() + ").";
			else
				checked.path = _pathToUtf8(into);
		}
		if (!checked.ok)
		{
			fs::remove(part, ec);
			checked.path = chosen;
		}
		cemuLog_log(LogType::Force, "wwhd: start screen: copied in {} bytes: {}", done, checked.ok ? checked.path : checked.why);
		return checked;
	}

	bool Registered(Copy& out)
	{
		std::ifstream f(RegistryFile());
		std::string line, path, stamp, region;
		while (std::getline(f, line))
		{
			const size_t eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
			if (k == "path") path = v;
			else if (k == "stamp") stamp = v;
			else if (k == "region") region = v;
		}
		if (path.empty())
			return false;
		out = Copy{};
		out.path = path, out.region = region, out.version = kVersion;
		out.ok = !stamp.empty() && stamp == Stamp(path);       // checked as it is now
		if (!out.ok)
			out.why = Stamp(path).empty() ? "The game files are no longer there." : "The game files have changed since they were checked.";
		return true;
	}

	std::string RegisteredPath()
	{
		Copy c;
		return Registered(c) ? c.path : std::string();
	}

	void Register(const Copy& copy)
	{
		if (!copy.ok)
			return;
		const fs::path file = RegistryFile();
		{
			std::ofstream f(file.string() + ".new");
			f << "# the game files WindSong reads (the start screen's check passed for them as they were then)\n";
			f << "path=" << copy.path << "\nstamp=" << Stamp(copy.path) << "\nregion=" << copy.region << "\n";
		}
		std::error_code ec;
		fs::rename(file.string() + ".new", file, ec);
	}

	std::string Version()
	{
		std::string v = kBuildId;                      // "hash date" (a "+" after it: built from changes not committed)
		if (const size_t sp = v.find(' '); sp != std::string::npos)
			v.replace(sp, 1, " \xC2\xB7 ");
		return v;
	}
}
