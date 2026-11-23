// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Native PS4 output for the existing PCSX2 SPU2 audio queue.
#include "AudioStreamPS4.h"
#include "ThreadAffinityPS4.h"
#include "Host/AudioStream.h"
#include "common/Error.h"
#include "common/Threading.h"
#include "3rdparty/cubeb/subprojects/speex/speex_resampler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>

extern void ProbeLog(const char* format, ...);

namespace
{
	struct OutputCounters
	{
		std::atomic<bool> attempted{false};
		std::atomic<int> status{0}; // 1 = active, 0 = closed, negative = error
		std::atomic<u64> buffers{0}, pcm_buffers{0}, input_short{0}, paused{0};
		std::atomic<u64> refill_pending{0};
		std::atomic<u32> queue_min{0}, queue_max{0};
		std::atomic<u32> peak_q15{0}, rms_q15{0};
		std::atomic<u64> invalid_samples{0}, clipped_samples{0};
		std::atomic<int> port_result{0}, port_volume{0};
		std::atomic<u32> port_output{0}, port_channels{0};
		std::atomic<u64> port_flags{0};
	};
	OutputCounters s_output;

	class PS4AudioStream final : public AudioStream
	{
	public:
		PS4AudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
			: AudioStream(sample_rate, parameters)
		{
		}

		~PS4AudioStream() override
		{
			m_quit.store(true, std::memory_order_release);
			if (m_thread_started)
				pthread_join(m_thread, nullptr);
			if (m_resampler)
				speex_resampler_destroy(m_resampler);
			// (AI-assisted) The submitted buffer remains alive until the port closes.
			if (m_handle >= 0)
			{
				const int result = sceAudioOutClose(m_handle);
				ProbeLog("PS4 audio: close=%#x\n", result);
			}
			if (s_output.status.load(std::memory_order_relaxed) == 1)
				s_output.status.store(0, std::memory_order_relaxed);
			PS4Audio::LogStatus();
		}

		void SetPaused(bool paused) override
		{
			// SetStretchEnabled reallocates the queue after pausing. Wait for any
			// in-flight ReadFrames, and keep hardware waits outside this lock.
			std::lock_guard lock(m_read_mutex);
			if (paused != m_paused && m_resampler)
			{
				// Discard buffered samples/filter history when pausing or rebuilding
				// the base queue. Do not play pre-pause audio after resuming.
				speex_resampler_reset_mem(m_resampler);
				m_source_offset = m_source_frames = 0;
			}
			m_paused = paused;
		}

