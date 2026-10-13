// The TV's sound on Android straight on AAudio (m-android), in place of SDL's output there. SDL's AAudio
// output keeps two buffers of its own ahead of Android's (20 ms each by default) and leaves Android's buffer
// its whole capacity: where Android gives the stream no fast track (a normal mixer track, 2886 frames on the
// Odin) that buffer stays full, so SDL's sound came ~100 ms after ours (dumpsys media.audio_flinger: the
// track's latency 122 ms against the output's ~58). Here AAudio's callback reads Cemu's blocks from a ring
// directly, and the stream's buffer size starts at one of its bursts (Android's notification period: 960
// frames on a normal track, 192 on a fast one) and grows half a burst at a time while Android counts
// underruns, as Oboe's latency tuner does.
// Like SdlAudio it keeps at most IAudioAPI::kBlockCount blocks and asks for more while fewer than the
// configured audio delay are queued. A lost device (headphones, Bluetooth) opens the stream again.
#include "boot.h"
#include "audio/IAudioAPI.h"
#include "config/CemuConfig.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
	// Cemu's log and logcat (Cemu's log file is off on Android)
	template<typename... Args>
	void Log(fmt::format_string<Args...> format, Args&&... args)
	{
		const std::string line = fmt::format(format, std::forward<Args>(args)...);
		cemuLog_log(LogType::Force, "{}", line);
		__android_log_print(ANDROID_LOG_INFO, "wwhd", "%s", line.c_str());
	}

	class AAudioOut : public IAudioAPI
	{
	public:
		AAudioOut(uint32 rate, uint32 channels, uint32 samplesPerBlock)
			: IAudioAPI(rate, channels, samplesPerBlock, 16), m_ring(size_t(kBlockCount) * samplesPerBlock * channels) {}
		~AAudioOut() override
		{
			std::unique_lock lock(m_streamMutex);
			m_closing = true;
			lock.unlock();
			if (m_reopen.joinable())
				m_reopen.join();
			Close();
		}

		bool Open()
		{
			std::unique_lock lock(m_streamMutex);
			return OpenLocked();
		}

		AudioAPI GetType() const override { return Cubeb; }        // the portable setting's name; only Cemu's settings UI reads it
		bool NeedAdditionalBlocks() const override { return Queued() < GetAudioDelay() * m_samplesPerBlock; }
		bool FeedBlock(sint16* data) override
		{
			const uint64 write = m_write.load(std::memory_order_relaxed);
			if (write + m_samplesPerBlock - m_read.load(std::memory_order_acquire) > kBlockCount * m_samplesPerBlock)
				return false;
			const size_t frames = m_ring.size() / m_channels;
			for (uint32 i = 0; i < m_samplesPerBlock; i++)
				std::copy_n(data + i * m_channels, m_channels, &m_ring[((write + i) % frames) * m_channels]);
			m_write.store(write + m_samplesPerBlock, std::memory_order_release);
			if (++m_blocks % 64 == 0)      // ~0.8 s
				Tune();
			return true;
		}
		// (Cemu's AX output calls Play every AX frame: the stream is started or paused only when that changes)
		bool Play() override
		{
			std::unique_lock lock(m_streamMutex);
			m_started = true;
			return Apply();
		}
		bool Stop() override
		{
			std::unique_lock lock(m_streamMutex);
			m_started = false;
			return Apply();
		}
		// the app in the background (the screen off, another app): the stream paused whatever Cemu asks
		void Background(bool on)
		{
			std::unique_lock lock(m_streamMutex);
			m_background = on;
			Apply();
		}
		void SetVolume(sint32 volume) override
		{
			IAudioAPI::SetVolume(volume);
			m_gain.store(std::clamp(volume, 0, 100) * 65536 / 100, std::memory_order_relaxed);
		}

	private:
		uint32 Queued() const { return (uint32)(m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_relaxed)); }

		bool Apply()
		{
			const bool run = m_started && !m_background;
			if (!m_stream || run == m_running)
				return true;
			m_running = run;
			return (run ? AAudioStream_requestStart(m_stream) : AAudioStream_requestPause(m_stream)) == AAUDIO_OK;
		}

		bool OpenLocked()
		{
			AAudioStreamBuilder* builder = nullptr;
			if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK)
				return false;
			AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
			AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
			AAudioStreamBuilder_setSampleRate(builder, (int32_t)m_samplerate);
			AAudioStreamBuilder_setChannelCount(builder, (int32_t)m_channels);
			AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
			AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
			AAudioStreamBuilder_setDataCallback(builder, &AAudioOut::DataCallback, this);
			AAudioStreamBuilder_setErrorCallback(builder, &AAudioOut::ErrorCallback, this);
			const aaudio_result_t res = AAudioStreamBuilder_openStream(builder, &m_stream);
			AAudioStreamBuilder_delete(builder);
			if (res != AAUDIO_OK)
			{
				Log("wwhd: AAudio: {}", AAudio_convertResultToText(res));
				m_stream = nullptr;
				return false;
			}
			m_burst = std::max(1, AAudioStream_getFramesPerBurst(m_stream));
			// our queue at least a burst and a block: Android takes a burst at a time (20 ms on a normal track), and the
			// configured two blocks (24 ms) ran dry under it about once a second on the Odin (a gap each time)
			SetAudioDelayOverride(0);
			SetAudioDelayOverride(std::max(GetAudioDelay(), ((uint32)m_burst + m_samplesPerBlock - 1) / m_samplesPerBlock + 1));
			m_xruns = AAudioStream_getXRunCount(m_stream);
			const int32_t size = AAudioStream_setBufferSizeInFrames(m_stream, m_burst);
			Log("wwhd: sound on AAudio, {} Hz, {} channels, {} track, burst {}, buffer {} of {} frames, our queue {} blocks",
				AAudioStream_getSampleRate(m_stream), AAudioStream_getChannelCount(m_stream),
				AAudioStream_getPerformanceMode(m_stream) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "fast" : "normal",
				m_burst, size, AAudioStream_getBufferCapacityInFrames(m_stream), GetAudioDelay());
			m_running = false;
			Apply();
			return true;
		}

		void Close()
		{
			if (m_stream)
				AAudioStream_close(m_stream);
			m_stream = nullptr;
		}

		// half a burst more buffer each time Android counted underruns since the last look, up to the capacity
		void Tune()
		{
			if (const uint32 shortReads = m_short.load(std::memory_order_relaxed); shortReads != m_shortLogged)
			{
				Log("wwhd: AAudio short of blocks {} times", shortReads);      // a gap in the sound: our queue ran dry
				m_shortLogged = shortReads;
			}
			std::unique_lock lock(m_streamMutex, std::try_to_lock);
			if (!lock || !m_stream)
				return;
			const int32_t xruns = AAudioStream_getXRunCount(m_stream);
			if (xruns <= m_xruns)
				return;
			m_xruns = xruns;
			const int32_t size = AAudioStream_getBufferSizeInFrames(m_stream);
			if (size >= AAudioStream_getBufferCapacityInFrames(m_stream))
				return;
			Log("wwhd: AAudio underruns ({}): buffer {} frames", xruns,
				AAudioStream_setBufferSizeInFrames(m_stream, size + std::max(1, m_burst / 2)));
		}

		static aaudio_data_callback_result_t DataCallback(AAudioStream*, void* user, void* audioData, int32_t numFrames)
		{
			auto* self = static_cast<AAudioOut*>(user);
			sint16* out = static_cast<sint16*>(audioData);
			const uint32 ch = self->m_channels;
			const uint64 read = self->m_read.load(std::memory_order_relaxed);
			const uint32 n = (uint32)std::min<uint64>((uint64)numFrames, self->m_write.load(std::memory_order_acquire) - read);
			const size_t frames = self->m_ring.size() / ch;
			const sint32 gain = self->m_gain.load(std::memory_order_relaxed);
			for (uint32 i = 0; i < n; i++)
			{
				const sint16* in = &self->m_ring[((read + i) % frames) * ch];
				for (uint32 c = 0; c < ch; c++)
					out[i * ch + c] = (sint16)((in[c] * gain) >> 16);
			}
			std::fill(out + size_t(n) * ch, out + size_t(numFrames) * ch, 0);      // short of blocks: silence
			if (n < (uint32)numFrames && read != 0)
				self->m_short.fetch_add(1, std::memory_order_relaxed);
			self->m_read.store(read + n, std::memory_order_release);
			return AAUDIO_CALLBACK_RESULT_CONTINUE;
		}

		// AAudio's own thread: the stream can't be closed here, so another thread opens it again
		static void ErrorCallback(AAudioStream*, void* user, aaudio_result_t error)
		{
			auto* self = static_cast<AAudioOut*>(user);
			if (error != AAUDIO_ERROR_DISCONNECTED)
				return;
			std::unique_lock lock(self->m_streamMutex);
			if (self->m_closing || self->m_reopening)
				return;
			self->m_reopening = true;
			if (self->m_reopen.joinable())
				self->m_reopen.detach();
			self->m_reopen = std::thread([self] {
				std::unique_lock lock(self->m_streamMutex);
				if (!self->m_closing)
				{
					Log("wwhd: AAudio device lost, opening again");
					self->Close();
					self->OpenLocked();
				}
				self->m_reopening = false;
			});
		}

		std::vector<sint16> m_ring;
		std::atomic<uint64> m_write{ 0 }, m_read{ 0 };
		std::atomic<sint32> m_gain{ 65536 };
		std::atomic<uint32> m_short{ 0 };
		uint64 m_blocks = 0;
		uint32 m_shortLogged = 0;

		std::mutex m_streamMutex;      // m_stream and what follows
		AAudioStream* m_stream = nullptr;
		int32_t m_burst = 0, m_xruns = 0;
		bool m_started = false, m_background = false, m_running = false, m_closing = false, m_reopening = false;
		std::thread m_reopen;
	};
}

namespace wwhd
{
	void AudioBackground(bool on)
	{
		std::shared_lock lock(g_audioMutex);
		if (auto* out = dynamic_cast<AAudioOut*>(g_tvAudio.get()))
			out->Background(on);
	}

	bool OpenAAudio()
	{
		const uint32 channels = CemuConfig::AudioChannelsToNChannels(GetConfig().tv_channels);
		auto device = std::make_unique<AAudioOut>(48000, channels, snd_core::AX_SAMPLES_PER_3MS_48KHZ * 4);
		if (!device->Open())
			return false;
		device->SetVolume(GetConfig().tv_volume);
		std::unique_lock lock(g_audioMutex);
		g_tvAudio = std::move(device);
		return true;
	}
}
