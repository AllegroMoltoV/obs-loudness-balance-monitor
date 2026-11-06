#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <QJsonObject>
#include <QMainWindow>
#include <QProgressBar>
#include <QSlider>
#include <QTimer>
#include <QPushButton>
#include <QLabel>
#include <array>
#include <cmath>
#include <vector>
#include <memory>
#include <thread>
#include <functional>
#include <media-io/audio-resampler.h>
#ifdef _WIN32
#include "loopback-probe.h"
#endif

OBS_DECLARE_MODULE()
static constexpr const char *voice_name = "LBM baseline voice";
static constexpr const char *bgm_name = "LBM baseline bgm";
static obs_source_t *voice = nullptr;
static obs_source_t *bgm = nullptr;
static QTimer *producer = nullptr;
static QWidget *dock = nullptr;
static QDockWidget *dock_container = nullptr;
static QString result_dir;
static QString phase;
static QJsonObject observations;
static uint64_t position = 0;
static uint64_t origin = 0;
static bool finished = false;
static void stop_evidence();
struct EvidencePacket { uint64_t timestamp; size_t first; uint32_t frames; };
static std::vector<float> observed_mix, expected_pcm;
static std::vector<EvidencePacket> observed_packets;
static size_t observed_frames = 0;
static bool observed_overflow = false, observer_attached = false;
static uint32_t evidence_rate = 0;
static unsigned evidence_clip = 1;
static std::vector<float> first_clip_pcm;
#ifdef _WIN32
static std::unique_ptr<LoopbackProbe> loopback;
#endif

static void observe_mix(void *, size_t, audio_data *packet)
{
    if (observed_frames + packet->frames > observed_mix.size() / 2 ||
        observed_packets.size() == observed_packets.capacity()) { observed_overflow = true; return; }
    const auto *left = reinterpret_cast<const float *>(packet->data[0]);
    const auto *right = reinterpret_cast<const float *>(packet->data[1]);
    observed_packets.push_back({packet->timestamp, observed_frames, packet->frames});
    for (uint32_t i = 0; i < packet->frames; ++i) {
        observed_mix[(observed_frames + i) * 2] = left[i];
        observed_mix[(observed_frames + i) * 2 + 1] = right[i];
    }
    observed_frames += packet->frames;
}

static void detach_observer()
{
    if (!observer_attached) return;
    std::thread remove([] { obs_remove_raw_audio_callback(0, observe_mix, nullptr); });
    remove.join();
    observer_attached = false;
}

static bool save_bytes(const QString &path, const void *bytes, size_t count)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
           file.write(static_cast<const char *>(bytes), qint64(count)) == qint64(count) && file.flush();
}

static void stop_evidence()
{
    detach_observer();
#ifdef _WIN32
    if (loopback) {
        loopback->stop();
        try { loopback->save((result_dir + "/interrupted").toStdString()); }
        catch (const std::exception &error) { observations.insert("evidence_save_error", error.what()); }
        loopback.reset();
    }
#endif
}

static void finish_deferred(bool passed, const QString &reason)
{
    stop_evidence();
    if (producer) { producer->stop(); producer->deleteLater(); producer = nullptr; }
    if (voice) { obs_source_release(voice); voice = nullptr; }
    if (bgm) { obs_source_release(bgm); bgm = nullptr; }
    QJsonObject result{{"passed", passed}, {"reason", reason}, {"obs", obs_get_version_string()},
                       {"phase", phase}, {"observations", observations}};
#ifdef __APPLE__
    QJsonArray loaded_images;
    for (uint32_t index = 0; index < _dyld_image_count(); ++index)
        loaded_images.append(QString::fromUtf8(_dyld_get_image_name(index)));
    result.insert("loaded_images", loaded_images);
#endif
    QFile file(result_dir + "/result.json");
    if (!file.open(QIODevice::WriteOnly)) {
        passed = false;
        blog(LOG_ERROR, "LBM_BASELINE result open failed: %s", file.errorString().toUtf8().constData());
    } else {
        const QByteArray payload = QJsonDocument(result).toJson();
        if (file.write(payload) != payload.size() || !file.flush()) {
            passed = false;
            blog(LOG_ERROR, "LBM_BASELINE result write failed: %s", file.errorString().toUtf8().constData());
        }
        file.close();
    }
    blog(passed ? LOG_INFO : LOG_ERROR, "LBM_BASELINE %s phase=%s %s", passed ? "PASS" : "FAIL",
         phase.toUtf8().constData(), reason.toUtf8().constData());
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!window) return;
    // Leave frontend event dispatch before stopping outputs or destroying OBS.
    if (obs_frontend_recording_active()) obs_frontend_recording_stop();
    auto *shutdown = new QTimer(window);
    QObject::connect(shutdown, &QTimer::timeout, window, [window, shutdown] {
        if (obs_frontend_recording_active()) return;
        shutdown->stop();
        shutdown->deleteLater();
        window->close();
    });
    shutdown->start(50);
}

