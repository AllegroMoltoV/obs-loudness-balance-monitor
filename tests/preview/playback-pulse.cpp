// Independent probe; API references and OBS source references are in README.md.
#include "playback.h"
#include <pulse/pulseaudio.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <cstring>
using Clock = std::chrono::steady_clock;
struct Pulse {
    pa_mainloop* loop = nullptr;
    pa_context* context = nullptr;
    pa_stream* stream = nullptr;
    ~Pulse() {
        if (stream) {
            if (PA_STREAM_IS_GOOD(pa_stream_get_state(stream))) pa_stream_disconnect(stream);
            pa_stream_unref(stream);
        }
        if (context) { pa_context_disconnect(context); pa_context_unref(context); }
        if (loop) pa_mainloop_free(loop);
    }
    bool pump() {
        int ignored = 0;
        if (pa_mainloop_iterate(loop, 0, &ignored) < 0) return false;
        const auto state = pa_context_get_state(context);
        if (!PA_CONTEXT_IS_GOOD(state)) return false;
        if (stream && !PA_STREAM_IS_GOOD(pa_stream_get_state(stream))) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    }
};
struct Completion { bool called = false; bool success = false; };
static void completed(pa_stream*, int success, void* value) {
    auto& c = *static_cast<Completion*>(value);
    c.success = success != 0;
    c.called = true;
}
static bool operation(Pulse& p, pa_operation* op, Completion& done, Clock::time_point cancel_at = Clock::time_point::max(), bool cancellable = false) {
    if (!op) return false;
    const auto limit = Clock::now() + std::chrono::seconds(3);
    while (!done.called && pa_operation_get_state(op) == PA_OPERATION_RUNNING && Clock::now() < limit) {
        if (cancellable && (interrupted || Clock::now() >= cancel_at)) break;
        if (!p.pump()) break;
    }
    const bool ok = done.called && done.success;
    if (pa_operation_get_state(op) == PA_OPERATION_RUNNING) pa_operation_cancel(op);
    pa_operation_unref(op);
    return ok;
}
int playback(const std::vector<float>& pcm, uint32_t rate, const std::string& device, int cancel_ms)
{
    Pulse p;
    p.loop = pa_mainloop_new();
    if (!p.loop) return 2;
    p.context = pa_context_new(pa_mainloop_get_api(p.loop), "lbm-isolated-finite-preview");
    if (!p.context || pa_context_connect(p.context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) return 2;
    const auto setup_limit = Clock::now() + std::chrono::seconds(10);
    while (pa_context_get_state(p.context) != PA_CONTEXT_READY) {
        if (interrupted) return 3;
        if (Clock::now() >= setup_limit || !p.pump()) { std::cerr << "PulseAudio server unavailable\n"; return 2; }
    }
    pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, rate, 2};
    pa_channel_map map;
    pa_channel_map_init_stereo(&map);
    p.stream = pa_stream_new(p.context, "five-second-preview", &spec, &map);
    if (!p.stream) return 2;
    pa_buffer_attr attr;
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = rate * 8 / 10;
    attr.prebuf = 0;
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = static_cast<uint32_t>(-1);
    const auto flags = static_cast<pa_stream_flags_t>(PA_STREAM_START_CORKED | PA_STREAM_DONT_MOVE);
    if (pa_stream_connect_playback(p.stream, device.c_str(), &attr, flags, nullptr, nullptr) < 0) return 2;
    while (pa_stream_get_state(p.stream) != PA_STREAM_READY) {
        if (interrupted) return 3;
        if (Clock::now() >= setup_limit || !p.pump()) { std::cerr << "sink connection failed\n"; return 2; }
    }
    const char* actual = pa_stream_get_device_name(p.stream);
    if (!actual || device != actual) { std::cerr << "requested sink was not selected\n"; return 2; }
    const size_t total = pcm.size() * sizeof(float);
    size_t sent = 0;
    bool started = false;
    const auto begin = Clock::now();
    const auto cancel_at = cancel_ms < 0 ? Clock::time_point::max() : begin + std::chrono::milliseconds(cancel_ms);
    Completion drained;
    pa_operation* drain = nullptr;
    int result = 2;
    while (Clock::now() - begin < std::chrono::seconds(20)) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
        if (interrupted || (cancel_ms >= 0 && elapsed >= cancel_ms)) { result = 3; break; }
        if (!p.pump()) { std::cerr << "stream/server failed: " << pa_strerror(pa_context_errno(p.context)) << '\n'; break; }
        actual = pa_stream_get_device_name(p.stream);
        if (!actual || device != actual) { std::cerr << "sink changed\n"; break; }
        if (sent < total) {
            const size_t writable = pa_stream_writable_size(p.stream);
            if (writable == static_cast<size_t>(-1)) break;
            size_t bytes = std::min({writable, total - sent, size_t(4096 * 8)});
            bytes -= bytes % 8;
            if (bytes > 0) {
                const auto* data = reinterpret_cast<const uint8_t*>(pcm.data()) + sent;
                if (pa_stream_write(p.stream, data, bytes, nullptr, 0, PA_SEEK_RELATIVE) < 0) break;
                sent += bytes;
                if (!started) {
                    Completion uncorked;
                    if (!operation(p, pa_stream_cork(p.stream, 0, completed, &uncorked), uncorked, cancel_at, true)) {
                        if (interrupted || Clock::now() >= cancel_at) result = 3;
                        break;
                    }
                    started = true;
                }
            }
        }
        if (sent == total && !drain) {
            drain = pa_stream_drain(p.stream, completed, &drained);
            if (!drain) break;
        }
        if (drained.called) { result = drained.success ? 0 : 2; break; }
    }
    if (drain) {
        if (pa_operation_get_state(drain) == PA_OPERATION_RUNNING) pa_operation_cancel(drain);
        pa_operation_unref(drain);
    }
    if (result != 0 && pa_stream_get_state(p.stream) == PA_STREAM_READY) {
        Completion corked;
        const bool cork_ok = operation(p, pa_stream_cork(p.stream, 1, completed, &corked), corked);
        Completion flushed;
        const bool flush_ok = operation(p, pa_stream_flush(p.stream, completed, &flushed), flushed);
        if (!cork_ok || !flush_ok) { std::cerr << "cancel cleanup failed; disconnecting stream\n"; result = 2; }
    }
    if (pa_stream_get_state(p.stream) == PA_STREAM_READY && pa_stream_disconnect(p.stream) < 0) {
        std::cerr << "stream disconnect failed\n"; result = 2;
    }
    std::cout << "submitted_bytes=" << sent << " expected_bytes=" << total << " drain_success=" << drained.success << '\n';
    if (result == 0 && sent != total) return 2;
    return result;
}
