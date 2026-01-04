#include "loudness-analyzer.h"
#include <ebur128.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace lbm {

LoudnessAnalyzer::LoudnessAnalyzer()
{
	mix_buffer_.reserve(AudioFrame::kMaxSamples);
	last_bgm_samples_.reserve(AudioFrame::kMaxSamples);
}

LoudnessAnalyzer::~LoudnessAnalyzer()
{
	stop();
	destroy_ebur128_states();
}

void LoudnessAnalyzer::start()
{
	if (running_.load(std::memory_order_relaxed)) {
		return;
	}

	init_ebur128_states();
	running_.store(true, std::memory_order_release);
	worker_thread_ = std::thread(&LoudnessAnalyzer::worker_loop, this);
}

void LoudnessAnalyzer::stop()
{
	if (!running_.load(std::memory_order_relaxed)) {
		return;
	}

	running_.store(false, std::memory_order_release);
	if (worker_thread_.joinable()) {
		worker_thread_.join();
	}
}

void LoudnessAnalyzer::push_voice_frame(const float *samples, uint32_t frames)
{
	if (!samples || frames == 0 || frames > AudioFrame::kMaxSamples) {
		return;
	}

	AudioFrame frame;
	frame.source_type = AudioFrame::SourceType::Voice;
	frame.frame_count = frames;
	std::memcpy(frame.samples, samples, frames * sizeof(float));

	// Update peak (in audio callback for accuracy)
	double peak = 0.0;
	for (uint32_t i = 0; i < frames; ++i) {
		double abs_val = std::fabs(samples[i]);
		if (abs_val > peak)
			peak = abs_val;
	}
	voice_peak_.store(peak, std::memory_order_relaxed);

	voice_queue_.try_push(frame);
}

void LoudnessAnalyzer::push_bgm_frame(const float *samples, uint32_t frames)
{
	if (!samples || frames == 0 || frames > AudioFrame::kMaxSamples) {
		return;
	}

	AudioFrame frame;
	frame.source_type = AudioFrame::SourceType::BGM;
	frame.frame_count = frames;
	std::memcpy(frame.samples, samples, frames * sizeof(float));

	// Update peak
	double peak = 0.0;
	for (uint32_t i = 0; i < frames; ++i) {
		double abs_val = std::fabs(samples[i]);
		if (abs_val > peak)
			peak = abs_val;
	}
	bgm_peak_.store(peak, std::memory_order_relaxed);

	bgm_queue_.try_push(frame);
}

void LoudnessAnalyzer::set_sample_rate(uint32_t sample_rate)
{
	if (sample_rate == sample_rate_.load(std::memory_order_relaxed)) {
		return;
	}

	sample_rate_.store(sample_rate, std::memory_order_relaxed);
	vad_.set_sample_rate(sample_rate);

	// Reinitialize libebur128 states with new sample rate
	if (running_.load(std::memory_order_relaxed)) {
		// Worker thread will handle reinitialization
		reset_states();
	}
}

void LoudnessAnalyzer::reset_states()
{
	// This will be called from main thread, but states are owned by worker
	// For now, just reset the LUFS results
	results_.voice_lufs.store(-HUGE_VAL, std::memory_order_relaxed);
	results_.bgm_lufs.store(-HUGE_VAL, std::memory_order_relaxed);
	results_.mix_lufs.store(-HUGE_VAL, std::memory_order_relaxed);
}

void LoudnessAnalyzer::worker_loop()
{
	while (running_.load(std::memory_order_acquire)) {
		AudioFrame voice_frame, bgm_frame;
		bool has_voice = voice_queue_.try_pop(voice_frame);
		bool has_bgm = bgm_queue_.try_pop(bgm_frame);

		if (!has_voice && !has_bgm) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		// Process voice
		if (has_voice) {
			process_voice(voice_frame);
		}

		// Process BGM
		if (has_bgm) {
			process_bgm(bgm_frame);
		}

		// Update judgments
		update_balance_judgment();
		update_mix_judgment();
		update_clip_judgment();
	}
}