static void finish(bool passed, const QString &reason)
{
    if (finished) return;
    finished = true;
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    QTimer::singleShot(0, window, [passed, reason] { finish_deferred(passed, reason); });
}

static bool inspect_settings(bool configure)
{
    QComboBox *voice_combo = nullptr;
    QComboBox *preset_combo = nullptr;
    QCheckBox *bgm_check = nullptr;
    for (auto *combo : dock->findChildren<QComboBox *>()) {
        if (combo->findData(QString(voice_name)) >= 0) voice_combo = combo;
        else if (combo->count() == 3 && combo->itemData(1).toInt() == 1) preset_combo = combo;
    }
    for (auto *check : dock->findChildren<QCheckBox *>()) {
        if (check->property("source_name").toString() == bgm_name) bgm_check = check;
    }
    const auto sliders = dock->findChildren<QSlider *>();
    const auto balances = dock->findChildren<QDoubleSpinBox *>();
    if (!voice_combo || !preset_combo || !bgm_check || sliders.size() != 1 || balances.size() != 1)
        return false;
    if (configure) {
        voice_combo->setCurrentIndex(voice_combo->findData(QString(voice_name)));
        bgm_check->setChecked(true);
        sliders.front()->setValue(-35);
        balances.front()->setValue(8.0);
        preset_combo->setCurrentIndex(1);
    }
    observations.insert("voice_source", voice_combo->currentData().toString());
    observations.insert("bgm_selected", bgm_check->isChecked());
    observations.insert("vad_threshold", sliders.front()->value());
    observations.insert("balance_target", balances.front()->value());
    observations.insert("mix_preset", preset_combo->currentIndex());
    return voice_combo->currentData().toString() == voice_name && bgm_check->isChecked() &&
           sliders.front()->value() == -35 && balances.front()->value() == 8.0 && preset_combo->currentIndex() == 1;
}


static QString product_text(const char *key)
{
    return QString::fromUtf8(obs_module_get_locale_text(obs_get_module("loudness-balance-monitor"), key));
}

static bool preview_check(bool condition, const char *reason)
{
    if (!condition) {
        QJsonObject controls;
        for (const auto *name : {"previewRecord", "previewPlay"}) {
            if (auto *button = dock->findChild<QPushButton *>(name))
                controls.insert(name, QJsonObject{{"text", button->text()}, {"enabled", button->isEnabled()}});
        }
        if (auto *label = dock->findChild<QLabel *>("previewStatus"))
            controls.insert("status", label->text());
        observations.insert("preview_failure_controls", controls);
        dock_container->grab().save(result_dir + "/preview-failure.png");
        finish(false, QString::fromUtf8(reason));
    }
    return condition;
}

