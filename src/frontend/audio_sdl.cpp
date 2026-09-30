// The TV's sound on SDL3 (the platform shell, docs/recompiler-design.md D18), in place of Cemu's
// Cubeb output. Cemu's AX mixer (snd_core, still Cemu's) feeds it through Cemu's device interface in
// blocks of four AX frames (12 ms at 48 kHz, 16-bit, the configured channels). Like Cemu's devices it
// keeps at most IAudioAPI::kBlockCount blocks queued, dropping any beyond, and asks for more while
// fewer than the configured audio delay are queued. Installed as Cemu's TV device before the game
// starts, so snd_core never creates its own; the GamePad has no sound (D17).
#include "boot.h"
#include "audio/IAudioAPI.h"
#include "config/CemuConfig.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include <SDL3/SDL.h>

namespace
{
	class SdlAudio : public IAudioAPI
	{
	public:
		SdlAudio(SDL_AudioStream* stream, uint32 rate, uint32 channels, uint32 samplesPerBlock)
			: IAudioAPI(rate, channels, samplesPerBlock, 16), m_stream(stream) {}
		~SdlAudio() override { SDL_DestroyAudioStream(m_stream); }

		AudioAPI GetType() const override { return Cubeb; }        // the portable setting's name; only Cemu's settings UI reads it
		bool NeedAdditionalBlocks() const override { return Queued() < GetAudioDelay() * m_bytesPerBlock; }
		bool FeedBlock(sint16* data) override
		{
			if (Queued() + m_bytesPerBlock > kBlockCount * m_bytesPerBlock)
				return false;
			return SDL_PutAudioStreamData(m_stream, data, (int)m_bytesPerBlock);
		}
		bool Play() override { return SDL_ResumeAudioStreamDevice(m_stream); }
		bool Stop() override { return SDL_PauseAudioStreamDevice(m_stream); }
		void SetVolume(sint32 volume) override
		{
			IAudioAPI::SetVolume(volume);
			SDL_SetAudioStreamGain(m_stream, volume / 100.0f);
		}

	private:
		uint32 Queued() const { return (uint32)std::max(0, SDL_GetAudioStreamQueued(m_stream)); }

		SDL_AudioStream* m_stream;
	};
}

namespace
{
	// WWHD_AUDIO_HASH=path: a device that plays nothing and writes a hash of every block it is fed, one
	// line per block (index, hash), to check that our snd_core mixes exactly what Cemu's did
	// (tools/reference/stream_check.sh). Never short of data, never full, as a real device can be.
	class HashAudio : public IAudioAPI
	{
	public:
		HashAudio(FILE* out, uint32 rate, uint32 channels, uint32 samplesPerBlock)
			: IAudioAPI(rate, channels, samplesPerBlock, 16), m_out(out) {}
		AudioAPI GetType() const override { return Cubeb; }
		bool NeedAdditionalBlocks() const override { return false; }
		bool FeedBlock(sint16* data) override
		{
			uint64 h = 0xCBF29CE484222325ull;
			const uint8* p = (const uint8*)data;
			for (uint32 i = 0; i < m_bytesPerBlock; i++)
				h = (h ^ p[i]) * 0x100000001B3ull;
			fprintf(m_out, "%llu %016llx\n", (unsigned long long)m_blocks++, (unsigned long long)h);
			return true;
		}
		bool Play() override { return true; }
		bool Stop() override { return true; }

	private:
		FILE* m_out;
		uint64 m_blocks = 0;
	};
}

namespace wwhd
{
	bool OpenAudioHash()
	{
		const char* path = getenv("WWHD_AUDIO_HASH");
		if (!path || !*path)
			return false;
		FILE* out = fopen(path, "w");
		if (!out)
			return false;
		setvbuf(out, nullptr, _IOLBF, 0);    // whole lines even when the run ends abruptly
		const uint32 channels = CemuConfig::AudioChannelsToNChannels(GetConfig().tv_channels);
		std::unique_lock lock(g_audioMutex);
		g_tvAudio = std::make_unique<HashAudio>(out, 48000, channels, snd_core::AX_SAMPLES_PER_3MS_48KHZ * 4);
		cemuLog_log(LogType::Force, "wwhd: sound hashed into {} ({} channels)", path, channels);
		return true;
	}

	bool OpenAudio()
	{
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
		{
			cemuLog_log(LogType::Force, "wwhd: SDL audio: {}", SDL_GetError());
			return false;
		}
		constexpr uint32 kRate = 48000;
		const uint32 channels = CemuConfig::AudioChannelsToNChannels(GetConfig().tv_channels);
		SDL_AudioSpec spec{ SDL_AUDIO_S16, (int)channels, (int)kRate };
		SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (!stream)
		{
			cemuLog_log(LogType::Force, "wwhd: SDL audio device: {}", SDL_GetError());
			return false;
		}
		auto device = std::make_unique<SdlAudio>(stream, kRate, channels, snd_core::AX_SAMPLES_PER_3MS_48KHZ * 4);
		device->SetVolume(GetConfig().tv_volume);
		cemuLog_log(LogType::Force, "wwhd: sound on SDL's {} audio driver, {} channels", SDL_GetCurrentAudioDriver(), channels);
		std::unique_lock lock(g_audioMutex);
		g_tvAudio = std::move(device);
		return true;
	}
}