		bool Open(bool stretch_enabled, Error* error)
		{
			s_output.attempted.store(true, std::memory_order_relaxed);
			s_output.status.store(0, std::memory_order_relaxed);
			s_output.buffers.store(0, std::memory_order_relaxed);
			s_output.pcm_buffers.store(0, std::memory_order_relaxed);
			s_output.input_short.store(0, std::memory_order_relaxed);
			s_output.paused.store(0, std::memory_order_relaxed);
			s_output.refill_pending.store(0, std::memory_order_relaxed);
			s_output.queue_min.store(0, std::memory_order_relaxed);
			s_output.queue_max.store(0, std::memory_order_relaxed);
			s_output.peak_q15.store(0, std::memory_order_relaxed);
			s_output.rms_q15.store(0, std::memory_order_relaxed);
			s_output.invalid_samples.store(0, std::memory_order_relaxed);
			s_output.clipped_samples.store(0, std::memory_order_relaxed);
			s_output.port_result.store(0, std::memory_order_relaxed);
			s_output.port_output.store(0, std::memory_order_relaxed);
			s_output.port_channels.store(0, std::memory_order_relaxed);
			s_output.port_volume.store(0, std::memory_order_relaxed);
			s_output.port_flags.store(0, std::memory_order_relaxed);
			// (AI-assisted) Keep the queue/time-stretcher at the emulated console's
			// native rate and convert only at the hardware output boundary.
			if (m_sample_rate != OUTPUT_RATE && m_sample_rate != 44100)
			{
				Error::SetStringFmt(error, "PS4 audio supports 44100/48000 Hz input; requested {} Hz", m_sample_rate);
				ProbeLog("PS4 audio: unsupported sample_rate=%u; falling back to Null\n", m_sample_rate);
				s_output.status.store(-1, std::memory_order_relaxed);
				return false;
			}
			if (m_sample_rate != OUTPUT_RATE)
			{
				int result = RESAMPLER_ERR_SUCCESS;
				m_resampler = speex_resampler_init(2, m_sample_rate, OUTPUT_RATE, SPEEX_RESAMPLER_QUALITY_DEFAULT, &result);
				if (!m_resampler || result != RESAMPLER_ERR_SUCCESS)
					return Fail("audio resampler creation", result ? result : -1, error);
			}

			const int module_result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
			const int init_result = sceAudioOutInit();
			ProbeLog("PS4 audio: module=%#x init=%#x\n", module_result, init_result);
			if (init_result < 0 && static_cast<u32>(init_result) != ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT)
				return Fail("sceAudioOutInit", init_result, error);

			m_handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
				0, GRAIN, OUTPUT_RATE, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
			ProbeLog("PS4 audio: open=%#x input_rate=%u output_rate=%u resampler=%s channels=2 grain=%u format=s16_stereo stretch=%s buffer_ms=%u\n",
				m_handle, m_sample_rate, OUTPUT_RATE, m_resampler ? "speex" : "bypass", GRAIN, stretch_enabled ? "on" : "off", m_parameters.buffer_ms);
			if (m_handle < 0)
				return Fail("sceAudioOutOpen", m_handle, error);

			// (AI-assisted) Match DolphinPS4's main-port PCM16 and explicit 0 dB
			// gain. The upstream queue already applies the user's volume/mute.
			// No sceAudioOutGetPortState here: on console it crashed right after open
			// (sync log 2026-11-23), and the working intro player never calls it.
			int32_t levels[2] = {32768, 32768};
			const int volume_result = sceAudioOutSetVolume(m_handle, 0x3, levels);
			ProbeLog("PS4 audio: set_volume=%#x left=32768 right=32768 (0 dB)\n", volume_result);
			if (volume_result < 0)
				return Fail("sceAudioOutSetVolume", volume_result, error);

			BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);
			ProbeLog("PS4 audio stretch: sequence_ms=%u seek_ms=%u overlap_ms=%u quick_seek=%u target_ms=%u\n",
				m_parameters.stretch_sequence_length_ms, m_parameters.stretch_seekwindow_ms,
				m_parameters.stretch_overlap_ms, static_cast<unsigned>(m_parameters.stretch_use_quickseek),
				m_parameters.buffer_ms);
			pthread_attr_t attr;
			int result = pthread_attr_init(&attr);
			if (result == 0)
			{
				result = pthread_attr_setstacksize(&attr, 256 * 1024);
				if (result == 0)
				{
					s_output.status.store(1, std::memory_order_relaxed);
					result = pthread_create(&m_thread, &attr, ThreadEntry, this);
				}
				pthread_attr_destroy(&attr);
			}
			if (result != 0)
				return Fail("audio worker creation", result, error);
			m_thread_started = true;
			return true;
		}

	private:
		static constexpr u32 GRAIN = 512;
		static constexpr u32 OUTPUT_RATE = 48000;