static bool select_expected_pcm()
{
    detach_observer();
    QObject *session = dock->findChild<QObject *>("previewSession");
    if (!preview_check(session != nullptr, "preview session metadata unavailable")) return false;
    const uint64_t first = session->property("recordedTimestamp").toULongLong();
    const uint64_t frames = session->property("recordedFrames").toULongLong();
    const uint32_t rate = session->property("recordedRate").toUInt();
    if (!preview_check(!observed_overflow && first && frames && rate == evidence_rate,
                       "invalid recording metadata or independent capture overflow")) return false;
    size_t begin = observed_frames;
    for (const auto &packet : observed_packets)
        if (packet.timestamp == first) { begin = packet.first; break; }
    if (!preview_check(begin < observed_frames && frames <= observed_frames - begin,
                       "recorded interval missing from independent final mix")) return false;
    uint64_t prior_timestamp = 0;
    uint32_t prior_frames = 0;
    for (const auto &packet : observed_packets) {
        if (packet.first < begin || packet.first >= begin + frames) continue;
        if (prior_timestamp) {
            const uint64_t expected = prior_timestamp + uint64_t(prior_frames) * 1000000000ULL / rate;
            const uint64_t delta = packet.timestamp > expected ? packet.timestamp - expected : expected - packet.timestamp;
            if (!preview_check(delta <= 1, "independent final mix timestamp discontinuity")) return false;
        }
        prior_timestamp = packet.timestamp;
        prior_frames = packet.frames;
    }
    expected_pcm.assign(observed_mix.begin() + begin * 2, observed_mix.begin() + (begin + frames) * 2);
    observations.insert("recorded_frames", qint64(frames));
    observations.insert("recorded_timestamp_ns", QString::number(first));
    observations.insert("recorded_rate", int(rate));
    observations.insert(QString("clip_%1").arg(evidence_clip), QJsonObject{
        {"frames", qint64(frames)}, {"timestamp_ns", QString::number(first)},
        {"rate", int(rate)}, {"independent_first_frame", qint64(begin)}});
    const QString suffix = evidence_clip == 1 ? QString() : QString("-%1").arg(evidence_clip);
    if (!preview_check(save_bytes(result_dir + "/expected-input" + suffix + ".f32", expected_pcm.data(), expected_pcm.size() * sizeof(float)),
                       "expected PCM save failed")) return false;
    QByteArray csv("timestamp_ns,first_frame,frames\n");
    for (const auto &packet : observed_packets)
        csv += QByteArray::number(qulonglong(packet.timestamp)) + ',' + QByteArray::number(qulonglong(packet.first)) +
               ',' + QByteArray::number(packet.frames) + '\n';
    return preview_check(save_bytes(result_dir + "/observed-mix" + suffix + ".f32", observed_mix.data(), observed_frames * 8) &&
                         save_bytes(result_dir + "/observed-mix" + suffix + ".csv", csv.constData(), size_t(csv.size())),
                         "independent final mix evidence save failed");
}

static bool start_loopback()
{
#ifdef _WIN32
    try {
        loopback = std::make_unique<LoopbackProbe>(qEnvironmentVariable("LBM_PREVIEW_DEVICE").toStdString(), 45);
        return preview_check(loopback->start(), "independent WASAPI loopback start failed");
    } catch (const std::exception &error) {
        return preview_check(false, error.what());
    }
#else
    return true;
#endif
}

