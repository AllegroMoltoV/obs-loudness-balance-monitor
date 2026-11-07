#pragma once

#include "analysis-results.h"
#include "vad.h"

#include <ebur128.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace lbm {

struct PreviewAnalysisPoint {
	uint64_t frame_position{0};
	double voice_lufs{-HUGE_VAL};
	double bgm_lufs{-HUGE_VAL};
	double mix_lufs{-HUGE_VAL};
	double voice_peak_dbfs{-HUGE_VAL};
	double bgm_peak_dbfs{-HUGE_VAL};
	double mix_peak_dbfs{-HUGE_VAL};
	bool voice_active{false};
	std::optional<Status> balance_status;
	std::optional<Status> mix_status;
	std::optional<Status> clip_status;
};

struct PreviewAnalysisSummary {
	uint32_t valid_points{0};
	uint32_t mix_valid_points{0};
	// Frames distinguishes valid silence peaks from absent or failed analysis.
	uint64_t frames{0};
	double voice_lufs{-HUGE_VAL};
	double bgm_lufs{-HUGE_VAL};
	double mix_lufs{-HUGE_VAL};
	double voice_peak_dbfs{-HUGE_VAL};
	double bgm_peak_dbfs{-HUGE_VAL};
	double mix_peak_dbfs{-HUGE_VAL};
	std::optional<Status> balance_status;
	std::optional<Status> mix_status;
	std::optional<Status> clip_status;
};

// Confined to the recording worker. Read results only after worker publication
// or while holding the owner's synchronization lock.
class PreviewAnalysis {
public:
	static constexpr uint32_t kMaximumSeconds = 30;

	bool reset(uint32_t sample_rate, double vad_threshold, double target, double hysteresis,
		   double mix_ok = -18.0, double mix_warn = -22.0);
	// Inputs are time-aligned interleaved stereo, including silence. Returns false
	// on invalid input, excess duration, or library failure; discard its analysis.
	bool append(const float *voice_stereo, const float *bgm_stereo, uint32_t frames);
	const std::vector<PreviewAnalysisPoint> &series() const { return series_; }
	PreviewAnalysisSummary summary() const;
	// Latest completed 100 ms observation at or before playback position.
	const PreviewAnalysisPoint *point_at(uint64_t frame_position) const;
	uint64_t processed_frames() const { return processed_frames_; }

private:
	struct StateDeleter {
		void operator()(ebur128_state *state) const;
	};
	using State = std::unique_ptr<ebur128_state, StateDeleter>;
	void observe();
	Status basic_status(double delta) const;
	Status basic_mix_status(double lufs) const;
	uint64_t next_observation_frame() const;

	State voice_state_;
	State bgm_state_;
	State mix_state_;
	std::vector<float> mix_samples_;
	VoiceActivityDetector vad_;
	std::vector<float> vad_samples_;
	std::vector<PreviewAnalysisPoint> series_;
	uint32_t sample_rate_{0};
	uint64_t processed_frames_{0};
	uint64_t vad_blocks_{0};
	double target_{6.0};
	double hysteresis_{0.5};
	double mix_ok_{-18.0};
	double mix_warn_{-22.0};
	double voice_sum_{0.0};
	double bgm_sum_{0.0};
	double mix_sum_{0.0};
	// Linear peaks, independent of the LUFS windows and voice activity.
	double interval_voice_peak_{0.0};
	double interval_bgm_peak_{0.0};
	double interval_mix_peak_{0.0};
	double voice_peak_{0.0};
	double bgm_peak_{0.0};
	double mix_peak_{0.0};
	uint32_t valid_points_{0};
	uint32_t mix_valid_points_{0};
	std::optional<Status> dynamic_status_;
	std::optional<Status> dynamic_mix_status_;
	bool failed_{true};
};

} // namespace lbm
