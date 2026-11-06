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

static void finish(bool passed, const QString &reason)
{
    if (finished) return;
    finished = true;
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
    if (window) window->close();
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
        for (const auto *name : {"previewRecord", "previewPlay", "previewCancel"}) {
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

static void preview_test()
{
    auto *record = dock->findChild<QPushButton *>("previewRecord");
    auto *play = dock->findChild<QPushButton *>("previewPlay");
    auto *cancel = dock->findChild<QPushButton *>("previewCancel");
    auto *device = dock->findChild<QComboBox *>("previewDevice");
    auto *monitor = dock->findChild<QCheckBox *>("previewMonitorOff");
    auto *status = dock->findChild<QLabel *>("previewStatus");
    if (!preview_check(record && play && cancel && device && monitor && status, "preview controls missing")) return;
    const QString wanted = qEnvironmentVariable("LBM_PREVIEW_DEVICE");
    const int index = device->findData(wanted);
    if (!preview_check(index >= 0, "explicit preview device unavailable")) return;
    device->setCurrentIndex(index);
    monitor->setChecked(true);
    obs_source_set_audio_mixers(voice, 1);
    obs_source_set_audio_mixers(bgm, 1);
    record->click();
    if (!preview_check(!record->isEnabled() && !play->isEnabled(), "countdown did not start")) return;
    QTimer::singleShot(1000, dock, [=] {
        if (finished) return;
        preview_check(!record->isEnabled() && (status->text() == product_text("PreviewCountdown").arg(2) || status->text() == product_text("PreviewCountdown").arg(3)), "countdown not retained");
    });
    QTimer::singleShot(5500, dock, [=] {
        if (finished) return;
        if (!preview_check(record->isEnabled() && record->text() == product_text("PreviewFinish"), "recording did not start")) return;
        record->click();
        if (!preview_check(play->isEnabled() && record->text() == product_text("PreviewRerecord"), "manual recording did not freeze")) return;
        const QString ready_message = status->text();
        observations.insert("preview_recorded", ready_message);
        // End only these synthetic sources, as an end user ends external audio before preview.
        producer->stop();
        obs_source_set_monitoring_enabled(voice, true);
        play->click();
        if (!preview_check(play->text() == product_text("PreviewStop") && !obs_source_get_monitoring_enabled(voice), "monitor was not disabled for preview")) return;
        QTimer::singleShot(6000, dock, [=] {
            if (finished) return;
            if (!preview_check(play->text() == product_text("PreviewPlay") && play->isEnabled() &&
                               status->text() == ready_message && obs_source_get_monitoring_enabled(voice),
                               "natural playback completion or monitor restore failed")) return;
            observations.insert("preview_natural_end", true);
            play->click();
            QTimer::singleShot(300, dock, [=] {
                if (finished) return;
                if (!preview_check(play->text() == product_text("PreviewStop"), "replay did not start")) return;
                play->click();
                if (!preview_check(play->text() == product_text("PreviewPlay") && obs_source_get_monitoring_enabled(voice), "cancel did not restore monitoring")) return;
                observations.insert("preview_replay_stop", true);
                monitor->setChecked(false);
                play->click();
                if (!preview_check(obs_source_get_monitoring_enabled(voice), "opt-out changed monitoring")) return;
                QTimer::singleShot(300, dock, [=] {
                    if (finished) return;
                    play->click();
                    if (!preview_check(obs_source_get_monitoring_enabled(voice), "opt-out stop changed monitoring")) return;
                    monitor->setChecked(true);
                    record->click();
                    if (!preview_check(!record->isEnabled() && !play->isEnabled(), "rerecord countdown did not start")) return;
                    cancel->click();
                    if (!preview_check(record->isEnabled() && !play->isEnabled(), "record cancellation retained clip")) return;
                    observations.insert("preview_opt_out_and_rerecord_cancel", true);
                    observations.insert("preview_rate", int(audio_output_get_sample_rate(obs_get_audio())));
                    observations.insert("preview_device", wanted);
                    obs_source_set_monitoring_enabled(voice, false);
                    dock_container->resize(440, 1000);
                    if (!preview_check(dock_container->grab().save(result_dir + "/preview.png"), "preview screenshot failed")) return;
                    finish(true, "preview countdown/manual capture/playback/replay/stop/monitor restore/opt-out/cancel passed");
                });
            });
        });
    });
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
    QObject::connect(producer, &QTimer::timeout, window, [rate, frames] {
        std::array<float, 480> voice_pcm{}, bgm_pcm{};
        for (uint32_t i = 0; i < frames; ++i) {
            voice_pcm[i] = 0.1f * float(std::sin(double(position + i) * 6.283185307179586 * 440 / rate));
            bgm_pcm[i] = 0.05f * float(std::sin(double(position + i) * 6.283185307179586 * 660 / rate));
        }
        obs_source_audio data{};
        data.frames = frames;
        data.speakers = SPEAKERS_STEREO;
        data.samples_per_sec = rate;
        data.format = AUDIO_FORMAT_FLOAT_PLANAR;
        data.timestamp = origin + position * 1000000000ULL / rate;
        data.data[0] = reinterpret_cast<const uint8_t *>(voice_pcm.data());
        data.data[1] = data.data[0];
        obs_source_output_audio(voice, &data);
        data.data[0] = reinterpret_cast<const uint8_t *>(bgm_pcm.data());
        data.data[1] = data.data[0];
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