static bool save_loopback(const QString &label)
{
#ifdef _WIN32
    if (!preview_check(bool(loopback), "loopback observer missing")) return false;
    loopback->stop();
    const auto stats = loopback->stats();
    const QString prefix = result_dir + "/" + label;
    try { loopback->save(prefix.toStdString()); }
    catch (const std::exception &error) { loopback.reset(); return preview_check(false, error.what()); }
    loopback.reset();
    if (!preview_check(!stats.failures && !stats.dropped_frames && !stats.dropped_packets &&
                       !stats.timestamp_error_packets, "loopback capture failure or overflow")) return false;
    std::vector<float> expected = expected_pcm;
    if (stats.rate != evidence_rate) {
        std::vector<float> left(expected.size() / 2), right(left.size());
        for (size_t i = 0; i < left.size(); ++i) { left[i] = expected[i * 2]; right[i] = expected[i * 2 + 1]; }
        resample_info from{evidence_rate, AUDIO_FORMAT_FLOAT_PLANAR, SPEAKERS_STEREO};
        resample_info to{stats.rate, AUDIO_FORMAT_FLOAT, SPEAKERS_STEREO};
        auto *resampler = audio_resampler_create(&to, &from);
        if (!preview_check(resampler != nullptr, "reference resampler initialization failed")) return false;
        const uint8_t *input[MAX_AV_PLANES] = {reinterpret_cast<const uint8_t *>(left.data()),
                                             reinterpret_cast<const uint8_t *>(right.data())};
        uint8_t *output[MAX_AV_PLANES]{};
        uint32_t count = 0;
        uint64_t offset = 0;
        expected.clear();
        bool okay = audio_resampler_resample(resampler, output, &count, &offset, input, uint32_t(left.size()));
        if (okay && count) expected.insert(expected.end(), reinterpret_cast<float *>(output[0]), reinterpret_cast<float *>(output[0]) + count * 2);
        unsigned flush = 0;
        while (okay && flush++ < 16) {
            okay = audio_resampler_resample(resampler, output, &count, &offset, nullptr, 0);
            if (!okay || !count) break;
            expected.insert(expected.end(), reinterpret_cast<float *>(output[0]), reinterpret_cast<float *>(output[0]) + count * 2);
        }
        audio_resampler_destroy(resampler);
        if (!preview_check(okay && !count, "reference resampler flush failed")) return false;
    }
    if (!preview_check(save_bytes(prefix + "-expected.f32", expected.data(), expected.size() * sizeof(float)),
                       "reference output PCM save failed")) return false;
    observations.insert(label + "_output_frames", qint64(expected.size() / 2));
    observations.insert("audio_verification", "pending offline exact PCM and packet audit");
#else
    observations.insert("audio_verification", "not available on this platform");
#endif
    return true;
}

