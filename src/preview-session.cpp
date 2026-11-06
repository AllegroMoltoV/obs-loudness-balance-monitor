#include "preview-session.h"
#include "preview-output.h"
#include "preview-clock.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <algorithm>

namespace lbm {
PreviewSession::PreviewSession(QObject *parent) : QObject(parent) { PreviewMonitors::recover(); }
PreviewSession::~PreviewSession() { cancel(); }
bool PreviewSession::output_active()
{
    return obs_frontend_streaming_active() || obs_frontend_recording_active() || obs_frontend_replay_buffer_active();
}
bool PreviewSession::format_matches() const
{
    auto *output = obs_get_audio();
    return output && audio_output_get_sample_rate(output) == rate_ &&
           audio_output_get_info(output)->speakers == SPEAKERS_STEREO;
}
void PreviewSession::detach()
{
    if (!attached_) return;
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
void PreviewSession::prepare(unsigned track)
{
    cancel();
    if (output_active()) { fail(obs_module_text("PreviewOutputActive")); return; }
    auto *output = obs_get_audio();
    if (!output || track >= 6) { fail(obs_module_text("PreviewFormatError")); return; }
    rate_ = audio_output_get_sample_rate(output);
    if ((rate_ != 44100 && rate_ != 48000) || !format_matches()) {
        fail(obs_module_text("PreviewFormatError")); return;
    }
    try { recording_ = std::make_unique<Recording>(rate_, rate_ * 30); }
    catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); return; }
    track_ = track;
    frames_.store(0);
    prepare_started_ = os_gettime_ns();
    state_ = State::preparing;
    emit changed();
}
void PreviewSession::audio(void *data, size_t, struct audio_data *packet)
{
    auto &self = *static_cast<PreviewSession *>(data);
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
double PreviewSession::seconds() const { return rate_ ? double(frames_.load()) / rate_ : 0.0; }
void PreviewSession::finish()
{
    if (state_ != State::recording) return;
    detach();
    if (!recording_->freeze(true)) { fail(obs_module_text("PreviewCaptureError")); return; }
    try {
        pcm_.resize(size_t(recording_->frames()) * 2);
        for (size_t i = 0; i < recording_->frames(); ++i) {
            pcm_[i * 2] = recording_->left()[i];
            pcm_[i * 2 + 1] = recording_->right()[i];
        }
    } catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); return; }
    state_ = State::ready;
    emit changed();
}
void PreviewSession::play(const std::string &device, bool disable_monitoring)
{
    if (state_ != State::ready) return;
    if (output_active()) { fail(obs_module_text("PreviewOutputActive")); return; }
    if (!format_matches()) { fail(obs_module_text("PreviewFormatError")); return; }
    if (device.empty()) { message_ = obs_module_text("PreviewSelectDevice"); emit changed(); return; }
    if (!recording_->play()) return;
    cancel_playback_.store(false);
    playback_done_.store(false);
    playback_error_.clear();
    message_.clear();
    if (disable_monitoring) monitors_.begin();
    state_ = State::playing;
    try {
        worker_ = std::thread([this, device] {
            playback_ok_ = play_preview(pcm_, rate_, device, cancel_playback_, playback_error_);
            playback_done_.store(true, std::memory_order_release);
        });
    } catch (const std::exception &error) { fail(QString::fromUtf8(error.what())); return; }
    emit changed();
}
void PreviewSession::stop_playback()
{
    if (state_ != State::playing) return;
    cancel_playback_.store(true);
    if (worker_.joinable()) worker_.join();
    monitors_.stop();
    recording_->stop();
    state_ = State::ready;
    if (!playback_error_.empty()) message_ = QString::fromStdString(playback_error_);
    emit changed();
}
void PreviewSession::cancel()
{
    detach();
    stop_playback();
    monitors_.stop();
    recording_.reset();
    std::vector<float>().swap(pcm_);
    frames_.store(0);
    state_ = State::idle;
    message_.clear();
    emit changed();
}
void PreviewSession::tick()
{
    if (state_ == State::idle || state_ == State::failed) return;
    if (output_active()) { fail(obs_module_text("PreviewOutputActive")); return; }
    if (!format_matches()) { fail(obs_module_text("PreviewFormatError")); return; }
    if (state_ == State::preparing && countdown() == 0) {
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
        if (recording_->done()) finish();
        // A stalled callback is not valid silence. This only bounds failure reporting.
        else if (packet_idle_ns(last_packet_, os_gettime_ns) > 5000000000ULL)
            fail(obs_module_text("PreviewCaptureError"));
    }
    if (state_ == State::playing) {
        monitors_.refresh();
        if (playback_done_.load(std::memory_order_acquire)) {
            if (worker_.joinable()) worker_.join();
            monitors_.stop();
            recording_->stop();
            state_ = State::ready;
            if (!playback_ok_) message_ = QString::fromStdString(playback_error_);
            emit changed();
        }
    }
}
} // namespace lbm
