#pragma once

#include "audio-capture.h"
#include "loudness-analyzer.h"

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

#include <memory>
#include <vector>

namespace lbm {

class LoudnessDock : public QWidget {
	Q_OBJECT

public:
	explicit LoudnessDock(QWidget *parent = nullptr);
	~LoudnessDock() override;

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

	// UI Components - Meters
	QProgressBar *voice_meter_{nullptr};
	QProgressBar *bgm_meter_{nullptr};
	QProgressBar *mix_meter_{nullptr};
	QLabel *voice_lufs_label_{nullptr};
	QLabel *bgm_lufs_label_{nullptr};
	QLabel *mix_lufs_label_{nullptr};
	QLabel *voice_peak_label_{nullptr};
	QLabel *bgm_peak_label_{nullptr};
	QLabel *mix_peak_label_{nullptr};
	QLabel *delta_label_{nullptr};
	QLabel *vad_indicator_{nullptr};

	// UI Components - Status
	QFrame *balance_status_{nullptr};
	QFrame *mix_status_{nullptr};
	QFrame *clip_status_{nullptr};
	QLabel *balance_status_label_{nullptr};
	QLabel *mix_status_label_{nullptr};
	QLabel *clip_status_label_{nullptr};

	// UI Components - Settings
	QSlider *vad_threshold_slider_{nullptr};
	QLabel *vad_threshold_value_{nullptr};
	QDoubleSpinBox *balance_target_spin_{nullptr};
	QComboBox *mix_preset_combo_{nullptr};

	// Core components
	std::unique_ptr<LoudnessAnalyzer> analyzer_;
	std::unique_ptr<AudioCaptureManager> capture_manager_;
	QTimer *update_timer_{nullptr};
};

} // namespace lbm
