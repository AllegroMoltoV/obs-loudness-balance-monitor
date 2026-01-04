#pragma once

#include <atomic>
#include <cmath>

namespace lbm {

// Status for each judgment
enum class Status { OK, WARN, BAD };

// Analysis results shared between worker thread and UI thread
// All members are atomic for thread-safe access
struct AnalysisResults {
	// Voice metrics
	std::atomic<double> voice_lufs{-HUGE_VAL};
	std::atomic<double> voice_peak_dbfs{-HUGE_VAL};

	// BGM metrics (sum of selected sources)
	std::atomic<double> bgm_lufs{-HUGE_VAL};
	std::atomic<double> bgm_peak_dbfs{-HUGE_VAL};

	// Mix metrics (Voice + BGM)
	std::atomic<double> mix_lufs{-HUGE_VAL};
	std::atomic<double> mix_peak_dbfs{-HUGE_VAL};

	// Voice-BGM delta (in LU)
	std::atomic<double> balance_delta{0.0};

	// Voice Activity
	std::atomic<bool> voice_active{false};

	// Judgments
	std::atomic<Status> balance_status{Status::OK};
	std::atomic<Status> mix_status{Status::OK};
	std::atomic<Status> clip_status{Status::OK};

	// Reset all values
	void reset()
	{
		voice_lufs.store(-HUGE_VAL, std::memory_order_relaxed);
		voice_peak_dbfs.store(-HUGE_VAL, std::memory_order_relaxed);
		bgm_lufs.store(-HUGE_VAL, std::memory_order_relaxed);
		bgm_peak_dbfs.store(-HUGE_VAL, std::memory_order_relaxed);
		mix_lufs.store(-HUGE_VAL, std::memory_order_relaxed);
		mix_peak_dbfs.store(-HUGE_VAL, std::memory_order_relaxed);
		balance_delta.store(0.0, std::memory_order_relaxed);
		voice_active.store(false, std::memory_order_relaxed);
		balance_status.store(Status::OK, std::memory_order_relaxed);
		mix_status.store(Status::OK, std::memory_order_relaxed);
		clip_status.store(Status::OK, std::memory_order_relaxed);
	}
};

// Configuration for thresholds (atomic for runtime adjustment)
struct AnalysisConfig {
	// VAD threshold in dBFS
	std::atomic<double> vad_threshold{-40.0};

	// Balance target (Voice - BGM delta, in LU)
	std::atomic<double> balance_target{6.0};

	// Balance thresholds: OK >= target, WARN >= target-3, BAD < target-3
	// (target=6 means: OK>=6, WARN: 3-6, BAD<3)

	// Mix loudness thresholds (LUFS)
	std::atomic<double> mix_ok_threshold{-18.0};
	std::atomic<double> mix_warn_threshold{-22.0};

	// Hysteresis to prevent flickering (dB)
	std::atomic<double> hysteresis{0.5};

	// Clip detection thresholds (dBFS)
	static constexpr double kClipWarnThreshold = -1.0;
	static constexpr double kClipBadThreshold = 0.0;
};

} // namespace lbm
