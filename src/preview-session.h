#pragma once
#include "preview-monitor.h"
#include "preview-recording.h"
#include <QObject>
#include <QString>
#include <atomic>
#include <memory>
#include <thread>

namespace lbm {
class PreviewSession : public QObject {
    Q_OBJECT
    Q_PROPERTY(qulonglong recordedFrames READ recorded_frames)
    Q_PROPERTY(qulonglong recordedTimestamp READ recorded_timestamp)
    Q_PROPERTY(qulonglong recordedRate READ recorded_rate)
public:
    enum class State { idle, preparing, recording, waiting, ready, playing, failed };
    explicit PreviewSession(QObject *parent = nullptr);
    ~PreviewSession() override;
    void prepare(unsigned track, const std::string &device, bool disable_monitoring);
    void stop_waiting();
    unsigned wait_countdown() const;
    unsigned track() const { return track_; }
    // Immutable clip metadata, published only after the callback has detached.
    qulonglong recorded_frames() const { return clip_frames_; }
    qulonglong recorded_timestamp() const { return clip_timestamp_; }
    qulonglong recorded_rate() const { return clip_frames_ ? rate_ : 0; }
    void finish();
    void play(const std::string &device, bool disable_monitoring);
    void stop_playback();
    void cancel();
    void tick();
    State state() const { return state_; }
    QString message() const { return message_; }
    double seconds() const;
    unsigned countdown() const;
    static bool output_active();
signals:
    void changed();
private:
    static void audio(void *data, size_t mix, struct audio_data *packet);
    void detach();
    void fail(const QString &message);
    bool format_matches() const;
    State state_ = State::idle;
    QString message_;
    std::unique_ptr<Recording> recording_;
    std::vector<float> pcm_;
    uint32_t rate_ = 0;
    unsigned track_ = 0;
    uint64_t prepare_started_ = 0;
    uint64_t wait_started_ = 0;
    uint64_t clip_timestamp_ = 0;
    uint32_t clip_frames_ = 0;
    std::string prepared_device_;
    bool prepared_monitor_off_ = true;
    bool attached_ = false;
    std::atomic<uint64_t> last_packet_{0};
    std::atomic<uint32_t> frames_{0};
    std::atomic<bool> cancel_playback_{false};
    std::atomic<bool> playback_done_{false};
    std::thread worker_;
    std::string playback_error_;
    bool playback_ok_ = false;
    PreviewMonitors monitors_;
};
} // namespace lbm