static void preview_test()
{
    auto *record = dock->findChild<QPushButton *>("previewRecord");
    auto *play = dock->findChild<QPushButton *>("previewPlay");
    auto *device = dock->findChild<QComboBox *>("previewDevice");
    auto *monitor = dock->findChild<QCheckBox *>("previewMonitorOff");
    auto *status = dock->findChild<QLabel *>("previewStatus");
    auto *manual = dock->findChild<QCheckBox *>("previewManualTrack");
    auto *track = dock->findChild<QComboBox *>("previewTrack");
    if (!preview_check(record && play && device && monitor && status && manual && track, "preview controls missing")) return;
    const QString wanted = qEnvironmentVariable("LBM_PREVIEW_DEVICE");
    const QString test_case = qEnvironmentVariable("LBM_PREVIEW_CASE");
    observations.insert("preview_device", wanted);
    observations.insert("preview_case", test_case);
    const int index = device->findData(wanted);
    if (!preview_check(index >= 0, "explicit preview device unavailable")) return;
    device->setCurrentIndex(index);
    manual->setChecked(true);
    track->setCurrentIndex(track->findData(0));
    monitor->setChecked(true);
    obs_source_set_audio_mixers(voice, 1);
    obs_source_set_audio_mixers(bgm, 1);
    if (test_case == "output-failure") {
        device->addItem("Unavailable test endpoint", "{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}");
        device->setCurrentIndex(device->count() - 1);
    }
    evidence_rate = audio_output_get_sample_rate(obs_get_audio());
    observed_mix.resize(size_t(evidence_rate) * 40 * 2);
    observed_packets.reserve(4000);
    audio_convert_info conversion{};
    conversion.samples_per_sec = evidence_rate;
    conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
    conversion.speakers = SPEAKERS_STEREO;
    conversion.allow_clipping = false;
    obs_add_raw_audio_callback(0, &conversion, observe_mix, nullptr);
    observer_attached = true;
    record->click();
    if (!preview_check(record->isEnabled() && record->text() == product_text("PreviewCancelPreparation") && !play->isVisible(),
                       "preparation controls did not update")) return;
    dock_container->grab().save(result_dir + "/preview-preparing.png");
    struct Run {
        int stage = 0;
        uint64_t started = os_gettime_ns(), since = 0, waiting = 0;
        bool seen_recording = false, seen_waiting_again = false;
    };
    auto run = std::make_shared<Run>();
    auto *poll = new QTimer(dock);
    QObject::connect(poll, &QTimer::timeout, dock, [=] {
        if (finished) { poll->stop(); poll->deleteLater(); return; }
        const uint64_t now = os_gettime_ns();
        auto elapsed = [&](uint64_t since) { return double(now - since) / 1000000000.0; };
        const bool automatic = test_case == "automatic";
        const bool waiting_stop = test_case == "waiting-stop" || test_case == "manual";
        if (elapsed(run->started) > (automatic ? 85 : 45)) {
            preview_check(false, "preview scenario timed out"); return;
        }
        if (run->stage == 0) {
            if (record->text() == product_text("PreviewFinish")) {
                if (!run->seen_recording) {
                    run->seen_recording = true; run->since = now;
                    observations.insert("preparation_seconds", elapsed(run->started));
                    dock_container->grab().save(result_dir + "/preview-recording.png");
                }
                if (!automatic && elapsed(run->since) >= 2.0) record->click();
            }
            if (play->text() != product_text("PreviewStopWaiting") || !play->isVisible()) return;
            if (!preview_check(run->seen_recording && !record->isEnabled(), "waiting reached without recording")) return;
            if (!select_expected_pcm()) return;
            if (evidence_clip == 1) first_clip_pcm = expected_pcm;
            else if (!preview_check(expected_pcm != first_clip_pcm, "rerecord retained the previous audio")) return;
            if (automatic && !preview_check(expected_pcm.size() / 2 == size_t(evidence_rate) * 30,
                                             "automatic recording did not contain exactly 30 seconds")) return;
            producer->stop();
            obs_source_set_monitoring_enabled(voice, true);
            run->waiting = now;
            observations.insert("preview_automatic_30_seconds", automatic);
            dock_container->grab().save(result_dir + "/preview-waiting.png");
            if (!start_loopback()) return;
            if (waiting_stop && evidence_clip == 1) {
                play->click();
                if (!preview_check(play->text() == product_text("PreviewPlay") && record->isEnabled(),
                                   "stop waiting did not preserve recording")) return;
                run->since = now; run->stage = 1;
            } else run->stage = 2;
            return;
        }
        if (run->stage == 1) {
            if (elapsed(run->since) < 3.5) return;
            if (!preview_check(play->text() == product_text("PreviewPlay") && obs_source_get_monitoring_enabled(voice),
                               "stopped waiting started playback")) return;
            if (!save_loopback("waiting-stopped")) return;
            observations.insert("preview_waiting_stopped", true);
            if (!start_loopback()) return;
            play->click(); run->stage = 2;
            return;
        }
        if (run->stage == 2) {
            if (play->text() == product_text("PreviewStopWaiting")) {
                if (elapsed(run->waiting) >= 1.0) run->seen_waiting_again = true;
                return;
            }
            if (test_case == "output-failure" && play->text() != product_text("PreviewStopWaiting") &&
                play->text() != product_text("PreviewStop")) {
                if (!preview_check(record->isEnabled() && play->isVisible() && obs_source_get_monitoring_enabled(voice) &&
                                   !status->text().startsWith(product_text("PreviewReady").arg("2.0")),
                                   "output failure did not restore monitoring")) return;
                if (!save_loopback("output-failure")) return;
                observations.insert("preview_output_failure_message", status->text());
                observations.insert("preview_output_failure_restored", true);
                device->setCurrentIndex(device->findData(wanted));
                if (!start_loopback()) return;
                play->click(); run->stage = 3; run->since = now;
                return;
            }
            if (play->text() != product_text("PreviewStop")) return;
            if (!preview_check(!obs_source_get_monitoring_enabled(voice), "monitor not disabled during preview")) return;
            if ((!waiting_stop || evidence_clip == 2) && !preview_check(run->seen_waiting_again,
                                                "automatic playback skipped observable waiting")) return;
            observations.insert("waiting_seconds", elapsed(run->waiting));
            dock_container->grab().save(result_dir + "/preview-playing.png");
            run->since = now; run->stage = 3;
            if (test_case == "recording-interrupt") QTimer::singleShot(300, dock, [] { obs_frontend_recording_start(); });
            return;
        }
        if (run->stage == 3) {
            if (test_case == "recording-interrupt") return;
            if (play->text() == product_text("PreviewStop")) return;
            if (!preview_check(play->isVisible() && play->isEnabled() && play->text() == product_text("PreviewReplay") &&
                               record->isEnabled() && obs_source_get_monitoring_enabled(voice),
                               "natural playback completion or monitor restoration failed")) return;
            run->since = now; run->stage = 4;
            return;
        }
        if (run->stage == 4) {
            if (elapsed(run->since) < 0.3) return;
            if (!save_loopback(evidence_clip == 1 ? "playback-1" : "playback-3")) return;
            observations.insert("preview_natural_end", true);
            dock_container->grab().save(result_dir + "/preview-ready.png");
            if (!waiting_stop || evidence_clip == 2) {
                if (evidence_clip == 2) {
                    observations.insert("preview_rerecord_different_audio", true);
                    record->click();
                    if (!preview_check(record->text() == product_text("PreviewCancelPreparation") && !play->isVisible(),
                                       "rerecord preparation did not discard previous clip")) return;
                    record->click();
                    if (!preview_check(record->text() == product_text("PreviewRecord") && !play->isVisible(),
                                       "preparation cancellation retained clip")) return;
                    observations.insert("preview_rerecord_cancel", true);
                }
                obs_source_set_monitoring_enabled(voice, false);
                obs_source_set_audio_mixers(voice, 0); obs_source_set_audio_mixers(bgm, 0);
                finish(true, "preview UI sequence passed; audio artifacts require exact PCM audit");
                return;
            }
            if (!start_loopback()) return;
            play->click(); run->since = now; run->stage = 5;
            return;
        }
        if (run->stage == 5) {
            if (play->text() == product_text("PreviewStop")) return;
            if (!preview_check(play->text() == product_text("PreviewReplay") && obs_source_get_monitoring_enabled(voice),
                               "replay did not restore ready state")) return;
            run->since = now; run->stage = 6;
            return;
        }
        if (run->stage == 6) {
            if (elapsed(run->since) < 0.3) return;
            if (!save_loopback("playback-2")) return;
            observations.insert("preview_replay", true);
            evidence_clip = 2;
            observed_frames = 0; observed_overflow = false; observed_packets.clear();
            audio_convert_info next_conversion{};
            next_conversion.samples_per_sec = evidence_rate;
            next_conversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
            next_conversion.speakers = SPEAKERS_STEREO;
            next_conversion.allow_clipping = false;
            obs_add_raw_audio_callback(0, &next_conversion, observe_mix, nullptr);
            observer_attached = true;
            obs_source_set_monitoring_enabled(voice, false);
            origin = os_gettime_ns() - position * 1000000000ULL / evidence_rate;
            producer->start(10);
            record->click();
            if (!preview_check(record->text() == product_text("PreviewCancelPreparation") && !play->isVisible(),
                               "rerecord did not discard previous clip")) return;
            run->seen_recording = false; run->seen_waiting_again = false; run->stage = 0;
        }
    });
    poll->start(20);
}