		// Called with m_read_mutex held. Keep unconsumed input and filter state
		// across hardware blocks: 512 output frames are not 512 PS1 input frames.
		bool ReadOutputFrames(u64& input_short, u64& refill_pending, u32& queue_min, u32& queue_max)
		{
			const auto read_source = [&](float* samples) {
				const u32 queued = GetBufferedFramesRelaxed();
				input_short += queued < GRAIN;
				queue_min = std::min(queue_min, queued);
				queue_max = std::max(queue_max, queued);
				ReadFrames(samples, GRAIN);
				refill_pending += m_filling;
			};
			if (!m_resampler)
			{
				read_source(m_samples);
				return true;
			}
			u32 produced = 0;
			while (produced < GRAIN)
			{
				if (!m_source_frames)
				{
					read_source(m_source);
					m_source_frames = GRAIN;
					m_source_offset = 0;
				}
				spx_uint32_t consumed = m_source_frames, written = GRAIN - produced;
				const int result = speex_resampler_process_interleaved_float(m_resampler,
					m_source + m_source_offset * 2, &consumed, m_samples + produced * 2, &written);
				if (result != RESAMPLER_ERR_SUCCESS || (consumed == 0 && written == 0))
					return Fail("audio resampler process", result ? result : -1, nullptr);
				m_source_offset += consumed;
				m_source_frames -= consumed;
				produced += written;
			}
			return true;
		}

		static bool Fail(const char* operation, int result, Error* error)
		{
			Error::SetStringFmt(error, "{} failed: {:#x}", operation, static_cast<u32>(result));
			ProbeLog("PS4 audio: %s failed=%#x\n", operation, result);
			s_output.status.store(result < 0 ? result : -result, std::memory_order_relaxed);
			return false;
		}

		static void* ThreadEntry(void* context)
		{
			static_cast<PS4AudioStream*>(context)->Run();
			return nullptr;
		}

		void Run()
		{
			Threading::SetNameOfCurrentThread("PS4 AudioOut");
			PS4ThreadAffinity::PinCurrentThread("Audio", 5);
			u64 buffers = 0, pcm_buffers = 0, input_short = 0, paused_buffers = 0;
			u64 invalid_samples = 0, clipped_samples = 0;
			u64 refill_pending = 0;
			u32 queue_min = UINT32_MAX, queue_max = 0;
			u32 current_buffer = 0, window_frames = 0;
			float window_peak = 0.0f, window_energy = 0.0f;
			while (!m_quit.load(std::memory_order_acquire))
			{
				{
					std::lock_guard lock(m_read_mutex);
					if (m_paused)
					{
						std::memset(m_samples, 0, sizeof(m_samples));
						paused_buffers++;
					}
					else
					{
						if (!ReadOutputFrames(input_short, refill_pending, queue_min, queue_max))
							return;
					}
				}
				float peak = 0.0f;
				for (u32 i = 0; i < GRAIN * 2; i++)
				{
					float sample = m_samples[i];
					if (!std::isfinite(sample))
					{
						invalid_samples++;
						sample = 0.0f;
					}
					clipped_samples += sample < -1.0f || sample > 1.0f;
					sample = std::clamp(sample, -1.0f, 1.0f);
					// Scaling/clamping before conversion keeps both signed endpoints valid.
					const s32 pcm = static_cast<s32>(sample * 32768.0f);
					m_pcm[current_buffer][i] = static_cast<s16>(std::clamp(pcm, -32768, 32767));
					peak = std::max(peak, std::abs(sample));
					window_energy += sample * sample;
				}
				// (AI-assisted) Alternate buffers as in DolphinPS4. Output paces the
				// loop; do not drain/restart the port for every block. Needs proper
				// testing with console capture, pause/resume and shutdown.
				const int result = sceAudioOutOutput(m_handle, m_pcm[current_buffer]);
				if (result < 0)
				{
					Fail("sceAudioOutOutput submit", result, nullptr);
					return;
				}
				current_buffer ^= 1;
				buffers++;
				if (peak > 0.00001f)
					pcm_buffers++;
				s_output.buffers.store(buffers, std::memory_order_relaxed);
				s_output.pcm_buffers.store(pcm_buffers, std::memory_order_relaxed);
				s_output.input_short.store(input_short, std::memory_order_relaxed);
				s_output.paused.store(paused_buffers, std::memory_order_relaxed);
				s_output.refill_pending.store(refill_pending, std::memory_order_relaxed);
				window_peak = std::max(window_peak, peak);
				window_frames += GRAIN;
				if (window_frames >= OUTPUT_RATE)
				{
					s_output.queue_min.store(queue_min == UINT32_MAX ? 0 : queue_min, std::memory_order_relaxed);
					s_output.queue_max.store(queue_max, std::memory_order_relaxed);
					queue_min = UINT32_MAX;
					queue_max = 0;
					s_output.peak_q15.store(static_cast<u32>(window_peak * 32768.0f), std::memory_order_relaxed);
					s_output.rms_q15.store(static_cast<u32>(std::sqrt(window_energy / (window_frames * 2)) * 32768.0f),
						std::memory_order_relaxed);
					s_output.invalid_samples.store(invalid_samples, std::memory_order_relaxed);
					s_output.clipped_samples.store(clipped_samples, std::memory_order_relaxed);
					window_frames = 0;
					window_peak = window_energy = 0.0f;
				}
			}
			const int result = sceAudioOutOutput(m_handle, nullptr);
			if (result < 0)
				Fail("sceAudioOutOutput final drain", result, nullptr);
		}

