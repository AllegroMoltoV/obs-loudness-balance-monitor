#include "loudness-dock.h"
#include "plugin-support.h"

#include <obs-frontend-api.h>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>

#include <cmath>

namespace lbm {

LoudnessDock::LoudnessDock(QWidget *parent) : QWidget(parent)
{
	// Create core components
	analyzer_ = std::make_unique<LoudnessAnalyzer>();
	capture_manager_ = std::make_unique<AudioCaptureManager>(*analyzer_);

	// Set sample rate from OBS
	audio_t *audio = obs_get_audio();
	if (audio) {
		uint32_t sample_rate = audio_output_get_sample_rate(audio);
		analyzer_->set_sample_rate(sample_rate);
	}

	setup_ui();
	load_settings();

	// Start analyzer
	analyzer_->start();

	// Start update timer (10 Hz)
	update_timer_ = new QTimer(this);
	connect(update_timer_, &QTimer::timeout, this, &LoudnessDock::on_update_timer);
	update_timer_->start(100);
}

LoudnessDock::~LoudnessDock()
{
	save_settings();

	if (update_timer_) {
		update_timer_->stop();
	}

	capture_manager_.reset();
	analyzer_->stop();
	analyzer_.reset();
}

void LoudnessDock::setup_ui()
{
	auto *main_layout = new QVBoxLayout(this);
	main_layout->setSpacing(8);
	main_layout->setContentsMargins(8, 8, 8, 8);

	// === Source Selection Group ===
	auto *source_group = new QGroupBox(obs_module_text("SourceSelection"));
	auto *source_layout = new QVBoxLayout(source_group);

	// Voice source
	auto *voice_layout = new QHBoxLayout();
	voice_layout->addWidget(new QLabel(obs_module_text("VoiceSource")));
	voice_source_combo_ = new QComboBox();
	voice_source_combo_->setMinimumWidth(150);
	voice_layout->addWidget(voice_source_combo_, 1);
	source_layout->addLayout(voice_layout);

	// BGM sources
	source_layout->addWidget(new QLabel(obs_module_text("BGMSources")));
	auto *bgm_scroll = new QScrollArea();
	bgm_scroll->setWidgetResizable(true);
	bgm_scroll->setMaximumHeight(100);
	bgm_source_container_ = new QWidget();
	bgm_source_layout_ = new QVBoxLayout(bgm_source_container_);
	bgm_source_layout_->setSpacing(2);
	bgm_source_layout_->setContentsMargins(4, 4, 4, 4);
	bgm_scroll->setWidget(bgm_source_container_);
	source_layout->addWidget(bgm_scroll);

	// Refresh button
	refresh_button_ = new QPushButton(obs_module_text("RefreshSources"));
	connect(refresh_button_, &QPushButton::clicked, this, &LoudnessDock::on_refresh_sources);
	source_layout->addWidget(refresh_button_);

	main_layout->addWidget(source_group);

	// === Status Indicators ===
	auto *status_group = new QGroupBox(obs_module_text("Status"));
	auto *status_layout = new QHBoxLayout(status_group);

	// Balance status
	auto *balance_frame = new QVBoxLayout();
	balance_status_ = new QFrame();
	balance_status_->setFixedSize(60, 40);
	balance_status_->setFrameStyle(QFrame::Box);
	balance_status_->setToolTip(obs_module_text("BalanceTooltip"));
	balance_status_label_ = new QLabel(obs_module_text("Balance"));
	balance_status_label_->setAlignment(Qt::AlignCenter);
	balance_status_label_->setToolTip(obs_module_text("BalanceTooltip"));
	balance_frame->addWidget(balance_status_);
	balance_frame->addWidget(balance_status_label_);
	status_layout->addLayout(balance_frame);

	// Mix status
	auto *mix_frame = new QVBoxLayout();
	mix_status_ = new QFrame();
	mix_status_->setFixedSize(60, 40);
	mix_status_->setFrameStyle(QFrame::Box);
	mix_status_->setToolTip(obs_module_text("MixTooltip"));
	mix_status_label_ = new QLabel(obs_module_text("Mix"));
	mix_status_label_->setAlignment(Qt::AlignCenter);
	mix_status_label_->setToolTip(obs_module_text("MixTooltip"));
	mix_frame->addWidget(mix_status_);
	mix_frame->addWidget(mix_status_label_);
	status_layout->addLayout(mix_frame);

	// Clip status
	auto *clip_frame = new QVBoxLayout();
	clip_status_ = new QFrame();
	clip_status_->setFixedSize(60, 40);
	clip_status_->setFrameStyle(QFrame::Box);
	clip_status_->setToolTip(obs_module_text("ClipTooltip"));
	clip_status_label_ = new QLabel(obs_module_text("Clip"));
	clip_status_label_->setAlignment(Qt::AlignCenter);
	clip_status_label_->setToolTip(obs_module_text("ClipTooltip"));
	clip_frame->addWidget(clip_status_);
	clip_frame->addWidget(clip_status_label_);
	status_layout->addLayout(clip_frame);

	main_layout->addWidget(status_group);

	// === Meters ===
	auto *meter_group = new QGroupBox(obs_module_text("Meters"));
	auto *meter_layout = new QVBoxLayout(meter_group);

	// VAD indicator
	auto *vad_layout = new QHBoxLayout();
	vad_layout->addWidget(new QLabel(obs_module_text("VAD")));
	vad_indicator_ = new QLabel();
	vad_indicator_->setFixedSize(20, 20);
	vad_indicator_->setStyleSheet("background-color: #888888; border-radius: 10px;");
	vad_layout->addWidget(vad_indicator_);
	vad_layout->addStretch();
	meter_layout->addLayout(vad_layout);

	// Voice meter
	auto *voice_meter_layout = new QHBoxLayout();
	voice_meter_layout->addWidget(new QLabel(obs_module_text("Voice")));
	voice_meter_ = new QProgressBar();
	voice_meter_->setRange(0, 100);
	voice_meter_->setTextVisible(false);
	voice_meter_->setFixedHeight(20);
	voice_meter_layout->addWidget(voice_meter_, 1);
	voice_lufs_label_ = new QLabel("-- LUFS");
	voice_lufs_label_->setFixedWidth(80);
	voice_meter_layout->addWidget(voice_lufs_label_);
	voice_peak_label_ = new QLabel("-- dB");
	voice_peak_label_->setFixedWidth(60);
	voice_meter_layout->addWidget(voice_peak_label_);
	meter_layout->addLayout(voice_meter_layout);

	// BGM meter
	auto *bgm_meter_layout = new QHBoxLayout();
	bgm_meter_layout->addWidget(new QLabel(obs_module_text("BGM")));
	bgm_meter_ = new QProgressBar();
	bgm_meter_->setRange(0, 100);
	bgm_meter_->setTextVisible(false);
	bgm_meter_->setFixedHeight(20);
	bgm_meter_layout->addWidget(bgm_meter_, 1);
	bgm_lufs_label_ = new QLabel("-- LUFS");
	bgm_lufs_label_->setFixedWidth(80);
	bgm_meter_layout->addWidget(bgm_lufs_label_);
	bgm_peak_label_ = new QLabel("-- dB");
	bgm_peak_label_->setFixedWidth(60);
	bgm_meter_layout->addWidget(bgm_peak_label_);
	meter_layout->addLayout(bgm_meter_layout);

	// Mix meter
	auto *mix_meter_layout = new QHBoxLayout();
	mix_meter_layout->addWidget(new QLabel(obs_module_text("MixMeter")));
	mix_meter_ = new QProgressBar();
	mix_meter_->setRange(0, 100);
	mix_meter_->setTextVisible(false);
	mix_meter_->setFixedHeight(20);
	mix_meter_layout->addWidget(mix_meter_, 1);
	mix_lufs_label_ = new QLabel("-- LUFS");
	mix_lufs_label_->setFixedWidth(80);
	mix_meter_layout->addWidget(mix_lufs_label_);
	mix_peak_label_ = new QLabel("-- dB");
	mix_peak_label_->setFixedWidth(60);
	mix_meter_layout->addWidget(mix_peak_label_);
	meter_layout->addLayout(mix_meter_layout);

	// Delta display
	auto *delta_layout = new QHBoxLayout();
	delta_layout->addWidget(new QLabel(obs_module_text("Delta")));
	delta_label_ = new QLabel("-- LU");
	delta_label_->setStyleSheet("font-size: 18px; font-weight: bold;");
	delta_layout->addWidget(delta_label_);
	delta_layout->addStretch();
	meter_layout->addLayout(delta_layout);

	main_layout->addWidget(meter_group);

	// === Settings ===
	auto *settings_group = new QGroupBox(obs_module_text("Settings"));
	auto *settings_layout = new QVBoxLayout(settings_group);

	// VAD threshold
	auto *vad_layout2 = new QHBoxLayout();
	vad_layout2->addWidget(new QLabel(obs_module_text("VADThreshold")));
	vad_threshold_slider_ = new QSlider(Qt::Horizontal);
	vad_threshold_slider_->setRange(-60, -20);
	vad_threshold_slider_->setValue(-40);
	vad_layout2->addWidget(vad_threshold_slider_, 1);
	vad_threshold_value_ = new QLabel("-40 dB");
	vad_threshold_value_->setFixedWidth(50);
	vad_layout2->addWidget(vad_threshold_value_);
	settings_layout->addLayout(vad_layout2);

	// Balance target
	auto *balance_layout = new QHBoxLayout();
	balance_layout->addWidget(new QLabel(obs_module_text("BalanceTarget")));
	balance_target_spin_ = new QDoubleSpinBox();
	balance_target_spin_->setRange(0.0, 20.0);
	balance_target_spin_->setValue(6.0);
	balance_target_spin_->setSuffix(" LU");
	balance_layout->addWidget(balance_target_spin_);
	balance_layout->addStretch();
	settings_layout->addLayout(balance_layout);

	// Mix preset
	auto *mix_layout = new QHBoxLayout();
	mix_layout->addWidget(new QLabel(obs_module_text("MixPreset")));
	mix_preset_combo_ = new QComboBox();
	mix_preset_combo_->addItem(obs_module_text("PresetYouTube"), 0); // OK: -18, WARN: -22
	mix_preset_combo_->addItem(obs_module_text("PresetQuiet"), 1);   // OK: -20, WARN: -24
	mix_preset_combo_->addItem(obs_module_text("PresetLoud"), 2);    // OK: -16, WARN: -20
	mix_layout->addWidget(mix_preset_combo_);
	mix_layout->addStretch();
	settings_layout->addLayout(mix_layout);

	main_layout->addWidget(settings_group);

	// === Help Section ===
	auto *help_group = new QGroupBox(obs_module_text("Help"));
	help_group->setCheckable(true);
	help_group->setChecked(false);
	auto *help_layout = new QVBoxLayout(help_group);

	auto *help_usage = new QLabel(obs_module_text("HelpUsage"));
	help_usage->setWordWrap(true);
	help_usage->setTextFormat(Qt::RichText);
	help_layout->addWidget(help_usage);

	auto *help_balance = new QLabel(obs_module_text("HelpBalance"));
	help_balance->setWordWrap(true);
	help_balance->setTextFormat(Qt::RichText);
	help_layout->addWidget(help_balance);

	auto *help_mix = new QLabel(obs_module_text("HelpMix"));
	help_mix->setWordWrap(true);
	help_mix->setTextFormat(Qt::RichText);
	help_layout->addWidget(help_mix);

	auto *help_clip = new QLabel(obs_module_text("HelpClip"));
	help_clip->setWordWrap(true);
	help_clip->setTextFormat(Qt::RichText);
	help_layout->addWidget(help_clip);

	main_layout->addWidget(help_group);

	// Add stretch at bottom
	main_layout->addStretch();

	// Connect signals
	connect(voice_source_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&LoudnessDock::on_voice_source_changed);
	connect(vad_threshold_slider_, &QSlider::valueChanged, this, &LoudnessDock::on_vad_threshold_changed);
	connect(balance_target_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
		&LoudnessDock::on_balance_target_changed);
	connect(mix_preset_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		&LoudnessDock::on_mix_preset_changed);

	// Initial source list
	refresh_source_lists();
}

void LoudnessDock::refresh_source_lists()
{
	// Get current selection
	QString current_voice = voice_source_combo_->currentText();
	std::vector<std::string> current_bgm = capture_manager_->bgm_source_names();

	// Clear
	voice_source_combo_->blockSignals(true);
	voice_source_combo_->clear();
	voice_source_combo_->addItem(obs_module_text("None"), "");

	// Clear BGM checkboxes
	for (auto *cb : bgm_checkboxes_) {
		bgm_source_layout_->removeWidget(cb);
		delete cb;
	}
	bgm_checkboxes_.clear();

	// Enumerate sources
	auto sources = AudioCaptureManager::enumerate_audio_sources();

	for (const auto &name : sources) {
		QString qname = QString::fromStdString(name);

		// Add to voice combo
		voice_source_combo_->addItem(qname, qname);

		// Add BGM checkbox
		auto *cb = new QCheckBox(qname);
		cb->setProperty("source_name", qname);

		// Check if was previously selected
		bool was_selected = std::find(current_bgm.begin(), current_bgm.end(), name) != current_bgm.end();
		cb->setChecked(was_selected);

		connect(cb, &QCheckBox::toggled, this, &LoudnessDock::on_bgm_source_toggled);
		bgm_source_layout_->addWidget(cb);
		bgm_checkboxes_.push_back(cb);
	}

	// Restore voice selection
	int voice_idx = voice_source_combo_->findText(current_voice);
	if (voice_idx >= 0) {
		voice_source_combo_->setCurrentIndex(voice_idx);
	}
	voice_source_combo_->blockSignals(false);
}

void LoudnessDock::on_update_timer()
{
	update_meters();
	update_status_colors();
}

void LoudnessDock::on_voice_source_changed(int index)
{
	Q_UNUSED(index);
	QString name = voice_source_combo_->currentData().toString();
	capture_manager_->set_voice_source(name.toStdString());
}

void LoudnessDock::on_bgm_source_toggled(bool checked)
{
	auto *cb = qobject_cast<QCheckBox *>(sender());
	if (!cb)
		return;

	QString name = cb->property("source_name").toString();
	if (checked) {
		capture_manager_->add_bgm_source(name.toStdString());
	} else {
		capture_manager_->remove_bgm_source(name.toStdString());
	}
}

void LoudnessDock::on_vad_threshold_changed(int value)
{
	vad_threshold_value_->setText(QString("%1 dB").arg(value));
	analyzer_->config().vad_threshold.store(static_cast<double>(value), std::memory_order_relaxed);
}

void LoudnessDock::on_balance_target_changed(double value)
{
	analyzer_->config().balance_target.store(value, std::memory_order_relaxed);
}

void LoudnessDock::on_mix_preset_changed(int index)
{
	double ok_thresh = -18.0;
	double warn_thresh = -22.0;

	switch (index) {
	case 0: // YouTube standard
		ok_thresh = -18.0;
		warn_thresh = -22.0;
		break;
	case 1: // Quiet/Safe
		ok_thresh = -20.0;
		warn_thresh = -24.0;
		break;
	case 2: // Loud/Aggressive
		ok_thresh = -16.0;
		warn_thresh = -20.0;
		break;
	}

	analyzer_->config().mix_ok_threshold.store(ok_thresh, std::memory_order_relaxed);
	analyzer_->config().mix_warn_threshold.store(warn_thresh, std::memory_order_relaxed);
}

void LoudnessDock::on_refresh_sources()
{
	refresh_source_lists();
}

void LoudnessDock::update_meters()
{
	const auto &results = analyzer_->results();
	bool voice_active = results.voice_active.load(std::memory_order_relaxed);

	// VAD indicator
	if (voice_active) {
		vad_indicator_->setStyleSheet("background-color: #4CAF50; border-radius: 10px;");
	} else {
		vad_indicator_->setStyleSheet("background-color: #888888; border-radius: 10px;");
	}

	// Voice meter
	double voice_lufs = results.voice_lufs.load(std::memory_order_relaxed);
	double voice_peak = results.voice_peak_dbfs.load(std::memory_order_relaxed);
	if (voice_active && voice_lufs != -HUGE_VAL) {
		voice_meter_->setValue(lufs_to_meter(voice_lufs));
		voice_meter_->setEnabled(true);
		voice_lufs_label_->setText(QString("%1 LUFS").arg(voice_lufs, 0, 'f', 1));
	} else {
		voice_meter_->setValue(0);
		voice_meter_->setEnabled(false);
		voice_lufs_label_->setText("-- LUFS");
	}
	if (voice_peak != -HUGE_VAL) {
		voice_peak_label_->setText(QString("%1 dB").arg(voice_peak, 0, 'f', 1));
	} else {
		voice_peak_label_->setText("-- dB");
	}

	// BGM meter
	double bgm_lufs = results.bgm_lufs.load(std::memory_order_relaxed);
	double bgm_peak = results.bgm_peak_dbfs.load(std::memory_order_relaxed);
	if (bgm_lufs != -HUGE_VAL) {
		bgm_meter_->setValue(lufs_to_meter(bgm_lufs));
		bgm_lufs_label_->setText(QString("%1 LUFS").arg(bgm_lufs, 0, 'f', 1));
	} else {
		bgm_meter_->setValue(0);
		bgm_lufs_label_->setText("-- LUFS");
	}
	if (bgm_peak != -HUGE_VAL) {
		bgm_peak_label_->setText(QString("%1 dB").arg(bgm_peak, 0, 'f', 1));
	} else {
		bgm_peak_label_->setText("-- dB");
	}

	// Mix meter
	double mix_lufs = results.mix_lufs.load(std::memory_order_relaxed);
	double mix_peak = results.mix_peak_dbfs.load(std::memory_order_relaxed);
	if (voice_active && mix_lufs != -HUGE_VAL) {
		mix_meter_->setValue(lufs_to_meter(mix_lufs));
		mix_meter_->setEnabled(true);
		mix_lufs_label_->setText(QString("%1 LUFS").arg(mix_lufs, 0, 'f', 1));
	} else {
		mix_meter_->setValue(0);
		mix_meter_->setEnabled(false);
		mix_lufs_label_->setText("-- LUFS");
	}
	if (mix_peak != -HUGE_VAL) {
		mix_peak_label_->setText(QString("%1 dB").arg(mix_peak, 0, 'f', 1));
	} else {
		mix_peak_label_->setText("-- dB");
	}

	// Delta
	double delta = results.balance_delta.load(std::memory_order_relaxed);
	if (voice_active && voice_lufs != -HUGE_VAL && bgm_lufs != -HUGE_VAL) {
		delta_label_->setText(QString("%1%2 LU").arg(delta >= 0 ? "+" : "").arg(delta, 0, 'f', 1));
	} else {
		delta_label_->setText("-- LU");
	}
}

void LoudnessDock::update_status_colors()
{
	const auto &results = analyzer_->results();

	Status balance = results.balance_status.load(std::memory_order_relaxed);
	Status mix = results.mix_status.load(std::memory_order_relaxed);
	Status clip = results.clip_status.load(std::memory_order_relaxed);

	balance_status_->setStyleSheet(status_to_style(balance));
	mix_status_->setStyleSheet(status_to_style(mix));
	clip_status_->setStyleSheet(status_to_style(clip));
}

int LoudnessDock::lufs_to_meter(double lufs) const
{
	// Map LUFS to 0-100 range
	// -60 LUFS = 0, 0 LUFS = 100
	if (lufs <= -60.0)
		return 0;
	if (lufs >= 0.0)
		return 100;
	return static_cast<int>((lufs + 60.0) / 60.0 * 100.0);
}

QString LoudnessDock::status_to_style(Status status) const
{
	switch (status) {
	case Status::OK:
		return "background-color: #4CAF50;"; // Green
	case Status::WARN:
		return "background-color: #FFC107;"; // Yellow
	case Status::BAD:
		return "background-color: #F44336;"; // Red
	}
	return "";
}

void LoudnessDock::save_settings()
{
	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;

	obs_data_t *settings = obs_data_create();

	// Save source selections
	capture_manager_->save_settings(settings);

	// Save other settings
	obs_data_set_int(settings, "vad_threshold", vad_threshold_slider_->value());
	obs_data_set_double(settings, "balance_target", balance_target_spin_->value());
	obs_data_set_int(settings, "mix_preset", mix_preset_combo_->currentIndex());

	obs_data_save_json_safe(settings, path, "tmp", "bak");
	obs_data_release(settings);
	bfree(path);
}

void LoudnessDock::load_settings()
{
	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;

	obs_data_t *settings = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);

