#include "audio-capture.h"
#include "audio-frame.h"

#include <algorithm>
#include <cstring>

namespace lbm {

thread_local std::vector<float> AudioCaptureManager::downmix_buffer_;

AudioCaptureManager::AudioCaptureManager(LoudnessAnalyzer &analyzer) : analyzer_(analyzer) {}

AudioCaptureManager::~AudioCaptureManager()
{
	unregister_voice_callback();

	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &bgm : bgm_sources_) {
		if (bgm.source) {
			obs_source_remove_audio_capture_callback(bgm.source, bgm_audio_callback, this);
			obs_source_release(bgm.source);
		}
	}
	bgm_sources_.clear();
}

void AudioCaptureManager::set_voice_source(const std::string &source_name)
{
	std::lock_guard<std::mutex> lock(mutex_);

	if (voice_source_name_ == source_name) {
		return;
	}

	unregister_voice_callback();
	voice_source_name_ = source_name;
	register_voice_callback();
}

std::string AudioCaptureManager::voice_source_name() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return voice_source_name_;
}

bool AudioCaptureManager::has_voice_source() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return voice_source_ != nullptr;
}

void AudioCaptureManager::add_bgm_source(const std::string &source_name)
{
	std::lock_guard<std::mutex> lock(mutex_);

	// Check if already added
	for (const auto &bgm : bgm_sources_) {
		if (bgm.name == source_name) {
			return;
		}
	}

	obs_source_t *source = obs_get_source_by_name(source_name.c_str());
	if (!source) {
		return;
	}

	BGMSource bgm;
	bgm.name = source_name;
	bgm.source = source;

	obs_source_add_audio_capture_callback(source, bgm_audio_callback, this);
	bgm_sources_.push_back(bgm);
}

void AudioCaptureManager::remove_bgm_source(const std::string &source_name)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto it = std::find_if(bgm_sources_.begin(), bgm_sources_.end(),
			       [&source_name](const BGMSource &bgm) { return bgm.name == source_name; });

	if (it != bgm_sources_.end()) {
		if (it->source) {
			obs_source_remove_audio_capture_callback(it->source, bgm_audio_callback, this);
			obs_source_release(it->source);
		}
		bgm_sources_.erase(it);
	}
}

void AudioCaptureManager::clear_bgm_sources()
{
	std::lock_guard<std::mutex> lock(mutex_);

	for (auto &bgm : bgm_sources_) {
		if (bgm.source) {
			obs_source_remove_audio_capture_callback(bgm.source, bgm_audio_callback, this);
			obs_source_release(bgm.source);
		}
	}
	bgm_sources_.clear();
}

std::vector<std::string> AudioCaptureManager::bgm_source_names() const
{
	std::lock_guard<std::mutex> lock(mutex_);

	std::vector<std::string> names;
	names.reserve(bgm_sources_.size());
	for (const auto &bgm : bgm_sources_) {
		names.push_back(bgm.name);
	}
	return names;
}

bool AudioCaptureManager::has_bgm_sources() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return !bgm_sources_.empty();
}

std::vector<std::string> AudioCaptureManager::enumerate_audio_sources()
{
	std::vector<std::string> sources;

	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			auto *sources = static_cast<std::vector<std::string> *>(param);

			uint32_t flags = obs_source_get_output_flags(source);
			if (flags & OBS_SOURCE_AUDIO) {
				const char *name = obs_source_get_name(source);
				if (name && name[0] != '\0') {
					sources->push_back(name);
				}
			}
			return true; // continue enumeration
		},
		&sources);

	return sources;
}

void AudioCaptureManager::save_settings(obs_data_t *settings) const
{
	std::lock_guard<std::mutex> lock(mutex_);

	obs_data_set_string(settings, "voice_source", voice_source_name_.c_str());

	obs_data_array_t *bgm_array = obs_data_array_create();
	for (const auto &bgm : bgm_sources_) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", bgm.name.c_str());
		obs_data_array_push_back(bgm_array, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, "bgm_sources", bgm_array);
	obs_data_array_release(bgm_array);
}