static void inspect_meter()
{
    int responding = 0;
    for (auto *bar : dock->findChildren<QProgressBar *>()) {
        if (bar->value() > 0) ++responding;
    }
    observations.insert("responding_meters", responding);
    const bool screenshot = dock_container->grab().save(result_dir + "/dock.png");
    if (responding < 2 || !screenshot || !dock->isVisible()) {
        finish(false, "voice/BGM meter response or visible screenshot missing");
        return;
    }
    if (phase == "restore") dock_container->hide();
    observations.insert("saved_dock_visible", dock_container->isVisible());
    if (!qEnvironmentVariable("LBM_PREVIEW_DEVICE").isEmpty()) { preview_test(); return; }
    finish(true, "settings and visibility checked; voice/BGM meters responded");
}

static void start_pcm()
{
    const uint32_t rate = audio_output_get_sample_rate(obs_get_audio());
    if (rate != 44100 && rate != 48000) { finish(false, "unsupported audio rate"); return; }
    const uint32_t frames = rate / 100;
    origin = os_gettime_ns();
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    producer = new QTimer(window);
    const bool preview_signal = !qEnvironmentVariable("LBM_PREVIEW_DEVICE").isEmpty();
    QObject::connect(producer, &QTimer::timeout, window, [rate, frames, preview_signal] {
        std::array<float, 480> voice_pcm{}, bgm_pcm{}, voice_right{}, bgm_right{};
        // Reuse finite-preview's nonperiodic sample-index signal. Independent channels
        // make exact alignment and channel-swap detection observable in the final mix.
        auto sample = [](uint64_t i, uint64_t salt) {
            uint64_t x = i + salt;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
            x ^= x >> 31;
            const float magnitude = float((x & 0x7fff) + 1) / 262144.0f;
            return x >> 63 ? -magnitude : magnitude;
        };
        for (uint32_t i = 0; i < frames; ++i) {
            if (preview_signal) {
                voice_pcm[i] = sample(position + i, 0x9E3779B97F4A7C15ULL);
                voice_right[i] = sample(position + i, 0xD1B54A32D192ED03ULL);
                bgm_pcm[i] = sample(position + i, 0x94D049BB133111EBULL) * 0.5f;
                bgm_right[i] = sample(position + i, 0xBF58476D1CE4E5B9ULL) * 0.5f;
            } else {
                voice_pcm[i] = 0.1f * float(std::sin(double(position + i) * 6.283185307179586 * 440 / rate));
                bgm_pcm[i] = 0.05f * float(std::sin(double(position + i) * 6.283185307179586 * 660 / rate));
                voice_right[i] = voice_pcm[i]; bgm_right[i] = bgm_pcm[i];
            }
        }
        obs_source_audio data{};
        data.frames = frames;
        data.speakers = SPEAKERS_STEREO;
        data.samples_per_sec = rate;
        data.format = AUDIO_FORMAT_FLOAT_PLANAR;
        data.timestamp = origin + position * 1000000000ULL / rate;
        data.data[0] = reinterpret_cast<const uint8_t *>(voice_pcm.data());
        data.data[1] = reinterpret_cast<const uint8_t *>(voice_right.data());
        obs_source_output_audio(voice, &data);
        data.data[0] = reinterpret_cast<const uint8_t *>(bgm_pcm.data());
        data.data[1] = reinterpret_cast<const uint8_t *>(bgm_right.data());
        obs_source_output_audio(bgm, &data);
        position += frames;
    });
    producer->start(10);
    QTimer::singleShot(5000, window, inspect_meter);
}

