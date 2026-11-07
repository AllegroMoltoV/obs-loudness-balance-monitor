#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace lbm {
struct PreviewDevice {
	std::string id;
	std::string name;
};

std::vector<PreviewDevice> preview_devices();
// Call on a playback worker. A cancelled operation returns false with an empty error.
// Optional position reports device playback seconds, or -1 when unavailable.
// "default" resolves the OS default output once per call and stays fixed during playback.
bool play_preview(const std::vector<float> &interleaved, uint32_t rate, const std::string &id,
		  const std::atomic<bool> &cancel, std::string &error, std::atomic<double> *position = nullptr);
} // namespace lbm
