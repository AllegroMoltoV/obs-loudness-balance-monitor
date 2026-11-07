#pragma once

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLayout>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>

// Include after the probe globals. No event-loop pumping: captures must not
// re-enter the audio scenario's timer while its state transition is in progress.
static void activate_preview_layout(QWidget *widget)
{
	QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
	if (widget->layout())
		widget->layout()->activate();
	for (auto *child : widget->findChildren<QWidget *>())
		if (child->layout())
			child->layout()->activate();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
}

static QJsonObject preview_widget_geometry(QWidget *widget)
{
	return {{"width", widget->width()},
		{"height", widget->height()},
		{"logical_dpi_x", widget->logicalDpiX()},
		{"logical_dpi_y", widget->logicalDpiY()},
		{"device_pixel_ratio", widget->devicePixelRatioF()}};
}

static bool capture_preview_layout(const QString &name)
{
	if (!dock || !dock_container)
		return false;
	activate_preview_layout(dock_container);
	if (qEnvironmentVariable("LBM_PREVIEW_LAYOUT") != "1")
		return dock_container->grab().save(result_dir + "/" + name + ".png");

	auto *scroll = dock->findChild<QScrollArea *>(QString(), Qt::FindDirectChildrenOnly);
	if (!scroll || !scroll->widget() || scroll->viewport()->height() <= 0)
		return false;
	auto *bar = scroll->verticalScrollBar();
	const int original = bar->value();
	QJsonArray captures;
	bool saved = true;
	int index = 0;
	for (int position = bar->minimum();;) {
		bar->setValue(position);
		activate_preview_layout(dock_container);
		const QString file = QString("%1-%2.png").arg(name).arg(index++, 2, 10, QLatin1Char('0'));
		const bool okay = dock_container->grab().save(result_dir + "/" + file);
		saved = okay && saved;
		captures.append(QJsonObject{{"file", file}, {"scroll_y", bar->value()}, {"saved", okay}});
		if (position >= bar->maximum())
			break;
		position = qMin(bar->maximum(), position + scroll->viewport()->height());
	}
	bar->setValue(original);
	observations.insert(name + "_layout", QJsonObject{{"dock", preview_widget_geometry(dock_container)},
							  {"content", preview_widget_geometry(dock)},
							  {"viewport", preview_widget_geometry(scroll->viewport())},
							  {"language", QString::fromUtf8(obs_get_locale())},
							  {"scroll_maximum", bar->maximum()},
							  {"restored_scroll_y", bar->value()},
							  {"captures", captures}});
	return saved;
}

static void send_preview_key(QWidget *target, int key)
{
	QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
	QApplication::sendEvent(target, &press);
	QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
	QApplication::sendEvent(target, &release);
}

static bool check_responsive_layout()
{
	const int original_width = dock_container->width();
	const int metric_width = dock->fontMetrics().horizontalAdvance("-99.9 LUFS  -99.9 dB") + 20;
	const int boundary = 2 * (3 * metric_width + 40) + 40;
	QJsonArray results;
	bool passed = true;
	QMap<QString, QString> values;
	for (auto *label : dock->findChildren<QLabel *>())
		if (label->objectName().startsWith("previewRealtime") ||
		    label->objectName().startsWith("previewAverage"))
			values.insert(label->objectName(), label->text());
	QList<QPair<QComboBox *, int>> selections;
	for (auto *combo : dock->findChildren<QComboBox *>())
		selections.append({combo, combo->currentIndex()});
	for (int width : {232, boundary - 1, boundary, boundary + 1, 1400, 232}) {
		dock_container->setFixedWidth(width);
		activate_preview_layout(dock_container);
		const bool wide = dock->property("wideLayout").toBool();
		QJsonObject result{{"requested_width", width}, {"actual_width", dock->width()}, {"wide", wide}};
		bool okay = true;
		auto *outer_scroll = dock->findChild<QScrollArea *>(QString(), Qt::FindDirectChildrenOnly);
		okay = outer_scroll->widget()->width() <= outer_scroll->viewport()->width();
		for (const char *section : {"SourceSelection", "Status", "PreviewTitle", "Settings"}) {
			auto *heading = dock->findChild<QLabel *>(QString("heading_%1").arg(section));
			auto *help = dock->findChild<QToolButton *>(QString("help_%1").arg(section));
			okay = okay && heading && help &&
			       heading->mapTo(dock, QPoint()).y() == help->mapTo(dock, QPoint()).y() &&
			       heading->mapTo(dock, QPoint()).x() + heading->width() <= help->mapTo(dock, QPoint()).x();
		}
		for (auto it = values.cbegin(); it != values.cend(); ++it)
			okay = okay && dock->findChild<QLabel *>(it.key())->text() == it.value();
		for (const auto &selection : selections)
			okay = okay && selection.first->currentIndex() == selection.second;
		result.insert("headers_and_state_preserved", okay);
		result.insert("content_width", outer_scroll->widget()->width());
		result.insert("viewport_width", outer_scroll->viewport()->width());
		result.insert("saved", capture_preview_layout(QString("responsive-%1").arg(width)));
		results.append(result);
		passed = passed && okay;
	}
	dock_container->setFixedWidth(original_width);
	activate_preview_layout(dock_container);
	observations.insert("responsive_layout", results);
	return passed;
}

