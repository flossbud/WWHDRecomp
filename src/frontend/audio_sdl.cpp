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

namespace wwhd
{
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
