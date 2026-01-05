#pragma once

#include "loudness-analyzer.h"

#include <obs.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lbm {

class AudioCaptureManager {
public:
	explicit AudioCaptureManager(LoudnessAnalyzer &analyzer);
	~AudioCaptureManager();

	// Non-copyable
	AudioCaptureManager(const AudioCaptureManager &) = delete;
	AudioCaptureManager &operator=(const AudioCaptureManager &) = delete;

	// Voice source selection (single)
	void set_voice_source(const std::string &source_name);
	std::string voice_source_name() const;
	bool has_voice_source() const;

	// BGM source selection (multiple)
	void add_bgm_source(const std::string &source_name);
	void remove_bgm_source(const std::string &source_name);
	void clear_bgm_sources();
	std::vector<std::string> bgm_source_names() const;
	bool has_bgm_sources() const;

	// Enumerate all audio-capable sources
	static std::vector<std::string> enumerate_audio_sources();

	// Settings persistence
	void save_settings(obs_data_t *settings) const;
	void load_settings(obs_data_t *settings);

private:
	// Audio capture callbacks (static for OBS API)
	static void voice_audio_callback(void *param, obs_source_t *source, const audio_data *audio, bool muted);
	static void bgm_audio_callback(void *param, obs_source_t *source, const audio_data *audio, bool muted);

	// Internal helpers
	void register_voice_callback();
	void unregister_voice_callback();
	void register_bgm_callback(obs_source_t *source);
	void unregister_bgm_callback(obs_source_t *source);

	// Downmix stereo to mono
	static void downmix_to_mono(const audio_data *audio, float *out, uint32_t frames);

	// Apply volume multiplier to samples
	static void apply_volume(float *samples, uint32_t frames, float volume);

	// Reference to analyzer
	LoudnessAnalyzer &analyzer_;

	// Voice source
	std::string voice_source_name_;
	obs_source_t *voice_source_{nullptr};

	// BGM sources
	struct BGMSource {
		std::string name;
		obs_source_t *source{nullptr};
	};
	std::vector<BGMSource> bgm_sources_;

	// Mutex for source management (not audio callback)
	mutable std::mutex mutex_;

	// Temporary buffer for downmixing (avoid allocation in callback)
	static thread_local std::vector<float> downmix_buffer_;
};

} // namespace lbm
