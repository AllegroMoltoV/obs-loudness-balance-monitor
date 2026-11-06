#include "loudness-dock.h"
#include "plugin-support.h"
#include "preview-output.h"
#include "preview-track.h"

#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QDialog>
#include <QDialogButtonBox>
#include <QToolButton>
#include <QStyle>
#include <QSizePolicy>
#include <QPainter>

#include <cmath>

namespace lbm {
namespace {
QLabel *wrapped_label(const QString &text = {})
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

void add_section_help(QVBoxLayout *layout, QWidget *parent, const char *title_key, const QString &body)
{
    auto *help = new QToolButton(parent);
    help->setText("?");
    help->setObjectName(QString("help_%1").arg(title_key));
    const QString title = QString::fromUtf8(obs_module_text(title_key));
    help->setAccessibleName(QString::fromUtf8(obs_module_text("SectionHelp")).arg(title));
    help->setToolTip(help->accessibleName());
    help->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(help, 0, Qt::AlignRight);
    QObject::connect(help, &QToolButton::clicked, parent, [parent, title, body] {
        auto *dialog = new QDialog(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(title);
        auto *outer = new QVBoxLayout(dialog);
        auto *scroll = new QScrollArea(dialog);
        scroll->setWidgetResizable(true);
        auto *label = wrapped_label(body);
        label->setTextFormat(Qt::RichText);
        label->setMargin(12);
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        scroll->setWidget(label);
        outer->addWidget(scroll);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        outer->addWidget(buttons);
        dialog->resize(480, 360);
        dialog->show();
    });
}
} // namespace


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

	preview_ = std::make_unique<PreviewSession>(this);
    preview_->setObjectName("previewSession");
	setup_ui();
	load_settings();
	connect(preview_.get(), &PreviewSession::changed, this, &LoudnessDock::update_preview);
	update_preview();

	// Start analyzer
	analyzer_->start();

