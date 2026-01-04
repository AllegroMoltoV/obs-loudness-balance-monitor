#include "vad.h"
#include <cmath>

namespace lbm {

VoiceActivityDetector::VoiceActivityDetector()
{
	set_sample_rate(48000);
}

void VoiceActivityDetector::set_threshold(double threshold_dbfs)
{
	threshold_dbfs_.store(threshold_dbfs, std::memory_order_relaxed);
}

void VoiceActivityDetector::set_attack_time(double attack_ms)
{
	uint32_t sr = sample_rate_.load(std::memory_order_relaxed);
	uint32_t samples = static_cast<uint32_t>(attack_ms * sr / 1000.0);
	attack_samples_.store(samples, std::memory_order_relaxed);
}

void VoiceActivityDetector::set_release_time(double release_ms)
{
	uint32_t sr = sample_rate_.load(std::memory_order_relaxed);
	uint32_t samples = static_cast<uint32_t>(release_ms * sr / 1000.0);
	release_samples_.store(samples, std::memory_order_relaxed);
}

void VoiceActivityDetector::set_sample_rate(uint32_t sample_rate)
{
	sample_rate_.store(sample_rate, std::memory_order_relaxed);
	// Recalculate sample counts with default timing
	set_attack_time(kDefaultAttackMs);
	set_release_time(kDefaultReleaseMs);
}

double VoiceActivityDetector::attack_time_ms() const
{
	uint32_t sr = sample_rate_.load(std::memory_order_relaxed);
	uint32_t samples = attack_samples_.load(std::memory_order_relaxed);
	return (sr > 0) ? (samples * 1000.0 / sr) : kDefaultAttackMs;
}

double VoiceActivityDetector::release_time_ms() const
{
	uint32_t sr = sample_rate_.load(std::memory_order_relaxed);
	uint32_t samples = release_samples_.load(std::memory_order_relaxed);
	return (sr > 0) ? (samples * 1000.0 / sr) : kDefaultReleaseMs;
}

bool VoiceActivityDetector::update(const float *samples, uint32_t frame_count)
{
	if (!samples || frame_count == 0) {
		return is_active_.load(std::memory_order_relaxed);
	}

	double level_dbfs = calculate_rms_dbfs(samples, frame_count);
	double threshold = threshold_dbfs_.load(std::memory_order_relaxed);
	bool above_threshold = (level_dbfs >= threshold);

	uint32_t attack_target = attack_samples_.load(std::memory_order_relaxed);
	uint32_t release_target = release_samples_.load(std::memory_order_relaxed);
	bool is_active = is_active_.load(std::memory_order_relaxed);

	if (above_threshold) {
		// Reset release counter when above threshold
		release_counter_.store(0, std::memory_order_relaxed);

		if (!is_active) {
			// Accumulate attack counter
			uint32_t current = attack_counter_.load(std::memory_order_relaxed);
			current += frame_count;
			attack_counter_.store(current, std::memory_order_relaxed);

			if (current >= attack_target) {
				is_active_.store(true, std::memory_order_relaxed);
				attack_counter_.store(0, std::memory_order_relaxed);
			}
		}
	} else {
		// Reset attack counter when below threshold
		attack_counter_.store(0, std::memory_order_relaxed);

		if (is_active) {
			// Accumulate release counter
			uint32_t current = release_counter_.load(std::memory_order_relaxed);
			current += frame_count;
			release_counter_.store(current, std::memory_order_relaxed);

			if (current >= release_target) {
				is_active_.store(false, std::memory_order_relaxed);
				release_counter_.store(0, std::memory_order_relaxed);
			}
		}
	}

	return is_active_.load(std::memory_order_relaxed);
}

void VoiceActivityDetector::reset()
{
	attack_counter_.store(0, std::memory_order_relaxed);
	release_counter_.store(0, std::memory_order_relaxed);
	is_active_.store(false, std::memory_order_relaxed);
}

double VoiceActivityDetector::calculate_rms_dbfs(const float *samples, uint32_t frame_count) const
{
	if (!samples || frame_count == 0) {
		return -HUGE_VAL;
	}

	double sum_sq = 0.0;
	for (uint32_t i = 0; i < frame_count; ++i) {
		double s = static_cast<double>(samples[i]);
		sum_sq += s * s;
	}

	double rms = std::sqrt(sum_sq / frame_count);
	if (rms <= 0.0) {
		return -HUGE_VAL;
	}

	return 20.0 * std::log10(rms);
}

} // namespace lbm