static void inspect_dock()
{
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (window) {
        for (auto *candidate : window->findChildren<QWidget *>("loudness-balance-monitor-dock")) {
            if (candidate->metaObject()->indexOfMethod("on_refresh_sources()") >= 0) {
                dock = candidate;
                break;
            }
        }
    }
    if (!dock) { finish(false, "product dock missing"); return; }
    for (QWidget *parent = dock->parentWidget(); parent; parent = parent->parentWidget()) {
        if ((dock_container = qobject_cast<QDockWidget *>(parent))) break;
    }
    if (!dock_container) { finish(false, "dock container missing"); return; }
    if (QString(obs_get_version_string()) != "33.0.1") { finish(false, "runtime mismatch"); return; }
    observations.insert("initial_dock_visible", dock_container->isVisible());
    if (phase == "setup") {
        voice = obs_source_create("lbm_baseline_signal", voice_name, nullptr, nullptr);
        bgm = obs_source_create("lbm_baseline_signal", bgm_name, nullptr, nullptr);
        if (!voice || !bgm) { finish(false, "test source creation failed"); return; }
        obs_source_set_audio_mixers(voice, 0);
        obs_source_set_audio_mixers(bgm, 0);
        obs_source_t *scene_source = obs_frontend_get_current_scene();
        obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
        if (!scene) {
            if (scene_source) obs_source_release(scene_source);
            finish(false, "isolated scene missing"); return;
        }
        const bool attached = obs_scene_add(scene, voice) && obs_scene_add(scene, bgm);
        obs_source_release(scene_source);
        if (!attached) { finish(false, "test sources could not be attached to scene"); return; }
        if (!QMetaObject::invokeMethod(dock, "on_refresh_sources", Qt::DirectConnection) || !inspect_settings(true)) {
            finish(false, "setup controls unavailable"); return;
        }
    } else {
        // Inspect restored UI before changing controls, refreshing lists or showing the dock.
        if (!inspect_settings(false) || dock_container->isVisible() != (phase == "restore")) {
            finish(false, "settings or dock visibility did not restore"); return;
        }
        voice = obs_get_source_by_name(voice_name);
        bgm = obs_get_source_by_name(bgm_name);
        if (!voice || !bgm || QString(obs_source_get_id(voice)) != "lbm_baseline_signal" ||
            QString(obs_source_get_id(bgm)) != "lbm_baseline_signal" ||
            obs_source_get_audio_mixers(voice) != 0 || obs_source_get_audio_mixers(bgm) != 0) {
            finish(false, "saved test sources missing or changed"); return;
        }
    }
    dock_container->show();
    start_pcm();
}