	// Start update timer (10 Hz)
	update_timer_ = new QTimer(this);
	connect(update_timer_, &QTimer::timeout, this, &LoudnessDock::on_update_timer);
	update_timer_->start(100);
}

LoudnessDock::~LoudnessDock()
{
	disconnect(preview_.get(), nullptr, this, nullptr);
	preview_->cancel();
	save_settings();

	if (update_timer_) {
		update_timer_->stop();
	}

	capture_manager_.reset();
	analyzer_->stop();
	analyzer_.reset();
}

void LoudnessDock::on_scene_collection_cleanup()
{
	interrupt_preview();
	capture_manager_->detach_sources();
}

void LoudnessDock::on_scene_collection_changed()
{
	PreviewMonitors::recover();
	capture_manager_->reattach_sources();
	refresh_source_lists();
}

void LoudnessDock::setup_ui()
{
	// Create scroll area for the entire dock content
	auto *scroll_area = new QScrollArea(this);
	scroll_area->setWidgetResizable(true);
	scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll_area->setFrameShape(QFrame::NoFrame);

	// Container widget for all content
	auto *content_widget = new QWidget();
	auto *main_layout = new QVBoxLayout(content_widget);
	main_layout->setSpacing(8);
	main_layout->setContentsMargins(8, 8, 8, 8);


    auto *preview_group = new QGroupBox(obs_module_text("PreviewTitle"));
    auto *preview_layout = new QVBoxLayout(preview_group);
    add_section_help(preview_layout, preview_group, "PreviewTitle", obs_module_text("PreviewHelpDetail"));
    preview_layout->addWidget(wrapped_label(obs_module_text("PreviewHelp")));
    preview_status_ = wrapped_label();
    preview_status_->setObjectName("previewStatus");
    preview_status_->setMargin(6);
    preview_layout->addWidget(preview_status_);
    preview_recorded_track_ = wrapped_label();
    preview_recorded_track_->setObjectName("previewRecordedTrack");
    preview_layout->addWidget(preview_recorded_track_);
    preview_realtime_ = wrapped_label();
    preview_realtime_->setObjectName("previewRealtime");
    preview_average_ = wrapped_label();
    preview_average_->setObjectName("previewAverage");
    preview_realtime_->setToolTip(obs_module_text("PreviewAnalysisHelp"));
    preview_average_->setToolTip(obs_module_text("PreviewAnalysisHelp"));
    preview_layout->addWidget(preview_realtime_);
    preview_layout->addWidget(preview_average_);

    // Stacked actions keep their full text visible in a narrow OBS dock.
    preview_record_ = new QPushButton();
    preview_record_->setObjectName("previewRecord");
    preview_play_ = new QPushButton(obs_module_text("PreviewPlay"));
    preview_play_->setObjectName("previewPlay");
    preview_layout->addWidget(preview_record_);
    preview_layout->addWidget(preview_play_);
    preview_record_->setStyleSheet("QPushButton:enabled { border: 2px solid #d16969; padding: 6px; font-weight: bold; }");
    preview_play_->setStyleSheet("QPushButton:enabled { border: 2px solid #4c9ed9; padding: 6px; font-weight: bold; }");

    preview_layout->addWidget(wrapped_label(obs_module_text("PreviewDevice")));
    preview_device_ = new QComboBox();
    preview_device_->setObjectName("previewDevice");
    preview_device_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    preview_device_->setMinimumContentsLength(1);
    preview_device_->setMinimumWidth(0);
    preview_layout->addWidget(preview_device_);
    preview_refresh_ = new QPushButton(obs_module_text("PreviewRefresh"));
    connect(preview_refresh_, &QPushButton::clicked, this, &LoudnessDock::refresh_preview_devices);
    preview_layout->addWidget(preview_refresh_);

    preview_manual_track_ = new QCheckBox(obs_module_text("PreviewManualTrack"));
    preview_manual_track_->setObjectName("previewManualTrack");
    preview_manual_track_->setToolTip(obs_module_text("PreviewTrackHelp"));
    preview_layout->addWidget(preview_manual_track_);
    preview_track_ = new QComboBox();
    preview_track_->setObjectName("previewTrack");
    for (int i = 0; i < 6; ++i)
        preview_track_->addItem(QString::fromUtf8(obs_module_text("PreviewTrackNumber")).arg(i + 1), i);
    preview_track_->setToolTip(obs_module_text("PreviewTrackHelp"));
    preview_layout->addWidget(preview_track_);
    preview_track_info_ = wrapped_label();
    preview_track_info_->setObjectName("previewTrackInfo");
    preview_layout->addWidget(preview_track_info_);
    connect(preview_manual_track_, &QCheckBox::toggled, this, &LoudnessDock::update_preview);

    preview_monitor_ = new QCheckBox(obs_module_text("PreviewMonitorOff"));
    preview_monitor_->setObjectName("previewMonitorOff");
    preview_monitor_->setToolTip(obs_module_text("PreviewMonitorHelp"));
    preview_monitor_->setChecked(true);
    connect(preview_monitor_, &QCheckBox::toggled, this, &LoudnessDock::update_preview);
    preview_layout->addWidget(preview_monitor_);

    connect(preview_record_, &QPushButton::clicked, this, [this] {
        using State = PreviewSession::State;
        if (preview_->state() == State::preparing) {
            preview_->cancel();
        } else if (preview_->state() == State::recording) {
            preview_->finish();
        } else {
            const auto choice = preview_manual_track_->isChecked()
                ? PreviewTrackChoice{preview_track_->currentData().toInt(), {}}
                : resolve_preview_track();
            if (choice.track < 0) { update_preview(); return; }
            const auto &config = analyzer_->config();
            preview_->configure_analysis(capture_manager_->voice_source_name(), capture_manager_->bgm_source_names(),
                config.vad_threshold.load(), config.balance_target.load(), config.hysteresis.load());
            preview_has_played_ = false;
            preview_->prepare(unsigned(choice.track), preview_device_->currentData().toString().toStdString(),
                              preview_monitor_->isChecked());
        }
    });
    connect(preview_play_, &QPushButton::clicked, this, [this] {
        if (preview_->state() == PreviewSession::State::waiting) preview_->stop_waiting();
        else if (preview_->state() == PreviewSession::State::playing) preview_->stop_playback();
        else preview_->play(preview_device_->currentData().toString().toStdString(), preview_monitor_->isChecked());
    });
    refresh_preview_devices();

	// === Source Selection Group ===
	auto *source_group = new QGroupBox(obs_module_text("SourceSelection"));
	auto *source_layout = new QVBoxLayout(source_group);
	add_section_help(source_layout, source_group, "SourceSelection", obs_module_text("HelpUsage"));

	// Voice source
	auto *voice_layout = new QVBoxLayout();
	voice_layout->addWidget(new QLabel(obs_module_text("VoiceSource")));
	voice_source_combo_ = new QComboBox();
	voice_source_combo_->setMinimumWidth(0);
	voice_source_combo_->setMinimumContentsLength(1);
	voice_source_combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
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



	// === Status Indicators ===
	auto *status_group = new QGroupBox(obs_module_text("Status"));
	auto *status_outer = new QVBoxLayout(status_group);
	add_section_help(status_outer, status_group, "Status", QString::fromUtf8(obs_module_text("HelpBalance")) + "<br><br>" + obs_module_text("HelpMix") + "<br><br>" + obs_module_text("HelpClip"));
	auto *status_layout = new QHBoxLayout();
	status_outer->addLayout(status_layout);

	// Balance status
	auto *balance_frame = new QVBoxLayout();
	balance_status_ = new QFrame();
	balance_status_->setFixedSize(60, 40);
	balance_status_->setFrameStyle(QFrame::Box);
	balance_status_->setToolTip(obs_module_text("BalanceTooltip"));
	balance_status_label_ = new QLabel(obs_module_text("Balance"));
	balance_status_label_->setAlignment(Qt::AlignCenter);
	balance_status_label_->setWordWrap(true);
	balance_status_label_->setMinimumWidth(0);
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
	mix_status_label_->setWordWrap(true);
	mix_status_label_->setMinimumWidth(0);
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
	clip_status_label_->setWordWrap(true);
	clip_status_label_->setMinimumWidth(0);
	clip_status_label_->setToolTip(obs_module_text("ClipTooltip"));
	clip_frame->addWidget(clip_status_);
	clip_frame->addWidget(clip_status_label_);
	status_layout->addLayout(clip_frame);



	// === Meters ===
	auto *meter_group = new QGroupBox(obs_module_text("Meters"));
	auto *meter_layout = new QVBoxLayout(meter_group);
	add_section_help(meter_layout, meter_group, "Meters", obs_module_text("HelpMeters"));

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
	voice_meter_->setMinimumWidth(0);
	voice_meter_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
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
	bgm_meter_->setMinimumWidth(0);
	bgm_meter_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
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
	mix_meter_->setMinimumWidth(0);
	mix_meter_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
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



	// === Settings ===
	auto *settings_group = new QGroupBox(obs_module_text("Settings"));
	auto *settings_layout = new QVBoxLayout(settings_group);
	add_section_help(settings_layout, settings_group, "Settings", obs_module_text("HelpSettings"));

	// VAD threshold
	auto *vad_layout2 = new QVBoxLayout();
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
	auto *balance_layout = new QVBoxLayout();
	balance_layout->addWidget(new QLabel(obs_module_text("BalanceTarget")));
	balance_target_spin_ = new QDoubleSpinBox();
	balance_target_spin_->setRange(0.0, 20.0);
	balance_target_spin_->setValue(6.0);
	balance_target_spin_->setSuffix(" LU");
	balance_layout->addWidget(balance_target_spin_);
	balance_layout->addStretch();
	settings_layout->addLayout(balance_layout);

	// Mix preset
	auto *mix_layout = new QVBoxLayout();
	mix_layout->addWidget(new QLabel(obs_module_text("MixPreset")));
	mix_preset_combo_ = new QComboBox();
	mix_preset_combo_->addItem(obs_module_text("PresetYouTube"), 0); // OK: -18, WARN: -22
	mix_preset_combo_->addItem(obs_module_text("PresetQuiet"), 1);   // OK: -20, WARN: -24
	mix_preset_combo_->addItem(obs_module_text("PresetLoud"), 2);    // OK: -16, WARN: -20
	mix_layout->addWidget(mix_preset_combo_);
	mix_layout->addStretch();
	settings_layout->addLayout(mix_layout);



    main_layout->addWidget(status_group);
    main_layout->addWidget(meter_group);
    main_layout->addWidget(preview_group);
    main_layout->addWidget(source_group);
    main_layout->addWidget(settings_group);
    main_layout->addStretch();

	// Set the content widget in scroll area
	scroll_area->setWidget(content_widget);

	// Set the scroll area as the dock's main widget
	auto *dock_layout = new QVBoxLayout(this);
	dock_layout->setContentsMargins(0, 0, 0, 0);
	dock_layout->addWidget(scroll_area);

    // Let OBS size the dock; wrapped text must not enlarge its content width.
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

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


void LoudnessDock::interrupt_preview()
{
    preview_->cancel();
}

void LoudnessDock::refresh_preview_devices()
{
    const QString selected = preview_device_->currentData().toString();
    preview_device_->clear();
    preview_device_->addItem(obs_module_text("PreviewSelectDevice"), "");
    for (const auto &device : preview_devices())
        preview_device_->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id));
    int index = preview_device_->findData(selected);
    if (index < 0 && !selected.isEmpty()) {
        preview_device_->addItem(QString::fromUtf8(obs_module_text("PreviewUnavailable")).arg(selected), selected);
        index = preview_device_->count() - 1;
    }
    preview_device_->setCurrentIndex(std::max(index, 0));
}

