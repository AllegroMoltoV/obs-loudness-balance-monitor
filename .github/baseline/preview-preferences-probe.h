#pragma once

// Include after preview_check() and product_text(); call after inspect_settings().
// Returning true means this isolated preferences scenario owns normal shutdown.
static bool inspect_preview_preferences()
{
    const QString mode = qEnvironmentVariable("LBM_PREVIEW_PREFS");
    if (mode.isEmpty()) return false;
    if (!preview_check(mode == "migrate" || mode == "restored-play" || mode == "save-auto" || mode == "restore-auto",
                       "unknown preview preferences scenario")) return true;

    auto *device = dock->findChild<QComboBox *>("previewDevice");
    auto *manual = dock->findChild<QCheckBox *>("previewManualTrack");
    auto *track = dock->findChild<QComboBox *>("previewTrack");
    auto *monitor = dock->findChild<QCheckBox *>("previewMonitorOff");
    auto *record = dock->findChild<QPushButton *>("previewRecord");
    auto *play = dock->findChild<QPushButton *>("previewPlay");
    auto *status = dock->findChild<QLabel *>("previewStatus");
    auto *recorded_track = dock->findChild<QLabel *>("previewRecordedTrack");
    auto *session = dock->findChild<QObject *>("previewSession");
    if (!preview_check(device && manual && track && monitor && record && play && status && recorded_track && session,
                       "preview preferences controls or session missing")) return true;
    const QString expected_device = qEnvironmentVariable("LBM_PREVIEW_EXPECT_DEVICE");
    const bool expected_manual = mode != "restore-auto";
    observations.insert("preview_preferences_before", QJsonObject{
        {"scenario", mode}, {"device", device->currentData().toString()},
        {"manual", manual->isChecked()}, {"track", track->currentData().toInt()},
        {"monitor_off", monitor->isChecked()}, {"status", status->text()},
        {"recorded_frames", qint64(session->property("recordedFrames").toULongLong())},
        {"recorded_timestamp", QString::number(session->property("recordedTimestamp").toULongLong())},
        {"recorded_rate", int(session->property("recordedRate").toUInt())}});
    if (!preview_check(!expected_device.isEmpty() && device->currentData().toString() == expected_device &&
                       manual->isChecked() == expected_manual && track->currentData().toInt() == 2 &&
                       !monitor->isChecked(), "preview preferences did not restore before interaction")) return true;
    // isHidden checks the product's explicit visibility even when the dock was saved hidden.
    if (!preview_check(manual->isEnabled() && track->isEnabled() == expected_manual &&
                       track->isHidden() != expected_manual && record->text() == product_text("PreviewRecord") &&
                       play->isHidden() && recorded_track->isHidden() &&
                       status->text().startsWith(product_text("PreviewIdle")) &&
                       session->property("recordedFrames").isValid() &&
                       session->property("recordedTimestamp").isValid() &&
                       session->property("recordedRate").isValid() &&
                       session->property("recordedFrames").toULongLong() == 0 &&
                       session->property("recordedTimestamp").toULongLong() == 0 &&
                       session->property("recordedRate").toUInt() == 0,
                       "preview startup restored a clip or running state")) return true;

    if (mode == "save-auto") {
        manual->setChecked(false);
        if (!preview_check(!manual->isChecked() && !track->isEnabled() && track->isHidden() &&
                           track->currentData().toInt() == 2,
                           "automatic selection discarded the retained manual track")) return true;
    }
    observations.insert("preview_preferences_after_manual", manual->isChecked());
    observations.insert("preview_preferences_idle_without_clip", true);
    if (mode == "restored-play") return false;
    finish(true, "preview preferences observed before interaction; normal shutdown requested");
    return true;
}
