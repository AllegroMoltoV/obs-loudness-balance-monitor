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
#include <QMouseEvent>
#include <QScreen>
#include <QResizeEvent>

#include <cmath>

namespace lbm {
namespace {
QLabel *wrapped_label(const QString &text = {})
{
	auto *label = new QLabel(text);
	label->setWordWrap(true);
	label->setTextFormat(Qt::PlainText);
	label->setMinimumWidth(0);
	QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	policy.setHeightForWidth(true);
	label->setSizePolicy(policy);
	return label;
}

class CheckBoxLabel : public QLabel {
public:
	explicit CheckBoxLabel(QCheckBox *check, const QString &text) : QLabel(text), check_(check)
	{
		setWordWrap(true);
		setTextFormat(Qt::PlainText);
		setMinimumWidth(0);
		QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		policy.setHeightForWidth(true);
		setSizePolicy(policy);
		setBuddy(check);
	}

protected:
	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
			check_->click();
		QLabel::mouseReleaseEvent(event);
	}

private:
	QCheckBox *check_;
};

class PreviewNotice : public QLabel {
public:
	PreviewNotice()
	{
		setWordWrap(true);
		setTextFormat(Qt::PlainText);
		QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		policy.setHeightForWidth(true);
		setSizePolicy(policy);
	}
	int heightForWidth(int width) const override
	{
		int height = QLabel::heightForWidth(width);
		QFont measured_font = font();
		measured_font.setBold(true);
		const QFontMetrics metrics(measured_font);
		for (const auto &text :
		     {QString::fromUtf8(obs_module_text("PreviewIdle")) + "\n" + obs_module_text("PreviewSelectDevice"),
		      QString::fromUtf8(obs_module_text("PreviewCountdown")).arg(3),
		      QString::fromUtf8(obs_module_text("PreviewRecording")).arg(999.9, 0, 'f', 1),
		      QString::fromUtf8(obs_module_text("PreviewWaiting")).arg(3),
		      QString::fromUtf8(obs_module_text("PreviewReady")).arg(999.9, 0, 'f', 1),
		      QString::fromUtf8(obs_module_text("PreviewPlaying")) + "\n" +
			      obs_module_text("PreviewMonitorActive"),
		      QString::fromUtf8(obs_module_text("PreviewPlaying")) + "\n" +
			      obs_module_text("PreviewOverlapNotice")}) {
			height = std::max(
				height, metrics.boundingRect(QRect(0, 0, std::max(1, width - 2 * margin() - 8), 10000),
							     Qt::TextWordWrap, text)
							.height() +
						2 * margin() + 8);
		}
		return height;
	}
};

class GuideButton : public QPushButton {
protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QPushButton::resizeEvent(event);
		// QPushButton's themed size hint does not include its wrapped child label.
		if (layout())
			setMinimumHeight(layout()->totalHeightForWidth(event->size().width()));
	}
};

void add_wrapped_checkbox(QVBoxLayout *layout, QCheckBox *check, const QString &text)
{
	check->setAccessibleName(text);
	auto *row = new QHBoxLayout();
	row->addWidget(check, 0, Qt::AlignTop);
	row->addWidget(new CheckBoxLabel(check, text), 1);
	layout->addLayout(row);
}