void LoudnessDock::update_preview()
{
    if (!preview_manual_track_ || !preview_monitor_) return;
    using State = PreviewSession::State;
    const auto state = preview_->state();
    const bool has_recording = preview_->recorded_frames() != 0;
    auto show_analysis = [](QLabel *label, const char *title, double voice, double bgm, std::optional<Status> status) {
        const auto number = [](double v) { return std::isfinite(v) ? QString::number(v, 'f', 1) : QString("--"); };
        const char *key = !status ? "PreviewAnalysisUnrated" :
            *status == Status::OK ? "PreviewAnalysisGreen" : *status == Status::WARN ? "PreviewAnalysisYellow" : "PreviewAnalysisRed";
        const char *color = !status ? "#888888" :
            *status == Status::OK ? "#4CAF50" : *status == Status::WARN ? "#FFC107" : "#F44336";
        label->setText(QString::fromUtf8(obs_module_text("PreviewAnalysisRow"))
            .arg(obs_module_text(title), number(voice), number(bgm), obs_module_text(key)));
        label->setStyleSheet(QString("QLabel { border-left: 4px solid %1; padding: 4px; }").arg(color));
    };
    preview_realtime_->setVisible(has_recording);
    preview_average_->setVisible(has_recording);
    if (has_recording) {
        const auto *point = preview_->analysis_point();
        show_analysis(preview_realtime_, "PreviewAnalysisRealtime", point ? point->voice_lufs : -HUGE_VAL,
            point ? point->bgm_lufs : -HUGE_VAL, point ? point->balance_status : std::nullopt);
        const auto mean = preview_->analysis_summary();
        show_analysis(preview_average_, "PreviewAnalysisAverage", mean.voice_lufs, mean.bgm_lufs, mean.balance_status);
        if (!preview_->analysis_notice().isEmpty())
            preview_average_->setText(preview_average_->text() + QChar(10) + preview_->analysis_notice());
    }
    if (state == State::playing) preview_has_played_ = true;
    const bool busy = state == State::preparing || state == State::recording ||
                      state == State::waiting || state == State::playing;
    const bool blocked = PreviewSession::output_active();
    const bool manual = preview_manual_track_->isChecked();
    const auto choice = busy ? PreviewTrackChoice{int(preview_->track()), {}}
        : manual ? PreviewTrackChoice{preview_track_->currentData().toInt(), {}} : resolve_preview_track();
    preview_manual_track_->setEnabled(!busy);
    preview_track_->setEnabled(!busy && manual);
    preview_track_->setVisible(manual);
    preview_device_->setEnabled(!busy);
    preview_refresh_->setEnabled(!busy);
    preview_monitor_->setEnabled(!busy);
    preview_track_info_->setVisible(!busy);
    preview_track_info_->setText(manual ? QString::fromUtf8(obs_module_text("PreviewTrackHelp")) :
        choice.track >= 0 ? QString::fromUtf8(obs_module_text("PreviewAutoTrack")).arg(choice.track + 1) : choice.error);
    const bool valid_start = choice.track >= 0 && !preview_device_->currentData().toString().isEmpty();
    preview_record_->setEnabled(!blocked && state != State::waiting && state != State::playing &&
                               (state == State::preparing || state == State::recording || valid_start));
    preview_play_->setVisible(state == State::waiting || state == State::playing || state == State::ready);
    preview_play_->setEnabled(!blocked);
    preview_record_->setText(obs_module_text(state == State::preparing ? "PreviewCancelPreparation" :
        state == State::recording ? "PreviewFinish" : state == State::ready ? "PreviewRerecord" : "PreviewRecord"));
    preview_play_->setText(obs_module_text(state == State::waiting ? "PreviewStopWaiting" :
                                         state == State::playing ? "PreviewStop" : preview_has_played_ ? "PreviewReplay" : "PreviewPlay"));
    static const QIcon record_icon = [] {
        QPixmap record_mark(16, 16);
        record_mark.fill(Qt::transparent);
        QPainter painter(&record_mark);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#d16969"));
        painter.drawEllipse(2, 2, 12, 12);
        painter.end();
        return QIcon(record_mark);
    }();
    preview_record_->setIcon(state == State::preparing || state == State::recording
        ? style()->standardIcon(QStyle::SP_MediaStop) : record_icon);
    preview_play_->setIcon(style()->standardIcon(state == State::playing || state == State::waiting ? QStyle::SP_MediaStop : QStyle::SP_MediaPlay));
    const bool recorded = busy || state == State::ready;
    preview_recorded_track_->setVisible(recorded);
    preview_recorded_track_->setText(recorded ? QString::fromUtf8(obs_module_text("PreviewRecordedTrack")).arg(preview_->track() + 1) : QString());
    QString message;
    if (blocked) message = obs_module_text("PreviewOutputActive");
    else if (!preview_->message().isEmpty()) message = preview_->message();
    else switch (state) {
    case State::preparing:
        message = QString::fromUtf8(obs_module_text("PreviewCountdown")).arg(preview_->countdown());
        break;
    case State::recording:
        message = QString::fromUtf8(obs_module_text("PreviewRecording")).arg(preview_->seconds(), 0, 'f', 1);
        break;
    case State::waiting:
        message = QString::fromUtf8(obs_module_text("PreviewWaiting")).arg(preview_->wait_countdown());
        break;
    case State::ready:
        message = QString::fromUtf8(obs_module_text("PreviewReady")).arg(preview_->seconds(), 0, 'f', 1);
        break;
    case State::playing:
        message = obs_module_text("PreviewPlaying");
        message += QChar(10);
        message += obs_module_text(preview_monitor_->isChecked() ? "PreviewMonitorActive" : "PreviewOverlapNotice");
        break;
    default:
        message = obs_module_text("PreviewIdle");
        if (!valid_start) {
            message += QChar(10);
            message += choice.track < 0 ? choice.error : QString::fromUtf8(obs_module_text("PreviewSelectDevice"));
        }
        break;
    }
    preview_status_->setText(message);
    const QString status_style = state == State::playing ? "QLabel { border-left: 4px solid #4c9ed9; font-weight: bold; }" :
        state == State::recording ? "QLabel { border-left: 4px solid #d16969; font-weight: bold; }" :
        "QLabel { border-left: 4px solid transparent; }";
    if (preview_status_->styleSheet() != status_style) preview_status_->setStyleSheet(status_style);
}

