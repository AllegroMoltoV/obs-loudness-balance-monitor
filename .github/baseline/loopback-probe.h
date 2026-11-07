#pragma once
#include <cstdint>
#include <memory>
#include <string>

// Observation only: captures the shared render endpoint, never feeds OBS.
// Call start before render start, stop after render stop, then save/stats.
class LoopbackProbe {
public:
    struct Stats {
        uint32_t rate=0, channels=0, capacity_seconds=0, endpoint_buffer_frames=0;
        uint64_t pcm_capacity_frames=0, event_capacity=0, allocated_bytes=0;
        uint64_t packets=0, captured_frames=0, stored_frames=0, stored_packets=0;
        uint64_t dropped_frames=0, dropped_packets=0, silent_packets=0;
        uint64_t discontinuity_packets=0, timestamp_error_packets=0;
        uint64_t start_clock_ns=0, stop_clock_ns=0, failures=0;
        int32_t last_hresult=0;
        std::string failure;
    };
    // Capacity includes pre/post-render time; allowed range 1..310 seconds.
    LoopbackProbe(const std::string &explicit_render_device_id, uint32_t capacity_seconds);
    ~LoopbackProbe();
    LoopbackProbe(const LoopbackProbe&)=delete;
    LoopbackProbe& operator=(const LoopbackProbe&)=delete;
    bool start(); // Blocks until capture is running or initialization fails.
    void stop() noexcept;
    const Stats &stats() const noexcept; // Read only after stop or failed start.
    void save(const std::string &prefix) const; // Exclusive new files; throws on failure.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Resolve only; never change the OS default.
std::string default_render_device_id();
