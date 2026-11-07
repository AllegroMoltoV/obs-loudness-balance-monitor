#include "preview-session.h"
#include "preview-output.h"
#include "preview-clock.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace lbm {
PreviewSession::PreviewSession(QObject *parent) : QObject(parent)
{
	PreviewMonitors::recover();
}
PreviewSession::~PreviewSession()
{
	cancel();
}
void PreviewSession::configure_analysis(const std::string &voice, const std::vector<std::string> &bgm, double threshold,
					double target, double hysteresis, double mix_ok, double mix_warn)
{
	selected_voice_ = voice;
	selected_bgm_ = bgm;
	vad_threshold_ = threshold;
	balance_target_ = target;
	hysteresis_ = hysteresis;
	mix_ok_ = mix_ok;
	mix_warn_ = mix_warn;
}
void PreviewSession::stop_analysis()
{
	analysis_done_.store(true, std::memory_order_release);
	if (analysis_worker_.joinable())
		analysis_worker_.join();
	if (*category_.error()) {
		analysis_ok_ = false;
		analysis_error_ = category_.error();
	}
	category_.stop();
}
PreviewAnalysisSummary PreviewSession::analysis_summary() const
{
	return analysis_published_ ? analysis_.summary() : PreviewAnalysisSummary{};
}
QVariantMap PreviewSession::recorded_analysis() const
{
	// Read-only clip metadata; never expose a recording worker's mutable series.
	if (!analysis_published_)
		return {};
	const auto number = [](double value) {
		return std::isfinite(value) ? QVariant(value) : QVariant();
	};
	const auto status = [](std::optional<Status> value) {
		return value ? int(*value) : -1;
	};
	QVariantList points;
	for (const auto &point : analysis_.series())
		points.append(QVariantMap{{"frame", qulonglong(point.frame_position)},
					  {"voice", number(point.voice_lufs)},
					  {"bgm", number(point.bgm_lufs)},
					  {"mix", number(point.mix_lufs)},
					  {"voice_peak", number(point.voice_peak_dbfs)},
					  {"bgm_peak", number(point.bgm_peak_dbfs)},
					  {"mix_peak", number(point.mix_peak_dbfs)},
					  {"active", point.voice_active},
					  {"status", status(point.balance_status)},
					  {"mix_status", status(point.mix_status)},
					  {"clip_status", status(point.clip_status)}});
	const auto mean = analysis_.summary();
	return {{"points", points},
		{"voice", number(mean.voice_lufs)},
		{"bgm", number(mean.bgm_lufs)},
		{"mix", number(mean.mix_lufs)},
		{"voice_peak", number(mean.voice_peak_dbfs)},
		{"bgm_peak", number(mean.bgm_peak_dbfs)},
		{"mix_peak", number(mean.mix_peak_dbfs)},
		{"frames", qulonglong(mean.frames)},
		{"valid_points", mean.valid_points},
		{"mix_valid_points", mean.mix_valid_points},
		{"status", status(mean.balance_status)},
		{"mix_status", status(mean.mix_status)},
		{"clip_status", status(mean.clip_status)}};
}
const PreviewAnalysisPoint *PreviewSession::analysis_point() const
{
	const double seconds = playback_position_.load(std::memory_order_relaxed);
	return analysis_published_ && state_ == State::playing && seconds >= 0 && std::isfinite(seconds)
		       ? analysis_.point_at(uint64_t(seconds * rate_))
		       : nullptr;
}
QString PreviewSession::analysis_realtime_notice() const
{
	if (!analysis_published_)
		return analysis_notice_;
	if (state_ != State::playing)
		return obs_module_text("PreviewAnalysisNotPlaying");
	const double position = playback_position_.load(std::memory_order_relaxed);
	if (!std::isfinite(position) || position < 0.4)
		return obs_module_text("PreviewAnalysisWindowPending");
	return {};
}
bool PreviewSession::output_active()
{
	return obs_frontend_streaming_active() || obs_frontend_recording_active() ||
	       obs_frontend_replay_buffer_active();
}
bool PreviewSession::format_matches() const
{
	auto *output = obs_get_audio();
	return output && audio_output_get_sample_rate(output) == rate_ &&
	       audio_output_get_info(output)->speakers == SPEAKERS_STEREO;
}
void PreviewSession::detach()
{
	if (!attached_)
		return;
	// OBS synchronizes removal with the callback. No buffer access precedes it.
	std::thread remover([this] { obs_remove_raw_audio_callback(track_, audio, this); });
	remover.join();
	attached_ = false;
}
void PreviewSession::fail(const QString &message)
{
	cancel();
	message_ = message;
	state_ = State::failed;
	emit changed();
}
void PreviewSession::prepare(unsigned track, const std::string &device, bool disable_monitoring)
{
	cancel();
	if (output_active()) {
		fail(obs_module_text("PreviewOutputActive"));
		return;
	}
	if (device.empty()) {
		fail(obs_module_text("PreviewSelectDevice"));
		return;
	}
	prepared_device_ = device;
	prepared_monitor_off_ = disable_monitoring;
	auto *output = obs_get_audio();
	if (!output || track >= 6) {
		fail(obs_module_text("PreviewFormatError"));
		return;
	}
	rate_ = audio_output_get_sample_rate(output);
	if ((rate_ != 44100 && rate_ != 48000) || !format_matches()) {
		fail(obs_module_text("PreviewFormatError"));
		return;
	}
	try {
		recording_ = std::make_unique<Recording>(rate_, rate_ * 30);
	} catch (const std::exception &error) {
		fail(QString::fromUtf8(error.what()));
		return;
	}
	track_ = track;
	frames_.store(0);
	prepare_started_ = os_gettime_ns();
	state_ = State::preparing;
	emit changed();
}
void PreviewSession::audio(void *data, size_t, struct audio_data *packet)
{
	auto &self = *static_cast<PreviewSession *>(data);
	if (!self.recording_->done())
		self.category_.consume_raw(*packet);
	self.recording_->append(reinterpret_cast<const float *>(packet->data[0]),
				reinterpret_cast<const float *>(packet->data[1]), packet->frames, packet->timestamp);
	self.frames_.store(self.recording_->frames(), std::memory_order_relaxed);
	self.last_packet_.store(os_gettime_ns(), std::memory_order_release);
}
unsigned PreviewSession::countdown() const
{
	const uint64_t elapsed = os_gettime_ns() - prepare_started_;
	return elapsed >= 3000000000ULL ? 0 : unsigned((3000000000ULL - elapsed + 999999999ULL) / 1000000000ULL);
}
unsigned PreviewSession::wait_countdown() const
{
	const uint64_t elapsed = os_gettime_ns() - wait_started_;
	return elapsed >= 3000000000ULL ? 0 : unsigned((3000000000ULL - elapsed + 999999999ULL) / 1000000000ULL);
}
void PreviewSession::stop_waiting()
{
	if (state_ != State::waiting)
		return;
	state_ = State::ready;
	emit changed();
}
double PreviewSession::seconds() const
{
	return rate_ ? double(frames_.load()) / rate_ : 0.0;
}
void PreviewSession::finish()
{
	if (state_ != State::recording)
		return;
	detach();
	stop_analysis();
	if (!recording_->freeze(true)) {
		fail(obs_module_text("PreviewCaptureError"));
		return;
	}
	try {
		pcm_.resize(size_t(recording_->frames()) * 2);
		for (size_t i = 0; i < recording_->frames(); ++i) {
			pcm_[i * 2] = recording_->left()[i];
			pcm_[i * 2 + 1] = recording_->right()[i];
		}
	} catch (const std::exception &error) {
		fail(QString::fromUtf8(error.what()));
		return;
	}
	analysis_published_ = analysis_ok_ && analysis_.processed_frames() == recording_->frames();
	if (!analysis_published_) {
		if (analysis_notice_.isEmpty())
			analysis_notice_ = obs_module_text("PreviewAnalysisError");
		blog(LOG_WARNING, "[LBM] Preview analysis unavailable: %s",
		     analysis_error_.empty() ? "category frame count mismatch" : analysis_error_.c_str());
	}
	if (analysis_published_) {
		if (uint64_t(recording_->frames()) < uint64_t(rate_) * 3)
			analysis_notice_ = obs_module_text("PreviewAnalysisWindowShort");
		else if (analysis_.summary().valid_points == 0)
			analysis_notice_ = obs_module_text("PreviewAnalysisNoPairedSpeech");
		size_t voice_points = 0, bgm_points = 0, active_points = 0;
		for (const auto &point : analysis_.series()) {
			voice_points += std::isfinite(point.voice_lufs);
			bgm_points += std::isfinite(point.bgm_lufs);
			active_points += point.voice_active;
		}
		blog(LOG_INFO,
		     "[LBM] Preview analysis: rate=%u track=%u frames=%llu points=%zu voice_finite=%zu bgm_finite=%zu voice_active=%zu paired=%u",
		     rate_, track_ + 1, static_cast<unsigned long long>(recording_->frames()),
		     analysis_.series().size(), voice_points, bgm_points, active_points,
		     analysis_.summary().valid_points);
	}
	clip_timestamp_ = recording_->first_timestamp();
	clip_frames_ = recording_->frames();
	wait_started_ = os_gettime_ns();
	state_ = State::waiting;
	emit changed();
}
void PreviewSession::play(const std::string &device, bool disable_monitoring)
{
	if (state_ != State::ready)
		return;
	if (output_active()) {
		fail(obs_module_text("PreviewOutputActive"));
		return;
	}
	if (!format_matches()) {
		fail(obs_module_text("PreviewFormatError"));
		return;
	}
	if (device.empty()) {
		message_ = obs_module_text("PreviewSelectDevice");
		emit changed();
		return;
	}
	if (!recording_->play())
		return;
	cancel_playback_.store(false);
	playback_done_.store(false);
	playback_error_.clear();
	playback_position_.store(-1.0);
	message_.clear();
	if (disable_monitoring)
		monitors_.begin();
	state_ = State::playing;
	try {
		worker_ = std::thread([this, device] {
			playback_ok_ = play_preview(pcm_, rate_, device, cancel_playback_, playback_error_,
						    &playback_position_);
			playback_done_.store(true, std::memory_order_release);
		});
	} catch (const std::exception &error) {
		fail(QString::fromUtf8(error.what()));
		return;
	}
	emit changed();
}
void PreviewSession::stop_playback()
{
	if (state_ != State::playing)
		return;
	cancel_playback_.store(true);
	if (worker_.joinable())
		worker_.join();
	monitors_.stop();
	recording_->stop();
	state_ = State::ready;
	if (!playback_error_.empty())
		message_ = QString::fromStdString(playback_error_);
	emit changed();
}
void PreviewSession::cancel()
{
	detach();
	stop_analysis();
	analysis_published_ = false;
	analysis_notice_.clear();
	stop_playback();
	monitors_.stop();
	recording_.reset();
	clip_timestamp_ = 0;
	clip_frames_ = 0;
	std::vector<float>().swap(pcm_);
	frames_.store(0);
	state_ = State::idle;
	message_.clear();
	emit changed();
}
void PreviewSession::tick()
{
	if (state_ == State::idle || state_ == State::failed)
		return;
	if (output_active()) {
		fail(obs_module_text("PreviewOutputActive"));
		return;
	}
	if (!format_matches()) {
		fail(obs_module_text("PreviewFormatError"));
		return;
	}
	if (state_ == State::preparing && countdown() == 0) {
		analysis_ok_ = false;
		analysis_error_.clear();
		try {
			analysis_ok_ = analysis_.reset(rate_, vad_threshold_, balance_target_, hysteresis_, mix_ok_,
						       mix_warn_) &&
				       category_.start(selected_voice_, selected_bgm_, track_, rate_);
			if (!analysis_ok_) {
				analysis_error_ = category_.error();
				analysis_notice_ = obs_module_text(selected_voice_.empty() || selected_bgm_.empty()
									   ? "PreviewAnalysisSelectionMissing"
									   : "PreviewAnalysisError");
			}
			if (analysis_ok_) {
				analysis_done_.store(false);
				analysis_worker_ = std::thread([this] {
					try {
						CategoryBlock block;
						uint64_t previous_ts = 0;
						uint32_t previous_frames = 0;
						while (true) {
							if (category_.pop(block)) {
								if (analysis_.processed_frames() >=
								    uint64_t(rate_) * 30)
									continue;
								if (previous_ts) {
									const auto expected =
										previous_ts +
										uint64_t(previous_frames) *
											1000000000ULL / rate_;
									const auto delta =
										expected > block.timestamp
											? expected - block.timestamp
											: block.timestamp - expected;
									if (delta > 1) {
										analysis_ok_ = false;
										analysis_error_ =
											"Category timestamp discontinuity";
										break;
									}
								}
								previous_ts = block.timestamp;
								previous_frames = block.frames;
								const auto frames = uint32_t(std::min<uint64_t>(
									block.frames,
									uint64_t(rate_) * 30 -
										analysis_.processed_frames()));
								if (!analysis_.append(block.voice.data(),
										      block.bgm.data(), frames)) {
									analysis_ok_ = false;
									analysis_error_ = "Category analysis failed";
									break;
								}
							} else if (analysis_done_.load(std::memory_order_acquire))
								break;
							else
								std::this_thread::sleep_for(
									std::chrono::milliseconds(1));
						}
					} catch (const std::exception &error) {
						analysis_ok_ = false;
						analysis_error_ = error.what();
					}
				});
			}
		} catch (const std::exception &error) {
			analysis_ok_ = false;
			analysis_error_ = error.what();
			analysis_notice_ = obs_module_text("PreviewAnalysisError");
		}
		recording_->begin();
		last_packet_.store(os_gettime_ns());
		audio_convert_info conversion{};
		conversion.samples_per_sec = rate_;
		conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
		conversion.speakers = SPEAKERS_STEREO;
		conversion.allow_clipping = false;
		obs_add_raw_audio_callback(track_, &conversion, audio, this);
		attached_ = true;
		state_ = State::recording;
		emit changed();
	}
	if (state_ == State::recording) {
		if (recording_->done())
			finish();
		// A stalled callback is not valid silence. This only bounds failure reporting.
		else if (packet_idle_ns(last_packet_, os_gettime_ns) > 5000000000ULL)
			fail(obs_module_text("PreviewCaptureError"));
	}
	if (state_ == State::waiting && wait_countdown() == 0) {
		state_ = State::ready;
		play(prepared_device_, prepared_monitor_off_);
	}
	if (state_ == State::playing) {
		monitors_.refresh();
		if (playback_done_.load(std::memory_order_acquire)) {
			if (worker_.joinable())
				worker_.join();
			monitors_.stop();
			recording_->stop();
			state_ = State::ready;
			if (!playback_ok_)
				message_ = QString::fromStdString(playback_error_);
			emit changed();
		}
	}
}
} // namespace lbm
