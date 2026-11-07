#if !defined(_WIN32) && !defined(__APPLE__)
#include "preview-output.h"
#include <pulse/pulseaudio.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
#include <cstring>
namespace lbm {
namespace {
using Clock = std::chrono::steady_clock;
struct Pulse {
	pa_mainloop *loop = nullptr;
	pa_context *context = nullptr;
	pa_stream *stream = nullptr;
	~Pulse()
	{
		if (stream) {
			if (PA_STREAM_IS_GOOD(pa_stream_get_state(stream)))
				pa_stream_disconnect(stream);
			pa_stream_unref(stream);
		}
		if (context) {
			pa_context_disconnect(context);
			pa_context_unref(context);
		}
		if (loop)
			pa_mainloop_free(loop);
	}
	bool pump()
	{
		int ignored = 0;
		if (pa_mainloop_iterate(loop, 0, &ignored) < 0)
			return false;
		const auto state = pa_context_get_state(context);
		if (!PA_CONTEXT_IS_GOOD(state))
			return false;
		if (stream && !PA_STREAM_IS_GOOD(pa_stream_get_state(stream)))
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return true;
	}
};
struct Completion {
	bool called = false;
	bool success = false;
};
struct DefaultSink {
	bool called = false;
	std::string name;
};
static void server_info(pa_context *, const pa_server_info *info, void *value)
{
	auto &sink = *static_cast<DefaultSink *>(value);
	// The server owns info; retain the concrete name for this playback only.
	try {
		if (info && info->default_sink_name)
			sink.name = info->default_sink_name;
	} catch (...) {
		// Do not unwind through the PulseAudio C callback; report lookup failure.
		sink.name.clear();
	}
	sink.called = true;
}
static void completed(pa_stream *, int success, void *value)
{
	auto &c = *static_cast<Completion *>(value);
	c.success = success != 0;
	c.called = true;
}
static bool operation(Pulse &p, pa_operation *op, Completion &done, const std::atomic<bool> *cancel = nullptr)
{
	if (!op)
		return false;
	const auto limit = Clock::now() + std::chrono::seconds(3);
	while (!done.called && pa_operation_get_state(op) == PA_OPERATION_RUNNING && Clock::now() < limit) {
		if (cancel && cancel->load())
			break;
		if (!p.pump())
			break;
	}
	const bool ok = done.called && done.success;
	if (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
		pa_operation_cancel(op);
	pa_operation_unref(op);
	return ok;
}
} // namespace
bool play_preview_on_device(const std::vector<float> &pcm, uint32_t rate, const std::string &requested_device,
			    const std::atomic<bool> &cancel, std::string &error, std::atomic<double> *position)
{
	if (position)
		position->store(-1.0);
	Pulse p;
	p.loop = pa_mainloop_new();
	if (!p.loop)
		throw std::runtime_error("PulseAudio mainloop allocation failed");
	p.context = pa_context_new(pa_mainloop_get_api(p.loop), "Loudness Balance Monitor");
	if (!p.context || pa_context_connect(p.context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
		throw std::runtime_error("PulseAudio output initialization failed");
	const auto setup_limit = Clock::now() + std::chrono::seconds(10);
	while (pa_context_get_state(p.context) != PA_CONTEXT_READY) {
		if (cancel.load())
			return false;
		if (Clock::now() >= setup_limit || !p.pump()) {
			error = "PulseAudio server unavailable";
			throw std::runtime_error("PulseAudio output initialization failed");
		}
	}
	std::string device = requested_device;
	if (device == "default") {
		DefaultSink sink;
		pa_operation *op = pa_context_get_server_info(p.context, server_info, &sink);
		if (!op)
			throw std::runtime_error("PulseAudio default output lookup failed");
		while (!sink.called && pa_operation_get_state(op) == PA_OPERATION_RUNNING &&
		       Clock::now() < setup_limit) {
			if (cancel.load() || !p.pump())
				break;
		}
		if (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
			pa_operation_cancel(op);
		pa_operation_unref(op);
		if (cancel.load())
			return false;
		if (!sink.called || sink.name.empty() || sink.name.front() == '@')
			throw std::runtime_error("PulseAudio default output unavailable");
		device = std::move(sink.name);
	}
	pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, rate, 2};
	pa_channel_map map;
	pa_channel_map_init_stereo(&map);
	p.stream = pa_stream_new(p.context, "Recorded mix preview", &spec, &map);
	if (!p.stream)
		throw std::runtime_error("PulseAudio output initialization failed");
	pa_buffer_attr attr;
	attr.maxlength = static_cast<uint32_t>(-1);
	attr.tlength = rate * 8 / 10;
	// Finite playback must stop at the queued tail instead of advancing through silence.
	attr.prebuf = static_cast<uint32_t>(-1);
	attr.minreq = static_cast<uint32_t>(-1);
	attr.fragsize = static_cast<uint32_t>(-1);
	const auto flags = static_cast<pa_stream_flags_t>(PA_STREAM_START_CORKED | PA_STREAM_DONT_MOVE |
							  PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING);
	if (pa_stream_connect_playback(p.stream, device.c_str(), &attr, flags, nullptr, nullptr) < 0)
		throw std::runtime_error("PulseAudio output initialization failed");
	while (pa_stream_get_state(p.stream) != PA_STREAM_READY) {
		if (cancel.load())
			return false;
		if (Clock::now() >= setup_limit || !p.pump()) {
			error = "Output device connection failed";
			throw std::runtime_error("PulseAudio output initialization failed");
		}
	}
	const char *actual = pa_stream_get_device_name(p.stream);
	if (!actual || device != actual) {
		error = "Requested output device was not selected";
		throw std::runtime_error("PulseAudio output initialization failed");
	}
	const size_t total = pcm.size() * sizeof(float);
	size_t sent = 0;
	bool started = false;
	const double duration = double(pcm.size() / 2) / rate;
	double last_position = 0.0;
	const auto begin = Clock::now();
	Completion drained;
	pa_operation *drain = nullptr;
	int result = 2;
	while (Clock::now() - begin < std::chrono::duration<double>(double(pcm.size() / 2) / rate + 10.0)) {
		if (cancel.load()) {
			result = 3;
			break;
		}
		if (!p.pump()) {
			error = pa_strerror(pa_context_errno(p.context));
			break;
		}
		actual = pa_stream_get_device_name(p.stream);
		if (!actual || device != actual) {
			error = "Output device changed";
			break;
		}
		if (position && started) {
			pa_usec_t usec = 0;
			if (pa_stream_get_time(p.stream, &usec) == 0) {
				last_position = std::clamp(double(usec) / 1000000.0, last_position, duration);
				position->store(last_position);
			} else if (pa_context_errno(p.context) != PA_ERR_NODATA) {
				position->store(-1.0);
			}
		}
		if (sent < total) {
			const size_t writable = pa_stream_writable_size(p.stream);
			if (writable == static_cast<size_t>(-1))
				break;
			size_t bytes = std::min({writable, total - sent, size_t(4096 * 8)});
			bytes -= bytes % 8;
			if (bytes > 0) {
				const auto *data = reinterpret_cast<const uint8_t *>(pcm.data()) + sent;
				if (pa_stream_write(p.stream, data, bytes, nullptr, 0, PA_SEEK_RELATIVE) < 0)
					break;
				sent += bytes;
				if (!started) {
					Completion uncorked;
					if (!operation(p, pa_stream_cork(p.stream, 0, completed, &uncorked), uncorked,
						       &cancel)) {
						if (cancel.load())
							result = 3;
						break;
					}
					started = true;
				}
			}
		}
		if (sent == total && !drain) {
			drain = pa_stream_drain(p.stream, completed, &drained);
			if (!drain)
				break;
		}
		if (drained.called) {
			result = drained.success ? 0 : 2;
			break;
		}
	}
	if (drain) {
		if (pa_operation_get_state(drain) == PA_OPERATION_RUNNING)
			pa_operation_cancel(drain);
		pa_operation_unref(drain);
	}
	if (result != 0 && pa_stream_get_state(p.stream) == PA_STREAM_READY) {
		Completion corked;
		const bool cork_ok = operation(p, pa_stream_cork(p.stream, 1, completed, &corked), corked);
		Completion flushed;
		const bool flush_ok = operation(p, pa_stream_flush(p.stream, completed, &flushed), flushed);
		if (!cork_ok || !flush_ok) {
			error = "Output cancellation cleanup failed";
			result = 2;
		}
	}
	if (pa_stream_get_state(p.stream) == PA_STREAM_READY && pa_stream_disconnect(p.stream) < 0) {
		error = "Output stream disconnect failed";
		result = 2;
	}
	if (result == 2 && error.empty())
		error = "PulseAudio playback failed or timed out";
	if (result == 0 && sent == total && !cancel.load() && position)
		position->store(duration);
	return result == 0 && sent == total;
}
} // namespace lbm
#endif