static QJsonObject record_position(QPushButton *record)
{
	auto *scroll = dock->findChild<QScrollArea *>(QString(), Qt::FindDirectChildrenOnly);
	QJsonObject result{{"scroll_y", scroll->verticalScrollBar()->value()},
			   {"button_y", record->mapTo(scroll->viewport(), QPoint()).y()},
			   {"content_height", scroll->widget()->height()}};
	for (const char *name : {"previewStatus", "previewRecordedTrack", "previewTrackInfo"}) {
		auto *label = dock->findChild<QLabel *>(name);
		result.insert(name, QJsonObject{{"height", label->height()},
						{"y", label->mapTo(scroll->widget(), QPoint()).y()}});
	}
	return result;
}

static bool focused_record_click(QPushButton *record, const QString &name)
{
	if (qEnvironmentVariable("LBM_PREVIEW_LAYOUT") != "1") {
		record->click();
		return true;
	}
	auto *scroll = dock->findChild<QScrollArea *>(QString(), Qt::FindDirectChildrenOnly);
	scroll->ensureWidgetVisible(record);
	record->setFocus(Qt::MouseFocusReason);
	activate_preview_layout(dock_container);
	const auto before = record_position(record);
	record->click();
	activate_preview_layout(dock_container);
	const auto after = record_position(record);
	observations.insert(name, QJsonObject{{"before", before}, {"after", after}});
	// Check the original scroll regression and the action's visibility; retain exact geometry as evidence.
	const int y = after.value("button_y").toInt();
	return before.value("scroll_y") == after.value("scroll_y") && y >= 0 &&
	       y + record->height() <= scroll->viewport()->height();
}

static bool check_preview_help_keyboard()
{
	if (!dock || !dock_container)
		return false;
	auto *outer_scroll = dock->findChild<QScrollArea *>(QString(), Qt::FindDirectChildrenOnly);
	if (!outer_scroll)
		return false;
	const int original_scroll = outer_scroll->verticalScrollBar()->value();
	QPointer<QWidget> original_focus = QApplication::focusWidget();
	QJsonArray results;
	bool passed = true;
	for (const char *section : {"GuideTitle", "SourceSelection", "Status", "PreviewTitle", "Settings"}) {
		const QString object_name = QString("help_%1").arg(section);
		auto *help = dock->findChild<QAbstractButton *>(object_name);
		QJsonObject result{{"button", object_name}};
		bool okay = help && help->isEnabled();
		QPointer<QDialog> dialog;
		if (okay) {
			outer_scroll->ensureWidgetVisible(help);
			help->setFocus(Qt::TabFocusReason);
			okay = dock_container->focusWidget() == help;
			result.insert("button_focused", okay);
			const auto before = dock->findChildren<QDialog *>();
			send_preview_key(help, Qt::Key_Space);
			for (auto *candidate : dock->findChildren<QDialog *>())
				if (!before.contains(candidate) && candidate->isVisible())
					dialog = candidate;
		}
		result.insert("opened_by_space", !dialog.isNull());
		okay = okay && !dialog.isNull();
		if (dialog) {
			activate_preview_layout(dialog);
			auto *scroll = dialog->findChild<QScrollArea *>();
			auto *body = scroll ? qobject_cast<QLabel *>(scroll->widget()) : nullptr;
			auto *buttons = dialog->findChild<QDialogButtonBox *>();
			auto *close = buttons ? buttons->button(QDialogButtonBox::Close) : nullptr;
			const bool has_body = body && !body->text().trimmed().isEmpty();
			result.insert("body_present", has_body);
			result.insert("dialog", preview_widget_geometry(dialog));
			const QString top_file = object_name + "-top.png";
			const bool top_saved = dialog->grab().save(result_dir + "/" + top_file);
			result.insert("top_image", top_file);
			okay = okay && has_body && close && top_saved;
			if (scroll) {
				scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
				activate_preview_layout(dialog);
				const QString bottom_file = object_name + "-bottom.png";
				const bool bottom_saved = dialog->grab().save(result_dir + "/" + bottom_file);
				result.insert("bottom_image", bottom_file);
				result.insert("body_scroll_maximum", scroll->verticalScrollBar()->maximum());
				okay = okay && bottom_saved;
				// Start before Close; reaching it must exercise actual Tab handling.
				scroll->setFocus(Qt::TabFocusReason);
			}
			bool tab_reached_close = false;
			const int focus_limit = dialog->findChildren<QWidget *>().size() + 1;
			for (int i = 0; i < focus_limit && close; ++i) {
				QWidget *target = dialog->focusWidget();
				send_preview_key(target ? target : dialog.data(), Qt::Key_Tab);
				if (dialog->focusWidget() == close) {
					tab_reached_close = true;
					break;
				}
			}
			result.insert("close_reached_by_tab", tab_reached_close);
			if (tab_reached_close)
				send_preview_key(close, Qt::Key_Space);
			const bool closed = !dialog || !dialog->isVisible();
			result.insert("closed_by_space", closed);
			okay = okay && tab_reached_close && closed;
			// Cleanup on failure is not evidence of keyboard success.
			if (dialog && dialog->isVisible())
				dialog->close();
		}
		result.insert("passed", okay);
		results.append(result);
		passed = okay && passed;
	}
	outer_scroll->verticalScrollBar()->setValue(original_scroll);
	if (original_focus)
		original_focus->setFocus(Qt::OtherFocusReason);
	observations.insert("preview_help_keyboard", results);
	return passed;
}
