#pragma once
#include <util/config-file.h>
#include <util/bmem.h>

// OBS settings are isolated fixtures. Product actions use the visible controls.
static void prepare_profile_track_test(std::function<void()> next)
{
    auto *manual = dock->findChild<QCheckBox *>("previewManualTrack");
    auto *track = dock->findChild<QComboBox *>("previewTrack");
    auto *record = dock->findChild<QPushButton *>("previewRecord");
    auto *info = dock->findChild<QLabel *>("previewTrackInfo");
    if (!preview_check(manual && track && record && info, "profile scenario controls missing")) return;
    struct Run {
        int step = 0;
        bool applied = false;
        uint64_t started = os_gettime_ns();
        QJsonArray checks;
        std::shared_ptr<obs_service_t> original;
    };
    auto run = std::make_shared<Run>();
    auto *poll = new QTimer(dock);
    QObject::connect(poll, &QTimer::timeout, dock, [=] {
        if (finished) { poll->stop(); poll->deleteLater(); return; }
        if (os_gettime_ns() - run->started > 10000000000ULL) {
            observations.insert("track_setup_failed_step", run->step);
            preview_check(false, "profile scenario timed out waiting for product controls"); return;
        }
        auto *config = obs_frontend_get_profile_config();
        if (!preview_check(config != nullptr, "profile config missing")) return;
        if (!run->applied) {
            run->applied = true;
            if (run->step == 2) { obs_frontend_set_current_profile("ProfileAlt"); return; }
            config_set_string(config, "Output", "Mode", "Advanced");
            if (run->step == 0) { config_set_int(config, "AdvOut", "TrackIndex", 2); manual->setChecked(false); }
            if (run->step == 1) config_set_int(config, "AdvOut", "TrackIndex", 3);
            if (run->step == 3) { track->setCurrentIndex(track->findData(4)); manual->setChecked(true); config_set_int(config, "AdvOut", "TrackIndex", 6); }
            if (run->step == 4) { config_set_int(config, "AdvOut", "TrackIndex", 0); manual->setChecked(false); }
            if (run->step == 5 || run->step == 7) manual->setChecked(true);
            if (run->step == 6) {
                run->original.reset(obs_service_get_ref(obs_frontend_get_streaming_service()), obs_service_release);
                auto *settings = obs_data_create();
                obs_data_set_string(settings, "server", "srt://127.0.0.1:9");
                auto *service = obs_service_create("rtmp_custom", "isolated-track-fixture", settings, nullptr);
                obs_data_release(settings);
                if (!preview_check(service != nullptr, "SRT fixture service unavailable")) return;
                obs_frontend_set_streaming_service(service);
                obs_service_release(service);
                config_set_uint(config, "AdvOut", "StreamMultiTrackAudioMixes", 3);
                manual->setChecked(false);
            }
            if (run->step == 8) {
                obs_frontend_set_streaming_service(run->original.get());
                config_set_int(config, "AdvOut", "TrackIndex", 4);
                manual->setChecked(false);
            }
            return;
        }
        bool matched = false;
        if (run->step == 4) matched = !record->isEnabled() && info->text() == product_text("PreviewTrackUnavailable");
        else if (run->step == 6) matched = !record->isEnabled() && info->text() == product_text("PreviewTrackMultiple");
        else if (run->step == 3 || run->step == 5 || run->step == 7)
            matched = record->isEnabled() && manual->isChecked() && track->isEnabled() && track->currentData().toInt() == 4;
        else {
            const int expected = run->step == 0 ? 2 : run->step == 1 ? 3 : 4;
            matched = record->isEnabled() && !manual->isChecked() && info->text() == product_text("PreviewAutoTrack").arg(expected);
            if (run->step == 2) {
                char *name = obs_frontend_get_current_profile();
                matched = matched && name && QString::fromUtf8(name) == "ProfileAlt";
                bfree(name);
            }
        }
        if (!matched) return;
        run->checks.append(QJsonObject{{"step", run->step}, {"manual", manual->isChecked()}, {"record_enabled", record->isEnabled()}, {"explanation", info->text()}});
        if (++run->step == 9) {
            observations.insert("track_selection_checks", run->checks);
            poll->stop(); poll->deleteLater(); next(); return;
        }
        run->applied = false; run->started = os_gettime_ns();
    });
    poll->start(20);
}
