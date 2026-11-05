#pragma once
#include <csignal>
#include <cstdint>
#include <string>
#include <vector>
extern volatile std::sig_atomic_t interrupted;
// 0: completed, 2: failure, 3: cancelled. PCM ownership lasts through teardown.
int playback(const std::vector<float>& pcm, uint32_t rate, const std::string& device, int cancel_ms);

#ifdef __APPLE__
int apple_virtual_output_uid();
#endif