void LoudnessAnalyzer::process_voice(const AudioFrame &frame)
{
	// Update VAD
	bool voice_active = vad_.update(frame.samples, frame.frame_count);
	results_.voice_active.store(voice_active, std::memory_order_relaxed);

	// Check for voice inactive transition
	if (prev_voice_active_ && !voice_active) {
		// Reset short-term windows when voice becomes inactive
		reset_ebur128_state(voice_state_);
		reset_ebur128_state(mix_state_);
	}
	prev_voice_active_ = voice_active;

	// Only process LUFS when voice is active
	if (voice_active && voice_state_) {
		ebur128_add_frames_float(voice_state_, frame.samples, frame.frame_count);
		update_voice_metrics();

		// Update mix (voice + last BGM)
		if (mix_state_ && last_bgm_frame_count_ > 0) {
			// Prepare mix buffer
			uint32_t mix_frames = std::min(frame.frame_count, last_bgm_frame_count_);
			mix_buffer_.resize(mix_frames);

			for (uint32_t i = 0; i < mix_frames; ++i) {
				mix_buffer_[i] = frame.samples[i] + last_bgm_samples_[i];
			}

			// Calculate mix peak
			double mix_pk = 0.0;
			for (uint32_t i = 0; i < mix_frames; ++i) {
				double abs_val = std::fabs(mix_buffer_[i]);
				if (abs_val > mix_pk)
					mix_pk = abs_val;
			}
			mix_peak_.store(mix_pk, std::memory_order_relaxed);

			ebur128_add_frames_float(mix_state_, mix_buffer_.data(), mix_frames);
			update_mix_metrics();
		}
	}
}

void LoudnessAnalyzer::process_bgm(const AudioFrame &frame)
{
	if (!bgm_state_) {
		return;
	}

	// Store last BGM samples for mix calculation
	last_bgm_frame_count_ = frame.frame_count;
	last_bgm_samples_.resize(frame.frame_count);
	std::memcpy(last_bgm_samples_.data(), frame.samples, frame.frame_count * sizeof(float));

	ebur128_add_frames_float(bgm_state_, frame.samples, frame.frame_count);
	update_bgm_metrics();
}

void LoudnessAnalyzer::update_voice_metrics()
{
	if (!voice_state_)
		return;

	double lufs = -HUGE_VAL;
	if (ebur128_loudness_shortterm(voice_state_, &lufs) == EBUR128_SUCCESS) {
		results_.voice_lufs.store(lufs, std::memory_order_relaxed);
	}

	// Peak in dBFS
	double peak = voice_peak_.load(std::memory_order_relaxed);
	double peak_dbfs = (peak > 0.0) ? 20.0 * std::log10(peak) : -HUGE_VAL;
	results_.voice_peak_dbfs.store(peak_dbfs, std::memory_order_relaxed);
}

void LoudnessAnalyzer::update_bgm_metrics()
{
	if (!bgm_state_)
		return;

	double lufs = -HUGE_VAL;
	if (ebur128_loudness_shortterm(bgm_state_, &lufs) == EBUR128_SUCCESS) {
		results_.bgm_lufs.store(lufs, std::memory_order_relaxed);
	}

	// Peak in dBFS
	double peak = bgm_peak_.load(std::memory_order_relaxed);
	double peak_dbfs = (peak > 0.0) ? 20.0 * std::log10(peak) : -HUGE_VAL;
	results_.bgm_peak_dbfs.store(peak_dbfs, std::memory_order_relaxed);
}

void LoudnessAnalyzer::update_mix_metrics()
{
	if (!mix_state_)
		return;

	double lufs = -HUGE_VAL;
	if (ebur128_loudness_shortterm(mix_state_, &lufs) == EBUR128_SUCCESS) {
		results_.mix_lufs.store(lufs, std::memory_order_relaxed);
	}

	// Peak in dBFS
	double peak = mix_peak_.load(std::memory_order_relaxed);
	double peak_dbfs = (peak > 0.0) ? 20.0 * std::log10(peak) : -HUGE_VAL;
	results_.mix_peak_dbfs.store(peak_dbfs, std::memory_order_relaxed);
}