		int m_handle = -1;
		pthread_t m_thread{};
		bool m_thread_started = false;
		std::atomic<bool> m_quit{false};
		std::mutex m_read_mutex;
		SpeexResamplerState* m_resampler = nullptr;
		u32 m_source_offset = 0, m_source_frames = 0;
		alignas(64) float m_source[GRAIN * 2]{};
		alignas(64) float m_samples[GRAIN * 2]{};
		alignas(64) s16 m_pcm[2][GRAIN * 2]{};
	};
} // namespace

void PS4Audio::LogStatus()
{
	if (!s_output.attempted.load(std::memory_order_relaxed))
		return;
	ProbeLog("PS4 audio output: status=%d buffers=%llu pcm_buffers=%llu input_short=%llu paused=%llu\n",
		s_output.status.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(s_output.buffers.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(s_output.pcm_buffers.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(s_output.input_short.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(s_output.paused.load(std::memory_order_relaxed)));
	ProbeLog("PS4 audio queue: min_frames=%u max_frames=%u refill_pending=%llu\n",
		s_output.queue_min.load(std::memory_order_relaxed), s_output.queue_max.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(s_output.refill_pending.load(std::memory_order_relaxed)));
	ProbeLog("PS4 audio signal: peak_q15=%u rms_q15=%u invalid_samples=%llu clipped_samples=%llu "
			 "port_result=%#x output=%#x channels=%u volume_raw=%d flags=%#llx\n",
		s_output.peak_q15.load(std::memory_order_relaxed), s_output.rms_q15.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(s_output.invalid_samples.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(s_output.clipped_samples.load(std::memory_order_relaxed)),
		s_output.port_result.load(std::memory_order_relaxed), s_output.port_output.load(std::memory_order_relaxed),
		s_output.port_channels.load(std::memory_order_relaxed), s_output.port_volume.load(std::memory_order_relaxed),
		static_cast<unsigned long long>(s_output.port_flags.load(std::memory_order_relaxed)));
}

// (AI-assisted) Reuse the backend slot supplied by the Orbis shim; no cubeb library.
std::vector<std::pair<std::string, std::string>> AudioStream::GetCubebDriverNames()
{
	return {};
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetCubebOutputDevices(const char*)
{
	return {};
}

std::unique_ptr<AudioStream> AudioStream::CreateCubebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	const char*, const char*, bool stretch_enabled, Error* error)
{
	AudioStreamParameters stereo = parameters;
	stereo.expansion_mode = AudioExpansionMode::Disabled;
	auto stream = std::make_unique<PS4AudioStream>(sample_rate, stereo);
	if (!stream->Open(stretch_enabled, error))
		return nullptr;
	return stream;
}

std::unique_ptr<AudioStream> AudioStream::CreateSDLAudioStream(u32, const AudioStreamParameters&, bool, Error* error)
{
	Error::SetString(error, "SDL audio unavailable on PS4; use native AudioOut");
	return nullptr;
}