static void event(obs_frontend_event e, void *)
{
    if (qEnvironmentVariable("LBM_PREVIEW_CASE") == "recording-interrupt" && voice && !finished) {
        if (e == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
            auto *play = dock->findChild<QPushButton *>("previewPlay");
            const bool restored = obs_source_get_monitoring_enabled(voice);
            observations.insert("monitor_restored_at_recording_started", restored);
            if (!preview_check(!play->isVisible() && restored,
                               "recording start failed to stop preview and restore monitor")) return;
            observations.insert("preview_recording_start_interrupt", true);
            // The product refreshes output-dependent controls on its 100 ms UI timer.
            QTimer::singleShot(300, dock, [] {
                if (finished) return;
                auto *record = dock->findChild<QPushButton *>("previewRecord");
                auto *play = dock->findChild<QPushButton *>("previewPlay");
                if (!preview_check(obs_frontend_recording_active() && !record->isEnabled() && !play->isVisible(),
                                   "recording controls did not become blocked")) return;
                observations.insert("recording_controls_blocked", true);
                obs_frontend_recording_stop();
            });
        }
        if (e == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
            QTimer::singleShot(300, dock, [] {
                if (finished) return;
                auto *record = dock->findChild<QPushButton *>("previewRecord");
                auto *play = dock->findChild<QPushButton *>("previewPlay");
                if (!preview_check(record->isEnabled() && !play->isVisible(), "interrupted recording retained preview clip")) return;
                if (!save_loopback("recording-interrupted")) return;
                obs_source_set_monitoring_enabled(voice, false);
                obs_source_set_audio_mixers(voice, 0);
                obs_source_set_audio_mixers(bgm, 0);
                finish(true, "actual OBS recording interrupted preview and restored monitor");
            });
        }
    }
    if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING)
        QTimer::singleShot(1000, static_cast<QMainWindow *>(obs_frontend_get_main_window()), inspect_dock);
}

bool obs_module_load()
{
    result_dir = qEnvironmentVariable("LBM_BASELINE_RESULT_DIR");
    phase = qEnvironmentVariable("LBM_BASELINE_PHASE");
    if (result_dir.isEmpty() || (phase != "setup" && phase != "restore" && phase != "restore-hidden")) return false;
    if (!QDir().mkpath(result_dir)) return false;
    obs_source_info info{};
    info.id = "lbm_baseline_signal";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = [](void *) { return "baseline test signal"; };
    info.create = [](obs_data_t *, obs_source_t *) -> void * { return new int(0); };
    info.destroy = [](void *p) { delete static_cast<int *>(p); };
    obs_register_source(&info);
    obs_frontend_add_event_callback(event, nullptr);
    return true;
}

void obs_module_unload() { obs_frontend_remove_event_callback(event, nullptr); }
