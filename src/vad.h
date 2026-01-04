#pragma once

#include <atomic>
#include <cstdint>

namespace lbm {

// Voice Activity Detector using threshold-based detection with attack/release
class VoiceActivityDetector {
public:
	VoiceActivityDetector();
	~VoiceActivityDetector() = default;

	// Configuration
	void set_threshold(double threshold_dbfs);
	void set_attack_time(double attack_ms);
	void set_release_time(double release_ms);
	void set_sample_rate(uint32_t sample_rate);

	// Get current configuration
	double threshold() const { return threshold_dbfs_.load(std::memory_order_relaxed); }
	double attack_time_ms() const;
	double release_time_ms() const;

	// Process audio block and update VAD state
	// Returns true if voice is currently active
	bool update(const float *samples, uint32_t frame_count);

	// Get current VAD state
	bool is_active() const { return is_active_.load(std::memory_order_relaxed); }

	// Reset VAD state
	void reset();

private:
	// Calculate RMS level in dBFS
	double calculate_rms_dbfs(const float *samples, uint32_t frame_count) const;

	// Configuration (atomic for thread-safe access)
	std::atomic<double> threshold_dbfs_{-40.0};
	std::atomic<uint32_t> attack_samples_{0};
	std::atomic<uint32_t> release_samples_{0};
	std::atomic<uint32_t> sample_rate_{48000};

	// State
	std::atomic<uint32_t> attack_counter_{0};
	std::atomic<uint32_t> release_counter_{0};
	std::atomic<bool> is_active_{false};

	// Default timing values
	static constexpr double kDefaultAttackMs = 150.0;
	static constexpr double kDefaultReleaseMs = 600.0;
};

} // namespace lbm
