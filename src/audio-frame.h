#pragma once

#include <cstdint>
#include <string>

namespace lbm {

// Audio frame for transfer between audio callback and worker thread
struct AudioFrame {
	// Maximum samples per frame (enough for 4096 samples at any sample rate)
	static constexpr size_t kMaxSamples = 4096;

	// Mono samples (downmixed from stereo if needed)
	float samples[kMaxSamples];

	// Number of valid samples in the buffer
	uint32_t frame_count{0};

	// Timestamp from OBS
	uint64_t timestamp{0};

	// Source type
	enum class SourceType { Voice, BGM };
	SourceType source_type{SourceType::Voice};

	// Source name (for identifying BGM sources)
	char source_name[256]{};

	// Default constructor
	AudioFrame() = default;

	// Clear the frame
	void clear()
	{
		frame_count = 0;
		timestamp = 0;
		source_name[0] = '\0';
	}
};

} // namespace lbm