void AudioCaptureManager::load_settings(obs_data_t *settings)
{
	// Load voice source
	const char *voice_name = obs_data_get_string(settings, "voice_source");
	if (voice_name && voice_name[0] != '\0') {
		set_voice_source(voice_name);
	}

	// Load BGM sources
	obs_data_array_t *bgm_array = obs_data_get_array(settings, "bgm_sources");
	if (bgm_array) {
		size_t count = obs_data_array_count(bgm_array);
		for (size_t i = 0; i < count; ++i) {
			obs_data_t *item = obs_data_array_item(bgm_array, i);
			const char *name = obs_data_get_string(item, "name");
			if (name && name[0] != '\0') {
				add_bgm_source(name);
			}
			obs_data_release(item);
		}
		obs_data_array_release(bgm_array);
	}
}

void AudioCaptureManager::voice_audio_callback(void *param, obs_source_t *source, const audio_data *audio, bool muted)
{
	if (!param || !audio || !audio->data[0] || muted) {
		return;
	}

	if (audio->frames == 0 || audio->frames > AudioFrame::kMaxSamples) {
		return;
	}

	auto *self = static_cast<AudioCaptureManager *>(param);

	// Get volume fader value (0.0 to 1.0+)
	float volume = obs_source_get_volume(source);

	// Downmix to mono
	downmix_buffer_.resize(audio->frames);
	downmix_to_mono(audio, downmix_buffer_.data(), audio->frames);

	// Apply volume fader
	apply_volume(downmix_buffer_.data(), audio->frames, volume);

	// Push to analyzer
	self->analyzer_.push_voice_frame(downmix_buffer_.data(), audio->frames);
}

void AudioCaptureManager::bgm_audio_callback(void *param, obs_source_t *source, const audio_data *audio, bool muted)
{
	if (!param || !audio || !audio->data[0] || muted) {
		return;
	}

	if (audio->frames == 0 || audio->frames > AudioFrame::kMaxSamples) {
		return;
	}

	auto *self = static_cast<AudioCaptureManager *>(param);

	// Get volume fader value (0.0 to 1.0+)
	float volume = obs_source_get_volume(source);

	// Downmix to mono
	downmix_buffer_.resize(audio->frames);
	downmix_to_mono(audio, downmix_buffer_.data(), audio->frames);

	// Apply volume fader
	apply_volume(downmix_buffer_.data(), audio->frames, volume);

	// Push to analyzer
	self->analyzer_.push_bgm_frame(downmix_buffer_.data(), audio->frames);
}

void AudioCaptureManager::register_voice_callback()
{
	if (voice_source_name_.empty()) {
		return;
	}

	voice_source_ = obs_get_source_by_name(voice_source_name_.c_str());
	if (!voice_source_) {
		return;
	}

	obs_source_add_audio_capture_callback(voice_source_, voice_audio_callback, this);
}

void AudioCaptureManager::unregister_voice_callback()
{
	if (voice_source_) {
		obs_source_remove_audio_capture_callback(voice_source_, voice_audio_callback, this);
		obs_source_release(voice_source_);
		voice_source_ = nullptr;
	}
}

void AudioCaptureManager::register_bgm_callback(obs_source_t *source)
{
	if (source) {
		obs_source_add_audio_capture_callback(source, bgm_audio_callback, this);
	}
}

void AudioCaptureManager::unregister_bgm_callback(obs_source_t *source)
{
	if (source) {
		obs_source_remove_audio_capture_callback(source, bgm_audio_callback, this);
	}
}

void AudioCaptureManager::downmix_to_mono(const audio_data *audio, float *out, uint32_t frames)
{
	const float *ch0 = reinterpret_cast<const float *>(audio->data[0]);
	const float *ch1 = audio->data[1] ? reinterpret_cast<const float *>(audio->data[1]) : nullptr;

	if (ch1) {
		// Stereo -> mono (average)
		for (uint32_t i = 0; i < frames; ++i) {
			out[i] = (ch0[i] + ch1[i]) * 0.5f;
		}
	} else {
		// Already mono
		std::memcpy(out, ch0, frames * sizeof(float));
	}
}

void AudioCaptureManager::apply_volume(float *samples, uint32_t frames, float volume)
{
	// Skip if volume is 1.0 (no change needed)
	if (volume == 1.0f) {
		return;
	}

	for (uint32_t i = 0; i < frames; ++i) {
		samples[i] *= volume;
	}
}

} // namespace lbm