	if (!settings)
		return;

	// Load source selections
	capture_manager_->load_settings(settings);

	// Refresh UI to show loaded sources
	refresh_source_lists();

	// Restore voice source selection in combo
	QString voice_name = QString::fromStdString(capture_manager_->voice_source_name());
	int voice_idx = voice_source_combo_->findData(voice_name);
	if (voice_idx >= 0) {
		voice_source_combo_->setCurrentIndex(voice_idx);
	}

	// Restore BGM checkboxes
	auto bgm_names = capture_manager_->bgm_source_names();
	for (auto *cb : bgm_checkboxes_) {
		QString name = cb->property("source_name").toString();
		bool selected = std::find(bgm_names.begin(), bgm_names.end(), name.toStdString()) != bgm_names.end();
		cb->blockSignals(true);
		cb->setChecked(selected);
		cb->blockSignals(false);
	}

	// Load other settings
	int vad_thresh = static_cast<int>(obs_data_get_int(settings, "vad_threshold"));
	if (vad_thresh != 0) {
		vad_threshold_slider_->setValue(vad_thresh);
		on_vad_threshold_changed(vad_thresh);
	}

	double balance_target = obs_data_get_double(settings, "balance_target");
	if (balance_target > 0.0) {
		balance_target_spin_->setValue(balance_target);
		on_balance_target_changed(balance_target);
	}

	int mix_preset = static_cast<int>(obs_data_get_int(settings, "mix_preset"));
	mix_preset_combo_->setCurrentIndex(mix_preset);
	on_mix_preset_changed(mix_preset);

	obs_data_release(settings);
}

} // namespace lbm
