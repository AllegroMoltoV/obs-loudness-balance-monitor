#pragma once

#include "audio-capture.h"
#include "loudness-analyzer.h"
#include "preview-session.h"

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <array>
#include <memory>
#include <vector>

namespace lbm {

class LoudnessDock : public QWidget {
	Q_OBJECT

public:
	explicit LoudnessDock(QWidget *parent = nullptr);
	~LoudnessDock() override;
	void on_scene_collection_cleanup();
	void on_scene_collection_changed();
	void interrupt_preview();

private slots:
	void on_update_timer();
	void on_voice_source_changed(int index);
	void on_bgm_source_toggled(bool checked);
	void on_vad_threshold_changed(int value);
	void on_balance_target_changed(double value);
	void on_mix_preset_changed(int index);
	void on_refresh_sources();

private:
	void setup_ui();
	void update_preview();
	void refresh_preview_devices();
	void refresh_source_lists();
	void update_meters();
	void update_status_colors();
	void save_settings();
	void load_settings();

	// Convert LUFS to meter value (0-100)
	int lufs_to_meter(double lufs) const;

	// Get status color stylesheet
	QString status_to_style(Status status) const;

	// UI Components - Source Selection
	QComboBox *voice_source_combo_{nullptr};
	QWidget *bgm_source_container_{nullptr};
	QVBoxLayout *bgm_source_layout_{nullptr};
	std::vector<QCheckBox *> bgm_checkboxes_;
	QPushButton *refresh_button_{nullptr};

    struct MonitorWidgets {
        std::array<QLabel *, 3> ratings{};
        std::array<QProgressBar *, 3> meters{};
        std::array<QLabel *, 3> lufs{};
        std::array<QLabel *, 3> peaks{};
        QLabel *vad{nullptr};
        QLabel *delta{nullptr};
    };
    MonitorWidgets add_monitor_widgets(QVBoxLayout *layout, const QString &prefix, bool whole_recording);
    void show_monitor_values(const MonitorWidgets &widgets, const std::array<double, 3> &lufs,
                             const std::array<double, 3> &peaks, double delta, bool voice_active);
    void show_monitor_rating(QLabel *label, std::optional<Status> status);
    MonitorWidgets live_monitor_;
    MonitorWidgets preview_instant_monitor_;
    MonitorWidgets preview_whole_monitor_;

	// UI Components - Settings
	QSlider *vad_threshold_slider_{nullptr};
	QLabel *vad_threshold_value_{nullptr};
	QDoubleSpinBox *balance_target_spin_{nullptr};
	QComboBox *mix_preset_combo_{nullptr};

    QComboBox *preview_track_{nullptr};
    QCheckBox *preview_manual_track_{nullptr};
    QLabel *preview_track_info_{nullptr};
    QLabel *preview_recorded_track_{nullptr};
    QComboBox *preview_device_{nullptr};
    QCheckBox *preview_monitor_{nullptr};
    QLabel *preview_status_{nullptr};
    QWidget *preview_analysis_group_{nullptr};
    QLabel *preview_realtime_{nullptr};
    QLabel *preview_average_{nullptr};
    QPushButton *preview_record_{nullptr};
    QPushButton *preview_play_{nullptr};
    QPushButton *preview_refresh_{nullptr};
    bool preview_has_played_{false};
    std::unique_ptr<PreviewSession> preview_;

	// Core components
	std::unique_ptr<LoudnessAnalyzer> analyzer_;
	std::unique_ptr<AudioCaptureManager> capture_manager_;
	QTimer *update_timer_{nullptr};
};

} // namespace lbm
