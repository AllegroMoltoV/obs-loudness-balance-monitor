#include "playback.h"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
volatile std::sig_atomic_t interrupted = 0;
static void interrupt_handler(int) { interrupted = 1; }
int main(int argc, char** argv)
{
    try {
        if (argc < 5 || argc > 6 || std::strcmp(argv[1], "--play") != 0) {
            std::cerr << "Usage: lbm-preview --play pcm-f32le-stereo.raw 44100|48000 explicit-device [cancel-ms]\n";
            return 2;
        }
        const std::string rate_text(argv[3]);
        if (rate_text != "44100" && rate_text != "48000") throw std::runtime_error("unsupported sample rate");
        const uint32_t rate = static_cast<uint32_t>(std::stoul(rate_text));
        const std::string device(argv[4]);
        if (device.empty() || device == "default" || device.front() == '@') throw std::runtime_error("explicit device UID/sink name required");
        int cancel_ms = -1;
        if (argc == 6) {
            size_t parsed = 0;
            cancel_ms = std::stoi(argv[5], &parsed);
            if (parsed != std::strlen(argv[5]) || cancel_ms < 0) throw std::runtime_error("invalid cancel-ms");
        }
        const uint16_t endian = 1;
        if (*reinterpret_cast<const uint8_t*>(&endian) != 1 || sizeof(float) != 4) throw std::runtime_error("requires little-endian float32 host");
        std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
        const size_t samples = static_cast<size_t>(rate) * 5 * 2;
        if (!input || input.tellg() != static_cast<std::streamoff>(samples * sizeof(float))) throw std::runtime_error("input must contain exactly five seconds of stereo float32 PCM");
        std::vector<float> pcm(samples);
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(samples * sizeof(float)))) throw std::runtime_error("PCM read failed");
        for (float value : pcm) if (!std::isfinite(value) || std::abs(value) > 1.0f) throw std::runtime_error("PCM contains non-finite or out-of-range samples");
        std::signal(SIGINT, interrupt_handler);
        std::signal(SIGTERM, interrupt_handler);
        const int result = playback(pcm, rate, device, cancel_ms);
        std::cout << "result=" << (result == 0 ? "completed" : result == 3 ? "cancelled" : "failed") << " frames=" << samples / 2 << '\n';
        return result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