void LoudnessAnalyzer::update_balance_judgment()
{
	double voice = results_.voice_lufs.load(std::memory_order_relaxed);
	double bgm = results_.bgm_lufs.load(std::memory_order_relaxed);

	if (voice == -HUGE_VAL || bgm == -HUGE_VAL) {
		return; // Keep previous state
	}

	double delta = voice - bgm;
	results_.balance_delta.store(delta, std::memory_order_relaxed);

	double target = config_.balance_target.load(std::memory_order_relaxed);
	double hyst = config_.hysteresis.load(std::memory_order_relaxed);

	auto current = results_.balance_status.load(std::memory_order_relaxed);
	Status new_status = current;

	// OK: delta >= target
	// WARN: target-3 <= delta < target
	// BAD: delta < target-3
	double ok_thresh = target;
	double warn_thresh = target - 3.0;

	if (delta >= ok_thresh + hyst) {
		new_status = Status::OK;
	} else if (delta < warn_thresh - hyst) {
		new_status = Status::BAD;
	} else if (delta < ok_thresh - hyst && delta >= warn_thresh + hyst) {
		new_status = Status::WARN;
	}
	// Otherwise keep current state (hysteresis zone)

	results_.balance_status.store(new_status, std::memory_order_relaxed);
}

void LoudnessAnalyzer::update_mix_judgment()
{
	double mix = results_.mix_lufs.load(std::memory_order_relaxed);

	if (mix == -HUGE_VAL) {
		return;
	}

	double ok_thresh = config_.mix_ok_threshold.load(std::memory_order_relaxed);
	double warn_thresh = config_.mix_warn_threshold.load(std::memory_order_relaxed);
	double hyst = config_.hysteresis.load(std::memory_order_relaxed);

	auto current = results_.mix_status.load(std::memory_order_relaxed);
	Status new_status = current;

	if (mix >= ok_thresh + hyst) {
		new_status = Status::OK;
	} else if (mix < warn_thresh - hyst) {
		new_status = Status::BAD;
	} else if (mix < ok_thresh - hyst && mix >= warn_thresh + hyst) {
		new_status = Status::WARN;
	}

	results_.mix_status.store(new_status, std::memory_order_relaxed);
}

void LoudnessAnalyzer::update_clip_judgment()
{
	double voice_peak = results_.voice_peak_dbfs.load(std::memory_order_relaxed);
	double bgm_peak = results_.bgm_peak_dbfs.load(std::memory_order_relaxed);
	double mix_peak = results_.mix_peak_dbfs.load(std::memory_order_relaxed);

	double max_peak = std::max({voice_peak, bgm_peak, mix_peak});

	Status status;
	if (max_peak >= AnalysisConfig::kClipBadThreshold) {
		status = Status::BAD;
	} else if (max_peak >= AnalysisConfig::kClipWarnThreshold) {
		status = Status::WARN;
	} else {
		status = Status::OK;
	}

	results_.clip_status.store(status, std::memory_order_relaxed);
}

void LoudnessAnalyzer::init_ebur128_states()
{
	destroy_ebur128_states();

	uint32_t sr = sample_rate_.load(std::memory_order_relaxed);
	unsigned int mode = EBUR128_MODE_S; // Short-term loudness (3s window)

	// All states are mono (we downmix to mono before processing)
	voice_state_ = ebur128_init(1, sr, mode);
	bgm_state_ = ebur128_init(1, sr, mode);
	mix_state_ = ebur128_init(1, sr, mode);

	if (voice_state_) {
		ebur128_set_channel(voice_state_, 0, EBUR128_CENTER);
	}
	if (bgm_state_) {
		ebur128_set_channel(bgm_state_, 0, EBUR128_CENTER);
	}
	if (mix_state_) {
		ebur128_set_channel(mix_state_, 0, EBUR128_CENTER);
	}
}

void LoudnessAnalyzer::destroy_ebur128_states()
{
	if (voice_state_) {
		ebur128_destroy(&voice_state_);
		voice_state_ = nullptr;
	}
	if (bgm_state_) {
		ebur128_destroy(&bgm_state_);
		bgm_state_ = nullptr;
	}
	if (mix_state_) {
		ebur128_destroy(&mix_state_);
		mix_state_ = nullptr;
	}
}

void LoudnessAnalyzer::reset_ebur128_state(ebur128_state *&state)
{
	if (!state)
		return;

	uint32_t sr = sample_rate_.load(std::memory_order_relaxed);
	unsigned int mode = EBUR128_MODE_S;

	ebur128_destroy(&state);
	state = ebur128_init(1, sr, mode);
	if (state) {
		ebur128_set_channel(state, 0, EBUR128_CENTER);
	}
}

} // namespace lbm