void show_help(QWidget *parent, const QString &title, const QString &body, bool guide = false)
{
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
	const QSize available = parent->screen()->availableGeometry().size() - QSize(40, 80);
	dialog->resize(QSize(guide ? 680 : 480, guide ? 560 : 360).boundedTo(available));
	dialog->show();
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
	help->setFixedWidth(help->sizeHint().width());
	auto *heading = wrapped_label(title);
	heading->setObjectName(QString("heading_%1").arg(title_key));
	auto font = heading->font();
	font.setBold(true);
	heading->setFont(font);
	auto *row = new QHBoxLayout();
	row->addWidget(heading, 1, Qt::AlignTop);
	row->addWidget(help, 0, Qt::AlignTop);
	layout->addLayout(row);
	QObject::connect(help, &QToolButton::clicked, parent,
			 [parent, title, body] { show_help(parent, title, body); });
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
	auto *guide = new GuideButton();
	guide->setObjectName("help_GuideTitle");
	guide->setAccessibleName(obs_module_text("GuideTitle"));
	QSizePolicy guide_policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	guide_policy.setHeightForWidth(true);
	guide->setSizePolicy(guide_policy);
	// OBS themes cap ordinary buttons to one line; this guide supports wrapped titles.
	guide->setStyleSheet("QPushButton { max-height: 16777215px; }");
	QPixmap book(20, 20);
	book.fill(Qt::transparent);
	{
		QPainter painter(&book);
		painter.setPen(QPen(palette().color(QPalette::ButtonText), 1.5));
		painter.drawRect(2, 3, 16, 14);
		painter.drawLine(10, 3, 10, 17);
		painter.drawLine(4, 6, 8, 6);
		painter.drawLine(12, 6, 16, 6);
	}
	auto *guide_row = new QHBoxLayout(guide);
	guide_row->setContentsMargins(10, 6, 10, 6);
	auto *book_label = new QLabel();
	book_label->setPixmap(book);
	book_label->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto *guide_label = wrapped_label(obs_module_text("GuideTitle"));
	guide_label->setAlignment(Qt::AlignCenter);
	guide_label->setAttribute(Qt::WA_TransparentForMouseEvents);
	guide_row->addWidget(book_label);
	guide_row->addWidget(guide_label, 1);
	connect(guide, &QPushButton::clicked, this,
		[this] { show_help(this, obs_module_text("GuideTitle"), obs_module_text("GuideBody"), true); });
	main_layout->addWidget(guide);

	auto *preview_group = new QGroupBox();
	preview_group->setObjectName("recordingMonitor");
	auto *preview_outer = new QVBoxLayout(preview_group);
	add_section_help(preview_outer, preview_group, "PreviewTitle", obs_module_text("PreviewHelpDetail"));
	preview_columns_ = new QBoxLayout(QBoxLayout::TopToBottom);
	preview_outer->addLayout(preview_columns_);
	preview_controls_ = new QWidget();
	preview_controls_->setObjectName("previewControls");
	preview_controls_->setFocusPolicy(Qt::ClickFocus);
	auto *preview_layout = new QVBoxLayout(preview_controls_);
	preview_layout->setContentsMargins(0, 0, 0, 0);
	preview_columns_->addWidget(preview_controls_, 1);
	preview_status_ = new PreviewNotice();
	preview_status_->setObjectName("previewStatus");
	preview_status_->setMargin(6);

	preview_recorded_track_ = wrapped_label();
	preview_recorded_track_->setObjectName("previewRecordedTrack");

	// Stacked actions keep their full text visible in a narrow OBS dock.
	preview_record_ = new QPushButton();
	preview_record_->setObjectName("previewRecord");
	preview_play_ = new QPushButton(obs_module_text("PreviewPlay"));
	preview_play_->setObjectName("previewPlay");
	preview_record_->setStyleSheet(
		"QPushButton:enabled { border: 2px solid #d16969; padding: 6px; font-weight: bold; }");
	preview_play_->setStyleSheet(
		"QPushButton:enabled { border: 2px solid #4c9ed9; padding: 6px; font-weight: bold; }");

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

	preview_manual_track_ = new QCheckBox();
	preview_manual_track_->setObjectName("previewManualTrack");
	add_wrapped_checkbox(preview_layout, preview_manual_track_, obs_module_text("PreviewManualTrack"));
	preview_track_ = new QComboBox();
	preview_track_->setObjectName("previewTrack");
	for (int i = 0; i < 6; ++i)
		preview_track_->addItem(QString::fromUtf8(obs_module_text("PreviewTrackNumber")).arg(i + 1), i);
	preview_layout->addWidget(preview_track_);
	preview_track_info_ = wrapped_label();
	preview_track_info_->setObjectName("previewTrackInfo");
	preview_layout->addWidget(preview_track_info_);
	connect(preview_manual_track_, &QCheckBox::toggled, this, &LoudnessDock::update_preview);

	preview_monitor_ = new QCheckBox();
	preview_monitor_->setObjectName("previewMonitorOff");
	preview_monitor_->setChecked(true);
	connect(preview_monitor_, &QCheckBox::toggled, this, &LoudnessDock::update_preview);
	add_wrapped_checkbox(preview_layout, preview_monitor_, obs_module_text("PreviewMonitorOff"));

	connect(preview_record_, &QPushButton::clicked, this, [this] {
		using State = PreviewSession::State;
		if (preview_->state() == State::preparing)
			return;
		if (preview_->state() == State::recording) {
			preview_->finish();
		} else {
			const auto choice = preview_manual_track_->isChecked()
						    ? PreviewTrackChoice{preview_track_->currentData().toInt(), {}}
						    : resolve_preview_track();
			if (choice.track < 0) {
				update_preview();
				return;
			}
			const auto &config = analyzer_->config();
			preview_->configure_analysis(capture_manager_->voice_source_name(),
						     capture_manager_->bgm_source_names(), config.vad_threshold.load(),
						     config.balance_target.load(), config.hysteresis.load(),
						     config.mix_ok_threshold.load(), config.mix_warn_threshold.load());
			preview_has_played_ = false;
			preview_->prepare(unsigned(choice.track),
					  preview_device_->currentData().toString().toStdString(),
					  preview_monitor_->isChecked());
		}
	});
	connect(preview_play_, &QPushButton::clicked, this, [this] {
		if (preview_->state() == PreviewSession::State::waiting)
			preview_->stop_waiting();
		else if (preview_->state() == PreviewSession::State::playing)
			preview_->stop_playback();
		else
			preview_->play(preview_device_->currentData().toString().toStdString(),
				       preview_monitor_->isChecked());
	});
	refresh_preview_devices();

	// === Source Selection Group ===
	auto *source_group = new QGroupBox();
	source_group->setObjectName("sourceSelection");
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

	auto *status_group = new QGroupBox();
	status_group->setObjectName("liveMonitor");
	auto *status_outer = new QVBoxLayout(status_group);
	add_section_help(status_outer, status_group, "Status", obs_module_text("HelpMeters"));
	live_monitor_ = add_monitor_widgets(status_outer, "live", false);

	preview_layout->addWidget(preview_recorded_track_);
	preview_layout->addWidget(preview_status_);
	preview_layout->addWidget(preview_record_);
	preview_layout->addWidget(preview_play_);
	preview_analysis_group_ = new QWidget();
	auto *analysis_layout = new QVBoxLayout(preview_analysis_group_);
	analysis_layout->setContentsMargins(0, 0, 0, 0);
	auto *instant_group = new QGroupBox(obs_module_text("PreviewAnalysisRealtime"));
	auto *instant_layout = new QVBoxLayout(instant_group);
	preview_instant_monitor_ = add_monitor_widgets(instant_layout, "previewRealtime", false);
	preview_realtime_ = wrapped_label();
	preview_realtime_->setObjectName("previewRealtime");
	instant_layout->addWidget(preview_realtime_);
	analysis_layout->addWidget(instant_group);
	auto *whole_group = new QGroupBox(obs_module_text("PreviewAnalysisAverage"));
	auto *whole_layout = new QVBoxLayout(whole_group);
	preview_whole_monitor_ = add_monitor_widgets(whole_layout, "previewAverage", true);
	preview_average_ = wrapped_label();
	preview_average_->setObjectName("previewAverage");
	whole_layout->addWidget(preview_average_);
	analysis_layout->addWidget(whole_group);
	preview_columns_->addWidget(preview_analysis_group_, 1);

	// === Settings ===
	auto *settings_group = new QGroupBox();
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

	top_sections_ = new QBoxLayout(QBoxLayout::TopToBottom);
	top_sections_->setContentsMargins(preview_outer->contentsMargins());
	top_sections_->addWidget(source_group, 1);
	top_sections_->addWidget(status_group, 1);
	main_layout->addLayout(top_sections_);
	main_layout->addWidget(preview_group);
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

void LoudnessDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	update_responsive_layout();
}

