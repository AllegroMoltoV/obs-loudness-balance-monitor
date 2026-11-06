#include "preview-analysis.h"

#include <algorithm>
#include <cmath>

namespace lbm {

void PreviewAnalysis::StateDeleter::operator()(ebur128_state *state) const
{
	if (state)
		ebur128_destroy(&state);
}

bool PreviewAnalysis::reset(uint32_t sample_rate, double vad_threshold, double target, double hysteresis)
{
	voice_state_.reset();
	bgm_state_.reset();
	series_.clear();
	vad_samples_.clear();
	vad_.reset();
	processed_frames_ = 0;
	vad_blocks_ = 0;
	valid_points_ = 0;
	voice_sum_ = bgm_sum_ = 0.0;
	dynamic_status_.reset();
	failed_ = true;
	sample_rate_ = 0;
	if (sample_rate < 100 || !std::isfinite(vad_threshold) || !std::isfinite(target) ||
	    !std::isfinite(hysteresis) || hysteresis < 0.0)
		return false;

	sample_rate_ = sample_rate;
	target_ = target;
	hysteresis_ = hysteresis;
	vad_.set_sample_rate(sample_rate);
	vad_.set_threshold(vad_threshold);
	series_.reserve(kMaximumSeconds * 10);
	vad_samples_.reserve((static_cast<uint64_t>(sample_rate) + 99) / 100);
	voice_state_.reset(ebur128_init(2, sample_rate, EBUR128_MODE_S));
	bgm_state_.reset(ebur128_init(2, sample_rate, EBUR128_MODE_S));
	if (!voice_state_ || !bgm_state_)
		return false;
	for (auto *state : {voice_state_.get(), bgm_state_.get()}) {
		if (ebur128_set_channel(state, 0, EBUR128_LEFT) != EBUR128_SUCCESS ||
		    ebur128_set_channel(state, 1, EBUR128_RIGHT) != EBUR128_SUCCESS)
			return false;
	}
	failed_ = false;
	return true;
}

uint64_t PreviewAnalysis::next_observation_frame() const
{
	// Rounded up so a point never includes fewer than its elapsed-time samples.
	return ((series_.size() + 1) * static_cast<uint64_t>(sample_rate_) + 9) / 10;
}

bool PreviewAnalysis::append(const float *voice_stereo, const float *bgm_stereo, uint32_t frames)
{
	if (failed_)
		return false;
	if (frames == 0)
		return true;
	if (!voice_stereo || !bgm_stereo ||
	    processed_frames_ + frames > static_cast<uint64_t>(sample_rate_) * kMaximumSeconds) {
		failed_ = true;
		return false;
	}
	for (uint64_t i = 0; i < static_cast<uint64_t>(frames) * 2; ++i) {
		if (!std::isfinite(voice_stereo[i]) || !std::isfinite(bgm_stereo[i])) {
			failed_ = true;
			return false;
		}
	}
	uint32_t offset = 0;
	while (offset < frames) {
		const uint64_t next_vad_frame = ((vad_blocks_ + 1) * sample_rate_ + 99) / 100;
		const auto count = static_cast<uint32_t>(std::min<uint64_t>(
			frames - offset, std::min(next_observation_frame(), next_vad_frame) - processed_frames_));
		const auto *voice = voice_stereo + static_cast<uint64_t>(offset) * 2;
		const auto *bgm = bgm_stereo + static_cast<uint64_t>(offset) * 2;
		if (ebur128_add_frames_float(voice_state_.get(), voice, count) != EBUR128_SUCCESS ||
		    ebur128_add_frames_float(bgm_state_.get(), bgm, count) != EBUR128_SUCCESS) {
			failed_ = true;
			return false;
		}
		for (uint32_t i = 0; i < count; ++i) {
			const double left = voice[static_cast<uint64_t>(i) * 2];
			const double right = voice[static_cast<uint64_t>(i) * 2 + 1];
			// VAD only measures RMS: preserve stereo energy without phase cancellation.
			vad_samples_.push_back(static_cast<float>(std::sqrt((left * left + right * right) * 0.5)));
		}
		processed_frames_ += count;
		offset += count;
		if (processed_frames_ == next_vad_frame) {
			vad_.update(vad_samples_.data(), static_cast<uint32_t>(vad_samples_.size()));
			vad_samples_.clear();
			++vad_blocks_;
		}
		if (processed_frames_ == next_observation_frame())
			observe();
	}
	return true;
}

Status PreviewAnalysis::basic_status(double delta) const
{
	return delta >= target_ ? Status::OK : (delta >= target_ - 3.0 ? Status::WARN : Status::BAD);
}

void PreviewAnalysis::observe()
{
	PreviewAnalysisPoint point;
	point.frame_position = processed_frames_;
	point.voice_active = vad_.is_active();
	if (processed_frames_ >= static_cast<uint64_t>(sample_rate_) * 3) {
		double voice = -HUGE_VAL;
		double bgm = -HUGE_VAL;
		if (ebur128_loudness_shortterm(voice_state_.get(), &voice) == EBUR128_SUCCESS && std::isfinite(voice))
			point.voice_lufs = voice;
		if (ebur128_loudness_shortterm(bgm_state_.get(), &bgm) == EBUR128_SUCCESS && std::isfinite(bgm))
			point.bgm_lufs = bgm;
	}
	if (point.voice_active && std::isfinite(point.voice_lufs) && std::isfinite(point.bgm_lufs)) {
		const double delta = point.voice_lufs - point.bgm_lufs;
		if (!dynamic_status_) {
			dynamic_status_ = basic_status(delta);
		} else if (delta >= target_ + hysteresis_) {
			dynamic_status_ = Status::OK;
		} else if (delta < target_ - 3.0 - hysteresis_) {
			dynamic_status_ = Status::BAD;
		} else if (delta < target_ - hysteresis_ && delta >= target_ - 3.0 + hysteresis_) {
			dynamic_status_ = Status::WARN;
		}
		point.balance_status = dynamic_status_;
		voice_sum_ += point.voice_lufs;
		bgm_sum_ += point.bgm_lufs;
		++valid_points_;
	}
	series_.push_back(point);
}

PreviewAnalysisSummary PreviewAnalysis::summary() const
{
	PreviewAnalysisSummary result;
	if (failed_ || valid_points_ == 0)
		return result;
	result.valid_points = valid_points_;
	result.voice_lufs = voice_sum_ / valid_points_;
	result.bgm_lufs = bgm_sum_ / valid_points_;
	result.balance_status = basic_status(result.voice_lufs - result.bgm_lufs);
	return result;
}

const PreviewAnalysisPoint *PreviewAnalysis::point_at(uint64_t frame_position) const
{
	if (failed_)
		return nullptr;
	const auto it = std::upper_bound(series_.begin(), series_.end(), frame_position,
		[](uint64_t position, const PreviewAnalysisPoint &point) { return position < point.frame_position; });
	return it == series_.begin() ? nullptr : &*(it - 1);
}

} // namespace lbm
