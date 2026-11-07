#pragma once
#include <QString>
namespace lbm {
struct PreviewTrackChoice {
	int track;
	QString error;
};
PreviewTrackChoice resolve_preview_track();
} // namespace lbm