void LoudnessDock::update_responsive_layout()
{
	if (!top_sections_ || !preview_columns_)
		return;
	// Fit three numeric pairs in each monitor column, including localized font scaling.
	const int metric_width = fontMetrics().horizontalAdvance("-99.9 LUFS  -99.9 dB") + 20;
	const int breakpoint = 2 * (3 * metric_width + 40) + 40;
	const bool wide = width() >= breakpoint;
	setProperty("wideLayout", wide);
	top_sections_->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
	preview_columns_->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
	preview_columns_->setAlignment(preview_controls_, Qt::AlignTop);
	for (auto *monitor : {&live_monitor_, &preview_instant_monitor_, &preview_whole_monitor_})
		if (monitor->metrics)
			monitor->metrics->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
}

void LoudnessDock::refresh_preview_devices()
{
	const QString selected = preview_device_->currentData().toString();
	preview_device_->clear();
	preview_device_->addItem(obs_module_text("PreviewSelectDevice"), "");
	preview_device_->addItem(obs_module_text("PreviewDefaultDevice"), "default");
	for (const auto &device : preview_devices())
		preview_device_->addItem(QString::fromStdString(device.name), QString::fromStdString(device.id));
	int index = preview_device_->findData(selected);
	if (index < 0 && !selected.isEmpty()) {
		preview_device_->addItem(QString::fromUtf8(obs_module_text("PreviewUnavailable")).arg(selected),
					 selected);
		index = preview_device_->count() - 1;
	}
	preview_device_->setCurrentIndex(std::max(index, 0));
}

