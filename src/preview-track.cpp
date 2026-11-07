#include "preview-track.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

namespace lbm {
PreviewTrackChoice resolve_preview_track()
{
	auto invalid = [] {
		return PreviewTrackChoice{-1, QString::fromUtf8(obs_module_text("PreviewTrackUnavailable"))};
	};
	config_t *config = obs_frontend_get_profile_config();
	if (!config)
		return invalid();
	const QString mode = QString::fromUtf8(config_get_string(config, "Output", "Mode"));
	if (mode == "Simple")
		return {0, {}};
	if (mode != "Advanced")
		return invalid();
	// The frontend owns the service; this accessor does not add a reference.
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (!service)
		return invalid();
	const QString protocol = QString::fromUtf8(obs_service_get_protocol(service));
	if (protocol.startsWith("SRT", Qt::CaseInsensitive) || protocol.startsWith("RIST", Qt::CaseInsensitive)) {
		const auto mask = config_get_uint(config, "AdvOut", "StreamMultiTrackAudioMixes");
		if (!mask || (mask & ~uint64_t(63)) || (mask & (mask - 1)))
			return {-1, QString::fromUtf8(obs_module_text("PreviewTrackMultiple"))};
		for (int i = 0; i < 6; ++i)
			if (mask & (uint64_t(1) << i))
				return {i, {}};
	}
	const auto track = config_get_int(config, "AdvOut", "TrackIndex");
	if (track < 1 || track > 6)
		return invalid();
	return {int(track - 1), {}};
}
} // namespace lbm