void LoudnessDock::refresh_source_lists()
{
	// Restore the saved selection after a scene collection without this source.
	QString current_voice = QString::fromStdString(capture_manager_->voice_source_name());
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
	int voice_idx = voice_source_combo_->findData(current_voice);
	if (voice_idx >= 0) {
		voice_source_combo_->setCurrentIndex(voice_idx);
	}
	voice_source_combo_->blockSignals(false);
}

void LoudnessDock::on_update_timer()
{
	preview_->tick();
	update_preview();
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
	char *config_dir = obs_module_config_path(NULL);
	if (!config_dir)
		return;

	if (os_mkdirs(config_dir) == MKDIR_ERROR) {
		obs_log(LOG_ERROR, "Failed to create plugin config directory: %s", config_dir);
		bfree(config_dir);
		return;
	}
	bfree(config_dir);

	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;

	obs_data_t *settings = obs_data_create();

	// Save source selections
	capture_manager_->save_settings(settings);

    obs_data_set_bool(settings, "preview_manual_track", preview_manual_track_->isChecked());
    obs_data_set_int(settings, "preview_track", preview_track_->currentData().toInt());
    obs_data_set_string(settings, "preview_device", preview_device_->currentData().toString().toUtf8().constData());
    obs_data_set_bool(settings, "preview_monitor_off", preview_monitor_->isChecked());

	// Save other settings
	obs_data_set_int(settings, "vad_threshold", vad_threshold_slider_->value());
	obs_data_set_double(settings, "balance_target", balance_target_spin_->value());
	obs_data_set_int(settings, "mix_preset", mix_preset_combo_->currentIndex());

	if (!obs_data_save_json_safe(settings, path, "tmp", "bak"))
		obs_log(LOG_ERROR, "Failed to save plugin settings: %s", path);
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

    // Preserve an explicit track from the previous version instead of silently switching it.
    const bool manual = obs_data_has_user_value(settings, "preview_manual_track")
        ? obs_data_get_bool(settings, "preview_manual_track")
        : obs_data_has_user_value(settings, "preview_track");
    preview_manual_track_->setChecked(manual);
    const int track = int(obs_data_get_int(settings, "preview_track"));
    preview_track_->setCurrentIndex(track >= 0 && track < 6 ? track : 0);
    const QString device = QString::fromUtf8(obs_data_get_string(settings, "preview_device"));
    if (!device.isEmpty()) {
        int index = preview_device_->findData(device);
        if (index < 0) {
            preview_device_->addItem(QString::fromUtf8(obs_module_text("PreviewUnavailable")).arg(device), device);
            index = preview_device_->count() - 1;
        }
        preview_device_->setCurrentIndex(index);
    }
    obs_data_set_default_bool(settings, "preview_monitor_off", true);
    preview_monitor_->setChecked(obs_data_get_bool(settings, "preview_monitor_off"));

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
