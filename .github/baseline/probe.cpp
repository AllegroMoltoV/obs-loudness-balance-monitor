#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QTimer>
#include <cmath>
#include <array>

OBS_DECLARE_MODULE()
static obs_source_t *source = nullptr;
static QTimer *producer = nullptr;
static QWidget *dock = nullptr;
static QString result_dir;
static uint64_t position = 0;
static uint64_t origin = 0;

static void finish(bool passed, const QString &reason)
{
    if (producer) { producer->stop(); producer->deleteLater(); producer = nullptr; }
    if (source) { obs_source_release(source); source = nullptr; }
    QJsonObject result{{"passed", passed}, {"reason", reason}, {"obs", obs_get_version_string()}};
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
    blog(passed ? LOG_INFO : LOG_ERROR, "LBM_BASELINE %s %s", passed ? "PASS" : "FAIL",
         reason.toUtf8().constData());
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (window) window->close();
}
static void inspect_meter()
{
    bool measured = false;
    for (auto *bar : dock->findChildren<QProgressBar *>())
        measured |= bar->value() > 0;
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    bool screenshot = window->grab().save(result_dir + "/dock.png");
    finish(measured && screenshot && dock->isVisible(),
           measured ? "dock visible; meter responded to test PCM" : "meter did not respond");
}
static void inspect_dock()
{
    auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (window) {
        for (auto *candidate : window->findChildren<QWidget *>("loudness-balance-monitor-dock")) {
            if (candidate->metaObject()->indexOfMethod("on_refresh_sources()") >= 0) {
                dock = candidate; break;
            }
        }
    }
    if (!dock) { finish(false, "product dock missing"); return; }
    if (QString(obs_get_version_string()) != "33.0.1") { finish(false, "runtime mismatch"); return; }
    for (QWidget *p = dock; p; p = p->parentWidget()) p->show();
    const char *name = "LBM baseline signal";
    source = obs_source_create("lbm_baseline_signal", name, nullptr, nullptr);
    if (!source) { finish(false, "test source creation failed"); return; }
    obs_source_set_audio_mixers(source, 0);
    if (!QMetaObject::invokeMethod(dock, "on_refresh_sources", Qt::DirectConnection)) {
        finish(false, "source refresh unavailable"); return;
    }
    bool selected = false;
    for (auto *combo : dock->findChildren<QComboBox *>()) {
        int index = combo->findData(QString(name));
        if (index >= 0) { combo->setCurrentIndex(index); selected = true; break; }
    }
    if (!selected) { finish(false, "voice selection unavailable"); return; }
    const uint32_t rate = audio_output_get_sample_rate(obs_get_audio());
    const uint32_t frames = rate / 100;
    origin = os_gettime_ns();
    producer = new QTimer(window);
    QObject::connect(producer, &QTimer::timeout, window, [rate, frames] {
        std::array<float, 480> samples{};
        if (frames > samples.size()) { finish(false, "unsupported audio rate"); return; }
        for (uint32_t i = 0; i < frames; ++i)
            samples[i] = 0.1f * float(std::sin(double(position+i)*6.283185307179586*440/rate));
        obs_source_audio data{};
        data.data[0] = reinterpret_cast<const uint8_t *>(samples.data());
        data.data[1] = data.data[0];
        data.frames = frames; data.speakers = SPEAKERS_STEREO;
        data.samples_per_sec = rate; data.format = AUDIO_FORMAT_FLOAT_PLANAR;
        data.timestamp = origin + position*1000000000ULL/rate;
        obs_source_output_audio(source, &data); position += frames;
    });
    producer->start(10);
    QTimer::singleShot(5000, window, inspect_meter);
}
static void event(obs_frontend_event e, void *)
{
    if (e == OBS_FRONTEND_EVENT_FINISHED_LOADING)
        QTimer::singleShot(1000, static_cast<QMainWindow *>(obs_frontend_get_main_window()), inspect_dock);
}
bool obs_module_load()
{
    result_dir = qEnvironmentVariable("LBM_BASELINE_RESULT_DIR");
    if (result_dir.isEmpty()) return false;
    QDir().mkpath(result_dir);
    obs_source_info info{};
    info.id = "lbm_baseline_signal"; info.type = OBS_SOURCE_TYPE_INPUT; info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = [](void *) { return "baseline test signal"; };
    info.create = [](obs_data_t *, obs_source_t *) -> void * { return new int(0); };
    info.destroy = [](void *p) { delete static_cast<int *>(p); };
    obs_register_source(&info);
    obs_frontend_add_event_callback(event, nullptr);
    return true;
}
void obs_module_unload() { obs_frontend_remove_event_callback(event, nullptr); }
