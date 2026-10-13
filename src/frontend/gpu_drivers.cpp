// a4-drivers (Android): a GPU driver package the player picked (the zip other Android emulators take: meta.json, whose
// "libraryName" names the driver's library beside it; Turnip builds, Qualcomm's own drivers repackaged) unpacked into
// WWHD_DRIVERS_DIR/<its folder>/, the app's internal files, where dlopen may load from (gpu/vk/vk.cpp loads it with
// libadrenotools when Settings' GPU Driver names it). The pick is a content:// URI from the system's picker, which
// SDL_IOFromFile opens; the zip is copied beside the drivers first, as libzip reads a file.
#include "boot.h"
#include "../os/settings.h"
#include <SDL3/SDL.h>
#include <zip.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	// a JSON string field's value (the packages' meta.json; no JSON parser needed for two fields)
	std::string Field(const std::string& json, const char* key)
	{
		const std::string k = std::string("\"") + key + "\"";
		const size_t at = json.find(k);
		if (at == std::string::npos)
			return "";
		const size_t q0 = json.find('"', json.find(':', at + k.size()) + 1);
		const size_t q1 = q0 == std::string::npos ? q0 : json.find('"', q0 + 1);
		return q1 == std::string::npos ? "" : json.substr(q0 + 1, q1 - q0 - 1);
	}

	bool ReadEntry(zip_t* z, const std::string& name, std::vector<char>& out)
	{
		zip_stat_t st;
		zip_stat_init(&st);
		if (zip_stat(z, name.c_str(), ZIP_FL_NOCASE | ZIP_FL_NODIR, &st) != 0 || !(st.valid & ZIP_STAT_SIZE))
			return false;
		zip_file_t* f = zip_fopen(z, name.c_str(), ZIP_FL_NOCASE | ZIP_FL_NODIR);
		if (!f)
			return false;
		out.resize(st.size);
		const zip_int64_t n = zip_fread(f, out.data(), st.size);
		zip_fclose(f);
		return n == (zip_int64_t)st.size;
	}

	bool Write(const std::filesystem::path& p, const std::vector<char>& data)
	{
		std::ofstream out(p, std::ios::binary | std::ios::trunc);
		out.write(data.data(), (std::streamsize)data.size());
		return (bool)out;
	}
}

namespace wwhd
{
	bool ImportGpuDriver(const std::string& source, std::string& name, std::string& id, std::string& why)
	{
		namespace fs = std::filesystem;
		const char* dirs = getenv("WWHD_DRIVERS_DIR");
		if (!dirs || !*dirs)
		{
			why = "no place for drivers";
			return false;
		}
		std::error_code ec;
		fs::create_directories(dirs, ec);
		const fs::path tmp = fs::path(dirs) / ".import.zip";
		{
			SDL_IOStream* in = SDL_IOFromFile(source.c_str(), "rb");
			if (!in)
			{
				why = std::string("couldn't open it (") + SDL_GetError() + ")";
				return false;
			}
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			char buf[1 << 16];
			for (size_t n; (n = SDL_ReadIO(in, buf, sizeof(buf))) > 0;)
				out.write(buf, (std::streamsize)n);
			SDL_CloseIO(in);
		}
		int err = 0;
		zip_t* z = zip_open(tmp.string().c_str(), ZIP_RDONLY, &err);
		auto done = [&](bool ok) {
			if (z)
				zip_close(z);
			fs::remove(tmp, ec);
			return ok;
		};
		if (!z)
		{
			why = "it isn't a zip";
			return done(false);
		}
		std::vector<char> meta, lib;
		if (!ReadEntry(z, "meta.json", meta))
		{
			why = "no meta.json in it (a driver package for Android emulators has one)";
			return done(false);
		}
		const std::string json(meta.begin(), meta.end());
		const std::string library = Field(json, "libraryName");
		name = Field(json, "name");
		if (library.empty() || library.find('/') != std::string::npos || !ReadEntry(z, library, lib))
		{
			why = "its meta.json names no library in it";
			return done(false);
		}
		if (name.empty())
			name = library;
		// its folder: the package's name and version, as a file name
		id = name + " " + Field(json, "packageVersion");
		for (char& c : id)
			if (!isalnum((unsigned char)c) && c != '.' && c != '-')
				c = '_';
		const fs::path dir = fs::path(dirs) / id;
		fs::remove_all(dir, ec);
		fs::create_directories(dir, ec);
		if (!Write(dir / "meta.json", meta) || !Write(dir / library, lib))
		{
			fs::remove_all(dir, ec);
			why = "couldn't write it (storage full?)";
			return done(false);
		}
		os::settings::RescanDrivers();
		cemuLog_log(LogType::Force, "wwhd: GPU driver imported: {} ({}, {} bytes) in {}", name, library, lib.size(), dir.string());
		return done(true);
	}
}
