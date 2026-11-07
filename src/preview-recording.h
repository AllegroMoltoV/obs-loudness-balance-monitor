#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <vector>

// One callback producer. The owner must detach and join that producer before
// freeze/cancel/reset. Only done() may be polled while the producer runs.
namespace lbm {
class Recording {
public:
	enum class State { idle, capturing, frozen, playing, cancelled, failed };
	Recording(uint32_t rate, uint32_t frames) : rate_(rate), target_(frames), left_(frames), right_(frames)
	{
		if ((rate != 44100 && rate != 48000) || !frames || frames > rate * 30)
			throw std::invalid_argument("Invalid recording capacity");
	}
	bool begin()
	{
		if (state_ == State::capturing || state_ == State::playing)
			return false;
		frames_ = 0;
		packets_ = 0;
		ignored_ = 0;
		invalid_ = false;
		first_ = last_ = 0;
		last_frames_ = 0;
		done_.store(false);
		state_ = State::capturing;
		return true;
	}
	void append(const float *left, const float *right, uint32_t frames, uint64_t timestamp) noexcept
	{
		if (state_ != State::capturing || done_.load(std::memory_order_relaxed)) {
			++ignored_;
			return;
		}
		if (!left || !right || !frames) {
			invalid_ = true;
			done_.store(true, std::memory_order_release);
			return;
		}
		if (packets_) {
			const uint64_t expected = last_ + uint64_t(last_frames_) * 1000000000ULL / rate_;
			const auto delta = timestamp > expected ? timestamp - expected : expected - timestamp;
			if (delta > 1) {
				invalid_ = true;
				done_.store(true, std::memory_order_release);
				return;
			}
		} else
			first_ = timestamp;
		last_ = timestamp;
		last_frames_ = frames;
		++packets_;
		const uint32_t n = std::min(frames, target_ - frames_);
		std::copy_n(left, n, left_.data() + frames_);
		std::copy_n(right, n, right_.data() + frames_);
		frames_ += n;
		if (frames_ == target_)
			done_.store(true, std::memory_order_release);
	}
	bool done() const { return done_.load(std::memory_order_acquire); }
	bool freeze(bool manual_finish = false)
	{
		if (state_ != State::capturing)
			return false;
		state_ = !invalid_ && frames_ > 0 && (manual_finish || frames_ == target_) ? State::frozen
											   : State::failed;
		return state_ == State::frozen;
	}
	void cancel()
	{
		state_ = State::cancelled;
		frames_ = 0;
	}
	bool play()
	{
		if (state_ != State::frozen)
			return false;
		state_ = State::playing;
		return true;
	}
	void stop()
	{
		if (state_ == State::playing)
			state_ = State::frozen;
	}
	State state() const { return state_; }
	uint32_t frames() const { return frames_; }
	uint64_t first_timestamp() const { return first_; }
	uint64_t packets() const { return packets_; }
	const std::vector<float> &left() const { return left_; }
	const std::vector<float> &right() const { return right_; }

private:
	uint32_t rate_, target_, frames_ = 0, last_frames_ = 0;
	uint64_t first_ = 0, last_ = 0, packets_ = 0, ignored_ = 0;
	bool invalid_ = false;
	std::atomic<bool> done_{false};
	State state_ = State::idle;
	std::vector<float> left_, right_;
};

} // namespace lbm
