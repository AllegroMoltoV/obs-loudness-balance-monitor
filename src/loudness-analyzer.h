#pragma once

#include "analysis-results.h"
#include "audio-frame.h"
#include "spsc-queue.h"
#include "vad.h"

#include <ebur128.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace lbm {

class LoudnessAnalyzer {
public:
	LoudnessAnalyzer();
	~LoudnessAnalyzer();

	// Non-copyable
	LoudnessAnalyzer(const LoudnessAnalyzer &) = delete;
	LoudnessAnalyzer &operator=(const LoudnessAnalyzer &) = delete;

	// Start/stop worker thread
	void start();
	void stop();
	bool is_running() const { return running_.load(std::memory_order_relaxed); }

	// Push audio frames from audio callback (producer side)
	// These must be called from audio callback thread only
	void push_voice_frame(const float *samples, uint32_t frames);
	void push_bgm_frame(const float *samples, uint32_t frames);

	// Get analysis results (consumer side, UI thread)
	const AnalysisResults &results() const { return results_; }
	AnalysisResults &results() { return results_; }

	// Get/set configuration
	const AnalysisConfig &config() const { return config_; }
	AnalysisConfig &config() { return config_; }

	// Set sample rate (called when OBS audio config changes)
	void set_sample_rate(uint32_t sample_rate);
	uint32_t sample_rate() const { return sample_rate_.load(std::memory_order_relaxed); }

	// Reset all LUFS states (called when VAD transitions from active to inactive)
	void reset_states();

private:
	void worker_loop();

	// Process voice audio
	void process_voice(const AudioFrame &frame);

	// Process BGM audio
	void process_bgm(const AudioFrame &frame);

	// Update metrics from libebur128 states
	void update_voice_metrics();
	void update_bgm_metrics();
	void update_mix_metrics();

	// Update judgments based on current metrics
	void update_balance_judgment();
	void update_mix_judgment();
	void update_clip_judgment();

	// Initialize/destroy libebur128 states
	void init_ebur128_states();
	void destroy_ebur128_states();
	void reset_ebur128_state(ebur128_state *&state);

	// Worker thread
	std::thread worker_thread_;
	std::atomic<bool> running_{false};

	// Audio queues (lock-free)
	SPSCQueue<AudioFrame, 256> voice_queue_;
	SPSCQueue<AudioFrame, 256> bgm_queue_;

	// libebur128 states (owned by worker thread)
	ebur128_state *voice_state_{nullptr};
	ebur128_state *bgm_state_{nullptr};
	ebur128_state *mix_state_{nullptr};

	// Mix buffer for combining voice + bgm
	std::vector<float> mix_buffer_;

	// VAD
	VoiceActivityDetector vad_;
	bool prev_voice_active_{false};

	// Peak tracking (per-frame max)
	std::atomic<double> voice_peak_{0.0};
	std::atomic<double> bgm_peak_{0.0};
	std::atomic<double> mix_peak_{0.0};

	// Sample rate
	std::atomic<uint32_t> sample_rate_{48000};

	// Results and config
	AnalysisResults results_;
	AnalysisConfig config_;

	// Last BGM samples for mix calculation
	std::vector<float> last_bgm_samples_;
	uint32_t last_bgm_frame_count_{0};
};

} // namespace lbm