void LoudnessDock::update_preview()
{
	if (!preview_manual_track_ || !preview_monitor_)
		return;
	using State = PreviewSession::State;
	const auto state = preview_->state();
	const bool has_recording = preview_->recorded_frames() != 0;
	if (has_recording) {
		const auto *point = preview_->analysis_point();
		show_monitor_values(preview_instant_monitor_,
				    {point ? point->voice_lufs : -HUGE_VAL, point ? point->bgm_lufs : -HUGE_VAL,
				     point ? point->mix_lufs : -HUGE_VAL},
				    {point ? point->voice_peak_dbfs : -HUGE_VAL,
				     point ? point->bgm_peak_dbfs : -HUGE_VAL,
				     point ? point->mix_peak_dbfs : -HUGE_VAL},
				    point && point->voice_active ? point->voice_lufs - point->bgm_lufs : -HUGE_VAL,
				    point && point->voice_active);
		show_monitor_rating(preview_instant_monitor_.ratings[0], point ? point->balance_status : std::nullopt);
		show_monitor_rating(preview_instant_monitor_.ratings[1], point ? point->mix_status : std::nullopt);
		show_monitor_rating(preview_instant_monitor_.ratings[2], point ? point->clip_status : std::nullopt);
		const auto mean = preview_->analysis_summary();
		show_monitor_values(preview_whole_monitor_, {mean.voice_lufs, mean.bgm_lufs, mean.mix_lufs},
				    {mean.voice_peak_dbfs, mean.bgm_peak_dbfs, mean.mix_peak_dbfs},
				    mean.valid_points ? mean.voice_lufs - mean.bgm_lufs : -HUGE_VAL, false);
		show_monitor_rating(preview_whole_monitor_.ratings[0], mean.balance_status);
		show_monitor_rating(preview_whole_monitor_.ratings[1], mean.mix_status);
		show_monitor_rating(preview_whole_monitor_.ratings[2], mean.clip_status);
		preview_realtime_->setText(preview_->analysis_realtime_notice());
		preview_realtime_->setVisible(!preview_realtime_->text().isEmpty());
		preview_average_->setText(preview_->analysis_notice());
		preview_average_->setVisible(!preview_average_->text().isEmpty());
	} else {
		for (const auto *monitor : {&preview_instant_monitor_, &preview_whole_monitor_}) {
			show_monitor_values(*monitor, {-HUGE_VAL, -HUGE_VAL, -HUGE_VAL},
					    {-HUGE_VAL, -HUGE_VAL, -HUGE_VAL}, -HUGE_VAL, false);
			for (auto *rating : monitor->ratings)
				show_monitor_rating(rating, std::nullopt);
		}
		preview_realtime_->clear();
		preview_average_->clear();
		preview_realtime_->hide();
		preview_average_->hide();
	}
	if (state == State::playing)
		preview_has_played_ = true;
	const bool busy = state == State::preparing || state == State::recording || state == State::waiting ||
			  state == State::playing;
	const bool blocked = PreviewSession::output_active();
	const bool manual = preview_manual_track_->isChecked();
	const auto choice = busy     ? PreviewTrackChoice{int(preview_->track()), {}}
			    : manual ? PreviewTrackChoice{preview_track_->currentData().toInt(), {}}
				     : resolve_preview_track();
	// Disabling the focused button otherwise makes QScrollArea scroll to the next control.
	if (busy && preview_controls_->isAncestorOf(focusWidget()) && focusWidget() != preview_play_ &&
	    !(state == State::recording && focusWidget() == preview_record_))
		preview_controls_->setFocus(Qt::OtherFocusReason);
	preview_manual_track_->setEnabled(!busy);
	preview_track_->setEnabled(!busy && manual);
	preview_track_->setVisible(manual);
	preview_device_->setEnabled(!busy);
	preview_refresh_->setEnabled(!busy);
	preview_monitor_->setEnabled(!busy);
	preview_track_info_->setVisible(!manual);
	preview_track_info_->setText(choice.track >= 0 ? QString::fromUtf8(obs_module_text("PreviewAutoTrack"))
						       : choice.error);
	const bool valid_start = choice.track >= 0 && !preview_device_->currentData().toString().isEmpty();
	preview_record_->setEnabled(!blocked && state != State::preparing && state != State::waiting &&
				    state != State::playing && (state == State::recording || valid_start));
	preview_play_->setEnabled(!blocked &&
				  (state == State::waiting || state == State::playing || state == State::ready));
	preview_record_->setText(
		state == State::preparing
			? QString::fromUtf8(obs_module_text("PreviewRecordCountdown")).arg(preview_->countdown())
			: QString::fromUtf8(obs_module_text(state == State::recording ? "PreviewFinish"
							    : state == State::ready   ? "PreviewRerecord"
										      : "PreviewRecord")));
	preview_play_->setText(obs_module_text(state == State::waiting   ? "PreviewStopWaiting"
					       : state == State::playing ? "PreviewStop"
					       : preview_has_played_     ? "PreviewReplay"
									 : "PreviewPlay"));
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
	preview_record_->setIcon(state == State::preparing   ? QIcon()
				 : state == State::recording ? style()->standardIcon(QStyle::SP_MediaStop)
							     : record_icon);
	preview_play_->setIcon(style()->standardIcon(
		state == State::playing || state == State::waiting ? QStyle::SP_MediaStop : QStyle::SP_MediaPlay));
	const bool recorded = busy || state == State::ready;
	preview_recorded_track_->setVisible(true);
	preview_recorded_track_->setText(
		recorded ? QString::fromUtf8(obs_module_text("PreviewRecordedTrack")).arg(preview_->track() + 1)
			 : QString(" "));
	QString message;
	if (blocked)
		message = obs_module_text("PreviewOutputActive");
	else if (!preview_->message().isEmpty())
		message = preview_->message();
	else
		switch (state) {
		case State::preparing:
			message = QString::fromUtf8(obs_module_text("PreviewCountdown")).arg(preview_->countdown());
			break;
		case State::recording:
			message = QString::fromUtf8(obs_module_text("PreviewRecording"))
					  .arg(preview_->seconds(), 0, 'f', 1);
			break;
		case State::waiting:
			message = QString::fromUtf8(obs_module_text("PreviewWaiting")).arg(preview_->wait_countdown());
			break;
		case State::ready:
			message =
				QString::fromUtf8(obs_module_text("PreviewReady")).arg(preview_->seconds(), 0, 'f', 1);
			break;
		case State::playing:
			message = obs_module_text("PreviewPlaying");
			message += QChar(10);
			message += obs_module_text(preview_monitor_->isChecked() ? "PreviewMonitorActive"
										 : "PreviewOverlapNotice");
			break;
		default:
			message = obs_module_text("PreviewIdle");
			if (!valid_start) {
				message += QChar(10);
				message += choice.track < 0 ? choice.error
							    : QString::fromUtf8(obs_module_text("PreviewSelectDevice"));
			}
			break;
		}
	preview_status_->setText(message);
	const QString status_style =
		state == State::playing     ? "QLabel { border-left: 4px solid #4c9ed9; font-weight: bold; }"
		: state == State::recording ? "QLabel { border-left: 4px solid #d16969; font-weight: bold; }"
					    : "QLabel { border-left: 4px solid transparent; }";
	if (preview_status_->styleSheet() != status_style)
		preview_status_->setStyleSheet(status_style);
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

LoudnessDock::MonitorWidgets LoudnessDock::add_monitor_widgets(QVBoxLayout *layout, const QString &prefix,
							       bool whole_recording)
{
	MonitorWidgets widgets;
	auto *ratings = new QHBoxLayout();
	const std::array<const char *, 3> rating_keys{"Balance", "Mix", "Clip"};
	const std::array<const char *, 3> metric_keys{"Voice", "BGM", "MixMeter"};
	const std::array<const char *, 3> metric_names{"Voice", "Bgm", "Mix"};
	for (size_t i = 0; i < rating_keys.size(); ++i) {
		auto *column = new QVBoxLayout();
		auto *title = wrapped_label(obs_module_text(rating_keys[i]));
		title->setAlignment(Qt::AlignCenter);
		column->addWidget(title);
		widgets.ratings[i] = wrapped_label();
		widgets.ratings[i]->setObjectName(prefix + rating_keys[i]);
		widgets.ratings[i]->setAlignment(Qt::AlignCenter);
		widgets.ratings[i]->setMinimumHeight(32);
		column->addWidget(widgets.ratings[i]);
		show_monitor_rating(widgets.ratings[i], std::nullopt);
		ratings->addLayout(column, 1);
	}
	layout->addLayout(ratings);
	if (!whole_recording) {
		auto *vad_row = new QHBoxLayout();
		vad_row->addWidget(wrapped_label(obs_module_text("VAD")));
		widgets.vad = new QLabel();
		widgets.vad->setObjectName(prefix + "Vad");
		widgets.vad->setFixedSize(20, 20);
		widgets.vad->setStyleSheet("background-color: #888888; border-radius: 10px;");
		vad_row->addWidget(widgets.vad);
		layout->addLayout(vad_row);
	}
	if (whole_recording) {
		auto *columns = new QHBoxLayout();
		columns->addWidget(wrapped_label(obs_module_text("PreviewAverageLufs")), 1);
		auto *peak_title = wrapped_label(obs_module_text("PreviewMaximumPeak"));
		peak_title->setAlignment(Qt::AlignRight);
		columns->addWidget(peak_title, 1);
		layout->addLayout(columns);
	}
	widgets.metrics = new QBoxLayout(QBoxLayout::TopToBottom);
	layout->addLayout(widgets.metrics);
	for (size_t i = 0; i < metric_keys.size(); ++i) {
		auto *metric = new QVBoxLayout();
		widgets.metrics->addLayout(metric, 1);
		metric->addWidget(wrapped_label(obs_module_text(metric_keys[i])));
		widgets.meters[i] = new QProgressBar();
		widgets.meters[i]->setObjectName(prefix + metric_names[i] + "Meter");
		widgets.meters[i]->setRange(0, 100);
		widgets.meters[i]->setMinimumWidth(0);
		widgets.meters[i]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		widgets.meters[i]->setTextVisible(false);
		widgets.meters[i]->setFixedHeight(16);
		metric->addWidget(widgets.meters[i]);
		auto *numbers = new QHBoxLayout();
		widgets.lufs[i] = wrapped_label("-- LUFS");
		widgets.lufs[i]->setObjectName(prefix + metric_names[i] + "Lufs");
		widgets.peaks[i] = wrapped_label("-- dB");
		widgets.peaks[i]->setObjectName(prefix + metric_names[i] + "Peak");
		widgets.peaks[i]->setAlignment(Qt::AlignRight);
		numbers->addWidget(widgets.lufs[i], 1);
		numbers->addWidget(widgets.peaks[i], 1);
		metric->addLayout(numbers);
	}
	layout->addWidget(wrapped_label(obs_module_text(whole_recording ? "PreviewAverageDelta" : "Delta")));
	widgets.delta = wrapped_label("-- LU");
	widgets.delta->setObjectName(prefix + "Delta");
	layout->addWidget(widgets.delta);
	return widgets;
}

void LoudnessDock::show_monitor_values(const MonitorWidgets &widgets, const std::array<double, 3> &lufs,
				       const std::array<double, 3> &peaks, double delta, bool voice_active)
{
	for (size_t i = 0; i < lufs.size(); ++i) {
		const bool measured = std::isfinite(lufs[i]);
		widgets.meters[i]->setValue(measured ? lufs_to_meter(lufs[i]) : 0);
		widgets.meters[i]->setEnabled(measured);
		widgets.lufs[i]->setText(measured ? QString("%1 LUFS").arg(lufs[i], 0, 'f', 1) : QString("-- LUFS"));
		widgets.peaks[i]->setText(std::isfinite(peaks[i]) ? QString("%1 dB").arg(peaks[i], 0, 'f', 1)
								  : QString("-- dB"));
	}
	widgets.delta->setText(std::isfinite(delta)
				       ? QString("%1%2 LU").arg(delta >= 0 ? "+" : "").arg(delta, 0, 'f', 1)
				       : QString("-- LU"));
	if (widgets.vad)
		widgets.vad->setStyleSheet(voice_active ? "background-color: #4CAF50; border-radius: 10px;"
							: "background-color: #888888; border-radius: 10px;");
}

void LoudnessDock::show_monitor_rating(QLabel *label, std::optional<Status> status)
{
	const char *key = !status                   ? "PreviewAnalysisUnrated"
			  : *status == Status::OK   ? "PreviewAnalysisGreen"
			  : *status == Status::WARN ? "PreviewAnalysisYellow"
						    : "PreviewAnalysisRed";
	label->setText(obs_module_text(key));
	label->setStyleSheet((status ? status_to_style(*status) : "background-color: #888888;") +
			     QString(" color: #111111; padding: 3px; border-radius: 3px;"));
}

void LoudnessDock::update_meters()
{
	const auto &results = analyzer_->results();
	const bool active = results.voice_active.load(std::memory_order_relaxed);
	const double voice = results.voice_lufs.load(std::memory_order_relaxed);
	const double bgm = results.bgm_lufs.load(std::memory_order_relaxed);
	show_monitor_values(live_monitor_,
			    {active ? voice : -HUGE_VAL, bgm,
			     active ? results.mix_lufs.load(std::memory_order_relaxed) : -HUGE_VAL},
			    {results.voice_peak_dbfs.load(std::memory_order_relaxed),
			     results.bgm_peak_dbfs.load(std::memory_order_relaxed),
			     results.mix_peak_dbfs.load(std::memory_order_relaxed)},
			    active && std::isfinite(voice) && std::isfinite(bgm)
				    ? results.balance_delta.load(std::memory_order_relaxed)
				    : -HUGE_VAL,
			    active);
}

void LoudnessDock::update_status_colors()
{
	const auto &results = analyzer_->results();
	show_monitor_rating(live_monitor_.ratings[0], results.balance_status.load(std::memory_order_relaxed));
	show_monitor_rating(live_monitor_.ratings[1], results.mix_status.load(std::memory_order_relaxed));
	show_monitor_rating(live_monitor_.ratings[2], results.clip_status.load(std::memory_order_relaxed));
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
			preview_device_->addItem(QString::fromUtf8(obs_module_text("PreviewUnavailable")).arg(device),
						 device);
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
