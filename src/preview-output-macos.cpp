#ifdef __APPLE__
#include "preview-output.h"
#include <AudioToolbox/AudioQueue.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace lbm {
namespace {
using Clock = std::chrono::steady_clock;
struct Queue {
    AudioQueueRef value = nullptr;
    const std::vector<float>& pcm;
    size_t submitted = 0;
    OSStatus error = noErr;
    bool accepting = true;
    const std::atomic<bool>& cancel;
    Queue(const std::vector<float>& input, const std::atomic<bool>& stop) : pcm(input), cancel(stop) {}
    ~Queue() { if (value) AudioQueueDispose(value, true); }
};
static bool device_matches(AudioDeviceID id, CFStringRef wanted)
{
    AudioObjectPropertyAddress addr{kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef uid = nullptr;
    UInt32 bytes = sizeof(uid);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &bytes, &uid) != noErr || !uid) return false;
    const bool match = CFEqual(uid, wanted);
    CFRelease(uid);
    if (!match) return false;
    addr.mSelector = kAudioDevicePropertyDeviceIsAlive;
    UInt32 alive = 0;
    bytes = sizeof(alive);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &bytes, &alive) != noErr || !alive) return false;
    addr.mSelector = kAudioDevicePropertyStreams;
    addr.mScope = kAudioDevicePropertyScopeOutput;
    bytes = 0;
    return AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &bytes) == noErr && bytes > 0;
}
static std::vector<AudioDeviceID> list_devices()
{
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &bytes) != noErr || !bytes) return {};
    std::vector<AudioDeviceID> devices(bytes / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &bytes, devices.data()) != noErr) return {};
    devices.resize(bytes / sizeof(AudioDeviceID));
    return devices;
}
static AudioDeviceID find_device(CFStringRef wanted)
{
    for (AudioDeviceID id : list_devices())
        if (device_matches(id, wanted)) return id;
    return kAudioObjectUnknown;
}
static CFStringRef default_output_uid()
{
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID id = kAudioObjectUnknown;
    UInt32 bytes = sizeof(id);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &bytes, &id) != noErr || id == kAudioObjectUnknown)
        throw std::runtime_error("Default output device unavailable");
    addr.mSelector = kAudioDevicePropertyDeviceUID;
    CFStringRef uid = nullptr;
    bytes = sizeof(uid);
    const OSStatus status = AudioObjectGetPropertyData(id, &addr, 0, nullptr, &bytes, &uid);
    if (status != noErr || !uid || CFStringGetLength(uid) == 0) {
        if (uid) CFRelease(uid);
        throw std::runtime_error("Default output device UID unavailable");
    }
    return uid;
}
static void refill(void* data, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    auto& state = *static_cast<Queue*>(data);
    if (!state.accepting || state.error != noErr || state.cancel.load()) return;
    const size_t total = state.pcm.size() * sizeof(float);
    const size_t bytes = std::min(total - state.submitted, static_cast<size_t>(buffer->mAudioDataBytesCapacity));
    if (bytes == 0) return;
    std::memcpy(buffer->mAudioData, reinterpret_cast<const uint8_t*>(state.pcm.data()) + state.submitted, bytes);
    buffer->mAudioDataByteSize = static_cast<UInt32>(bytes);
    state.error = AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    if (state.error == noErr) state.submitted += bytes;
}
} // namespace
bool play_preview_on_device(const std::vector<float>& pcm, uint32_t rate, const std::string& device, const std::atomic<bool>& cancel, std::string& error, std::atomic<double>* position)
{
    if (position) position->store(-1.0);
    CFStringRef uid = device == "default" ? default_output_uid() : CFStringCreateWithCString(nullptr, device.c_str(), kCFStringEncodingUTF8);
    if (!uid) throw std::runtime_error("Invalid output device UID");
    const AudioDeviceID id = find_device(uid);
    if (id == kAudioObjectUnknown) { CFRelease(uid); throw std::runtime_error("Output device unavailable"); }
    Queue state(pcm, cancel);
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 8;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 8;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    // All callbacks run on this thread's run loop. State is not shared with a worker.
    state.error = AudioQueueNewOutput(&format, refill, &state, CFRunLoopGetCurrent(), kCFRunLoopCommonModes, 0, &state.value);
    if (state.error == noErr) state.error = AudioQueueSetProperty(state.value, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
    if (state.error == noErr) state.error = AudioQueueSetParameter(state.value, kAudioQueueParam_Volume, 1.0f);
    for (int i = 0; i < 3 && state.error == noErr; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        state.error = AudioQueueAllocateBuffer(state.value, 4096 * 8, &buffer);
        if (state.error == noErr) refill(&state, state.value, buffer);
    }
    int result = 2;
    bool draining = false;
    AudioQueueTimelineRef timeline = nullptr;
    if (position && state.error == noErr && AudioQueueCreateTimeline(state.value, &timeline) != noErr)
        timeline = nullptr;
    const double duration = double(pcm.size() / 2) / rate;
    double last_position = 0.0;
    const auto begin = Clock::now();
    if (state.error == noErr && !cancel.load()) {
        state.error = AudioQueueStart(state.value, nullptr);
        while (state.error == noErr && Clock::now() - begin < std::chrono::duration<double>(double(pcm.size() / 2) / rate + 10.0)) {
            if (cancel.load()) { result = 3; break; }
            if (!device_matches(id, uid)) { error = "Output device disappeared or changed"; break; }
            if (position && timeline) {
                AudioTimeStamp timestamp{};
                Boolean discontinuity = false;
                const OSStatus status = AudioQueueGetCurrentTime(state.value, timeline, &timestamp, &discontinuity);
                if (status == noErr && !discontinuity && (timestamp.mFlags & kAudioTimeStampSampleTimeValid) &&
                    std::isfinite(timestamp.mSampleTime) && timestamp.mSampleTime >= 0.0) {
                    last_position = std::clamp(timestamp.mSampleTime / rate, last_position, duration);
                    position->store(last_position);
                } else {
                    position->store(-1.0);
                }
            }
            if (!draining && state.submitted == pcm.size() * sizeof(float)) {
                state.accepting = false;
                state.error = AudioQueueStop(state.value, false);
                if (state.error != noErr) break;
                draining = true;
            }
            if (draining) {
                UInt32 running = 1;
                UInt32 bytes = sizeof(running);
                state.error = AudioQueueGetProperty(state.value, kAudioQueueProperty_IsRunning, &running, &bytes);
                if (state.error != noErr) break;
                if (!running) { result = 0; break; }
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, false);
        }
    } else if (state.error == noErr && cancel.load()) result = 3;
    state.accepting = false;
    if (state.value) {
        if (result != 0 && AudioQueueStop(state.value, true) != noErr) result = 2;
        const OSStatus disposed = AudioQueueDispose(state.value, true);
        state.value = nullptr;
        if (disposed != noErr) { error = "AudioQueue disposal failed: " + std::to_string(disposed); result = 2; }
    }
    CFRelease(uid);
    if (result == 2 && error.empty()) error = "AudioQueue playback failed or timed out: " + std::to_string(state.error);
    if (result == 0 && !cancel.load() && position) position->store(duration);
    return result == 0;
}
} // namespace lbm
#endif
